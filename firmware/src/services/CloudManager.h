// =============================================================================
//  services/CloudManager.h — deciding what to send, and when it is safe to
//  delete (M17).
//
//  This is the whole of the milestone's judgement, and none of its plumbing.
//  It knows nothing about TLS, OAuth or Yandex; it knows about a provider, a
//  queue, a clock and whether the network is up.  Every rule that matters is
//  therefore an ordinary host test with a fake provider:
//
//    * a local file is deleted only after the FINAL remote object has been read
//      back and matched on size and MD5;
//    * an upload interrupted by a reset checks the remote before re-sending, so
//      a transfer that actually completed is recognised instead of duplicated;
//    * a remote file that exists with DIFFERENT content is never overwritten —
//      it stops and asks;
//    * a wrong or revoked token pauses the queue and keeps the data;
//    * a cloud failure never stops the experiment, the sensors or the log.
//
//  WHAT IT DELIBERATELY DOES NOT DO.
//  It does not cut CSV files, and it does not decide what a segment is: that is
//  LogStore's job from M15, and a cloud uploader that re-cut an open file would
//  be reading a dataset while it is still being written to.
// =============================================================================
#pragma once

#include "core/Clock.h"
#include "core/EventBus.h"
#include "services/CloudPath.h"
#include "services/ICloudProvider.h"
#include "services/INetworkManager.h"
#include "storage/CloudUploadQueue.h"

namespace lc {

/** What the uploader as a whole is doing, for the interface. */
enum class CloudState : std::uint8_t {
  kDisabled = 0,
  kIdle,
  kWaitingNetwork,
  kWaitingTime,
  kWaitingAuthorization,
  kUploading,
  kPaused,
  kBlocked,      // queue corrupt, or something that needs a person
};

const char* toString(CloudState state);

struct CloudProgress {
  std::uint32_t jobId = 0;
  FixedString<64> file;
  std::uint64_t sentBytes = 0;
  std::uint64_t totalBytes = 0;
  std::uint16_t attempt = 0;
};

class CloudManager final : public ICloudUploadObserver {
 public:
  /** Fresh enough to trust a TLS certificate.  Before this the clock has never
   *  been set and every certificate looks expired or not yet valid — so the
   *  uploader waits rather than being tempted into setInsecure(). */
  static constexpr EpochMs kPlausibleEpochMs = 1600000000000ull;  // 2020-09

  CloudManager(const IClock& clock, CloudUploadQueue& queue, EventBus& events)
      : clock_(clock), queue_(queue), events_(events) {}

  void setProvider(ICloudProvider* provider) { provider_ = provider; }
  void setNetwork(const INetworkManager* network) { network_ = network; }
  void setStorage(IStorageBackend* storage) { storage_ = storage; }
  void setControllerId(const char* id) { controllerId_.assign(id); }

  Status begin();

  bool enabled() const { return enabled_; }
  Status setEnabled(bool enabled);
  Status setRoot(const char* root);
  const CloudPathString& root() const { return root_; }

  /** Queues a closed segment.  Called when LogStore announces one; never for
   *  the file currently being written. */
  Result<std::uint32_t> enqueueSegment(const char* sessionId,
                                       const char* segmentId,
                                       const char* localPath,
                                       const char* fileName,
                                       std::uint64_t bytes,
                                       const char* crc32Hex);

  /**
   * Advances at most one job by one step.  Returns true when there is more to
   * do immediately, so a worker can loop without a delay while it is making
   * progress and sleep when it is not.
   *
   * ONE STEP, not one job: an upload of 100 KiB must not hold the only thread
   * that also has to answer "what is the queue doing".
   */
  bool tick();

  CloudState state() const { return state_; }
  const CloudProgress& progress() const { return progress_; }
  const Error& lastError() const { return lastError_; }
  EpochMs lastSuccessEpochMs() const { return lastSuccessEpochMs_; }

  /** Puts a job back to the front of the schedule.  The only thing a client may
   *  ask for by id — never a path, and never an upload URL. */
  Status retry(std::uint32_t jobId);

  /** Pauses or resumes the whole queue.  Persisted, so a controller paused
   *  before a reset comes back paused rather than quietly resuming. */
  Status setQueuePaused(bool paused) { return queue_.setPaused(paused); }
  bool queuePaused() const { return queue_.paused(); }

  void describe(JsonObject out) const;

  // --- ICloudUploadObserver -------------------------------------------------
  void onUploadProgress(std::uint64_t sent, std::uint64_t total) override;

 private:
  bool advance(CloudJob& job);
  /** §9: is the work already done, or half done, on the far side? */
  bool reconcileWithRemote(CloudJob& job, bool& finished);
  void failJob(CloudJob& job, const CloudResult& result);
  void publish(EventType type, std::uint8_t severity, const char* detail,
               ErrorCode code, std::uint32_t source);
  /** Computes the MD5 of a local file 1 KiB at a time.  Never holds it whole. */
  bool computeMd5(const char* path, FixedString<Md5::kTextBytes>& out,
                  std::uint64_t& bytes);
  bool networkReady() const;
  bool timeReady() const;

  const IClock& clock_;
  CloudUploadQueue& queue_;
  EventBus& events_;
  ICloudProvider* provider_ = nullptr;
  const INetworkManager* network_ = nullptr;
  IStorageBackend* storage_ = nullptr;

  bool enabled_ = false;
  CloudPathString root_;
  KeyString controllerId_;
  CloudState state_ = CloudState::kDisabled;
  CloudProgress progress_;
  Error lastError_;
  EpochMs lastSuccessEpochMs_ = 0;
  std::uint32_t jitter_ = 0x2545F491;
  /** One refresh per 401, not a loop: a token that will not work is a reason to
   *  stop and say so, not to hammer the endpoint. */
  bool refreshedForThisAttempt_ = false;
};

}  // namespace lc
