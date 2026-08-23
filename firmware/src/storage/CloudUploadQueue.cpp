#include "storage/CloudUploadQueue.h"

#include <cstring>
#include <new>

#include "storage/JsonUtils.h"

namespace lc {
namespace {

struct StateName {
  CloudJobState state;
  const char* text;
};

constexpr StateName kStates[] = {
    {CloudJobState::kPending, "PENDING"},
    {CloudJobState::kWaitingNetwork, "WAITING_NETWORK"},
    {CloudJobState::kWaitingTime, "WAITING_TIME"},
    {CloudJobState::kRefreshingToken, "REFRESHING_TOKEN"},
    {CloudJobState::kCreatingDirectories, "CREATING_DIRECTORIES"},
    {CloudJobState::kRequestingUploadUrl, "REQUESTING_UPLOAD_URL"},
    {CloudJobState::kUploading, "UPLOADING"},
    {CloudJobState::kVerifyingTemporary, "VERIFYING_TEMPORARY"},
    {CloudJobState::kMoving, "MOVING"},
    {CloudJobState::kVerifyingFinal, "VERIFYING_FINAL"},
    {CloudJobState::kAcknowledged, "ACKNOWLEDGED"},
    {CloudJobState::kPausedNoAuth, "PAUSED_NO_AUTH"},
    {CloudJobState::kRemoteConflict, "REMOTE_CONFLICT"},
    {CloudJobState::kPermanentError, "PERMANENT_ERROR"},
};

// The published schedule from §12: 5 s, 15 s, 1 min, 5 min, 15 min, 30 min.
constexpr std::uint32_t kBackoff[] = {5000, 15000, 60000, 300000, 900000,
                                      1800000};

}  // namespace

const char* toString(CloudJobState state) {
  for (const StateName& entry : kStates) {
    if (entry.state == state) return entry.text;
  }
  return "PENDING";
}

bool parseCloudJobState(const char* text, CloudJobState& out) {
  if (text == nullptr) return false;
  for (const StateName& entry : kStates) {
    if (std::strcmp(entry.text, text) == 0) {
      out = entry.state;
      return true;
    }
  }
  return false;
}

bool isInFlight(CloudJobState state) {
  switch (state) {
    case CloudJobState::kRefreshingToken:
    case CloudJobState::kCreatingDirectories:
    case CloudJobState::kRequestingUploadUrl:
    case CloudJobState::kUploading:
    case CloudJobState::kVerifyingTemporary:
    case CloudJobState::kMoving:
    case CloudJobState::kVerifyingFinal:
      return true;
    default:
      return false;
  }
}

/**
 * How long before attempt `attempt`.
 *
 * The jitter is not decoration.  Several controllers in one lab lose the same
 * router at the same instant and would otherwise come back at the same instant
 * — and the thing they would all hit together is the service that was already
 * struggling.  Up to 20 % spread costs nothing and removes the synchronisation.
 */
std::uint32_t CloudUploadQueue::backoffMs(std::uint16_t attempt,
                                          std::uint32_t jitter) {
  const std::size_t steps = sizeof(kBackoff) / sizeof(kBackoff[0]);
  const std::size_t index = attempt == 0 ? 0 : (attempt - 1);
  const std::uint32_t base = index < steps ? kBackoff[index] : kMaxBackoffMs;
  const std::uint32_t spread = base / 5;  // 20 %
  return base + (spread == 0 ? 0 : (jitter % spread));
}

Status CloudUploadQueue::begin() {
  count_ = 0;
  nextId_ = 1;
  corrupt_ = false;
  loadError_ = Error{};

  if (!backend_.exists(kPath)) {
    loaded_ = true;
    return ok();
  }

  const Result<std::size_t> fileSize = backend_.size(kPath);
  if (!fileSize.ok()) return fileSize.error();
  if (fileSize.value() >= kMaxBytes) {
    corrupt_ = true;
    loadError_ = fail(ErrorCode::kPayloadTooLarge, "the upload queue is too big");
    return loadError_;
  }

  const std::size_t capacity = fileSize.value() + 1;
  char* buffer = new (std::nothrow) char[capacity];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const Result<std::size_t> read = backend_.read(kPath, buffer, capacity);
  if (!read.ok()) {
    delete[] buffer;
    return read.error();
  }

  JsonDocument document;
  const DeserializationError parsed =
      deserializeJson(document, static_cast<const char*>(buffer), read.value());
  delete[] buffer;

  if (parsed) {
    // NOT overwritten, and the uploader stops.  This file is the only record of
    // which measurements exist here and nowhere else; starting fresh would turn
    // "I cannot read the schedule" into "there was nothing to send", and the
    // CSVs would then look like ordinary local datasets nobody is waiting on.
    corrupt_ = true;
    loadError_ = fail(ErrorCode::kConfigCorrupt, parsed.c_str());
    return loadError_;
  }

  paused_ = document["paused"] | false;
  nextId_ = document["nextJobId"] | 1u;
  for (JsonObjectConst entry : document["jobs"].as<JsonArrayConst>()) {
    if (count_ >= kMaxJobs) break;
    CloudJob& job = jobs_[count_];
    job = CloudJob{};
    job.id = entry["id"] | 0u;
    job.sessionId.assign(entry["sessionId"] | "");
    job.segmentId.assign(entry["segmentId"] | "");
    job.localPath.assign(entry["localPath"] | "");
    job.remotePath.assign(entry["remotePath"] | "");
    job.bytes = entry["bytes"] | 0ull;
    job.crc32.assign(entry["crc32"] | "");
    job.md5.assign(entry["md5"] | "");
    job.attempts = entry["attempts"] | 0u;
    job.nextAttemptEpochMs = entry["nextAttemptEpochMs"] | 0ull;
    job.lastError.assign(entry["lastError"] | "");
    if (!parseCloudJobState(entry["state"] | "PENDING", job.state)) {
      job.state = CloudJobState::kPending;
    }
    if (job.id == 0 || job.localPath.empty()) continue;  // unusable, skip
    if (job.id >= nextId_) nextId_ = job.id + 1;
    ++count_;
  }

  loaded_ = true;
  return ok();
}

Status CloudUploadQueue::save() {
  JsonDocument document;
  document["schemaVersion"] = kSchemaVersion;
  document["nextJobId"] = nextId_;
  document["paused"] = paused_;
  JsonArray jobs = document["jobs"].to<JsonArray>();
  for (std::size_t i = 0; i < count_; ++i) {
    const CloudJob& job = jobs_[i];
    JsonObject entry = jobs.add<JsonObject>();
    entry["id"] = job.id;
    entry["provider"] = "yandex";
    entry["sessionId"] = jsonCopy(job.sessionId.c_str());
    entry["segmentId"] = jsonCopy(job.segmentId.c_str());
    entry["localPath"] = jsonCopy(job.localPath.c_str());
    entry["remotePath"] = jsonCopy(job.remotePath.c_str());
    entry["bytes"] = job.bytes;
    entry["crc32"] = jsonCopy(job.crc32.c_str());
    entry["md5"] = jsonCopy(job.md5.c_str());
    entry["state"] = toString(job.state);
    entry["attempts"] = job.attempts;
    entry["nextAttemptEpochMs"] = job.nextAttemptEpochMs;
    if (job.lastError.empty()) {
      entry["lastError"] = nullptr;
    } else {
      entry["lastError"] = jsonCopy(job.lastError.c_str());
    }
  }

  const std::size_t needed = measureJson(document) + 1;
  if (needed >= kMaxBytes) {
    return fail(ErrorCode::kPayloadTooLarge, "the upload queue is full");
  }
  char* buffer = new (std::nothrow) char[needed];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const std::size_t written = serializeJson(document, buffer, needed);
  // writeAtomic: an interrupted save leaves the PREVIOUS queue, never half a
  // document.  A truncated queue would be read as "fewer files are waiting",
  // which is the one wrong answer that loses data.
  const Status saved = backend_.writeAtomic(kPath, buffer, written);
  delete[] buffer;
  return saved;
}

Result<std::uint32_t> CloudUploadQueue::enqueue(const CloudJob& incoming) {
  if (corrupt_) {
    return fail(ErrorCode::kInvalidState, "the upload queue could not be read");
  }
  // Already queued?  LogStore can announce the same closed segment twice across
  // a reset, and two jobs racing each other to one remote path is exactly the
  // duplicate §9 exists to prevent.
  for (std::size_t i = 0; i < count_; ++i) {
    if (jobs_[i].localPath.equals(incoming.localPath.c_str())) {
      return jobs_[i].id;
    }
  }
  if (count_ >= kMaxJobs) {
    return fail(ErrorCode::kOutOfCapacity,
                "the upload queue is full; nothing is being collected");
  }

  CloudJob& job = jobs_[count_];
  job = incoming;
  job.id = nextId_++;
  job.state = CloudJobState::kPending;
  job.attempts = 0;
  job.nextAttemptEpochMs = 0;
  job.lastError.assign("");
  ++count_;

  const Status saved = save();
  if (!saved.ok()) {
    --count_;
    --nextId_;
    return saved;
  }
  return job.id;
}

CloudJob* CloudUploadQueue::find(std::uint32_t id) {
  for (std::size_t i = 0; i < count_; ++i) {
    if (jobs_[i].id == id) return &jobs_[i];
  }
  return nullptr;
}

const CloudJob* CloudUploadQueue::find(std::uint32_t id) const {
  for (std::size_t i = 0; i < count_; ++i) {
    if (jobs_[i].id == id) return &jobs_[i];
  }
  return nullptr;
}

CloudJob* CloudUploadQueue::nextRunnable(EpochMs nowEpochMs) {
  if (paused_ || corrupt_) return nullptr;
  for (std::size_t i = 0; i < count_; ++i) {
    CloudJob& job = jobs_[i];
    switch (job.state) {
      case CloudJobState::kAcknowledged:
      case CloudJobState::kPausedNoAuth:
      case CloudJobState::kRemoteConflict:
      case CloudJobState::kPermanentError:
        // These need a person, not a timer.  Skipping to the next segment is
        // deliberate (§12): one stuck file must not stop everything behind it.
        continue;
      default:
        break;
    }
    if (job.nextAttemptEpochMs > nowEpochMs) continue;
    return &job;
  }
  return nullptr;
}

Status CloudUploadQueue::update(const CloudJob& incoming) {
  CloudJob* job = find(incoming.id);
  if (job == nullptr) return fail(ErrorCode::kNotFound, "no such job");
  *job = incoming;
  return save();
}

Status CloudUploadQueue::remove(std::uint32_t id) {
  for (std::size_t i = 0; i < count_; ++i) {
    if (jobs_[i].id != id) continue;
    for (std::size_t j = i + 1; j < count_; ++j) jobs_[j - 1] = jobs_[j];
    --count_;
    return save();
  }
  return fail(ErrorCode::kNotFound, "no such job");
}

Status CloudUploadQueue::forgetAcknowledged() {
  std::size_t kept = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    if (jobs_[i].state == CloudJobState::kAcknowledged) continue;
    if (kept != i) jobs_[kept] = jobs_[i];
    ++kept;
  }
  if (kept == count_) return ok();
  count_ = kept;
  return save();
}

