#include "services/ExperimentEngine.h"

#include <cmath>
#include <cstring>

namespace lc {
namespace {

// The two channels the run state is published on.  Named like channels because
// they ARE channels: a dashboard tile, a safety limit and a rule can all point
// at them without knowing what an experiment is.
constexpr const char* kStateChannelKey = "experiment_state";
constexpr const char* kStepChannelKey = "experiment_step";

const char* firstDot(const char* text) {
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == '.') return p;
  }
  return nullptr;
}

}  // namespace

const char* toString(StepOp op) {
  switch (op) {
    case StepOp::kSet:          return "SET";
    case StepOp::kWait:         return "WAIT";
    case StepOp::kWaitUntil:    return "WAIT_UNTIL";
    case StepOp::kRunFor:       return "RUN_FOR";
    case StepOp::kMarkEvent:    return "MARK_EVENT";
    case StepOp::kEnable:       return "ENABLE";
    case StepOp::kDisable:      return "DISABLE";
    case StepOp::kStop:         return "STOP";
    case StepOp::kStartLogging: return "START_LOGGING";
    case StepOp::kStopLogging:  return "STOP_LOGGING";
  }
  return "WAIT";
}

const char* toString(ExperimentState state) {
  switch (state) {
    case ExperimentState::kIdle:     return "IDLE";
    case ExperimentState::kRunning:  return "RUNNING";
    case ExperimentState::kPaused:   return "PAUSED";
    case ExperimentState::kFinished: return "FINISHED";
    case ExperimentState::kAborted:  return "ABORTED";
  }
  return "IDLE";
}

const char* toString(StopReason reason) {
  switch (reason) {
    case StopReason::kNone:          return "none";
    case StopReason::kScenario:      return "scenario";
    case StopReason::kOperator:      return "operator";
    case StopReason::kTimeout:       return "timeout";
    case StopReason::kSafety:        return "safety";
    case StopReason::kTargetMissing: return "target_missing";
    case StopReason::kRestarted:     return "restarted";
    case StopReason::kReconfigured:  return "reconfigured";
    case StopReason::kInternal:      return "internal";
  }
  return "none";
}

const char* toString(Comparison comparison) {
  switch (comparison) {
    case Comparison::kAtLeast: return ">=";
    case Comparison::kAtMost:  return "<=";
    case Comparison::kAbove:   return ">";
    case Comparison::kBelow:   return "<";
  }
  return ">=";
}

bool parseStepOp(const char* text, StepOp& out) {
  if (text == nullptr) return false;
  struct Entry { const char* name; StepOp op; };
  static const Entry kTable[] = {
      {"SET", StepOp::kSet},
      {"WAIT", StepOp::kWait},
      {"WAIT_UNTIL", StepOp::kWaitUntil},
      {"RUN_FOR", StepOp::kRunFor},
      {"MARK_EVENT", StepOp::kMarkEvent},
      {"ENABLE", StepOp::kEnable},
      {"DISABLE", StepOp::kDisable},
      {"STOP", StepOp::kStop},
      {"START_LOGGING", StepOp::kStartLogging},
      {"STOP_LOGGING", StepOp::kStopLogging},
  };
  for (const Entry& entry : kTable) {
    if (std::strcmp(text, entry.name) == 0) {
      out = entry.op;
      return true;
    }
  }
  return false;
}

bool parseComparison(const char* text, Comparison& out) {
  if (text == nullptr) return false;
  if (std::strcmp(text, ">=") == 0) { out = Comparison::kAtLeast; return true; }
  if (std::strcmp(text, "<=") == 0) { out = Comparison::kAtMost;  return true; }
  if (std::strcmp(text, ">") == 0)  { out = Comparison::kAbove;   return true; }
  if (std::strcmp(text, "<") == 0)  { out = Comparison::kBelow;   return true; }
  return false;
}

