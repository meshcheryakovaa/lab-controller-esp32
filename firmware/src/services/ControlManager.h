// =============================================================================
//  services/ControlManager.h — PID loops and rules (§28, §29, ADR-0017).
//
//  WHAT A CONTROLLER DOES WITHOUT DATA
//  This is the question the milestone had to answer before a single line of PID
//  was written, because every plausible answer except one is dangerous.
//
//    "Hold the last output."   A heater held at 60 % because the thermocouple
//                              fell off is the textbook runaway.
//    "Assume the setpoint."    Invents a measurement.  Worse.
//    "Keep integrating."       Winds up on a number that is not a temperature.
//
//  So: a loop whose input is STALE or FAULTED stops being a loop.  It releases
//  its output to the safe value, freezes the integral, and says why.  It does
//  that after a short grace period, because one missed sample on a 1 Hz sensor
//  is not evidence of anything — but the grace period is measured in seconds
//  and is bounded by the manifest.
//
//  THREE LAYERS, NONE TRUSTING THE ONE ABOVE IT
//    1. the loop decides and commands;
//    2. SafetyManager watches the CHANNELS, not the loop, and can trip;
//    3. OutputManager's command deadline releases anything nobody renews —
//       including the output of a loop that has hung.
//  Layer 3 is what makes layers 1 and 2 non-critical to get perfectly right.
//
//  RULES ARE NOT SAFETY.
//  A rule is convenience automation: "turn the fan on above 40 °C".  It runs at
//  kControl, after the safety pass, it cannot trip anything, and §30 forbids
//  relying on one for a limit.  The separation is in the priority, in the API
//  and in this comment, so that nobody has to guess.
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

enum class LoopMode : std::uint8_t {
  kOff = 0,     // not commanding anything
  kManual,      // the operator's number, passed through
  kAutomatic,   // the PID's number
};

enum class LoopState : std::uint8_t {
  kIdle = 0,       // off
  kRunning,        // commanding
  kNoInput,        // input stale/faulted: output released, integral frozen
  kBlocked,        // the safety layer refused the command
};

const char* toString(LoopMode mode);
const char* toString(LoopState state);
bool parseLoopMode(const char* text, LoopMode& out);

struct ControlLoop {
  KeyString id;
  KeyString inputKey;
  KeyString outputKey;

  float setpoint = 0.0f;
  float kp = 1.0f;
  float ki = 0.0f;     // per second
  float kd = 0.0f;     // seconds
  float outputMin = 0.0f;
  float outputMax = 100.0f;
  float manualValue = 0.0f;
  bool invert = false;  // cooling: more output for a HIGHER measurement

  Micros periodUs = 1000000;
  // How long a non-GOOD input is tolerated before the output is released.
  Micros inputGraceUs = 5000000;

  LoopMode mode = LoopMode::kOff;

  // --- live state ---------------------------------------------------------
  LoopState state = LoopState::kIdle;
  float integral = 0.0f;
  float lastMeasurement = 0.0f;
  float lastOutput = 0.0f;
  float lastError = 0.0f;
  bool primed = false;
  Micros lastUpdateUs = 0;
  Micros badInputSinceUs = 0;
  Error lastFault;
  bool active = false;
};

// A rule is a threshold with hysteresis and a minimum dwell, acting on one
// output.  Deliberately small: anything more expressive belongs in the formula
// engine and the experiment scripts, not in something that switches a relay.
struct ControlRule {
  KeyString id;
  KeyString inputKey;
  KeyString outputKey;
  LabelString note;

  float onAbove = 0.0f;
  float offBelow = 0.0f;    // hysteresis band: offBelow < onAbove
  float onValue = 1.0f;
  float offValue = 0.0f;
  Micros minHoldUs = 0;     // stay in a state at least this long

  bool enabled = true;

  bool engaged = false;
  // The rule has commanded this output and is responsible for keeping the
  // command alive.  Without this a rule would engage once, the hold deadline
  // would expire, the fan would stop, and the rule — still "engaged" — would
  // never notice.
  bool owns = false;
  Micros changedAtUs = 0;
  std::uint32_t activations = 0;
  bool active = false;
};

class ControlManager {
 public:
  ControlManager(const IClock& clock, ChannelManager& channels,
                 OutputManager& outputs, Scheduler& scheduler, EventBus& events)
      : clock_(clock), channels_(channels), outputs_(outputs),
        scheduler_(scheduler), events_(events) {}

  static constexpr std::size_t loopCapacity() { return limits::kMaxControlLoops; }
  static constexpr std::size_t ruleCapacity() { return limits::kMaxRules; }

  Status begin(Micros periodUs = 100000);

  Status addLoop(const ControlLoop& loop);
  Status removeLoop(const char* id);
  Status setMode(const char* id, LoopMode mode);
  Status setSetpoint(const char* id, float setpoint);
  Status setManual(const char* id, float value);
  const ControlLoop* findLoop(const char* id) const;
  std::size_t loopCount() const { return loopCount_; }
  const ControlLoop& loopAt(std::size_t index) const { return loops_[index]; }

  Status addRule(const ControlRule& rule);
  Status removeRule(const char* id);
  const ControlRule* findRule(const char* id) const;
  std::size_t ruleCount() const { return ruleCount_; }
  const ControlRule& ruleAt(std::size_t index) const { return rules_[index]; }

  void clearAll();
  void tick(Micros now);

 private:
  static void tickTrampoline(void* context);
  ControlLoop* mutableFindLoop(const char* id);
  ControlRule* mutableFindRule(const char* id);
  void updateLoop(ControlLoop& loop, Micros now);
  void updateRule(ControlRule& rule, Micros now);
  void releaseLoop(ControlLoop& loop, LoopState state, const Error& reason);
  void releaseRule(ControlRule& rule);

  const IClock& clock_;
  ChannelManager& channels_;
  OutputManager& outputs_;
  Scheduler& scheduler_;
  EventBus& events_;

  ControlLoop loops_[limits::kMaxControlLoops];
  std::size_t loopCount_ = 0;
  ControlRule rules_[limits::kMaxRules];
  std::size_t ruleCount_ = 0;
  TaskId task_ = kInvalidTask;
};

}  // namespace lc
