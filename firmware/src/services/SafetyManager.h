// =============================================================================
//  services/SafetyManager.h — limits that do not trust the controller (§30).
//
//  THE POINT OF THIS FILE IS INDEPENDENCE.
//  A PID that has gone mad, a rule somebody wrote wrong, a driver stuck at full
//  power: none of them can be relied upon to notice.  So the limits live here,
//  are evaluated from the CHANNELS rather than from anything a controller says,
//  and run at TaskPriority::kSafety — before every controller, every acquisition
//  task and every byte of telemetry.
//
//  THE RULE THAT MATTERS MOST
//  A limit whose input is STALE or FAULTED TRIPS.  It does not wait, and it does
//  not give the benefit of the doubt.  An interlock that can be switched off by
//  unplugging a thermocouple is not an interlock — it is a thermocouple.  This
//  is the one behaviour in the whole project that must never be made
//  configurable into silence, and it is why `requireFreshInput` defaults to
//  true and is documented as a decision rather than a preference.
//
//  LIMITS LATCH.
//  A temperature that crossed 300 °C and came back does not un-cross it.  Once a
//  limit has acted it stays acted until a human resets it, because the whole
//  reason it exists is that nobody was watching at the time.
//
//  Debounce, on the other hand, is real: one noisy sample must not stop an
//  eight-hour experiment, so a violation has to persist for `forUs` first.
//  Debounce delays the trip; it never cancels it.
// =============================================================================
#pragma once

#include "core/Clock.h"
#include "core/Error.h"
#include "core/EventBus.h"
#include "core/Scheduler.h"
#include "core/Types.h"
#include "services/ChannelManager.h"
#include "services/OutputManager.h"

namespace lc {

enum class SafetyCondition : std::uint8_t {
  kAbove = 0,   // value > high
  kBelow,       // value < low
  kOutside,     // value < low || value > high
};

enum class SafetyAction : std::uint8_t {
  kTripAll = 0,     // every output to its safe value, nothing commandable
  kReleaseOutput,   // one named output to its safe value
  kAlarmOnly,       // raise an alarm and change nothing
};

const char* toString(SafetyCondition condition);
const char* toString(SafetyAction action);
bool parseSafetyCondition(const char* text, SafetyCondition& out);
bool parseSafetyAction(const char* text, SafetyAction& out);

struct SafetyLimit {
  KeyString id;
  KeyString channelKey;
  KeyString targetKey;        // output to release, for kReleaseOutput
  LabelString message;

  SafetyCondition condition = SafetyCondition::kAbove;
  SafetyAction action = SafetyAction::kTripAll;
  float low = 0.0f;
  float high = 0.0f;

  // A violation must persist this long before acting.  Delays the trip; never
  // cancels it.
  Micros forUs = 0;

  // Whether a stale or faulted input counts as a violation.  Defaults to true
  // and should stay true; see the header.
  bool requireFreshInput = true;
  bool enabled = true;

  // --- live state ---------------------------------------------------------
  bool violating = false;
  bool latched = false;
  Micros violatingSinceUs = 0;
  std::uint32_t trips = 0;
  Error lastReason;
  bool active = false;
};

class SafetyManager {
 public:
  SafetyManager(const IClock& clock, ChannelManager& channels,
                OutputManager& outputs, Scheduler& scheduler, EventBus& events)
      : clock_(clock), channels_(channels), outputs_(outputs),
        scheduler_(scheduler), events_(events) {}

  static constexpr std::size_t capacity() { return limits::kMaxSafetyLimits; }

  Status begin(Micros periodUs = 100000);

  Status add(const SafetyLimit& limit);
  Status remove(const char* id);
  void clearAll();

  // Clears the latch on one limit, or on all of them.  Deliberately explicit:
  // "the reading came back" is not a reason to re-arm something that fired.
  Status reset(const char* id);
  void resetAll();

  void tick(Micros now);

  std::size_t count() const { return count_; }
  const SafetyLimit& at(std::size_t index) const { return limits_[index]; }
  const SafetyLimit* find(const char* id) const;
  std::size_t latchedCount() const;

 private:
  static void tickTrampoline(void* context);
  SafetyLimit* mutableFind(const char* id);
  void act(SafetyLimit& limit, const Error& reason, Micros now);
  void raise(const SafetyLimit& limit, const char* detail, std::uint8_t severity);

  const IClock& clock_;
  ChannelManager& channels_;
  OutputManager& outputs_;
  Scheduler& scheduler_;
  EventBus& events_;

  SafetyLimit limits_[limits::kMaxSafetyLimits];
  std::size_t count_ = 0;
  TaskId task_ = kInvalidTask;
};

}  // namespace lc