bool parseOnTimeout(const char* text, OnTimeout& out) {
  if (text == nullptr) return false;
  if (std::strcmp(text, "abort") == 0)    { out = OnTimeout::kAbort;    return true; }
  if (std::strcmp(text, "continue") == 0) { out = OnTimeout::kContinue; return true; }
  return false;
}

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
Status ExperimentEngine::begin(Micros periodUs) {
  if (task_ != kInvalidTask) return ok();

  // kControl, and registered BEFORE ControlManager by the boot sequence: the
  // experiment decides the setpoint, the loop acts on it in the same pass.  It
  // is deliberately NOT kSafety — an experiment is not a safety mechanism, and
  // giving it that priority would say otherwise.
  const Result<TaskId> added = scheduler_.addPeriodic(
      "experiment.run", periodUs, TaskPriority::kControl, tickTrampoline, this);
  if (!added.ok()) return added.error();
  task_ = added.value();

  if (stateChannel_ == kInvalidChannel) {
    ChannelDescriptor descriptor;
    descriptor.key.assign(kStateChannelKey);
    descriptor.name.assign("Experiment state");
    descriptor.quantity.assign("state");
    descriptor.source = kInvalidDevice;  // system-owned, not a sensor
    descriptor.precision = 0;
    descriptor.minimum = 0.0f;
    descriptor.maximum = 4.0f;
    descriptor.logged = false;
    // Not on the default dashboard: "Experiment state = 4" is true and useless,
    // and a freshly seeded dashboard that leads with it teaches nothing.  The
    // channel still exists for rules, limits and anyone who charts it on
    // purpose; the RunWidget is what an operator actually reads.
    descriptor.visible = false;
    descriptor.expectedIntervalUs = periodUs * 3;
    const Result<ChannelHandle> created = channels_.create(descriptor);
    if (created.ok()) stateChannel_ = created.value();

    descriptor.key.assign(kStepChannelKey);
    descriptor.name.assign("Experiment step");
    descriptor.quantity.assign("index");
    descriptor.maximum = static_cast<float>(limits::kMaxExperimentSteps);
    const Result<ChannelHandle> step = channels_.create(descriptor);
    if (step.ok()) stepChannel_ = step.value();
  }

  publishState(clock_.nowMicros());
  return ok();
}

void ExperimentEngine::tickTrampoline(void* context) {
  ExperimentEngine* self = static_cast<ExperimentEngine*>(context);
  self->tick(self->clock_.nowMicros());
}

// ---------------------------------------------------------------------------
//  Resolving what a step addresses
// ---------------------------------------------------------------------------
Status ExperimentEngine::resolveTarget(ExperimentStep& step) const {
  step.targetKind = TargetKind::kNone;
  if (step.target.empty()) {
    return fail(ErrorCode::kInvalidArgument, "step needs a target");
  }

  const char* text = step.target.c_str();
  const char* dot = firstDot(text);

  if (dot != nullptr) {
    KeyString id;
    char buffer[limits::kKeyLength];
    const std::size_t length = static_cast<std::size_t>(dot - text);
    if (length >= sizeof(buffer)) {
      return fail(ErrorCode::kInvalidArgument, "target is too long");
    }
    std::memcpy(buffer, text, length);
    buffer[length] = '\0';
    id.assign(buffer);

    if (control_.findLoop(id.c_str()) == nullptr) {
      return fail(ErrorCode::kNotFound, id.c_str());
    }
    step.targetId = id;
    const char* field = dot + 1;
    if (std::strcmp(field, "setpoint") == 0) {
      step.targetKind = TargetKind::kLoopSetpoint;
      return ok();
    }
    if (std::strcmp(field, "manual") == 0) {
      step.targetKind = TargetKind::kLoopManual;
      return ok();
    }
    if (std::strcmp(field, "mode") == 0) {
      LoopMode mode = LoopMode::kOff;
      if (!parseLoopMode(step.mode.c_str(), mode)) {
        return fail(ErrorCode::kInvalidArgument, "mode: off | manual | automatic");
      }
      step.targetKind = TargetKind::kLoopMode;
      return ok();
    }
    return fail(ErrorCode::kInvalidArgument, "target: <loop>.setpoint|manual|mode");
  }

  step.targetId = step.target;

  // A bare name is a loop, a device or an output channel, in that order.  The
  // order is fixed so that two things called the same thing cannot make a
  // scenario mean different things on two rigs.
  if (step.op == StepOp::kEnable || step.op == StepOp::kDisable) {
    if (control_.findLoop(step.targetId.c_str()) != nullptr) {
      step.targetKind = TargetKind::kLoop;
      return ok();
    }
    if (devices_.findByKey(step.targetId.c_str()) != nullptr) {
      step.targetKind = TargetKind::kDevice;
      return ok();
    }
    return fail(ErrorCode::kNotFound, step.targetId.c_str());
  }

  const ChannelHandle channel = channels_.findByKey(step.targetId.c_str());
  if (channel == kInvalidChannel) {
    return fail(ErrorCode::kChannelNotFound, step.targetId.c_str());
  }
  const ChannelDescriptor* descriptor = channels_.descriptor(channel);
  if (descriptor == nullptr || descriptor->direction != ChannelDirection::kOutput) {
    // Setting an input channel is not a typo the engine should interpret: it is
    // somebody expecting the scenario to change a measurement.
    return fail(ErrorCode::kInvalidArgument, "not an output channel");
  }
  step.targetKind = TargetKind::kOutputChannel;
  return ok();
}

