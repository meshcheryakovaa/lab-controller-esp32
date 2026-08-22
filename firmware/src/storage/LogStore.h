// =============================================================================
//  storage/LogStore.h — datasets on disk: the CSV, its header, and the index
//  (§33, §48, ADR-0019).
//
//  DataLogger formats rows and counts what it could not write.  This class owns
//  everything that touches the filesystem: the file, the header that makes the
//  dataset re-analysable in a year, the footer that says how it ended, and the
//  index the Logs page reads.
//
//  THE HEADER IS THE POINT.
//  A column of numbers is not a measurement.  What makes it one is the block
//  above it: which experiment, which operator, which sample, which
//  configuration revision, and which calibration produced every unit in the
//  column names.  Written once, at open, from the rig as it is at that moment.
//
//  THE FOOTER IS THE OTHER POINT.
//  Every dataset ends with a line that says how it ended — including
//  `truncated: …` when the medium filled.  A file that simply stops is a file
//  somebody will assume is complete.
//
//  THE LOGGER NEVER DELETES ANYBODY'S DATA.
//  Not to make room, not to keep the index tidy.  When the index is full or the
//  medium is at its reserve, new sessions are REFUSED and the operator decides
//  what goes.  That is the difference between an instrument and an appliance.
//
//  File layout:
//    /data/logs/<id>.csv     one dataset
//    /data/logs.json         the index (metadata only, never the samples)
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Crc32.h"
#include "core/Error.h"
#include "core/Lock.h"
#include "services/CalibrationManager.h"
#include "services/DataLogger.h"
#include "storage/ConfigStorage.h"
#include "storage/IStorageBackend.h"

namespace lc {

class LogStore final : public ILogSink {
 public:
  static constexpr const char* kIndexPath = "/data/logs.json";
  static constexpr const char* kDirectory = "/data/logs";
  static constexpr std::size_t kMaxIndexBytes = 8 * 1024;
  // Space the log may never consume.  An instrument whose filesystem has been
  // eaten by its own dataset cannot serve its web interface or save its
  // configuration; that is not a full disk, that is a brick.
  static constexpr std::size_t kReserveBytes = 64 * 1024;
  // One dataset never grows past this: §33 asks for rotation at 16 MB, and on a
  // device whose whole filesystem is smaller than that, this is simply the
  // ceiling nothing may cross.
  static constexpr std::size_t kMaxSessionBytes = 16 * 1024 * 1024;

  // --- M15: segments ---------------------------------------------------------
  // A segment is a whole file — header, rows and footer — not a row budget.
  // 100 KiB is small enough that a browser holds one comfortably and that the
  // queue on a 640 KiB filesystem is several parts deep, and large enough that
  // a 2 KiB/s run rotates about once a minute rather than continuously.
  static constexpr std::size_t kDefaultSegmentBytes = 100 * 1024;
  static constexpr std::size_t kMinSegmentBytes = 32 * 1024;
  static constexpr std::size_t kMaxSegmentBytes = 256 * 1024;
  // Room kept free at the end of every segment so the footer always fits.  A
  // segment whose footer did not fit would be a file that cannot be verified,
  // which is the same as a file that cannot be trusted.
  static constexpr std::size_t kSegmentFooterReserve = 512;
  // How many finished segments may wait at once.  The filesystem would run out
  // first on a 4 MB board; this bounds the INDEX, which must not grow with the
  // length of the run (§33.12).
  static constexpr std::size_t kMaxPendingSegments = 8;
  // How far back from the newest segment number listOrphans() looks.  See the
  // comment on that function: bounded on purpose.
  static constexpr std::uint32_t kOrphanProbeDepth = 32;

  // --- how wide the header can be (0.15.1-m15) ------------------------------
  //
  // Derived from the limits, not guessed.  The column line was a 640-byte
  // buffer that a 16-channel rig with raw columns overflowed silently, and the
  // whole line was then appended into a 1024-byte header buffer that could not
  // hold it either — after which `used` exceeded the buffer and the append that
  // followed handed the filesystem a length longer than the array it read from.

  /** ",<key>.<unit>" per column, both columns per channel, plus the fixed
   *  "t_ms,epoch_ms", M15's ",global_row" and the trailing ",quality_mask". */
  static constexpr std::size_t kColumnLineBytes =
      16 + 16 +
      limits::kMaxLoggedChannels * 2 *
          (1 + limits::kKeyLength + limits::kUnitLength + 8) +
      16;
  /** The metadata block: everything above the column line.  The column line is
   *  appended to the FILE separately and deliberately never travels through
   *  this buffer — that is what made the old bound impossible to satisfy. */
  static constexpr std::size_t kHeaderBytes = 1024;

  /** What a waiting segment looks like to the API and to the collector. */
  struct SegmentInfo {
    std::uint32_t sequence = 0;
    std::size_t bytes = 0;
    std::uint32_t rows = 0;
    std::uint32_t firstRow = 0;
    std::uint32_t lastRow = 0;
    std::uint32_t payloadCrc32 = 0;
    // READY, or RECOVERED_TRUNCATED for one that a power cut interrupted.
    FixedString<24> state;
    FixedString<64> path;
  };

  LogStore(IStorageBackend& backend, ConfigStorage& storage,
           CalibrationManager* calibrations = nullptr)
      : backend_(backend), storage_(storage), calibrations_(calibrations) {}

  Status begin();

