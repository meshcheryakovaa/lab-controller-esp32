#include "services/ControlManager.h"

#include <cmath>
#include <cstring>

namespace lc {

const char* toString(LoopMode mode) {
  switch (mode) {
    case LoopMode::kOff:       return "off";
    case LoopMode::kManual:    return "manual";
    case LoopMode::kAutomatic: return "automatic";
  }
  return "off";
}

const char* toString(LoopState state) {
  switch (state) {
    case LoopState::kIdle:    return "IDLE";
    case LoopState::kRunning: return "RUNNING";
    case LoopState::kNoInput: return "NO_INPUT";
    case LoopState::kBlocked: return "BLOCKED";
  }
  return "IDLE";
}

bool parseLoopMode(const char* text, LoopMode& out) {
  if (text == nullptr) return false;
  if (std::strcmp(text, "off") == 0)       { out = LoopMode::kOff;       return true; }
  if (std::strcmp(text, "manual") == 0)    { out = LoopMode::kManual;    return true; }
  if (std::strcmp(text, "automatic") == 0) { out = LoopMode::kAutomatic; return true; }
  return false;
}

// ---------------------------------------------------------------------------
//  Lookup and lifecycle
// ---------------------------------------------------------------------------
ControlLoop* ControlManager::mutableFindLoop(const char* id) {
  if (id == nullptr) return nullptr;
  for (std::size_t i = 0; i < loopCount_; ++i) {
    if (loops_[i].active && loops_[i].id.equals(id)) return &loops_[i];
  }
  return nullptr;
}

const ControlLoop* ControlManager::findLoop(const char* id) const {
  return const_cast<ControlManager*>(this)->mutableFindLoop(id);
}

ControlRule* ControlManager::mutableFindRule(const char* id) {
  if (id == nullptr) return nullptr;
  for (std::size_t i = 0; i < ruleCount_; ++i) {
    if (rules_[i].active && rules_[i].id.equals(id)) return &rules_[i];
  }
  return nullptr;
}

const ControlRule* ControlManager::findRule(const char* id) const {
  return const_cast<ControlManager*>(this)->mutableFindRule(id);
}

Status ControlManager::begin(Micros periodUs) {
  if (task_ != kInvalidTask) return ok();
  // kControl: after the safety pass, before acquisition.  A loop must never be
  // able to run in a pass where the limit that overrules it did not.
  const Result<TaskId> added = scheduler_.addPeriodic(
      "control.loops", periodUs, TaskPriority::kControl, tickTrampoline, this);
  if (!added.ok()) return added.error();
  task_ = added.value();
  return ok();
}

void ControlManager::tickTrampoline(void* context) {
  ControlManager* self = static_cast<ControlManager*>(context);
  self->tick(self->clock_.nowMicros());
}

Status ControlManager::addLoop(const ControlLoop& loop) {
  if (loop.id.empty()) return fail(ErrorCode::kInvalidArgument, "loop id is required");
  if (loop.inputKey.empty() || loop.outputKey.empty()) {
    return fail(ErrorCode::kRuleInvalid, "a loop needs an input and an output");
  }
  if (!(loop.outputMax > loop.outputMin)) {
    return fail(ErrorCode::kRuleInvalid, "output max must exceed output min");
  }
  if (loop.periodUs == 0) {
    return fail(ErrorCode::kRuleInvalid, "period must be greater than zero");
  }
  if (!std::isfinite(loop.kp) || !std::isfinite(loop.ki) || !std::isfinite(loop.kd)) {
    return fail(ErrorCode::kRuleInvalid, "gains must be finite");
  }

  ControlLoop* slot = mutableFindLoop(loop.id.c_str());
  // Re-defining a loop that is currently commanding lets go of the old output
  // first: the new definition may point somewhere else entirely, and the
  // actuator it used to drive would then be held by nobody.
  if (slot != nullptr) releaseLoop(*slot, LoopState::kIdle, ok());
  if (slot == nullptr) {
    if (loopCount_ >= loopCapacity()) {
      return fail(ErrorCode::kOutOfCapacity, "too many control loops");
    }
    slot = &loops_[loopCount_];
    ++loopCount_;
  }

  *slot = loop;
  slot->active = true;
  // A loop always starts OFF, whatever the file says about its mode.  Coming up
  // already regulating means the first thing a freshly booted controller does
  // is drive an actuator using an integral it inherited from nothing.
  slot->mode = LoopMode::kOff;
  slot->state = LoopState::kIdle;
  slot->integral = 0.0f;
  slot->primed = false;
  return ok();
}

Status ControlManager::removeLoop(const char* id) {
  for (std::size_t i = 0; i < loopCount_; ++i) {
    if (!loops_[i].active || !loops_[i].id.equals(id)) continue;
    releaseLoop(loops_[i], LoopState::kIdle, ok());
    loops_[i] = loops_[loopCount_ - 1];
    --loopCount_;
    return ok();
  }
  return fail(ErrorCode::kNotFound, "control loop");
}

Status ControlManager::setMode(const char* id, LoopMode mode) {
  ControlLoop* loop = mutableFindLoop(id);
  if (loop == nullptr) return fail(ErrorCode::kNotFound, "control loop");

  if (mode == LoopMode::kOff) {
    releaseLoop(*loop, LoopState::kIdle, ok());
    loop->mode = mode;
    return ok();
  }

  if (mode == LoopMode::kAutomatic && loop->mode != LoopMode::kAutomatic) {
    // Bumpless transfer: start the integral from whatever the output is doing
    // now, so switching to automatic does not step the actuator.  Without this
    // a manual 40 % becomes an automatic 0 % for one period, and on a heater
    // that is a visible dip in the process.
    // The integral carries the whole steady-state output in this form, so
    // seeding it with what the actuator is doing right now is exactly the
    // bumpless transfer: the first automatic command equals the last manual one
    // plus a proportional term that is small when the error is small.
    loop->integral = loop->lastOutput;
    loop->primed = false;
  }
  loop->mode = mode;
  return ok();
}

Status ControlManager::setSetpoint(const char* id, float setpoint) {
  ControlLoop* loop = mutableFindLoop(id);
  if (loop == nullptr) return fail(ErrorCode::kNotFound, "control loop");
  if (!std::isfinite(setpoint)) {
    return fail(ErrorCode::kInvalidArgument, "setpoint is not a number");
  }
  loop->setpoint = setpoint;
  return ok();
}

Status ControlManager::setManual(const char* id, float value) {
  ControlLoop* loop = mutableFindLoop(id);
  if (loop == nullptr) return fail(ErrorCode::kNotFound, "control loop");
  if (!std::isfinite(value)) {
    return fail(ErrorCode::kInvalidArgument, "value is not a number");
  }
  // Clamped to the loop's own output band rather than refused: the band is the
  // rig's statement about what this actuator may be asked for, and it applies
  // to a human's number exactly as much as to the PID's.
  if (value > loop->outputMax) value = loop->outputMax;
  if (value < loop->outputMin) value = loop->outputMin;
  loop->manualValue = value;
  // Takes effect on the next period, and immediately if the loop is already
  // manual: waiting a period to see a slider move is indistinguishable from a
  // control that does not work.
  if (loop->mode == LoopMode::kManual) loop->lastUpdateUs = 0;
  return ok();
}

Status ControlManager::addRule(const ControlRule& rule) {
  if (rule.id.empty()) return fail(ErrorCode::kInvalidArgument, "rule id is required");
  if (rule.inputKey.empty() || rule.outputKey.empty()) {
    return fail(ErrorCode::kRuleInvalid, "a rule needs an input and an output");
  }
  if (!(rule.onAbove > rule.offBelow)) {
    // Without a band the rule chatters at exactly the threshold, which on a
    // relay is a contact life problem and on a heater is a noise problem.
    return fail(ErrorCode::kRuleInvalid,
                "on_above must be greater than off_below (hysteresis)");
  }

  ControlRule* slot = mutableFindRule(rule.id.c_str());
  if (slot != nullptr) releaseRule(*slot);
  if (slot == nullptr) {
    if (ruleCount_ >= ruleCapacity()) {
      return fail(ErrorCode::kOutOfCapacity, "too many rules");
    }
    slot = &rules_[ruleCount_];
    ++ruleCount_;
  }
  *slot = rule;
  slot->active = true;
  slot->engaged = false;
  slot->owns = false;
  return ok();
}

Status ControlManager::removeRule(const char* id) {
  for (std::size_t i = 0; i < ruleCount_; ++i) {
    if (!rules_[i].active || !rules_[i].id.equals(id)) continue;
    releaseRule(rules_[i]);
    rules_[i] = rules_[ruleCount_ - 1];
    --ruleCount_;
    return ok();
  }
  return fail(ErrorCode::kNotFound, "rule");
}

void ControlManager::clearAll() {
  // Everything this manager was holding up goes back to its safe value first.
  // Reloading the configuration must not leave a relay energised by a rule that
  // no longer exists — nothing would ever renew it, but "nothing renews it" is
  // a deadline away from safe, and the deadline may be long or waived.
  for (std::size_t i = 0; i < loopCount_; ++i) {
    if (loops_[i].active) releaseLoop(loops_[i], LoopState::kIdle, ok());
  }
  for (std::size_t i = 0; i < ruleCount_; ++i) {
    if (rules_[i].active) releaseRule(rules_[i]);
  }
  loopCount_ = 0;
  ruleCount_ = 0;
}

// ---------------------------------------------------------------------------
//  Running
// ---------------------------------------------------------------------------
void ControlManager::releaseLoop(ControlLoop& loop, LoopState state,
                                 const Error& reason) {
  const ChannelHandle output = channels_.findByKey(loop.outputKey.c_str());
  if (output != kInvalidChannel) {
    // Explicitly, rather than by letting the command deadline expire: the
    // deadline may be ten minutes, and a loop that has lost its sensor should
    // not keep heating for ten minutes on the strength of a timer.
    outputs_.release(output, OutputHoldState::kSafe);
  }
  loop.state = state;
  loop.lastFault = reason;
  loop.primed = false;
  // The integral is frozen, not cleared: when the sensor comes back, the loop
  // has to be re-armed by a human anyway, and keeping the number makes the
  // diagnostics honest about how far it had wound.
}

void ControlManager::releaseRule(ControlRule& rule) {
  if (!rule.owns) return;
  const ChannelHandle output = channels_.findByKey(rule.outputKey.c_str());
  if (output != kInvalidChannel) {
    outputs_.release(output, OutputHoldState::kSafe);
  }
  rule.owns = false;
  rule.engaged = false;
}

void ControlManager::updateLoop(ControlLoop& loop, Micros now) {
  if (loop.mode == LoopMode::kOff) {
    loop.state = LoopState::kIdle;
    return;
  }

  const ChannelHandle output = channels_.findByKey(loop.outputKey.c_str());
  if (output == kInvalidChannel) {
    loop.state = LoopState::kNoInput;
    loop.lastFault = fail(ErrorCode::kChannelNotFound, loop.outputKey.c_str());
    loop.lastUpdateUs = now;
    return;
  }

  // --- is the input still there? ------------------------------------------
  // Checked on EVERY tick, deliberately ahead of the period gate.  Putting this
  // after the gate — which is where it naturally lands when the function is
  // written in the order a control engineer thinks — means a loop with a 60 s
  // period keeps a heater on for up to 60 s after its thermocouple dies, no
  // matter how short its grace period says it should be.  The grace period has
  // to be measured against the sensor's silence, not against the controller's
  // convenience.
  const ChannelHandle input = channels_.findByKey(loop.inputKey.c_str());
  const ChannelValue* measurement =
      (input != kInvalidChannel) ? channels_.value(input) : nullptr;
  const bool trustworthy =
      measurement != nullptr &&
      (measurement->quality == ChannelQuality::kGood ||
       measurement->quality == ChannelQuality::kOutOfRange);

  if (loop.mode == LoopMode::kAutomatic) {
    if (!trustworthy) {
      // A short gap is not evidence of anything: one missed sample on a 1 Hz
      // sensor happens.  A gap that persists means the loop is controlling on a
      // number it does not have.
      if (loop.badInputSinceUs == 0) loop.badInputSinceUs = now;
      if (now - loop.badInputSinceUs >= loop.inputGraceUs) {
        releaseLoop(loop, LoopState::kNoInput,
                    fail(ErrorCode::kSafetyInterlock,
                         "input is stale or faulted; output released"));
      }
      return;
    }
    loop.badInputSinceUs = 0;
  }

  // Keep the command alive between recomputations.  A loop with a 10 s period
  // driving an output with a 5 s hold would otherwise command, expire, sit at
  // the safe value for half its period and command again — a controller
  // oscillating against its own safety timer.  Renewing every tick is also the
  // honest statement of what the deadline means: somebody still wants this
  // value, and that somebody is this loop, and it is still running.
  if (loop.state == LoopState::kRunning) (void)outputs_.renew(output);

  if (loop.lastUpdateUs != 0 && now < loop.lastUpdateUs + loop.periodUs) return;

  // --- manual ------------------------------------------------------------
  if (loop.mode == LoopMode::kManual) {
    const Status commanded = outputs_.command(output, loop.manualValue);
    loop.lastOutput = loop.manualValue;
    loop.state = commanded.ok() ? LoopState::kRunning : LoopState::kBlocked;
    loop.lastFault = commanded;
    loop.lastUpdateUs = now;
    return;
  }

  // --- automatic ----------------------------------------------------------
  const double dt = (loop.lastUpdateUs == 0)
                        ? static_cast<double>(loop.periodUs) * 1e-6
                        : static_cast<double>(now - loop.lastUpdateUs) * 1e-6;
  loop.lastUpdateUs = now;
  if (dt <= 0.0) return;

  const float measured = measurement->processed;
  const float error = loop.invert ? (measured - loop.setpoint)
                                  : (loop.setpoint - measured);

  // Derivative ON THE MEASUREMENT, not on the error: differentiating the error
  // makes a setpoint change produce an impulse straight into the actuator, and
  // on a heater that is a visible spike for no reason.
  float derivative = 0.0f;
  if (loop.primed) {
    const float change = measured - loop.lastMeasurement;
    derivative = -static_cast<float>((loop.invert ? -change : change) / dt);
  }
  loop.lastMeasurement = measured;
  loop.primed = true;

  const float proportional = loop.kp * error;
  const float derivativeTerm = loop.kd * derivative;

  // Conditional integration: the integral only moves if doing so does not push
  // the output further past a limit it is already against.  This is the cheap
  // anti-windup, and the one that matters — a loop that spends an hour
  // saturated must not need an hour to come back.
  float candidate = loop.integral + loop.ki * error * static_cast<float>(dt);
  float unclamped = proportional + candidate + derivativeTerm;
  if (unclamped > loop.outputMax && error > 0.0f) {
    candidate = loop.integral;
  } else if (unclamped < loop.outputMin && error < 0.0f) {
    candidate = loop.integral;
  }
  loop.integral = candidate;
  // And a hard bound on the integral itself, so a mis-typed ki cannot make the
  // number unrecoverable.
  if (loop.integral > loop.outputMax) loop.integral = loop.outputMax;
  if (loop.integral < loop.outputMin) loop.integral = loop.outputMin;

  float command = proportional + loop.integral + derivativeTerm;
  if (command > loop.outputMax) command = loop.outputMax;
  if (command < loop.outputMin) command = loop.outputMin;
  if (!std::isfinite(command)) {
    releaseLoop(loop, LoopState::kNoInput,
                fail(ErrorCode::kInternal, "loop produced a non-finite output"));
    return;
  }

  const Status commanded = outputs_.command(output, command);
  loop.lastOutput = command;
  loop.lastError = error;
  loop.state = commanded.ok() ? LoopState::kRunning : LoopState::kBlocked;
  loop.lastFault = commanded;
}

void ControlManager::updateRule(ControlRule& rule, Micros now) {
  if (!rule.enabled) {
    // A rule that has just been switched off lets go of its output rather than
    // leaving it wherever it happened to be.
    releaseRule(rule);
    return;
  }

  const ChannelHandle held = channels_.findByKey(rule.outputKey.c_str());
  if (rule.owns && held != kInvalidChannel) {
    // Same reasoning as a loop: whoever commanded an output is the one who has
    // to keep saying so.  If the hold lapsed anyway — a trip that was cleared,
    // a device that came back — re-issue the value instead of pretending the
    // relay is still where the rule left it.
    if (!outputs_.renew(held).ok()) {
      const Status recovered =
          outputs_.command(held, rule.engaged ? rule.onValue : rule.offValue);
      if (!recovered.ok()) rule.owns = false;
    }
  }

  const ChannelHandle input = channels_.findByKey(rule.inputKey.c_str());
  const ChannelValue* value =
      (input != kInvalidChannel) ? channels_.value(input) : nullptr;
  if (value == nullptr) return;
  if (value->quality != ChannelQuality::kGood &&
      value->quality != ChannelQuality::kOutOfRange) {
    // A rule with no input simply does nothing.  It does NOT fall back to a
    // safe action, and that is the difference from a safety limit: a rule is
    // convenience, and convenience that guesses is worse than convenience that
    // waits (§30).
    return;
  }

  const bool shouldEngage = rule.engaged ? (value->processed > rule.offBelow)
                                         : (value->processed > rule.onAbove);
  if (shouldEngage == rule.engaged) return;

  // A minimum dwell keeps a relay from being cycled by noise sitting on the
  // threshold, on top of the hysteresis band.
  if (rule.minHoldUs > 0 && rule.changedAtUs != 0 &&
      now >= rule.changedAtUs && (now - rule.changedAtUs) < rule.minHoldUs) {
    return;
  }

  if (held == kInvalidChannel) return;

  const Status commanded =
      outputs_.command(held, shouldEngage ? rule.onValue : rule.offValue);
  if (!commanded.ok()) return;  // tripped, or the device is not running

  rule.engaged = shouldEngage;
  rule.owns = true;
  rule.changedAtUs = now;
  if (shouldEngage) ++rule.activations;
}

void ControlManager::tick(Micros now) {
  for (std::size_t i = 0; i < loopCount_; ++i) {
    if (loops_[i].active) updateLoop(loops_[i], now);
  }
  for (std::size_t i = 0; i < ruleCount_; ++i) {
    if (rules_[i].active) updateRule(rules_[i], now);
  }
}

}  // namespace lc