Status ExperimentEngine::validate(const Experiment& experiment,
                                  std::size_t& offendingStep) const {
  offendingStep = 0;
  if (experiment.key.empty()) {
    return fail(ErrorCode::kInvalidArgument, "an experiment needs a key");
  }
  if (experiment.stepCount == 0) {
    return fail(ErrorCode::kInvalidArgument, "an experiment with no steps");
  }

  for (std::size_t i = 0; i < experiment.stepCount; ++i) {
    ExperimentStep step = experiment.steps[i];
    offendingStep = i + 1;

    switch (step.op) {
      case StepOp::kSet:
      case StepOp::kEnable:
      case StepOp::kDisable: {
        const Status resolved = resolveTarget(step);
        if (!resolved.ok()) return resolved;
        if (step.op == StepOp::kSet && step.targetKind != TargetKind::kLoopMode &&
            !std::isfinite(step.value)) {
          return fail(ErrorCode::kInvalidArgument, "value is not a number");
        }
        break;
      }
      case StepOp::kWaitUntil: {
        if (step.channel.empty()) {
          return fail(ErrorCode::kInvalidArgument, "wait_until needs a channel");
        }
        if (channels_.findByKey(step.channel.c_str()) == kInvalidChannel) {
          return fail(ErrorCode::kChannelNotFound, step.channel.c_str());
        }
        if (step.timeoutUs == 0) {
          // The rule this milestone refuses to soften.  A wait with no deadline
          // is a rig left running by a scenario that is technically still
          // "working".
          return fail(ErrorCode::kRuleInvalid,
                      "every wait_until needs a timeout");
        }
        if (!std::isfinite(step.threshold)) {
          return fail(ErrorCode::kInvalidArgument, "threshold is not a number");
        }
        break;
      }
      case StepOp::kWait:
      case StepOp::kRunFor:
        if (step.durationUs == 0) {
          return fail(ErrorCode::kInvalidArgument, "duration must be positive");
        }
        break;
      case StepOp::kMarkEvent:
        if (step.label.empty()) {
          return fail(ErrorCode::kInvalidArgument, "an event needs a label");
        }
        break;
      case StepOp::kStop:
        break;
      case StepOp::kStartLogging:
        if (logger_ == nullptr) {
          return fail(ErrorCode::kNotSupported, "this build cannot record data");
        }
        if (experiment.logging.channelCount == 0) {
          // A scenario that says "record" and names nothing would open a
          // dataset with no columns — a file that exists and answers nothing.
          return fail(ErrorCode::kInvalidArgument,
                      "this scenario logs, but no channels are selected");
        }
        for (std::size_t c = 0; c < experiment.logging.channelCount; ++c) {
          if (channels_.findByKey(experiment.logging.channels[c].c_str()) ==
              kInvalidChannel) {
            return fail(ErrorCode::kChannelNotFound,
                        experiment.logging.channels[c].c_str());
          }
        }
        break;
      case StepOp::kStopLogging:
        if (logger_ == nullptr) {
          return fail(ErrorCode::kNotSupported, "this build cannot record data");
        }
        break;
    }
  }
  offendingStep = 0;
  return ok();
}

