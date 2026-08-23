// =============================================================================
//  storage/CloudUploadQueue.h — what still has to reach the cloud (M17).
//
//  The queue holds METADATA ONLY.  The measurements stay where DataLogger put
//  them, in /data/logs; a job is a note saying "this file has not been proved to
//  be in the cloud yet".  That separation is what makes the failure modes
//  survivable: a corrupt queue costs the schedule, never the data.
//
//  THREE PROPERTIES IT HAS TO KEEP.
//
//  1. It survives a power cut.  Written through IStorageBackend::writeAtomic(),
//     so an interrupted save leaves the previous queue intact rather than half
//     a document.  Anything caught mid-transfer comes back as kPending and is
//     re-checked against the remote before a byte is sent again.
//
//  2. It is bounded.  kMaxJobs and kMaxBytes exist because this file lives on
//     the same 640 KiB filesystem as the datasets it is describing, and a queue
//     that grew without limit would eventually consume the space it exists to
//     help empty.
//
//  3. It never causes a deletion.  Nothing in here removes a CSV.  A job
//     reaching kAcknowledged is the RECORD of a proof that happened elsewhere;
//     the deletion is a separate step that re-verifies first.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Error.h"
#include "core/Md5.h"
#include "core/Types.h"
#include "storage/IStorageBackend.h"

namespace lc {

/**
 * Where one file has got to.
 *
 * The states between kPending and kAcknowledged are all "in flight" and all
 * mean the same thing after a reset: nothing was proved, start again from the
 * remote check.  They are kept apart anyway because the difference is exactly
 * what an operator watching a stuck upload needs to see.
 */
enum class CloudJobState : std::uint8_t {
  kPending = 0,
  kWaitingNetwork,
  kWaitingTime,
  kRefreshingToken,
  kCreatingDirectories,
  kRequestingUploadUrl,
  kUploading,
  kVerifyingTemporary,
  kMoving,
  kVerifyingFinal,
  kAcknowledged,
  kPausedNoAuth,
  kRemoteConflict,
  kPermanentError,
};

const char* toString(CloudJobState state);
bool parseCloudJobState(const char* text, CloudJobState& out);

/** True for the states that mean "a transfer was interrupted, not finished".
 *  These are reset to kPending at boot — after which the remote is checked
 *  before anything is re-sent (§9). */
bool isInFlight(CloudJobState state);

struct CloudJob {
  std::uint32_t id = 0;
  KeyString sessionId;
  KeyString segmentId;
  FixedString<64> localPath;
  FixedString<160> remotePath;
  std::uint64_t bytes = 0;
  FixedString<12> crc32;
  FixedString<Md5::kTextBytes> md5;
  CloudJobState state = CloudJobState::kPending;
  std::uint16_t attempts = 0;
  EpochMs nextAttemptEpochMs = 0;
  DetailString lastError;
};

class CloudUploadQueue {
 public:
  static constexpr const char* kPath = "/data/cloud-queue.json";
  /** More than a day of segments at a realistic rate, and small enough that the
   *  document stays well inside kMaxBytes. */
  static constexpr std::size_t kMaxJobs = 48;
  static constexpr std::size_t kMaxBytes = 16 * 1024;
  static constexpr int kSchemaVersion = 1;

  explicit CloudUploadQueue(IStorageBackend& backend) : backend_(backend) {}

  /** Loads the queue, or starts an empty one when there is no file.  A file
   *  that will not parse is reported and NOT overwritten: see the comment in
   *  the .cpp — silently starting fresh would discard the only record of which
   *  measurements are still only on this device. */
  Status begin();

  bool loaded() const { return loaded_; }
  bool corrupt() const { return corrupt_; }
  const Error& loadError() const { return loadError_; }

  std::size_t size() const { return count_; }
  const CloudJob& at(std::size_t index) const { return jobs_[index]; }

  /** Adds a segment.  Re-adding one already queued is a no-op rather than a
   *  duplicate: LogStore can announce the same closed segment twice across a
   *  reset, and two jobs for one file would race each other to the same
   *  remote path. */
  Result<std::uint32_t> enqueue(const CloudJob& job);

  CloudJob* find(std::uint32_t id);
  const CloudJob* find(std::uint32_t id) const;

  /** The next job that may be attempted at `nowEpochMs`, or nullptr.  Skips
   *  anything waiting out a backoff, and anything in a state that needs a
   *  person. */
  CloudJob* nextRunnable(EpochMs nowEpochMs);

  Status update(const CloudJob& job);
  Status remove(std::uint32_t id);
  /** Clears jobs whose state needs no further work.  Called after the local
   *  file has actually gone. */
  Status forgetAcknowledged();

  /** Puts every interrupted job back to kPending.  Called once at boot. */
  std::size_t recoverInFlight();

  /** Pauses or resumes the whole queue.  Persisted, so a controller that was
   *  paused stays paused across a reset. */
  Status setPaused(bool paused);
  bool paused() const { return paused_; }

  void describe(JsonObject out) const;

  /** The delay before attempt number `attempt` (1-based), with jitter.
   *  Static and testable: the schedule is a decision, not a side effect. */
  static std::uint32_t backoffMs(std::uint16_t attempt, std::uint32_t jitter);
  static constexpr std::uint32_t kMaxBackoffMs = 30u * 60u * 1000u;

 private:
  Status save();

  IStorageBackend& backend_;
  CloudJob jobs_[kMaxJobs];
  std::size_t count_ = 0;
  std::uint32_t nextId_ = 1;
  bool loaded_ = false;
  bool corrupt_ = false;
  bool paused_ = false;
  Error loadError_;
};

}  // namespace lc
