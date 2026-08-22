// =============================================================================
//  services/DataLogger.h — writing the experiment down (§33, §48, ADR-0019).
//
//  THE QUESTION THIS MILESTONE HAD TO ANSWER FIRST
//  What happens when the card fills up on the eighth hour of a run?  Both
//  obvious answers are wrong:
//
//    "Stop writing."      A dataset that ends without saying it ended is the
//                         one that gets published.  Silence is the failure.
//    "Stop the run."      A full card is a Reproducibility problem, not a
//                         Safety one (§49).  Killing a physical process — a
//                         sample that took eight hours to prepare — because of
//                         a storage problem trades a recoverable loss for an
//                         unrecoverable one.
//
//  So: RUNNING OUT OF SPACE STOPS THE LOG, NOT THE RIG — loudly.  The session
//  closes, the dataset is marked TRUNCATED in three places (a footer line in
//  the CSV, the entry in the index, and the run record of the experiment that
//  was writing it), a severity-4 event goes out, and the experiment keeps
//  running so the operator can decide what to save.
//
//  A RESERVE THE LOG'S DATA MAY NEVER TOUCH.
//  Logging stops at `kReserveBytes` free, not at zero.  An instrument whose
//  filesystem has been eaten by a dataset cannot serve its own web interface or
//  save its configuration — it is bricked in the field by its own success.
//  Precisely: SAMPLES never cross the reserve; the footer and the index entry,
//  which are what make the dataset legible at all, are metadata and may.  Rows
//  that would have to cross it are counted as dropped instead.
//
//  AND A SESSION THAT CANNOT FINISH DOES NOT START.
//  Rate × columns × duration is arithmetic anybody can do, so the firmware does
//  it: a run that would need 40 MB on a device with 300 KB free is refused at
//  the start, with both numbers in the message, rather than discovered eight
//  hours later.
//
//  ROWS ARE INSTANTS, NOT EVENTS.
//  One row is every logged channel at one moment, taken at a fixed rate.  A
//  channel that has not updated since the last row repeats its value — and its
//  quality bit says STALE, which is why `quality_mask` exists.  Per-channel
//  "log on change" would produce ragged rows with holes, and deciding what a
//  hole means is exactly the ambiguity a dataset must not carry.
//
//  NOTHING TOUCHES THE FILESYSTEM ON THE SAMPLING PATH.
//  Rows are formatted into a fixed RAM buffer at kTelemetry; a kBackground task
//  writes them out.  In a cooperative scheduler a flash write blocks every task
//  behind it, including the safety pass (the lesson of ADR-0018, applied on
//  purpose this time).  When the buffer overflows, the rows are counted and
//  reported — a log that silently drops samples is a log nobody can reason from.
// =============================================================================
#pragma once

#include "core/Clock.h"
#include "core/Error.h"
#include "core/EventBus.h"
#include "core/Scheduler.h"
#include "core/Types.h"
#include "services/CalibrationManager.h"
#include "services/ChannelManager.h"

namespace lc {

// Why a session is no longer recording.
enum class LogStopReason : std::uint8_t {
  kNone = 0,
  kOperator,     // somebody pressed stop, or the scenario said STOP_LOGGING
  kFull,         // the medium reached its reserve — the dataset is TRUNCATED
  kWriteFailed,  // the medium answered with an error — also TRUNCATED
  kShutdown,     // the rig was reconfigured or the controller is going down
  // M15: the offload queue filled the filesystem because nobody was collecting.
  // Distinct from kFull on purpose — "the disk is full" and "the tablet that
  // was supposed to be emptying it went away" call for different fixes.
  kOffloadBacklogFull,
};

const char* toString(LogStopReason reason);

/**
 * Where a dataset lives while it is being written (M15).
 *
 *  kSingle             One CSV that stays on the controller until somebody
 *                      downloads it.  Needs no client, and is what every
 *                      existing session and every old API caller gets.
 *  kContinuousOffload  One LOGICAL session split into ~100 KiB CSV segments.
 *                      A closed segment is handed to the browser, verified,
 *                      acknowledged, and only then deleted from flash.
 *
 * The second mode turns 640 KiB of LittleFS into a transfer buffer rather than
 * an archive.  It is strictly more capable and strictly more fragile: it needs
 * a tab open at the other end, and the interface says so.
 */
enum class LogStorageMode : std::uint8_t {
  kSingle = 0,
  kContinuousOffload,
};

const char* toString(LogStorageMode mode);
bool parseStorageMode(const char* text, LogStorageMode& out);

struct LogSpec {
  LabelString name;
  ChannelHandle channels[limits::kMaxLoggedChannels] = {};
  std::size_t channelCount = 0;
  // Rows per second.  Bounded: the point of the limit is that the arithmetic
  // below ("will this fit") stays arithmetic.
  float rateHz = 1.0f;
  // Write the raw value beside the processed one, so the dataset can be
  // recalculated after a calibration is corrected (§48).
  bool includeRaw = true;
  // What this run was, for the header.  Filled by whoever starts the session.
  LabelString experiment;
  LabelString operatorName;
  LabelString sample;

