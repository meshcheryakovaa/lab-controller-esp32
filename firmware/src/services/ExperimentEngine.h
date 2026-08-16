// =============================================================================
//  services/ExperimentEngine.h — scenarios that run for hours without anybody
//  in the room (§31, §32, §48, ADR-0018).
//
//  A SCENARIO IS DATA, NOT CODE.
//  Every step is an object with an `op` from a closed vocabulary, and this file
//  is a small state machine that walks them.  That is not a stylistic
//  preference: the brief forbids executing arbitrary user C/C++ and forbids
//  loading binary plugins through the browser, so "the experiment language" can
//  never become a language.  It also buys the visual editor — an editor for
//  data is a form; an editor for code is an IDE.
//
//  THE QUESTION THIS MILESTONE HAD TO ANSWER FIRST
//  What happens when the rig disagrees with the scenario?  A scenario is
//  written weeks before it runs, by somebody who assumed the thermocouple would
//  still be attached.  Four answers, decided here rather than per step:
//
//    1. AN EXPERIMENT IS NEVER ABOVE THE SAFETY LAYER.  It sits between the
//       interlocks and the regulators: it may command outputs and setpoints,
//       and a trip aborts it.  Not pauses — aborts.  A run that continued
//       after an interlock fired would be a run whose data is a record of the
//       interlock, not of the sample.
//    2. EVERY WAIT HAS A DEADLINE.  `WAIT_UNTIL` without `timeout_s` is refused
//       at validation, not warned about.  A scenario waiting for 59 °C from a
//       heater that died is a rig heating nothing until somebody walks in on
//       Monday.
//    3. AN EXPERIMENT DOES NOT SURVIVE A REBOOT.  Same reasoning as outputs
//       coming up safe (ADR-0016) and loops coming up OFF (ADR-0017): a
//       controller that reboots mid-run cannot know how long it was away or
//       what the rig did meanwhile.  The run is marked ABORTED with the reason
//       "controller restarted" and stays that way.
//    4. WHAT THE EXPERIMENT TURNED ON, THE EXPERIMENT TURNS OFF.  Ending —
//       finished, stopped or aborted — releases every output it commanded and
//       switches off every loop it started.  Leaving a PID regulating a heater
//       after the scenario that started it has ended is the same failure this
//       project keeps closing, one milestone at a time.
//
//  AN ABORTED RUN MUST NEVER LOOK LIKE A FINISHED ONE.
//  The run record carries the final state, the reason, the step it reached and
//  the error that stopped it — and it is written when the run ENDS, however it
//  ends.  A dataset from a half-run that reads as complete is worse than no
//  dataset: it gets published.
// =============================================================================
#pragma once

#include "core/Clock.h"
#include "core/Error.h"
#include "core/EventBus.h"
#include "core/Scheduler.h"
#include "core/Types.h"
#include "services/ChannelManager.h"
#include "services/ControlManager.h"
#include "services/DataLogger.h"
#include "services/DeviceManager.h"
#include "services/OutputManager.h"