// ---------------------------------------------------------------------------
//  Starting, pausing, ending
// ---------------------------------------------------------------------------
Status ExperimentEngine::start(const Experiment& experiment) {
  if (busy()) return fail(ErrorCode::kResourceBusy, "an experiment is running");
  if (outputs_.tripped()) {
    // Starting a run into a raised emergency stop would produce a scenario that
    // commands nothing and a dataset of the trip.
    return fail(ErrorCode::kSafetyInterlock,
                "the outputs are stopped; clear that first");
  }

  std::size_t offending = 0;
  const Status valid = validate(experiment, offending);
  if (!valid.ok()) return valid;

  experiment_ = experiment;
  for (std::size_t i = 0; i < experiment_.stepCount; ++i) {
    // Resolution is redone here, against the rig as it is right now, and the
    // resolved kind is what the run uses.  A scenario validated last week
    // against a loop that has since been deleted must not start.
    const Status resolved =
        (experiment_.steps[i].op == StepOp::kSet ||
         experiment_.steps[i].op == StepOp::kEnable ||
         experiment_.steps[i].op == StepOp::kDisable)
            ? resolveTarget(experiment_.steps[i])
            : ok();
    if (!resolved.ok()) return resolved;
  }

  const Micros now = clock_.nowMicros();
  run_ = RunRecord{};
  run_.experimentKey = experiment_.key;
  run_.name = experiment_.name;
  run_.metadata = experiment_.metadata;
  run_.startedUs = now;
  run_.startedEpochMs = clock_.epochMillis();
  run_.stepCount = static_cast<std::uint32_t>(experiment_.stepCount);
  run_.finalState = ExperimentState::kRunning;

  ownedOutputCount_ = 0;
  ownedLoopCount_ = 0;
  stepIndex_ = 0;
  stepStartedUs_ = now;
  stepEntered_ = false;
  state_ = ExperimentState::kRunning;

  if (sink_ != nullptr) sink_->onRunStarted(run_);

  Event event;
  event.type = EventType::kExperimentStarted;
  event.severity = 1;
  event.detail = "experiment started";
  event.timestamp = now;
  events_.publish(event);

  publishState(now);
  return ok();
}

Status ExperimentEngine::pause() {
  if (state_ != ExperimentState::kRunning) {
    return fail(ErrorCode::kInvalidState, "nothing is running");
  }
  state_ = ExperimentState::kPaused;
  pausedAtUs_ = clock_.nowMicros();
  // Paused holds the rig where it is: outputs stay commanded and keep being
  // renewed.  Pausing is "wait for me", not "stop" — and the operator who wants
  // "stop" has a button that says stop.
  publishState(pausedAtUs_);
  return ok();
}

Status ExperimentEngine::resume() {
  if (state_ != ExperimentState::kPaused) {
    return fail(ErrorCode::kInvalidState, "nothing is paused");
  }
  const Micros now = clock_.nowMicros();
  // The clock a step waits on does not run while paused: a RUN_FOR of thirty
  // minutes means thirty minutes of experiment, not thirty minutes of wall time
  // minus a coffee break.
  if (now > pausedAtUs_) stepStartedUs_ += now - pausedAtUs_;
  state_ = ExperimentState::kRunning;
  publishState(now);
  return ok();
}

Status ExperimentEngine::stop(StopReason reason) {
  if (!busy()) return fail(ErrorCode::kInvalidState, "nothing is running");
  finish(ExperimentState::kAborted, reason,
         fail(ErrorCode::kExperimentAborted, "stopped before the scenario ended"),
         clock_.nowMicros());
  return ok();
}

void ExperimentEngine::rememberOutput(ChannelHandle handle) {
  for (std::size_t i = 0; i < ownedOutputCount_; ++i) {
    if (ownedOutputs_[i] == handle) return;
  }
  if (ownedOutputCount_ >= limits::kMaxOutputs) return;
  ownedOutputs_[ownedOutputCount_++] = handle;
}

void ExperimentEngine::rememberLoop(const char* id) {
  for (std::size_t i = 0; i < ownedLoopCount_; ++i) {
    if (ownedLoops_[i].equals(id)) return;
  }
  if (ownedLoopCount_ >= limits::kMaxControlLoops) return;
  ownedLoops_[ownedLoopCount_++].assign(id);
}