std::size_t CloudUploadQueue::recoverInFlight() {
  std::size_t moved = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    if (!isInFlight(jobs_[i].state)) continue;
    // Back to the start of the pipeline.  NOT straight to uploading: §9 says
    // the remote is checked first, because the transfer that was interrupted
    // may well have completed on the server.
    jobs_[i].state = CloudJobState::kPending;
    jobs_[i].nextAttemptEpochMs = 0;
    ++moved;
  }
  if (moved > 0) save();
  return moved;
}

Status CloudUploadQueue::setPaused(bool paused) {
  if (paused_ == paused) return ok();
  paused_ = paused;
  return save();
}

void CloudUploadQueue::describe(JsonObject out) const {
  out["paused"] = paused_;
  out["corrupt"] = corrupt_;
  if (corrupt_) out["error"] = jsonCopy(loadError_.detail.c_str());

  std::size_t pending = 0;
  std::size_t failed = 0;
  std::size_t acknowledged = 0;
  JsonArray jobs = out["jobs"].to<JsonArray>();
  for (std::size_t i = 0; i < count_; ++i) {
    const CloudJob& job = jobs_[i];
    switch (job.state) {
      case CloudJobState::kAcknowledged: ++acknowledged; break;
      case CloudJobState::kPausedNoAuth:
      case CloudJobState::kRemoteConflict:
      case CloudJobState::kPermanentError: ++failed; break;
      default: ++pending; break;
    }
    JsonObject entry = jobs.add<JsonObject>();
    entry["id"] = job.id;
    entry["sessionId"] = jsonCopy(job.sessionId.c_str());
    entry["segmentId"] = jsonCopy(job.segmentId.c_str());
    entry["bytes"] = job.bytes;
    entry["state"] = toString(job.state);
    entry["attempts"] = job.attempts;
    entry["nextAttemptEpochMs"] = job.nextAttemptEpochMs;
    // The remote PATH is safe to show and useful; no token, URL or secret is.
    entry["remotePath"] = jsonCopy(job.remotePath.c_str());
    if (job.lastError.empty()) {
      entry["lastError"] = nullptr;
    } else {
      entry["lastError"] = jsonCopy(job.lastError.c_str());
    }
  }
  out["pending"] = pending;
  out["failed"] = failed;
  out["acknowledged"] = acknowledged;
}

}  // namespace lc