namespace lc {

enum class StepOp : std::uint8_t {
  kSet = 0,      // a setpoint, a loop mode, or an output value
  kWait,         // a fixed delay
  kWaitUntil,    // a condition on a channel, with a mandatory deadline
  kRunFor,       // hold the current state for a duration (the experiment body)
  kMarkEvent,    // a labelled instant in the run record
  kEnable,       // loop → automatic, device → enabled
  kDisable,      // loop → off, device → disabled
  kStop,         // finish, deliberately
  // Milestone 10.  WHAT is recorded belongs to the experiment (one channel set
  // per scenario, in `Experiment::logging`); these two steps say WHEN.  A
  // per-step channel list would put sixteen keys in every step of every
  // scenario, and scenarios that record different things in different phases
  // are rare enough not to be worth that.
  kStartLogging,
  kStopLogging,
};

enum class Comparison : std::uint8_t {
  kAtLeast = 0,  // >=
  kAtMost,       // <=
  kAbove,        // >
  kBelow,        // <
};

enum class OnTimeout : std::uint8_t { kAbort = 0, kContinue };

// What a SET / ENABLE / DISABLE step actually addresses, resolved once when the
// scenario is parsed rather than re-guessed from a string every tick.
enum class TargetKind : std::uint8_t {
  kNone = 0,
  kOutputChannel,   // "heater"
  kLoopSetpoint,    // "bath.setpoint"
  kLoopManual,      // "bath.manual"
  kLoopMode,        // "bath.mode"
  kLoop,            // "bath"        (ENABLE / DISABLE)
  kDevice,          // "heat_01"     (ENABLE / DISABLE)
};

enum class ExperimentState : std::uint8_t {
  kIdle = 0,
  kRunning,
  kPaused,
  kFinished,
  kAborted,
};

enum class StopReason : std::uint8_t {
  kNone = 0,
  kScenario,       // the scenario said STOP, or ran off its last step
  kOperator,       // somebody pressed stop
  kTimeout,        // a WAIT_UNTIL deadline expired with on_timeout = abort
  kSafety,         // the safety layer tripped
  kTargetMissing,  // a channel, loop or device the step names is not there
  kRestarted,      // the controller rebooted while a run was in progress
  kReconfigured,   // the rig was rebuilt underneath it (import, profile switch)
  kInternal,
};

const char* toString(StepOp op);
const char* toString(ExperimentState state);
const char* toString(StopReason reason);
const char* toString(Comparison comparison);
bool parseStepOp(const char* text, StepOp& out);
bool parseComparison(const char* text, Comparison& out);
bool parseOnTimeout(const char* text, OnTimeout& out);

struct ExperimentStep {
  StepOp op = StepOp::kWait;

  // SET / ENABLE / DISABLE
  KeyString target;            // as written: "bath.setpoint", "heater", "heat_01"
  TargetKind targetKind = TargetKind::kNone;
  KeyString targetId;          // the part before the dot, or the whole key
  float value = 0.0f;
  FixedString<12> mode;        // SET on "<loop>.mode"

  // WAIT_UNTIL
  KeyString channel;
  Comparison comparison = Comparison::kAtLeast;
  float threshold = 0.0f;
  Micros timeoutUs = 0;        // mandatory, > 0, enforced by the parser
  OnTimeout onTimeout = OnTimeout::kAbort;

  // WAIT / RUN_FOR
  Micros durationUs = 0;

  // MARK_EVENT
  LabelString label;
};

// What a scenario records, if it records anything.  Empty channel list means
// the scenario has no logging steps — and a scenario with a START_LOGGING step
// and no channels is refused at validation rather than starting a dataset with
// no columns.
struct ExperimentLogging {
  KeyString channels[limits::kMaxLoggedChannels];
  std::size_t channelCount = 0;
  float rateHz = 1.0f;
  bool includeRaw = true;
};

struct ExperimentMetadata {
  LabelString operatorName;
  LabelString sample;
  LabelString description;
  LabelString notes;
};

struct Experiment {
  KeyString key;
  NameString name;
  ExperimentMetadata metadata;
  ExperimentLogging logging;
  ExperimentStep steps[limits::kMaxExperimentSteps];
  std::size_t stepCount = 0;
};

struct RunEvent {
  Micros atUs = 0;
  std::uint32_t step = 0;
  LabelString label;
};

// Everything needed a year later to answer "measured with what, in what
// configuration, and did it actually finish" (§48).
struct RunRecord {
  KeyString experimentKey;
  NameString name;
  ExperimentMetadata metadata;

  EpochMs startedEpochMs = 0;
  EpochMs endedEpochMs = 0;
  Micros startedUs = 0;
  Micros endedUs = 0;

  ExperimentState finalState = ExperimentState::kIdle;
  StopReason reason = StopReason::kNone;
  Error detail;

  std::uint32_t stepReached = 0;
  std::uint32_t stepCount = 0;
  std::uint32_t configRevision = 0;

