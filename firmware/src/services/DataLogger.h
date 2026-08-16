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
};

const char* toString(LogStopReason reason);

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
};

// Implemented by storage/LogStore: the logger formats and counts, the store
// knows about files, headers and the index.
class ILogSink {
 public:
  virtual ~ILogSink() = default;
  // Opens a dataset and writes its header.  `id` is filled in by the sink.
  virtual Status openSession(const LogSpec& spec, const char* const* columns,
                             std::size_t columnCount, KeyString& id) = 0;
  virtual Status appendRows(const char* text, std::size_t bytes) = 0;
  // Closes the dataset: footer, totals, and the index entry.  Called for every
  // ending, including the ones nobody wanted.
  virtual void closeSession(const LogStatus& status) = 0;
  // Bytes that may still be written before the reserve is reached.
  virtual std::size_t writableBytes() const = 0;
};

class DataLogger {
 public:
  // Formatted rows waiting to be written.  Two flush periods at the highest
  // supported rate, so an ordinary hiccup in the filesystem costs nothing and a
  // real stall is counted rather than hidden.
  static constexpr std::size_t kBufferBytes = 4096;
  static constexpr float kMaxRateHz = 50.0f;

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
  std::size_t buffered_ = 0;
  bool truncationNotice_ = false;

  TaskId sampleTask_ = kInvalidTask;
  TaskId flushTask_ = kInvalidTask;
};

}  // namespace lc
