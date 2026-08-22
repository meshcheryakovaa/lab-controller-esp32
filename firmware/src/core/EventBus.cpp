#include "core/EventBus.h"

namespace lc {

namespace {
// One level of nesting is allowed (a handler may publish a follow-up event);
// deeper nesting is a design error and is dropped rather than risking a stack
// overflow on the ESP32's modest task stacks.
constexpr std::uint8_t kMaxDispatchDepth = 2;
}  // namespace

const char* toString(EventType type) {
  switch (type) {
    case EventType::kChannelUpdated:       return "CHANNEL_UPDATED";
    case EventType::kDeviceStateChanged:   return "DEVICE_STATE_CHANGED";
    case EventType::kDeviceError:          return "DEVICE_ERROR";
    case EventType::kDeviceConnected:      return "DEVICE_CONNECTED";
    case EventType::kDeviceDisconnected:   return "DEVICE_DISCONNECTED";
    case EventType::kExperimentStarted:    return "EXPERIMENT_STARTED";
    case EventType::kExperimentStepChanged:return "EXPERIMENT_STEP_CHANGED";
    case EventType::kExperimentStopped:    return "EXPERIMENT_STOPPED";
    case EventType::kAlarmTriggered:       return "ALARM_TRIGGERED";
    case EventType::kAlarmCleared:         return "ALARM_CLEARED";
    case EventType::kSafetyTripped:        return "SAFETY_TRIPPED";
    case EventType::kConfigChanged:        return "CONFIG_CHANGED";
    case EventType::kProfileActivated:     return "PROFILE_ACTIVATED";
    case EventType::kLoggingStarted:       return "LOGGING_STARTED";
    case EventType::kLoggingStopped:       return "LOGGING_STOPPED";
    case EventType::kLogSegmentReady:      return "LOG_SEGMENT_READY";
    case EventType::kSystemMessage:        return "SYSTEM_MESSAGE";
    case EventType::kCount:                break;
  }
  return "UNKNOWN";
}

SubscriptionId EventBus::subscribe(std::uint32_t mask, EventHandler handler,
                                   void* context) {
  if (handler == nullptr || mask == 0) return kInvalidSubscription;
  if (subscriberCount_ >= limits::kMaxEventSubscribers) return kInvalidSubscription;

  Subscriber& slot = subscribers_[subscriberCount_++];
  slot.id = nextId_++;
  if (nextId_ == kInvalidSubscription) nextId_ = 1;  // wrap without hitting 0
  slot.mask = mask;
  slot.handler = handler;
  slot.context = context;
  return slot.id;
}

bool EventBus::unsubscribe(SubscriptionId id) {
  if (id == kInvalidSubscription) return false;
  for (std::size_t i = 0; i < subscriberCount_; ++i) {
    if (subscribers_[i].id != id) continue;
    // Order of subscribers is not meaningful, so swap-with-last is fine and
    // keeps removal O(1).
    subscribers_[i] = subscribers_[subscriberCount_ - 1];
    --subscriberCount_;
    return true;
  }
  return false;
}

void EventBus::publish(const Event& event) {
  if (dispatchDepth_ >= kMaxDispatchDepth) {
    ++reentrancyDrops_;
    return;
  }
  ++dispatchDepth_;
  ++published_;

  const std::uint32_t bit = eventMask(event.type);
  // Snapshot the count: a handler that unsubscribes itself must not make us
  // walk past the end of the (now shorter) array.
  const std::size_t count = subscriberCount_;
  for (std::size_t i = 0; i < count && i < subscriberCount_; ++i) {
    const Subscriber& sub = subscribers_[i];
    if ((sub.mask & bit) != 0 && sub.handler != nullptr) {
      sub.handler(event, sub.context);
    }
  }

  --dispatchDepth_;
}

bool EventBus::post(const Event& event) {
  if (queueCount_ >= kQueueSize) {
    ++dropped_;
    return false;
  }
  const std::size_t tail = (queueHead_ + queueCount_) % kQueueSize;
  queue_[tail] = event;
  ++queueCount_;
  return true;
}

std::size_t EventBus::drainPending() {
  std::size_t dispatched = 0;
  // Bound the work: a handler that posts new events must not turn this into an
  // unbounded loop inside one scheduler pass.
  const std::size_t budget = queueCount_;
  while (dispatched < budget && queueCount_ > 0) {
    const Event event = queue_[queueHead_];
    queueHead_ = (queueHead_ + 1) % kQueueSize;
    --queueCount_;
    publish(event);
    ++dispatched;
  }
  return dispatched;
}

}  // namespace lc