  // --- M15 -----------------------------------------------------------------
  LogStorageMode storageMode = LogStorageMode::kSingle;
  // Ceiling for ONE segment file, header and footer included.  0 means "use the
  // default"; the store clamps it to its own bounds.
  std::size_t segmentBytes = 0;
  /**
   * Which browser tab is collecting.  Not a credential — the REST routes carry
   * the ordinary authorisation — but the thing that stops a second tab, opened
   * by the same operator on the same rig, from acknowledging and thereby
   * deleting segments the first tab never received.
   */
  KeyString collectorId;
};

struct LogStatus {
  bool recording = false;
  KeyString id;
  LabelString name;
  std::size_t channelCount = 0;
  float rateHz = 0.0f;
  std::uint32_t rows = 0;
  std::uint32_t droppedRows = 0;
  std::size_t bytesWritten = 0;
  EpochMs startedEpochMs = 0;
  Micros startedUs = 0;
  bool truncated = false;
  LogStopReason stopReason = LogStopReason::kNone;
  Error lastError;
  // --- M15 -----------------------------------------------------------------
  LogStorageMode storageMode = LogStorageMode::kSingle;
  // The row number the NEXT sample will carry.  One past the last written.
  std::uint32_t nextGlobalRow = 1;
};

/**
 * One handover of formatted rows from the logger to the store (M15).
 *
 * The old interface passed text and a byte count, which was enough to write a
 * file and not enough to describe a segment: a footer has to state how many
 * rows a part holds and which global row numbers it spans, and the store has no
 * way to work that out by looking at the characters.
 *
 * A batch is never split across two files.  It is at most kBufferBytes (4 KiB)
 * and the smallest permitted segment is 32 KiB, so a batch that does not fit in
 * the current segment always fits at the start of the next one.
 */
struct LogBatch {
  const char* text = nullptr;
  std::size_t bytes = 0;
  std::uint32_t rows = 0;
  std::uint32_t firstGlobalRow = 0;
  std::uint32_t lastGlobalRow = 0;
  // Rows lost to a slow medium since the session started.  Carried here, beyond
  // what the M15 sketch asked for, so that EVERY segment footer states it: a
  // part that travels to a tablet and is read there a week later has to be able
  // to say on its own that rows were missing.
  std::uint32_t droppedRowsTotal = 0;
};

// Implemented by storage/LogStore: the logger formats and counts, the store
// knows about files, headers and the index.
class ILogSink {
 public:
  virtual ~ILogSink() = default;
  // Opens a dataset and writes its header.  `id` is filled in by the sink.
  virtual Status openSession(const LogSpec& spec, const char* const* columns,
                             std::size_t columnCount, KeyString& id) = 0;
  virtual Status appendRows(const LogBatch& batch) = 0;
  // Closes the dataset: footer, totals, and the index entry.  Called for every
  // ending, including the ones nobody wanted.
  virtual void closeSession(const LogStatus& status) = 0;
  // Bytes that may still be written before the reserve is reached.
  virtual std::size_t writableBytes() const = 0;
  /** M15: the sequence number of a segment that has just been closed, or 0.
   *  Defaulted, because a store that never rotates has nothing to announce. */
  virtual std::uint32_t takeSegmentReadyNotice() { return 0; }
};

class DataLogger {
 public:
  // Formatted rows waiting to be written.  Two flush periods at the highest
  // supported rate, so an ordinary hiccup in the filesystem costs nothing and a
  // real stall is counted rather than hidden.
  static constexpr std::size_t kBufferBytes = 4096;
  static constexpr float kMaxRateHz = 50.0f;

