#include "services/CloudManager.h"

#include <cstring>

#include "core/Format.h"
#include "storage/JsonUtils.h"

namespace lc {
namespace {

constexpr std::size_t kHashBufferBytes = 4096;
constexpr const char* kDefaultRoot = "disk:/LabController";

}  // namespace

const char* toString(CloudState state) {
  switch (state) {
    case CloudState::kDisabled:             return "DISABLED";
    case CloudState::kIdle:                 return "IDLE";
    case CloudState::kWaitingNetwork:       return "WAITING_NETWORK";
    case CloudState::kWaitingTime:          return "WAITING_TIME";
    case CloudState::kWaitingAuthorization: return "WAITING_AUTHORIZATION";
    case CloudState::kUploading:            return "UPLOADING";
    case CloudState::kPaused:               return "PAUSED";
    case CloudState::kBlocked:              return "BLOCKED";
  }
  return "IDLE";
}

const char* toString(CloudFailure failure) {
  switch (failure) {
    case CloudFailure::kNone:          return "NONE";
    case CloudFailure::kNoNetwork:     return "NO_NETWORK";
    case CloudFailure::kTransient:     return "TRANSIENT";
    case CloudFailure::kRateLimited:   return "RATE_LIMITED";
    case CloudFailure::kUnauthorized:  return "UNAUTHORIZED";
    case CloudFailure::kAuthRevoked:   return "AUTH_REVOKED";
    case CloudFailure::kQuotaExceeded: return "QUOTA_EXCEEDED";
    case CloudFailure::kUntrustedHost: return "UNTRUSTED_UPLOAD_URL";
    case CloudFailure::kPermanent:     return "PERMANENT";
  }
  return "NONE";
}

const char* toString(CloudLinkState state) {
  switch (state) {
    case CloudLinkState::kIdle:           return "IDLE";
    case CloudLinkState::kRequestingCode: return "REQUESTING_CODE";
    case CloudLinkState::kWaitingUser:    return "WAITING_USER";
    case CloudLinkState::kAuthorized:     return "AUTHORIZED";
    case CloudLinkState::kExpired:        return "EXPIRED";
    case CloudLinkState::kFailed:         return "FAILED";
  }
  return "IDLE";
}

Status CloudManager::begin() {
  if (root_.empty()) root_.assign(kDefaultRoot);
  const Status loaded = queue_.begin();
  if (!loaded.ok()) {
    // A queue that will not parse stops the uploader and leaves every CSV
    // exactly where it is.  Nothing is deleted on a guess.
    state_ = CloudState::kBlocked;
    lastError_ = loaded;
    return loaded;
  }
  const std::size_t recovered = queue_.recoverInFlight();
  if (recovered > 0) {
    publish(EventType::kCloudUploadRetry, 2,
            "interrupted uploads will be re-checked before re-sending",
            ErrorCode::kOk, static_cast<std::uint32_t>(recovered));
  }
  state_ = enabled_ ? CloudState::kIdle : CloudState::kDisabled;
  return ok();
}

Status CloudManager::setEnabled(bool enabled) {
  enabled_ = enabled;
  if (!enabled_) {
    state_ = CloudState::kDisabled;
  } else if (state_ == CloudState::kDisabled) {
    state_ = CloudState::kIdle;
  }
  return ok();
}

Status CloudManager::setRoot(const char* root) {
  CloudPathString normalised;
  if (!normaliseCloudRoot(root, normalised)) {
    return fail(ErrorCode::kInvalidArgument,
                "a folder of letters, digits, '.', '_' and '-'");
  }
  root_ = normalised;
  return ok();
}

bool CloudManager::networkReady() const {
  if (network_ == nullptr) return false;
  const NetworkStatus status = network_->status();
  // The fallback access point is NOT the internet.  A controller sitting on its
  // own AP with a laptop attached has a network and no route anywhere, and
  // trying anyway would burn attempts against a certainty.
  return status.stationConnected;
}

bool CloudManager::timeReady() const {
  return clock_.epochMillis() >= kPlausibleEpochMs;
}

bool CloudManager::computeMd5(const char* path,
                              FixedString<Md5::kTextBytes>& out,
                              std::uint64_t& bytes) {
  if (storage_ == nullptr) return false;
  const Result<std::size_t> size = storage_->size(path);
  if (!size.ok()) return false;

  Md5 md5;
  char buffer[kHashBufferBytes];
  std::size_t offset = 0;
  while (offset < size.value()) {
    const std::size_t want =
        (size.value() - offset) < sizeof(buffer) ? (size.value() - offset)
                                                 : sizeof(buffer);
    const Result<std::size_t> read = storage_->readAt(path, offset, buffer, want);
    if (!read.ok() || read.value() == 0) return false;
    md5.update(reinterpret_cast<const std::uint8_t*>(buffer), read.value());
    offset += read.value();
  }

  char hex[Md5::kTextBytes];
  md5.finishHex(hex);
  out.assign(hex);
  bytes = size.value();
  return true;
}

Result<std::uint32_t> CloudManager::enqueueSegment(
    const char* sessionId, const char* segmentId, const char* localPath,
    const char* fileName, std::uint64_t bytes, const char* crc32Hex) {
  if (!enabled_) return fail(ErrorCode::kInvalidState, "cloud upload is off");
  if (storage_ == nullptr) return fail(ErrorCode::kNotSupported, "no storage");

  CloudPathString sessionPath;
  if (!buildCloudSessionPath(root_.c_str(), controllerId_.c_str(), sessionId,
                             sessionPath)) {
    return fail(ErrorCode::kInvalidArgument, "that folder name cannot be used");
  }
  CloudPathString remote;
  if (!buildCloudSegmentPath(sessionPath.c_str(), fileName, remote)) {
    return fail(ErrorCode::kInvalidArgument, "that file name cannot be used");
  }

  CloudJob job;
  job.sessionId.assign(sessionId);
  job.segmentId.assign(segmentId);
  job.localPath.assign(localPath);
  job.remotePath.assign(remote.c_str());
  job.bytes = bytes;
  job.crc32.assign(crc32Hex != nullptr ? crc32Hex : "");

  // The MD5 is computed HERE, once, while the file is definitely closed and
  // definitely still present — not at upload time, when the segment may have
  // been waiting for a week and the answer has to be trusted anyway.
  FixedString<Md5::kTextBytes> md5;
  std::uint64_t actual = 0;
  if (!computeMd5(localPath, md5, actual)) {
    return fail(ErrorCode::kNotFound, "the segment could not be read");
  }
  job.md5 = md5;
  job.bytes = actual;

  const Result<std::uint32_t> id = queue_.enqueue(job);
  if (id.ok()) {
    publish(EventType::kCloudUploadQueued, 1, "a segment is queued for upload",
            ErrorCode::kOk, id.value());
  }
  return id;
}

void CloudManager::onUploadProgress(std::uint64_t sent, std::uint64_t total) {
  progress_.sentBytes = sent;
  progress_.totalBytes = total;
}

void CloudManager::publish(EventType type, std::uint8_t severity,
                           const char* detail, ErrorCode code,
                           std::uint32_t source) {
  Event event;
  event.type = type;
  event.severity = severity;
  event.code = code;
  event.detail = detail;  // static lifetime only — never a token or a URL
  event.source = source;
  events_.publish(event);
}

void CloudManager::failJob(CloudJob& job, const CloudResult& result) {
  lastError_ = result.status;
  job.lastError.assign(result.status.detail.c_str());

  switch (result.failure) {
    case CloudFailure::kNoNetwork:
      // Not an attempt.  A week in a cupboard with no router must not exhaust
      // the retry schedule and land every segment in PERMANENT_ERROR.
      job.state = CloudJobState::kWaitingNetwork;
      job.nextAttemptEpochMs = 0;
      return;

    case CloudFailure::kAuthRevoked:
      job.state = CloudJobState::kPausedNoAuth;
      publish(EventType::kCloudAuthExpired, 3,
              "the cloud account needs to be linked again", result.status.code,
              job.id);
      return;

    case CloudFailure::kQuotaExceeded:
    case CloudFailure::kUntrustedHost:
    case CloudFailure::kPermanent:
      job.state = CloudJobState::kPermanentError;
      publish(EventType::kCloudUploadFailed, 4, "an upload cannot be completed",
              result.status.code, job.id);
      return;

    default:
      break;
  }

  ++job.attempts;
  jitter_ = jitter_ * 1664525u + 1013904223u;
  const std::uint32_t backoff =
      result.retryAfterMs > 0
          ? result.retryAfterMs
          : CloudUploadQueue::backoffMs(job.attempts, jitter_);
  job.state = CloudJobState::kPending;
  job.nextAttemptEpochMs = clock_.epochMillis() + backoff;
  publish(EventType::kCloudUploadRetry, 2, "an upload will be retried",
          result.status.code, job.id);
}

/**
 * §9 — what is already on the far side?
 *
 * This runs BEFORE any upload, every time, including the first.  A connection
 * can drop after the server stored the file and before the answer arrived, and
 * from here that is indistinguishable from a transfer that never happened.  The
 * only way to tell is to ask.
 */
bool CloudManager::reconcileWithRemote(CloudJob& job, bool& finished) {
  finished = false;
  CloudObjectInfo info;

  const CloudResult found = provider_->stat(job.remotePath.c_str(), info);
  if (!found.ok()) {
    failJob(job, found);
    return false;
  }
  if (info.exists) {
    if (info.isFile && info.size == job.bytes && info.md5.equals(job.md5.c_str())) {
      // Already there, and provably the same bytes.  This is a success, not a
      // conflict: it is what a retry after an unknown outcome is supposed to
      // find.
      job.state = CloudJobState::kAcknowledged;
      finished = true;
      return true;
    }
    // Something else lives at this path.  Never overwritten — overwrite=true is
    // banned for log files precisely so that a name collision cannot destroy a
    // dataset somebody else's controller wrote.
    job.state = CloudJobState::kRemoteConflict;
    job.lastError.assign("a different file already exists at that path");
    publish(EventType::kCloudUploadConflict, 3,
            "a different file already exists in the cloud at that path",
            ErrorCode::kCloudRemoteConflict, job.id);
    return false;
  }

  // No final object.  Is there a leftover from an interrupted transfer?
  CloudPathString temporary;
  if (!buildCloudTemporaryPath(job.remotePath.c_str(), temporary)) {
    failJob(job, cloudFail(CloudFailure::kPermanent, ErrorCode::kInvalidArgument,
                        "the remote path cannot be built"));
    return false;
  }
  CloudObjectInfo partial;
  const CloudResult probed = provider_->stat(temporary.c_str(), partial);
  if (!probed.ok()) {
    failJob(job, probed);
    return false;
  }
  if (partial.exists && partial.isFile && partial.size == job.bytes
      && partial.md5.equals(job.md5.c_str())) {
    // The bytes arrived; only the rename did not.  Skip straight to it.
    job.state = CloudJobState::kMoving;
    return true;
  }
  if (partial.exists) {
    // A partial or foreign `.uploading`.  This one IS ours by name, and a
    // half-written temporary is worthless, so it is removed rather than left to
    // block the upload for ever.  The FINAL path is never treated this way.
    const CloudResult cleared = provider_->remove(temporary.c_str());
    if (!cleared.ok()) {
      failJob(job, cleared);
      return false;
    }
  }
  job.state = CloudJobState::kCreatingDirectories;
  return true;
}

bool CloudManager::advance(CloudJob& job) {
  switch (job.state) {
    case CloudJobState::kPending:
    case CloudJobState::kWaitingNetwork:
    case CloudJobState::kWaitingTime: {
      // Gates first, in the order that costs least to check.
      if (!networkReady()) {
        job.state = CloudJobState::kWaitingNetwork;
        state_ = CloudState::kWaitingNetwork;
        return false;
      }
      if (!timeReady()) {
        // Without a plausible clock every TLS certificate looks invalid.  The
        // answer is to wait for SNTP, never to stop checking certificates.
        job.state = CloudJobState::kWaitingTime;
        state_ = CloudState::kWaitingTime;
        return false;
      }
      if (storage_ == nullptr || !storage_->exists(job.localPath.c_str())) {
        // The queue describes a file that is gone.  This must never become an
        // acknowledgement: the measurements are simply not here any more, and
        // saying otherwise would be a lie about where the data is.
        job.state = CloudJobState::kPermanentError;
        job.lastError.assign("the local segment is missing");
        publish(EventType::kCloudUploadFailed, 4,
                "a queued segment is no longer on the filesystem",
                ErrorCode::kNotFound, job.id);
        return false;
      }
      job.state = CloudJobState::kRefreshingToken;
      refreshedForThisAttempt_ = false;
      return true;
    }

    case CloudJobState::kRefreshingToken: {
      const CloudResult refreshed = provider_->refreshAuthorizationIfNeeded();
      if (!refreshed.ok()) {
        state_ = CloudState::kWaitingAuthorization;
        failJob(job, refreshed);
        return false;
      }
      if (!provider_->authorized()) {
        job.state = CloudJobState::kPausedNoAuth;
        state_ = CloudState::kWaitingAuthorization;
        return false;
      }
      bool finished = false;
      if (!reconcileWithRemote(job, finished)) return false;
      return true;
    }

    case CloudJobState::kCreatingDirectories: {
      CloudPathString sessionPath;
      // The parent of the final path.  Derived, not stored: one source for a
      // path means a rename of the scheme cannot leave the two disagreeing.
      char parent[kCloudPathLength];
      std::snprintf(parent, sizeof(parent), "%s", job.remotePath.c_str());
      char* slash = std::strrchr(parent, '/');
      if (slash == nullptr) {
        failJob(job, cloudFail(CloudFailure::kPermanent, ErrorCode::kInvalidArgument,
                            "the remote path has no folder"));
        return false;
      }
      *slash = '\0';
      const CloudResult made = provider_->ensureDirectory(parent);
      if (!made.ok()) {
        failJob(job, made);
        return false;
      }
      job.state = CloudJobState::kRequestingUploadUrl;
      return true;
    }

    case CloudJobState::kRequestingUploadUrl:
    case CloudJobState::kUploading: {
      CloudPathString temporary;
      if (!buildCloudTemporaryPath(job.remotePath.c_str(), temporary)) {
        failJob(job, cloudFail(CloudFailure::kPermanent, ErrorCode::kInvalidArgument,
                            "the remote path cannot be built"));
        return false;
      }
      progress_.jobId = job.id;
      progress_.file.assign(job.localPath.c_str());
      progress_.sentBytes = 0;
      progress_.totalBytes = job.bytes;
      progress_.attempt = static_cast<std::uint16_t>(job.attempts + 1);
      state_ = CloudState::kUploading;
      job.state = CloudJobState::kUploading;
      publish(EventType::kCloudUploadStarted, 1, "sending a segment",
              ErrorCode::kOk, job.id);

      // Uploaded to the TEMPORARY name.  A partial transfer therefore never
      // occupies the final path, so a later attempt can tell "not sent yet"
      // from "sent and renamed".
      const CloudResult sent =
          provider_->upload(temporary.c_str(), *storage_, job.localPath.c_str(),
                            job.bytes, this);
      if (!sent.ok()) {
        failJob(job, sent);
        return false;
      }
      job.state = CloudJobState::kVerifyingTemporary;
      return true;
    }

    case CloudJobState::kVerifyingTemporary: {
      CloudPathString temporary;
      buildCloudTemporaryPath(job.remotePath.c_str(), temporary);
      CloudObjectInfo info;
      const CloudResult probed = provider_->stat(temporary.c_str(), info);
      if (!probed.ok()) {
        failJob(job, probed);
        return false;
      }
      if (!info.exists || !info.isFile || info.size != job.bytes
          || !info.md5.equals(job.md5.c_str())) {
        // The PUT reported success and the stored object does not match.  That
        // is a transient truth — a retry is right — but it must never be
        // mistaken for a completed upload.
        failJob(job, cloudFail(CloudFailure::kTransient, ErrorCode::kCloudChecksumMismatch,
                            "the uploaded copy does not match"));
        return false;
      }
      job.state = CloudJobState::kMoving;
      return true;
    }

    case CloudJobState::kMoving: {
      CloudPathString temporary;
      buildCloudTemporaryPath(job.remotePath.c_str(), temporary);
      const CloudResult moved =
          provider_->move(temporary.c_str(), job.remotePath.c_str());
      if (!moved.ok()) {
        failJob(job, moved);
        return false;
      }
      job.state = CloudJobState::kVerifyingFinal;
      return true;
    }

    case CloudJobState::kVerifyingFinal: {
      CloudObjectInfo info;
      const CloudResult probed = provider_->stat(job.remotePath.c_str(), info);
      if (!probed.ok()) {
        failJob(job, probed);
        return false;
      }
      if (!info.exists || !info.isFile || info.size != job.bytes
          || !info.md5.equals(job.md5.c_str())) {
        failJob(job, cloudFail(CloudFailure::kTransient, ErrorCode::kCloudChecksumMismatch,
                            "the moved copy does not match"));
        return false;
      }
      // THE acknowledgement.  Everything before this was preparation; this is
      // the only point at which the cloud copy has been read back from its
      // final path and found identical.
      job.state = CloudJobState::kAcknowledged;
      job.lastError.assign("");
      lastSuccessEpochMs_ = clock_.epochMillis();
      publish(EventType::kCloudUploadVerified, 1,
              "a segment is stored in the cloud and verified", ErrorCode::kOk,
              job.id);
      return true;
    }

    default:
      return false;
  }
}

bool CloudManager::tick() {
  if (!enabled_) { state_ = CloudState::kDisabled; return false; }
  if (provider_ == nullptr || storage_ == nullptr) return false;
  if (queue_.corrupt()) { state_ = CloudState::kBlocked; return false; }
  if (queue_.paused()) { state_ = CloudState::kPaused; return false; }

  CloudJob* found = queue_.nextRunnable(clock_.epochMillis());
  if (found == nullptr) {
    if (state_ == CloudState::kUploading) state_ = CloudState::kIdle;
    return false;
  }

  CloudJob job = *found;
  const bool more = advance(job);
  queue_.update(job);

  if (job.state == CloudJobState::kAcknowledged) {
    // Only NOW is the local file removed — and even here the acknowledgement is
    // already persisted, so a power cut between the two leaves a queue entry
    // that re-verifies and finishes the deletion next time.
    if (storage_->exists(job.localPath.c_str())) {
      const Status removed = storage_->remove(job.localPath.c_str());
      if (!removed.ok()) {
        // Keep the acknowledgement.  The cloud copy is proved; the tidying up
        // can be retried, and a failed delete must not un-prove it.
        job.lastError.assign("uploaded, but the local copy could not be removed");
        queue_.update(job);
        return true;
      }
    }
    queue_.remove(job.id);
    state_ = CloudState::kIdle;
    return true;
  }

  return more;
}

Status CloudManager::retry(std::uint32_t jobId) {
  CloudJob* job = queue_.find(jobId);
  if (job == nullptr) return fail(ErrorCode::kNotFound, "no such upload");
  CloudJob updated = *job;
  updated.state = CloudJobState::kPending;
  updated.attempts = 0;
  updated.nextAttemptEpochMs = 0;
  updated.lastError.assign("");
  return queue_.update(updated);
}

void CloudManager::describe(JsonObject out) const {
  out["provider"] = provider_ != nullptr ? provider_->name() : "none";
  out["enabled"] = enabled_;
  out["authorized"] = provider_ != nullptr && provider_->authorized();
  out["state"] = toString(state_);
  out["rootPath"] = jsonCopy(root_.c_str());
  out["networkReady"] = networkReady();
  out["timeReady"] = timeReady();
  out["lastSuccessEpochMs"] = lastSuccessEpochMs_;

  JsonObject queue = out["queue"].to<JsonObject>();
  queue_.describe(queue);

  if (progress_.jobId != 0) {
    JsonObject current = out["current"].to<JsonObject>();
    current["jobId"] = progress_.jobId;
    current["file"] = jsonCopy(progress_.file.c_str());
    current["sentBytes"] = progress_.sentBytes;
    current["totalBytes"] = progress_.totalBytes;
    current["attempt"] = progress_.attempt;
  }

  if (lastError_.ok()) {
    out["lastError"] = nullptr;
  } else {
    JsonObject error = out["lastError"].to<JsonObject>();
    error["code"] = lastError_.symbol();
    error["detail"] = jsonCopy(lastError_.detail.c_str());
  }
}

}  // namespace lc
