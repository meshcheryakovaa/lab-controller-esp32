// =============================================================================
//  core/EventBus.h — control-plane publish/subscribe (§18).
//
//  SCOPE — read this before using it:
//    The EventBus carries *control-plane* events: a device changed state, an
//    alarm fired, configuration changed, an experiment step advanced.  These
//    are rare (units per second at most).
//
//    It deliberately does NOT carry every measurement sample.  At 80 Hz across
//    twenty channels a per-sample event would mean 1600 dispatches/s through a
//    generic bus — pure overhead on a 240 MHz core.  Measurement data flows
//    through ChannelManager's own typed subscriber list, which batches.
//    kChannelUpdated exists for the few consumers that genuinely need an event
//    (rule engine on slow channels, experiment WAIT_UNTIL), and is opt-in per
//    channel and rate-limited.  See ADR-0002.
//
//  ALLOCATION: none.  Events are PODs, subscribers live in a fixed array.
//  THREADING: publish() dispatches synchronously on the calling task.  Code
//    running on another FreeRTOS task (or an ISR) must use post(), which only
//    enqueues; drainPending() then dispatches on the main task.
// =============================================================================
#pragma once

#include <cstdint>

#include "core/Error.h"
#include "core/Types.h"

namespace lc {

enum class EventType : std::uint8_t {
  kChannelUpdated = 0,
  kDeviceStateChanged,
  kDeviceError,
  kDeviceConnected,
  kDeviceDisconnected,
  kExperimentStarted,
  kExperimentStepChanged,
  kExperimentStopped,
  kAlarmTriggered,
  kAlarmCleared,
  kSafetyTripped,
  kConfigChanged,
  kProfileActivated,
  kLoggingStarted,
  kLoggingStopped,
  // M15: a log segment has been closed and is waiting to be collected.  Only a
  // nudge — the client re-reads the queue over REST regardless, so a dropped
  // frame delays a transfer and can never lose one.
  kLogSegmentReady,
  kSystemMessage,

  kCount  // keep last; must stay <= 32 (mask is a uint32)
};

static_assert(static_cast<int>(EventType::kCount) <= 32,
              "EventBus subscription masks are 32 bits wide");

const char* toString(EventType type);

// Bitmask helper: eventMask(EventType::kDeviceError) | eventMask(...)
constexpr std::uint32_t eventMask(EventType type) {
  return 1u << static_cast<std::uint32_t>(type);
}
inline constexpr std::uint32_t kAllEvents = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
//  Event — fixed 32-byte POD.  No pointers to dynamically owned memory:
//  `detail` must point at a string literal or an interned symbol that outlives
//  dispatch (typically errorSymbol()).
// ---------------------------------------------------------------------------
struct Event {
  EventType type = EventType::kSystemMessage;
  std::uint8_t severity = 0;   // 0=debug .. 4=critical, mirrors LogLevel
  std::uint16_t source = 0;    // DeviceHandle or ChannelHandle, context-dependent
  Micros timestamp = 0;
  float number = 0.0f;         // measured value / setpoint / percentage
  std::int32_t integer = 0;    // enum payload (e.g. new DeviceState)
  ErrorCode code = ErrorCode::kOk;
  const char* detail = nullptr;  // static lifetime only
};

using EventHandler = void (*)(const Event& event, void* context);
using SubscriptionId = std::uint16_t;
inline constexpr SubscriptionId kInvalidSubscription = 0;

class EventBus {
 public:
  // `mask` selects the event types the handler wants; use kAllEvents to get
  // everything.  Returns kInvalidSubscription when the table is full.
  SubscriptionId subscribe(std::uint32_t mask, EventHandler handler, void* context);
  bool unsubscribe(SubscriptionId id);

  // Synchronous dispatch on the calling task.  Handlers must be short and must
  // not publish recursively more than one level deep (guarded below).
  void publish(const Event& event);

  // Enqueue for later dispatch from the main task.  Safe to call from another
  // FreeRTOS task.  Returns false if the queue is full (counted in dropped()).
  bool post(const Event& event);

  // Dispatches everything queued by post().  Call once per main-loop pass.
  // Returns the number of events dispatched.
  std::size_t drainPending();

  std::size_t subscriberCount() const { return subscriberCount_; }
  std::uint32_t publishedCount() const { return published_; }
  std::uint32_t droppedCount() const { return dropped_; }
  std::uint32_t reentrancyDrops() const { return reentrancyDrops_; }

 private:
  struct Subscriber {
    SubscriptionId id = kInvalidSubscription;
    std::uint32_t mask = 0;
    EventHandler handler = nullptr;
    void* context = nullptr;
  };

  static constexpr std::size_t kQueueSize = 16;

  Subscriber subscribers_[limits::kMaxEventSubscribers];
  std::size_t subscriberCount_ = 0;
  SubscriptionId nextId_ = 1;

  Event queue_[kQueueSize];
  std::size_t queueHead_ = 0;
  std::size_t queueCount_ = 0;

  std::uint32_t published_ = 0;
  std::uint32_t dropped_ = 0;
  std::uint32_t reentrancyDrops_ = 0;
  std::uint8_t dispatchDepth_ = 0;
};

}  // namespace lc