  // --- how wide one row can be (0.15.1-m15) --------------------------------
  //
  // These were a guess — `char row[512]` — and the guess was wrong, which is
  // how a 16-channel rig reading a saturated sensor overran its task's stack.
  // They are now DERIVED, so the bound cannot drift away from the format that
  // has to fit inside it.  If a column ever gains a field, this arithmetic is
  // where it has to be paid for.

  /** Decimals for the processed value.  A float carries about seven
   *  significant digits; a column printed with more decimals than that is not
   *  more precise, it is noise with a wider stack footprint.  Configurations
   *  asking for more are clamped rather than refused. */
  static constexpr std::uint8_t kMaxPrecision = 9;
  /** ",%.6g" of a float: sign, digit, point, six digits, "e-38" → 13.  */
  static constexpr std::size_t kRawColumnBytes = 24;
  /** ",%.9f" of ±3.4e38: sign, 39 integer digits, point, 9 decimals → 51. */
  static constexpr std::size_t kValueColumnBytes = 56;
  /** "t_ms,epoch_ms" as two 64-bit values, plus M15's ",global_row". */
  static constexpr std::size_t kRowPrefixBytes = 64;
  /** ",4294967295\n" and the terminator. */
  static constexpr std::size_t kRowSuffixBytes = 16;
  static constexpr std::size_t kRowBytes =
      kRowPrefixBytes +
      limits::kMaxLoggedChannels * (kRawColumnBytes + kValueColumnBytes) +
      kRowSuffixBytes + 1;

  DataLogger(const IClock& clock, ChannelManager& channels, Scheduler& scheduler,
             EventBus& events)
      : clock_(clock), channels_(channels), scheduler_(scheduler), events_(events) {}

  Status begin();
  void setSink(ILogSink* sink) { sink_ = sink; }

  // Starts recording.  Refuses when a session is already open, when the
  // channels do not exist, and when the arithmetic says the dataset cannot fit
  // in the space that is left.  `expectedSeconds` of 0 means "unknown", and
  // then only the reserve is checked.
  Status start(const LogSpec& spec, double expectedSeconds = 0.0);
  Status stop(LogStopReason reason = LogStopReason::kOperator);

  const LogStatus& status() const { return status_; }
  bool recording() const { return status_.recording; }

  // Rises to true exactly once per truncated session, so a caller that polls —
  // the experiment engine, writing the fact into its run record — can notice
  // without subscribing to anything.
  bool takeTruncationNotice();

  // Exposed for tests; the scheduler calls these.
  void sampleTick(Micros now);
  void flushTick();

 private:
  static void sampleTrampoline(void* context);
  static void flushTrampoline(void* context);
  void appendRow(Micros now);
  LogBatch pendingBatch() const;
  void endSession(LogStopReason reason, const Error& detail);
  void publish(std::uint8_t severity, const char* detail, ErrorCode code);

  const IClock& clock_;
  ChannelManager& channels_;
  Scheduler& scheduler_;
  EventBus& events_;
  ILogSink* sink_ = nullptr;

  LogSpec spec_;
  LogStatus status_;
  Micros periodUs_ = 1000000;
  Micros lastRowUs_ = 0;

  char buffer_[kBufferBytes];
  // One row under construction.  A MEMBER, not a local: the logger runs on the
  // Arduino loop task alongside Wi-Fi and LittleFS, and 1.4 KB of row on that
  // task's stack is 1.4 KB the deep call chains underneath do not have.  In
  // .bss it costs the same RAM and cannot take the return address with it.
  char row_[kRowBytes];
  std::size_t buffered_ = 0;
  bool truncationNotice_ = false;

  // --- M15: continuity across segment boundaries ---------------------------
  // The global row number is the thing that survives rotation.  If a row is
  // dropped because flash fell behind, the number is still consumed — so the
  // gap is VISIBLE in the CSV as a jump, rather than hidden by renumbering.
  std::uint32_t nextGlobalRow_ = 1;
  std::uint32_t bufferedRows_ = 0;
  std::uint32_t firstBufferedGlobalRow_ = 0;
  std::uint32_t lastBufferedGlobalRow_ = 0;

  TaskId sampleTask_ = kInvalidTask;
  TaskId flushTask_ = kInvalidTask;
};

}  // namespace lc