void ExperimentEngine::releaseEverythingWeTouched() {
  // Loops first, then outputs: switching a loop off releases the output it was
  // driving, and doing it the other way round would let the loop re-command
  // between the two passes.
  for (std::size_t i = 0; i < ownedLoopCount_; ++i) {
    control_.setMode(ownedLoops_[i].c_str(), LoopMode::kOff);
  }
  for (std::size_t i = 0; i < ownedOutputCount_; ++i) {
    outputs_.release(ownedOutputs_[i], OutputHoldState::kSafe);
  }
  ownedLoopCount_ = 0;
  ownedOutputCount_ = 0;
}

void ExperimentEngine::finish(ExperimentState state, StopReason reason,
                              const Error& detail, Micros now) {
  releaseEverythingWeTouched();

  // The dataset this run opened closes with it, whatever ended the run.  A
  // session left recording after its experiment has stopped would keep writing
  // rows of a rig that is no longer doing anything.
  if (logger_ != nullptr && loggingOwned_ && logger_->recording()) {
    logger_->stop(LogStopReason::kShutdown);
  }
  if (logger_ != nullptr && logger_->takeTruncationNotice()) {
    addEvent("dataset truncated: medium full", now);
  }
  loggingOwned_ = false;

  run_.finalState = state;
  run_.reason = reason;
  run_.detail = detail;
  run_.endedUs = now;
  run_.endedEpochMs = clock_.epochMillis();
  run_.stepReached = static_cast<std::uint32_t>(stepIndex_ + 1);
  state_ = state;

  Event event;
  event.type = EventType::kExperimentStopped;
  event.severity = (state == ExperimentState::kAborted) ? 3 : 1;
  event.code = detail.code;
  event.integer = static_cast<std::int32_t>(reason);
  event.detail = (state == ExperimentState::kAborted) ? "experiment aborted"
                                                      : "experiment finished";
  event.timestamp = now;
  events_.publish(event);

  // Written on the way out, whichever way out it was.  A record that only
  // exists for successful runs is a record that hides exactly the runs somebody
  // needs to look at.
  if (sink_ != nullptr) sink_->onRunFinished(run_);
  publishState(now);
}

void ExperimentEngine::addEvent(const char* label, Micros now) {
  if (run_.eventCount >= limits::kMaxRunEvents) {
    ++run_.eventsDropped;
    return;
  }
  RunEvent& event = run_.events[run_.eventCount++];
  event.atUs = now - run_.startedUs;
  event.step = static_cast<std::uint32_t>(stepIndex_ + 1);
  event.label.assign(label);
}

// ---------------------------------------------------------------------------
//  Running
// ---------------------------------------------------------------------------
bool ExperimentEngine::conditionMet(const ExperimentStep& step,
                                    bool& trustworthy) const {
  trustworthy = false;
  const ChannelHandle handle = channels_.findByKey(step.channel.c_str());
  if (handle == kInvalidChannel) return false;
  const ChannelValue* value = channels_.value(handle);
  if (value == nullptr) return false;
  if (value->quality != ChannelQuality::kGood &&
      value->quality != ChannelQuality::kOutOfRange) {
    // A stale reading does not satisfy a condition and does not fail it either:
    // the step keeps waiting and the deadline decides.  Treating the last known
    // value as current is how a scenario proceeds on evidence it does not have.
    return false;
  }
  trustworthy = true;

  switch (step.comparison) {
    case Comparison::kAtLeast: return value->processed >= step.threshold;
    case Comparison::kAtMost:  return value->processed <= step.threshold;
    case Comparison::kAbove:   return value->processed > step.threshold;
    case Comparison::kBelow:   return value->processed < step.threshold;
  }
  return false;
}