  RunEvent events[limits::kMaxRunEvents];
  std::size_t eventCount = 0;
  // More events happened than fit.  Said out loud rather than silently
  // truncated: a log that quietly drops entries is a log nobody can reason from.
  std::uint32_t eventsDropped = 0;
};

// Implemented by storage/RunLog.  The engine finishes a run inside a scheduler
// task; nothing else is awake to notice, so it hands the record over rather
// than waiting to be asked for it.
class IRunSink {
 public:
  virtual ~IRunSink() = default;
  // Called from the REST path, where a small write is affordable.  Its whole
  // job is to leave evidence that a run was in progress: a controller that
  // reboots mid-run cannot write its own obituary, so the marker written here
  // is what turns a power cut into an ABORTED record instead of a run that
  // simply vanishes.
  virtual void onRunStarted(const RunRecord& record) = 0;
  virtual void onRunFinished(const RunRecord& record) = 0;
};

class ExperimentEngine {
 public:
  ExperimentEngine(const IClock& clock, ChannelManager& channels,
                   OutputManager& outputs, ControlManager& control,
                   DeviceManager& devices, Scheduler& scheduler, EventBus& events)
      : clock_(clock), channels_(channels), outputs_(outputs), control_(control),
        devices_(devices), scheduler_(scheduler), events_(events) {}

  // Optional: a build without a logger runs scenarios that do not log, and
  // refuses the ones that do rather than pretending to record.
  void setLogger(DataLogger* logger) { logger_ = logger; }

  // Registers the task and creates the two channels that publish the run state.
  // The state is a CHANNEL and not a private field on purpose: dashboards, the
  // rule engine and safety limits all speak channels, so publishing it that way
  // means none of them needs to know that experiments exist (§10).
  Status begin(Micros periodUs = 200000);

  void setRunSink(IRunSink* sink) { sink_ = sink; }

  // Validates a parsed scenario against the rig it is about to run on: every
  // target has to resolve NOW, not when the step is reached at three in the
  // morning.  `offendingStep` is the 1-based index for the editor.
  Status validate(const Experiment& experiment, std::size_t& offendingStep) const;

  // Starts a run.  Refuses while another one is in progress, while the outputs
  // are tripped, and if validation fails.
  Status start(const Experiment& experiment);
  Status pause();
  Status resume();
  // Operator stop.  Ends the run as ABORTED — a run somebody stopped by hand is
  // not a run that finished, and the record has to be able to say which.
  Status stop(StopReason reason = StopReason::kOperator);

  void tick(Micros now);

  ExperimentState state() const { return state_; }
  const RunRecord& run() const { return run_; }
  const Experiment& current() const { return experiment_; }
  std::size_t stepIndex() const { return stepIndex_; }
  // Seconds remaining on the current WAIT / RUN_FOR / WAIT_UNTIL, or 0.
  double remainingSeconds(Micros now) const;
  bool busy() const {
    return state_ == ExperimentState::kRunning || state_ == ExperimentState::kPaused;
  }

 private:
  static void tickTrampoline(void* context);
  Status resolveTarget(ExperimentStep& step) const;
  bool conditionMet(const ExperimentStep& step, bool& trustworthy) const;
  Status applyStep(ExperimentStep& step, Micros now);
  void advance(Micros now);
  void finish(ExperimentState state, StopReason reason, const Error& detail,
              Micros now);
  void releaseEverythingWeTouched();
  void rememberOutput(ChannelHandle handle);
  void rememberLoop(const char* id);
  void addEvent(const char* label, Micros now);
  Status startLogging(Micros now);
  // Seconds the scenario still expects to take, from the timed steps after the
  // current one.  The scenario is data, so this is arithmetic rather than a
  // guess — and it is what lets the logger refuse a dataset that cannot fit.
  double remainingScenarioSeconds() const;
  void publishState(Micros now);

  const IClock& clock_;
  ChannelManager& channels_;
  OutputManager& outputs_;
  ControlManager& control_;
  DeviceManager& devices_;
  Scheduler& scheduler_;
  EventBus& events_;
  IRunSink* sink_ = nullptr;
  DataLogger* logger_ = nullptr;
  bool loggingOwned_ = false;

  Experiment experiment_;
  RunRecord run_;
  ExperimentState state_ = ExperimentState::kIdle;
  std::size_t stepIndex_ = 0;
  Micros stepStartedUs_ = 0;
  Micros pausedAtUs_ = 0;
  bool stepEntered_ = false;

  ChannelHandle ownedOutputs_[limits::kMaxOutputs] = {};
  std::size_t ownedOutputCount_ = 0;
  KeyString ownedLoops_[limits::kMaxControlLoops];
  std::size_t ownedLoopCount_ = 0;

  ChannelHandle stateChannel_ = kInvalidChannel;
  ChannelHandle stepChannel_ = kInvalidChannel;
  TaskId task_ = kInvalidTask;
};

}  // namespace lc
