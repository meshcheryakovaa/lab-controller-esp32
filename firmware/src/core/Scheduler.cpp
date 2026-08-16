#include "core/Scheduler.h"

namespace lc {

Result<TaskId> Scheduler::insert(const char* name, Micros periodUs,
                                 Micros firstDueUs, TaskPriority priority,
                                 TaskCallback callback, void* context,
                                 bool oneShot) {
  if (callback == nullptr) {
    return fail(ErrorCode::kInvalidArgument, "callback is null");
  }
  if (taskCount_ >= limits::kMaxSchedulerTasks) {
    return fail(ErrorCode::kOutOfCapacity, "scheduler task table full");
  }

  Task& task = tasks_[taskCount_++];
  task = Task{};
  task.id = nextId_++;
  if (nextId_ == kInvalidTask) nextId_ = 1;
  task.name = (name != nullptr) ? name : "";
  task.periodUs = periodUs;
  task.nextDueUs = firstDueUs;
  task.callback = callback;
  task.context = context;
  task.priority = priority;
  task.enabled = true;
  task.oneShot = oneShot;
  return task.id;
}

Result<TaskId> Scheduler::addPeriodic(const char* name, Micros periodUs,
                                      TaskPriority priority,
                                      TaskCallback callback, void* context) {
  return insert(name, periodUs, clock_.nowMicros() + periodUs, priority,
                callback, context, /*oneShot=*/false);
}

Result<TaskId> Scheduler::addOneShot(const char* name, Micros delayUs,
                                     TaskPriority priority,
                                     TaskCallback callback, void* context) {
  return insert(name, 0, clock_.nowMicros() + delayUs, priority, callback,
                context, /*oneShot=*/true);
}

Scheduler::Task* Scheduler::find(TaskId id) {
  for (std::size_t i = 0; i < taskCount_; ++i) {
    if (tasks_[i].id == id) return &tasks_[i];
  }
  return nullptr;
}

const Scheduler::Task* Scheduler::find(TaskId id) const {
  return const_cast<Scheduler*>(this)->find(id);
}

Status Scheduler::remove(TaskId id) {
  for (std::size_t i = 0; i < taskCount_; ++i) {
    if (tasks_[i].id != id) continue;
    tasks_[i] = tasks_[taskCount_ - 1];
    --taskCount_;
    return ok();
  }
  return fail(ErrorCode::kNotFound, "task id");
}

Status Scheduler::setPeriod(TaskId id, Micros periodUs) {
  Task* task = find(id);
  if (task == nullptr) return fail(ErrorCode::kNotFound, "task id");
  task->periodUs = periodUs;
  task->nextDueUs = clock_.nowMicros() + periodUs;
  return ok();
}

Status Scheduler::setEnabled(TaskId id, bool enabled) {
  Task* task = find(id);
  if (task == nullptr) return fail(ErrorCode::kNotFound, "task id");
  if (enabled && !task->enabled) {
    // Re-arm relative to now so a task disabled for ten minutes does not fire
    // six hundred times in a row when it comes back.
    task->nextDueUs = clock_.nowMicros() + task->periodUs;
  }
  task->enabled = enabled;
  return ok();
}

void Scheduler::execute(Task& task, Micros now) {
  const Micros lateness = (now > task.nextDueUs) ? (now - task.nextDueUs) : 0;

  task.callback(task.context);

  const Micros finished = clock_.nowMicros();
  const Micros duration = (finished > now) ? (finished - now) : 0;

  TaskStats& stats = task.stats;
  ++stats.runs;
  stats.lastDurationUs = static_cast<std::uint32_t>(duration);
  if (duration > stats.maxDurationUs) {
    stats.maxDurationUs = static_cast<std::uint32_t>(duration);
  }
  if (lateness > stats.maxLatenessUs) {
    stats.maxLatenessUs = static_cast<std::uint32_t>(lateness);
  }
  if (task.periodUs > 0 && duration > task.periodUs) ++stats.overruns;

  if (task.oneShot) {
    task.enabled = false;  // compacted out by runPass()
    return;
  }

  if (task.periodUs == 0) {
    task.nextDueUs = finished;
    return;
  }

  // Fixed-rate scheduling with catch-up suppression: advance by whole periods
  // so sampling stays on grid, but never try to "make up" missed slots — that
  // would produce a burst of back-to-back reads after a long blocking event.
  task.nextDueUs += task.periodUs;
  if (task.nextDueUs <= finished) {
    const Micros behind = finished - task.nextDueUs;
    const Micros skip = (behind / task.periodUs) + 1;
    task.nextDueUs += skip * task.periodUs;
    stats.misses += static_cast<std::uint32_t>(skip);
  }
}

std::size_t Scheduler::runPass(Micros budgetUs) {
  const Micros passStart = clock_.nowMicros();
  std::size_t executed = 0;
  bool budgetHit = false;

  for (std::uint8_t level = 0;
       level < static_cast<std::uint8_t>(TaskPriority::kCount); ++level) {
    for (std::size_t i = 0; i < taskCount_; ++i) {
      Task& task = tasks_[i];
      if (static_cast<std::uint8_t>(task.priority) != level) continue;
      if (!task.enabled || task.callback == nullptr) continue;

      const Micros now = clock_.nowMicros();
      if (now < task.nextDueUs) continue;

      // Safety tasks ignore the budget entirely (§30, §49): an interlock that
      // gets deferred because the WebSocket was chatty is not an interlock.
      if (budgetUs > 0 && task.priority != TaskPriority::kSafety &&
          (now - passStart) >= budgetUs) {
        ++task.stats.misses;
        budgetHit = true;
        continue;
      }

      execute(task, now);
      ++executed;
    }
  }

  // Compact finished one-shots.
  for (std::size_t i = 0; i < taskCount_;) {
    if (tasks_[i].oneShot && !tasks_[i].enabled) {
      tasks_[i] = tasks_[taskCount_ - 1];
      --taskCount_;
    } else {
      ++i;
    }
  }

  const Micros passEnd = clock_.nowMicros();
  const std::uint32_t passDuration =
      static_cast<std::uint32_t>(passEnd - passStart);
  if (passDuration > maxPassDurationUs_) maxPassDurationUs_ = passDuration;
  if (budgetHit) ++budgetExhausted_;
  ++passCount_;

  return executed;
}

Micros Scheduler::microsUntilNextDue() const {
  const Micros now = clock_.nowMicros();
  Micros best = static_cast<Micros>(-1);
  bool found = false;
  for (std::size_t i = 0; i < taskCount_; ++i) {
    const Task& task = tasks_[i];
    if (!task.enabled || task.callback == nullptr) continue;
    if (task.nextDueUs <= now) return 0;
    const Micros remaining = task.nextDueUs - now;
    if (!found || remaining < best) {
      best = remaining;
      found = true;
    }
  }
  // With no runnable task at all, report a modest idle interval rather than 0
  // so the caller's main loop yields instead of spinning at 100 % CPU.
  return found ? best : static_cast<Micros>(1000);
}

const TaskStats* Scheduler::stats(TaskId id) const {
  const Task* task = find(id);
  return (task != nullptr) ? &task->stats : nullptr;
}

bool Scheduler::taskAt(std::size_t index, TaskInfo& out) const {
  if (index >= taskCount_) return false;
  const Task& task = tasks_[index];
  out.id = task.id;
  out.name = task.name;
  out.priority = task.priority;
  out.periodUs = task.periodUs;
  out.enabled = task.enabled;
  out.stats = task.stats;
  return true;
}

const char* Scheduler::name(TaskId id) const {
  const Task* task = find(id);
  return (task != nullptr) ? task->name : nullptr;
}

void Scheduler::resetPeakStats() {
  maxPassDurationUs_ = 0;
  budgetExhausted_ = 0;
  for (std::size_t i = 0; i < taskCount_; ++i) {
    tasks_[i].stats.maxDurationUs = 0;
    tasks_[i].stats.maxLatenessUs = 0;
  }
}

}  // namespace lc