  // --- ILogSink ------------------------------------------------------------
  Status openSession(const LogSpec& spec, const char* const* columns,
                     std::size_t columnCount, KeyString& id) override;
  Status appendRows(const LogBatch& batch) override;
  void closeSession(const LogStatus& status) override;
  std::size_t writableBytes() const override;
  std::uint32_t takeSegmentReadyNotice() override;

  // --- the index -----------------------------------------------------------
  Status loadIndex(JsonDocument& out) const;
  // Removes a dataset and its file.  The only way data is ever deleted here is
  // somebody asking for it by name.
  Status removeSession(const char* id);
  // Path of a dataset's file, for the streaming export.  Empty if unknown.
  bool pathFor(const char* id, FixedString<64>& out) const;

  std::size_t sessionCount() const;

  // --- M15: the offload queue ------------------------------------------------

  /** Waiting segments of one session, oldest first.  Returns how many were
   *  written into `out`; `capacity` bounds it. */
  std::size_t listSegments(const char* id, SegmentInfo* out,
                           std::size_t capacity) const;

  /** One waiting segment by number.  False when the session or the segment is
   *  unknown — including a segment that is still being WRITTEN, which must
   *  never be served: its footer does not exist yet. */
  bool segmentInfo(const char* id, std::uint32_t sequence, SegmentInfo& out) const;

  /**
   * Delete a segment because the collector proved it has it.
   *
   * Every check here exists because passing it is what authorises a deletion:
   * the owning collector, the READY state, the exact byte count and the exact
   * checksum.  A repeat of an ACK that already succeeded is not an error — the
   * response to the first one may simply have been lost — and sets
   * `alreadyAcknowledged` instead.
   */
  Status acknowledgeSegment(const char* id, std::uint32_t sequence,
                            const char* collectorId, std::size_t bytes,
                            std::uint32_t payloadCrc32,
                            bool& alreadyAcknowledged);

  /** Files under the log directory that no index entry claims.  Never deleted
   *  automatically — an unexplained file holding measurements is a question for
   *  the operator, not a tidiness problem. */
  std::size_t listOrphans(FixedString<64>* out, std::size_t capacity) const;

  /** True while a continuous session is writing segments. */
  bool rotating() const { return open_ && mode_ == LogStorageMode::kContinuousOffload; }

 private:
  Status saveIndex(JsonDocument& document);
  Status writeHeader(const LogSpec& spec, const char* const* columns,
                     std::size_t columnCount);
  // --- M15 -------------------------------------------------------------------
  void segmentPath(std::uint32_t sequence, FixedString<64>& out) const;
  Status openSegment(std::uint32_t sequence);
  Status ensureSegmentHeader(std::uint32_t firstGlobalRow);
  // `nextSequence` is the part that will be opened straight afterwards, or 0
  // when the session is ending.  Passed through so that closing one part and
  // claiming the next happen in a single index write.
  Status finalizeSegment(bool truncated, std::uint32_t nextSequence);
  Status recordSegmentInIndex(bool truncated, std::uint32_t nextSequence);
  Status refreshActiveInIndex();
  Status recoverSessions();
  static void describeSegment(JsonObjectConst entry, SegmentInfo& out);

  IStorageBackend& backend_;
  ConfigStorage& storage_;
  CalibrationManager* calibrations_ = nullptr;

  // Serialises the store against itself.  The logger's flush runs on the loop
  // task and the offload routes run on the HTTP server's task, and both
  // read-modify-write /data/logs.json.  See core/Lock.h for what that costs
  // when it is not serialised.  `mutable` because the const query methods —
  // listSegments, segmentInfo, pathFor — read the same file the writers are
  // rewriting, and a torn read there is a segment described wrongly to a
  // collector that is about to ask for it to be deleted.
  mutable RecursiveMutex mutex_;

  FixedString<64> currentPath_;
  KeyString currentId_;
  bool open_ = false;

  // --- M15: the active segment ----------------------------------------------
  LogStorageMode mode_ = LogStorageMode::kSingle;
  std::size_t segmentLimit_ = kDefaultSegmentBytes;
  std::uint32_t sequence_ = 0;
  std::size_t segmentBytes_ = 0;      // whole file, header included
  std::size_t payloadBytes_ = 0;      // data rows only — what the CRC covers
  std::uint32_t segmentRows_ = 0;
  std::uint32_t segmentFirstRow_ = 0;
  std::uint32_t segmentLastRow_ = 0;
  std::uint32_t previousSegment_ = 0;
  Crc32 payloadCrc_;
  KeyString collectorId_;
  // The header is rewritten for every segment, so what it is built from has to
  // outlive the call that started the session.
  LogSpec spec_;
  char columnLine_[kColumnLineBytes] = {0};
  std::size_t columnLineLength_ = 0;
  // Writing the active size into the index on every flush would be a flash
  // write every half second for the length of the run (§22).  Throttled by
  // BYTES rather than by time so that no clock is needed here and the cost is
  // proportional to the data actually produced.
  static constexpr std::size_t kActiveSaveEveryBytes = 32 * 1024;
  std::size_t lastActiveSaveBytes_ = 0;
  bool headerWritten_ = false;
  // The session-wide dropped count as of the last batch, so a segment footer
  // can state it without the store having to count rows itself.
  std::uint32_t droppedAtSegmentEnd_ = 0;
  // Rises once per finished segment, read by the logger, which owns the event
  // bus.  The same pattern as takeTruncationNotice(): the store reports facts,
  // the service publishes them.
  std::uint32_t segmentReadyNotice_ = 0;
};

}  // namespace lc
