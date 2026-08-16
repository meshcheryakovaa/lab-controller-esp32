// =============================================================================
//  api/TelemetryBatcher.h — turns the channel data plane into WebSocket frames.
//
//  Three rules, each of them the difference between a responsive instrument and
//  a stuttering one:
//
//  1. BATCH BY TIME, NOT BY SAMPLE.  Twenty channels at 80 Hz is 1600 updates a
//     second; the browser needs five frames a second.  The batcher marks
//     channels dirty as they update and emits one frame per tick.
//
//  2. SEND ONLY WHAT WAS ASKED FOR.  A client subscribes to a set of handles.
//     An open "System" page costs the firmware no telemetry at all.
//
//  3. DROP, NEVER QUEUE.  If the socket is busy the frame is discarded and the
//     dirty marks are kept, so the next frame simply carries fresher values.
//     Nothing is lost that anyone wanted.
//
//  Subscriptions are a single union across clients rather than per-client sets:
//  one browser tab is the normal case, and per-client filtering would mean a
//  serialisation pass per client.  Clients ignore handles they did not ask for.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "api/IWebSocketSink.h"
#include "core/Clock.h"
#include "core/EventBus.h"
#include "core/Scheduler.h"
#include "services/ChannelManager.h"

namespace lc {

class TelemetryBatcher {
 public:
  static constexpr std::size_t kFrameBufferBytes = 2048;
  static constexpr float kDefaultRateHz = 5.0f;
  static constexpr float kMaxRateHz = 20.0f;

  TelemetryBatcher(ChannelManager& channels, const IClock& clock)
      : channels_(channels), clock_(clock) {}

  // Registers the flush task and subscribes to the channel data plane and to
  // the control-plane events worth forwarding.
  Status begin(Scheduler& scheduler, EventBus& events, IWebSocketSink& sink,
               float rateHz = kDefaultRateHz);

  // Replaces the subscription set.  An empty set means "send nothing".
  Status subscribe(const ChannelHandle* handles, std::size_t count);
  void subscribeAll();
  void clearSubscriptions();
  bool isSubscribed(ChannelHandle handle) const;
  std::size_t subscriptionCount() const;

  Status setRateHz(float rateHz);
  float rateHz() const { return rateHz_; }

  // Builds and sends one frame.  Public so tests can drive it directly.
  void flush();

  // --- diagnostics ---------------------------------------------------------
  std::uint32_t framesSent() const { return framesSent_; }
  std::uint32_t framesDropped() const { return framesDropped_; }
  std::uint32_t framesSkipped() const { return framesSkipped_; }
  std::uint32_t bytesSent() const { return bytesSent_; }

 private:
  static void sampleTrampoline(ChannelHandle handle, const ChannelValue& value,
                               void* context);
  static void eventTrampoline(const Event& event, void* context);
  static void flushTrampoline(void* context);

  void markDirty(ChannelHandle handle);
  void sendEvent(const Event& event);

  static constexpr std::size_t kMaskWords = (limits::kMaxChannels + 31) / 32;

  ChannelManager& channels_;
  const IClock& clock_;
  IWebSocketSink* sink_ = nullptr;
  Scheduler* scheduler_ = nullptr;
  TaskId flushTask_ = kInvalidTask;

  std::uint32_t subscribed_[kMaskWords] = {0};
  std::uint32_t dirty_[kMaskWords] = {0};
  ChannelQuality lastQuality_[limits::kMaxChannels] = {ChannelQuality::kUnknown};

  float rateHz_ = kDefaultRateHz;
  char buffer_[kFrameBufferBytes] = {0};

  std::uint32_t framesSent_ = 0;
  std::uint32_t framesDropped_ = 0;
  std::uint32_t framesSkipped_ = 0;
  std::uint32_t bytesSent_ = 0;
};

}  // namespace lc
