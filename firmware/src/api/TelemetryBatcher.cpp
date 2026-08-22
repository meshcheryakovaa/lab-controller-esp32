#include "api/TelemetryBatcher.h"

#include <cstdio>

#include "api/Serializers.h"

namespace lc {
namespace {

inline std::size_t wordOf(ChannelHandle handle) { return (handle - 1) / 32; }
inline std::uint32_t bitOf(ChannelHandle handle) {
  return 1u << ((handle - 1) % 32);
}

}  // namespace

Status TelemetryBatcher::begin(Scheduler& scheduler, EventBus& events,
                               IWebSocketSink& sink, float rateHz) {
  sink_ = &sink;
  scheduler_ = &scheduler;

  const Status rate = setRateHz(rateHz);
  if (!rate.ok()) return rate;

  const Status listening = channels_.addListener(sampleTrampoline, this);
  if (!listening.ok()) return listening;

  // Control-plane events the browser needs: device state, alarms, experiment
  // progress, system messages.  Measurements do NOT come through here
  // (core/EventBus.h explains why).
  events.subscribe(eventMask(EventType::kDeviceStateChanged) |
                       eventMask(EventType::kDeviceError) |
                       eventMask(EventType::kDeviceConnected) |
                       eventMask(EventType::kDeviceDisconnected) |
                       eventMask(EventType::kAlarmTriggered) |
                       eventMask(EventType::kAlarmCleared) |
                       eventMask(EventType::kSafetyTripped) |
                       eventMask(EventType::kConfigChanged) |
                       eventMask(EventType::kLogSegmentReady) |
                       eventMask(EventType::kSystemMessage),
                   eventTrampoline, this);

  const Result<TaskId> task = scheduler.addPeriodic(
      "ws.telemetry", static_cast<Micros>(1000000.0f / rateHz_),
      TaskPriority::kTelemetry, flushTrampoline, this);
  if (!task.ok()) return task.error();
  flushTask_ = task.value();
  return ok();
}

Status TelemetryBatcher::setRateHz(float rateHz) {
  if (rateHz <= 0.0f || rateHz > kMaxRateHz) {
    return fail(ErrorCode::kInvalidArgument, "rate out of range");
  }
  rateHz_ = rateHz;
  if (scheduler_ != nullptr && flushTask_ != kInvalidTask) {
    scheduler_->setPeriod(flushTask_, static_cast<Micros>(1000000.0f / rateHz_));
  }
  return ok();
}

Status TelemetryBatcher::subscribe(const ChannelHandle* handles,
                                   std::size_t count) {
  clearSubscriptions();
  if (handles == nullptr) return ok();
  for (std::size_t i = 0; i < count; ++i) {
    const ChannelHandle handle = handles[i];
    if (handle == kInvalidChannel || handle > limits::kMaxChannels) {
      return fail(ErrorCode::kChannelNotFound, "handle out of range");
    }
    subscribed_[wordOf(handle)] |= bitOf(handle);
  }
  return ok();
}

void TelemetryBatcher::subscribeAll() {
  for (std::size_t i = 0; i < kMaskWords; ++i) subscribed_[i] = 0xFFFFFFFFu;
}

void TelemetryBatcher::clearSubscriptions() {
  for (std::size_t i = 0; i < kMaskWords; ++i) {
    subscribed_[i] = 0;
    dirty_[i] = 0;
  }
}

bool TelemetryBatcher::isSubscribed(ChannelHandle handle) const {
  if (handle == kInvalidChannel || handle > limits::kMaxChannels) return false;
  return (subscribed_[wordOf(handle)] & bitOf(handle)) != 0;
}

std::size_t TelemetryBatcher::subscriptionCount() const {
  std::size_t total = 0;
  for (std::size_t i = 1; i <= limits::kMaxChannels; ++i) {
    if (isSubscribed(static_cast<ChannelHandle>(i))) ++total;
  }
  return total;
}

void TelemetryBatcher::markDirty(ChannelHandle handle) {
  if (!isSubscribed(handle)) return;
  dirty_[wordOf(handle)] |= bitOf(handle);
}

void TelemetryBatcher::sampleTrampoline(ChannelHandle handle,
                                        const ChannelValue&, void* context) {
  // Deliberately cheap: a flag, nothing else.  This runs inside the acquisition
  // path, potentially 1600 times a second.
  static_cast<TelemetryBatcher*>(context)->markDirty(handle);
}

void TelemetryBatcher::flushTrampoline(void* context) {
  static_cast<TelemetryBatcher*>(context)->flush();
}

void TelemetryBatcher::eventTrampoline(const Event& event, void* context) {
  static_cast<TelemetryBatcher*>(context)->sendEvent(event);
}

void TelemetryBatcher::flush() {
  if (sink_ == nullptr) return;

  bool anyDirty = false;
  for (std::size_t i = 0; i < kMaskWords; ++i) {
    if (dirty_[i] != 0) {
      anyDirty = true;
      break;
    }
  }
  if (!anyDirty || sink_->clientCount() == 0) {
    ++framesSkipped_;
    return;
  }

  if (!sink_->canSend()) {
    // The previous frame is still in flight.  Drop this one but KEEP the dirty
    // marks: the next frame will carry newer values for the same channels, so
    // nothing anybody wanted is actually lost.
    ++framesDropped_;
    return;
  }

  JsonDocument frame;
  frame["type"] = "channels";
  frame["t"] = clock_.nowMicros() / 1000ULL;
  frame["epoch"] = clock_.epochMillis();
  JsonObject data = frame["data"].to<JsonObject>();
  JsonObject quality = frame["quality"].to<JsonObject>();

  char key[6];
  for (std::size_t index = 1; index <= limits::kMaxChannels; ++index) {
    const ChannelHandle handle = static_cast<ChannelHandle>(index);
    if ((dirty_[wordOf(handle)] & bitOf(handle)) == 0) continue;

    const ChannelValue* value = channels_.value(handle);
    if (value == nullptr) continue;  // channel disappeared since it was marked

    std::snprintf(key, sizeof(key), "%u", static_cast<unsigned>(handle));
    data[key] = value->processed;

    // Quality is only transmitted when it changes.  In a healthy rig this
    // object is empty and costs two bytes.
    if (value->quality != lastQuality_[handle - 1]) {
      lastQuality_[handle - 1] = value->quality;
      quality[key] = toString(value->quality);
    }
  }

  if (quality.size() == 0) frame.remove("quality");

  const std::size_t written = serializeJson(frame, buffer_, sizeof(buffer_));
  if (written == 0 || written >= sizeof(buffer_) - 1) {
    // Too many subscribed channels for one frame.  Reporting it is the honest
    // move: silently truncating telemetry would show a half-updated dashboard.
    ++framesDropped_;
    for (std::size_t i = 0; i < kMaskWords; ++i) dirty_[i] = 0;
    return;
  }

  if (sink_->broadcast(buffer_, written)) {
    ++framesSent_;
    bytesSent_ += static_cast<std::uint32_t>(written);
    for (std::size_t i = 0; i < kMaskWords; ++i) dirty_[i] = 0;
  } else {
    ++framesDropped_;
  }
}

void TelemetryBatcher::sendEvent(const Event& event) {
  if (sink_ == nullptr || sink_->clientCount() == 0) return;
  if (!sink_->canSend()) {
    ++framesDropped_;
    return;
  }

  JsonDocument message;
  switch (event.type) {
    case EventType::kDeviceStateChanged:
    case EventType::kDeviceError:
    case EventType::kDeviceConnected:
    case EventType::kDeviceDisconnected:
      message["type"] = "device";
      message["handle"] = event.source;
      message["state"] = (event.detail != nullptr) ? event.detail : "";
      break;

    case EventType::kAlarmTriggered:
    case EventType::kAlarmCleared:
    case EventType::kSafetyTripped:
      message["type"] = "alarm";
      message["active"] = event.type != EventType::kAlarmCleared;
      message["severity"] = event.severity;
      message["message"] = (event.detail != nullptr) ? event.detail : "";
      break;

    case EventType::kConfigChanged:
      message["type"] = "config";
      message["section"] = (event.detail != nullptr) ? event.detail : "";
      break;

    case EventType::kLogSegmentReady:
      message["type"] = "log_segment";
      message["sequence"] = event.source;
      break;

    default:
      message["type"] = "system";
      message["severity"] = event.severity;
      message["message"] = (event.detail != nullptr) ? event.detail : "";
      break;
  }
  message["code"] = errorSymbol(event.code);
  message["t"] = clock_.nowMicros() / 1000ULL;

  const std::size_t written = serializeJson(message, buffer_, sizeof(buffer_));
  if (written == 0 || written >= sizeof(buffer_) - 1) return;
  if (sink_->broadcast(buffer_, written)) {
    ++framesSent_;
    bytesSent_ += static_cast<std::uint32_t>(written);
  } else {
    ++framesDropped_;
  }
}

}  // namespace lc