Status ExperimentEngine::applyStep(ExperimentStep& step, Micros now) {
  switch (step.targetKind) {
    case TargetKind::kOutputChannel: {
      const ChannelHandle handle = channels_.findByKey(step.targetId.c_str());
      if (handle == kInvalidChannel) {
        return fail(ErrorCode::kChannelNotFound, step.targetId.c_str());
      }
      const Status commanded = outputs_.command(handle, step.value);
      if (!commanded.ok()) return commanded;
      rememberOutput(handle);
      return ok();
    }
    case TargetKind::kLoopSetpoint:
      return control_.setSetpoint(step.targetId.c_str(), step.value);
    case TargetKind::kLoopManual:
      return control_.setManual(step.targetId.c_str(), step.value);
    case TargetKind::kLoopMode: {
      LoopMode mode = LoopMode::kOff;
      if (!parseLoopMode(step.mode.c_str(), mode)) {
        return fail(ErrorCode::kInvalidArgument, "mode");
      }
      const Status changed = control_.setMode(step.targetId.c_str(), mode);
      if (changed.ok() && mode != LoopMode::kOff) rememberLoop(step.targetId.c_str());
      return changed;
    }
    case TargetKind::kLoop: {
      const LoopMode mode = (step.op == StepOp::kEnable) ? LoopMode::kAutomatic
                                                         : LoopMode::kOff;
      const Status changed = control_.setMode(step.targetId.c_str(), mode);
      if (changed.ok() && mode != LoopMode::kOff) rememberLoop(step.targetId.c_str());
      return changed;
    }
    case TargetKind::kDevice: {
      const DeviceRecord* record = devices_.findByKey(step.targetId.c_str());
      if (record == nullptr) return fail(ErrorCode::kNotFound, step.targetId.c_str());
      return devices_.setEnabled(record->handle, step.op == StepOp::kEnable);
    }
    case TargetKind::kNone:
      break;
  }
  (void)now;
  return fail(ErrorCode::kInternal, "unresolved target");
}

void ExperimentEngine::advance(Micros now) {
  ++stepIndex_;
  stepStartedUs_ = now;
  stepEntered_ = false;

  Event event;
  event.type = EventType::kExperimentStepChanged;
  event.integer = static_cast<std::int32_t>(stepIndex_ + 1);
  event.timestamp = now;
  events_.publish(event);
}

double ExperimentEngine::remainingScenarioSeconds() const {
  double total = 0.0;
  for (std::size_t i = stepIndex_; i < experiment_.stepCount; ++i) {
    const ExperimentStep& step = experiment_.steps[i];
    if (step.op == StepOp::kWait || step.op == StepOp::kRunFor) {
      total += static_cast<double>(step.durationUs) / 1e6;
    } else if (step.op == StepOp::kWaitUntil) {
      // The worst case, not the hoped-for one: a dataset must fit the run that
      // takes as long as it is allowed to, not the run that goes well.
      total += static_cast<double>(step.timeoutUs) / 1e6;
    }
  }
  return total;
}

Status ExperimentEngine::startLogging(Micros now) {
  (void)now;
  if (logger_ == nullptr) return fail(ErrorCode::kNotSupported, "no logger");
  if (logger_->recording()) return ok();  // already recording; not an error

  LogSpec spec;
  spec.name = experiment_.metadata.sample.empty()
                  ? LabelString(experiment_.name.c_str())
                  : experiment_.metadata.sample;
  spec.experiment.assign(experiment_.key.c_str());
  spec.operatorName = experiment_.metadata.operatorName;
  spec.sample = experiment_.metadata.sample;
  spec.rateHz = experiment_.logging.rateHz;
  spec.includeRaw = experiment_.logging.includeRaw;
  for (std::size_t i = 0; i < experiment_.logging.channelCount; ++i) {
    const ChannelHandle handle =
        channels_.findByKey(experiment_.logging.channels[i].c_str());
    if (handle == kInvalidChannel) {
      return fail(ErrorCode::kChannelNotFound,
                  experiment_.logging.channels[i].c_str());
    }
    spec.channels[spec.channelCount++] = handle;
  }

  const Status started = logger_->start(spec, remainingScenarioSeconds());
  if (started.ok()) loggingOwned_ = true;
  return started;
}

