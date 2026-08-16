// =============================================================================
//  core/Scheduler.h — cooperative, non-blocking periodic task scheduler (§17).
//
//  WHY NOT ONE FreeRTOS TASK PER MODULE
//    Each FreeRTOS task costs a stack (2–4 KiB is typical for anything that
//    touches Wire or ArduinoJson).  Thirty devices would eat the whole heap in
//    stacks alone, and every shared structure would need a mutex.  Instead the
//    firmware uses a small number of FreeRTOS tasks with clear ownership
//    (see docs/architecture.md, "Concurrency model") and runs all device
//    polling, processing and control cooperatively inside this scheduler on
//    the control task.  Drivers must therefore never block; a driver that
//    needs to wait returns and is re-entered on the next tick.
//
//  PRIORITY
//    Safety > Control > Acquisition > Processing > Telemetry > Background.
//    When the pass runs out of its time budget, low-priority tasks are
//    deferred, never the safety ones.
//
//  ALLOCATION: none.  Callbacks are plain function pointers plus a context
//  pointer, so no std::function heap allocation and no vtable churn.
// =============================================================================
#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/Error.h"
#include "core/Types.h"

namespace lc {

enum class TaskPriority : std::uint8_t {
  kSafety = 0,      // SafetyManager interlocks — always run
  kControl = 1,     // PID, rule engine, experiment engine
  kAcquisition = 2, // device sampling
  kProcessing = 3,  // filters, virtual channels
  kTelemetry = 4,   // WebSocket batching, logging flush
  kBackground = 5,  // diagnostics, housekeeping
  kCount = 6,
};

using TaskCallback = void (*)(void* context);
using TaskId = std::uint16_t;
inline constexpr TaskId kInvalidTask = 0;

struct TaskStats {
  std::uint32_t runs = 0;
  std::uint32_t overruns = 0;     // callback took longer than its own period
  std::uint32_t misses = 0;       // pass budget exhausted, execution deferred
  std::uint32_t lastDurationUs = 0;
  std::uint32_t maxDurationUs = 0;
  std::uint32_t maxLatenessUs = 0;  // how late the callback actually started
};

class Scheduler {
 public:
  explicit Scheduler(const IClock& clock) : clock_(clock) {}

  // periodUs == 0 means "run on every pass" (still subject to the budget).
  Result<TaskId> addPeriodic(const char* name, Micros periodUs,
                             TaskPriority priority, TaskCallback callback,
                             void* context);

  // Fires once after delayUs, then removes itself.
  Result<TaskId> addOneShot(const char* name, Micros delayUs,
                            TaskPriority priority, TaskCallback callback,
                            void* context);

  Status remove(TaskId id);
  Status setPeriod(TaskId id, Micros periodUs);
  Status setEnabled(TaskId id, bool enabled);

  // Runs every task whose deadline has passed, highest priority first, until
  // the budget is spent.  Returns the number of callbacks executed.
  // `budgetUs == 0` disables the budget (used by tests).
  std::size_t runPass(Micros budgetUs = kDefaultBudgetUs);

  // Microseconds until the next deadline; 0 if something is already due.
  // Lets the main loop yield precisely instead of spinning.
  Micros microsUntilNextDue() const;

  std::size_t taskCount() const { return taskCount_; }
  const TaskStats* stats(TaskId id) const;
  const char* name(TaskId id) const;

  // Read-only view of one task, for the diagnostics page.  Iterating by index
  // rather than exposing the table keeps the internals private while still
  // letting the API report which task is overrunning, by name.
  struct TaskInfo {
    TaskId id = kInvalidTask;
    const char* name = "";
    TaskPriority priority = TaskPriority::kBackground;
    Micros periodUs = 0;
    bool enabled = false;
    TaskStats stats;
  };
  bool taskAt(std::size_t index, TaskInfo& out) const;

  // Diagnostics (§41)
  std::uint32_t passCount() const { return passCount_; }
  std::uint32_t maxPassDurationUs() const { return maxPassDurationUs_; }
  std::uint32_t budgetExhaustedCount() const { return budgetExhausted_; }
  void resetPeakStats();

  static constexpr Micros kDefaultBudgetUs = 5000;  // 5 ms per pass

 private:
  struct Task {
    TaskId id = kInvalidTask;
    const char* name = "";        // static lifetime
    Micros periodUs = 0;
    Micros nextDueUs = 0;
    TaskCallback callback = nullptr;
    void* context = nullptr;
    TaskPriority priority = TaskPriority::kBackground;
    bool enabled = true;
    bool oneShot = false;
    TaskStats stats;
  };

  Task* find(TaskId id);
  const Task* find(TaskId id) const;
  Result<TaskId> insert(const char* name, Micros periodUs, Micros firstDueUs,
                        TaskPriority priority, TaskCallback callback,
                        void* context, bool oneShot);
  void execute(Task& task, Micros now);

  const IClock& clock_;
  Task tasks_[limits::kMaxSchedulerTasks];
  std::size_t taskCount_ = 0;
  TaskId nextId_ = 1;

  std::uint32_t passCount_ = 0;
  std::uint32_t maxPassDurationUs_ = 0;
  std::uint32_t budgetExhausted_ = 0;
};

}  // namespace lc