void ExperimentEngine::tick(Micros now) {
  if (!busy()) {
    publishState(now);
    return;
  }

  // The safety layer is checked before the scenario, every pass.  An experiment
  // is not allowed to be the thing that noticed last.
  if (outputs_.tripped()) {
    finish(ExperimentState::kAborted, StopReason::kSafety,
           fail(ErrorCode::kSafetyInterlock, outputs_.tripReason()), now);
    return;
  }

  if (state_ == ExperimentState::kPaused) {
    // Still holding whatever it commanded: a paused experiment has not let go.
    for (std::size_t i = 0; i < ownedOutputCount_; ++i) {
      (void)outputs_.renew(ownedOutputs_[i]);
    }
    publishState(now);
    return;
  }

  for (std::size_t i = 0; i < ownedOutputCount_; ++i) {
    // Whoever commanded an output keeps saying so (ADR-0016, ADR-0017).  A
    // thirty-minute RUN_FOR on an output with a five-second hold would
    // otherwise switch the heater off five seconds in and call it a run.
    (void)outputs_.renew(ownedOutputs_[i]);
  }

  // The medium filled while this run was recording.  The run continues — a full
  // card is a Reproducibility problem, not a Safety one (§49, ADR-0019) — but
  // the record says exactly when it stopped being a complete dataset.
  if (logger_ != nullptr && logger_->takeTruncationNotice()) {
    addEvent("dataset truncated: medium full", now);
    loggingOwned_ = false;
  }

  if (stepIndex_ >= experiment_.stepCount) {
    finish(ExperimentState::kFinished, StopReason::kScenario, ok(), now);
    return;
  }

  ExperimentStep& step = experiment_.steps[stepIndex_];

  switch (step.op) {
    case StepOp::kSet:
    case StepOp::kEnable:
    case StepOp::kDisable: {
      const Status applied = applyStep(step, now);
      if (!applied.ok()) {
        finish(ExperimentState::kAborted, StopReason::kTargetMissing, applied, now);
        return;
      }
      advance(now);
      return;
    }

    case StepOp::kMarkEvent:
      addEvent(step.label.c_str(), now);
      advance(now);
      return;

    case StepOp::kStop:
      finish(ExperimentState::kFinished, StopReason::kScenario, ok(), now);
      return;

    case StepOp::kWait:
    case StepOp::kRunFor:
      if (now - stepStartedUs_ >= step.durationUs) advance(now);
      publishState(now);
      return;

    case StepOp::kWaitUntil: {
      bool trustworthy = false;
      if (conditionMet(step, trustworthy)) {
        advance(now);
        publishState(now);
        return;
      }
      if (now - stepStartedUs_ >= step.timeoutUs) {
        if (step.onTimeout == OnTimeout::kContinue) {
          addEvent("wait timed out; continued", now);
          advance(now);
          publishState(now);
          return;
        }
        finish(ExperimentState::kAborted, StopReason::kTimeout,
               fail(ErrorCode::kTimeout,
                    trustworthy ? "the condition was never met"
                                : "the channel never reported a trustworthy value"),
               now);
        return;
      }
      publishState(now);
      return;
    }

    case StepOp::kStartLogging: {
      const Status started = startLogging(now);
      if (!started.ok()) {
        // A run that quietly did not record is the failure this whole milestone
        // is about.  If the dataset cannot be opened — no space, index full —
        // the scenario does not proceed as though it had been.
        finish(ExperimentState::kAborted, StopReason::kTargetMissing, started, now);
        return;
      }
      addEvent("logging started", now);
      advance(now);
      return;
    }

    case StepOp::kStopLogging:
      if (logger_ != nullptr && logger_->recording() && loggingOwned_) {
        logger_->stop(LogStopReason::kOperator);
        loggingOwned_ = false;
        addEvent("logging stopped", now);
      }
      advance(now);
      return;
  }
}

double ExperimentEngine::remainingSeconds(Micros now) const {
  if (state_ != ExperimentState::kRunning || stepIndex_ >= experiment_.stepCount) {
    return 0.0;
  }
  const ExperimentStep& step = experiment_.steps[stepIndex_];
  Micros limit = 0;
  if (step.op == StepOp::kWait || step.op == StepOp::kRunFor) limit = step.durationUs;
  if (step.op == StepOp::kWaitUntil) limit = step.timeoutUs;
  if (limit == 0) return 0.0;
  const Micros elapsed = (now > stepStartedUs_) ? (now - stepStartedUs_) : 0;
  if (elapsed >= limit) return 0.0;
  return static_cast<double>(limit - elapsed) / 1e6;
}

void ExperimentEngine::publishState(Micros now) {
  if (stateChannel_ != kInvalidChannel) {
    channels_.publishProcessed(stateChannel_, static_cast<float>(state_), now);
  }
  if (stepChannel_ != kInvalidChannel) {
    const float step = busy() ? static_cast<float>(stepIndex_ + 1) : 0.0f;
    channels_.publishProcessed(stepChannel_, step, now);
  }
  (void)stepEntered_;
}

}  // namespace lc
