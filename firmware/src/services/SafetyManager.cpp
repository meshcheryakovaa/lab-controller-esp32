#include "services/SafetyManager.h"

#include <cstring>

namespace lc {

const char* toString(SafetyCondition condition) {
  switch (condition) {
    case SafetyCondition::kAbove:   return "above";
    case SafetyCondition::kBelow:   return "below";
    case SafetyCondition::kOutside: return "outside";
  }
  return "above";
}

const char* toString(SafetyAction action) {
  switch (action) {
    case SafetyAction::kTripAll:       return "trip_all";
    case SafetyAction::kReleaseOutput: return "release_output";
    case SafetyAction::kAlarmOnly:     return "alarm_only";
  }
  return "trip_all";
}

bool parseSafetyCondition(const char* text, SafetyCondition& out) {
  if (text == nullptr) return false;
  if (std::strcmp(text, "above") == 0)   { out = SafetyCondition::kAbove;   return true; }
  if (std::strcmp(text, "below") == 0)   { out = SafetyCondition::kBelow;   return true; }
  if (std::strcmp(text, "outside") == 0) { out = SafetyCondition::kOutside; return true; }
  return false;
}

bool parseSafetyAction(const char* text, SafetyAction& out) {
  if (text == nullptr) return false;
  if (std::strcmp(text, "trip_all") == 0)       { out = SafetyAction::kTripAll;       return true; }
  if (std::strcmp(text, "release_output") == 0) { out = SafetyAction::kReleaseOutput; return true; }
  if (std::strcmp(text, "alarm_only") == 0)     { out = SafetyAction::kAlarmOnly;     return true; }
  return false;
}

SafetyLimit* SafetyManager::mutableFind(const char* id) {
  if (id == nullptr) return nullptr;
  for (std::size_t i = 0; i < count_; ++i) {
    if (limits_[i].active && limits_[i].id.equals(id)) return &limits_[i];
  }
  return nullptr;
}

const SafetyLimit* SafetyManager::find(const char* id) const {
  return const_cast<SafetyManager*>(this)->mutableFind(id);
}

std::size_t SafetyManager::latchedCount() const {
  std::size_t total = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    if (limits_[i].active && limits_[i].latched) ++total;
  }
  return total;
}

Status SafetyManager::begin(Micros periodUs) {
  if (task_ != kInvalidTask) return ok();
  // kSafety, and registered before any controller exists.  A limit that runs
  // after the PID it is supposed to overrule is decoration.
  const Result<TaskId> added = scheduler_.addPeriodic(
      "safety.limits", periodUs, TaskPriority::kSafety, tickTrampoline, this);
  if (!added.ok()) return added.error();
  task_ = added.value();
  return ok();
}

void SafetyManager::tickTrampoline(void* context) {
  SafetyManager* self = static_cast<SafetyManager*>(context);
  self->tick(self->clock_.nowMicros());
}

Status SafetyManager::add(const SafetyLimit& limit) {
  if (limit.id.empty()) return fail(ErrorCode::kInvalidArgument, "limit id is required");
  if (limit.channelKey.empty()) {
    return fail(ErrorCode::kInvalidArgument, "limit needs a channel");
  }
  if (limit.condition == SafetyCondition::kOutside && !(limit.high > limit.low)) {
    return fail(ErrorCode::kRuleInvalid, "outside needs high above low");
  }
  if (limit.action == SafetyAction::kReleaseOutput && limit.targetKey.empty()) {
    return fail(ErrorCode::kRuleInvalid, "release_output needs an output channel");
  }

  SafetyLimit* existing = mutableFind(limit.id.c_str());
  SafetyLimit* slot = existing;
  if (slot == nullptr) {
    if (count_ >= capacity()) {
      return fail(ErrorCode::kOutOfCapacity, "too many safety limits");
    }
    slot = &limits_[count_];
    ++count_;
  }

  *slot = limit;
  slot->active = true;
  // A newly installed limit starts un-latched but NOT trusted: the first tick
  // evaluates it, and if the rig is already past the limit it acts immediately.
  slot->latched = false;
  slot->violating = false;
  slot->violatingSinceUs = 0;
  return ok();
}

Status SafetyManager::remove(const char* id) {
  for (std::size_t i = 0; i < count_; ++i) {
    if (!limits_[i].active || !limits_[i].id.equals(id)) continue;
    limits_[i] = limits_[count_ - 1];
    --count_;
    return ok();
  }
  return fail(ErrorCode::kNotFound, "safety limit");
}

void SafetyManager::clearAll() { count_ = 0; }

Status SafetyManager::reset(const char* id) {
  SafetyLimit* limit = mutableFind(id);
  if (limit == nullptr) return fail(ErrorCode::kNotFound, "safety limit");
  limit->latched = false;
  limit->violating = false;
  limit->violatingSinceUs = 0;
  limit->lastReason = ok();
  return ok();
}

void SafetyManager::resetAll() {
  for (std::size_t i = 0; i < count_; ++i) reset(limits_[i].id.c_str());
}

void SafetyManager::raise(const SafetyLimit& limit, const char* detail,
                          std::uint8_t severity) {
  Event event;
  event.type = EventType::kSafetyTripped;
  event.severity = severity;
  event.number = limit.high;
  event.code = ErrorCode::kSafetyInterlock;
  event.detail = detail;  // static lifetime only
  event.timestamp = clock_.nowMicros();
  events_.publish(event);
}

void SafetyManager::act(SafetyLimit& limit, const Error& reason, Micros now) {
  (void)now;
  limit.latched = true;
  ++limit.trips;
  limit.lastReason = reason;

  switch (limit.action) {
    case SafetyAction::kTripAll:
      // Through the same door the operator's stop button uses.  One mechanism,
      // one place to get right (ADR-0016).
      outputs_.trip("safety limit");
      raise(limit, "safety limit tripped: all outputs released", /*severity=*/4);
      break;

    case SafetyAction::kReleaseOutput: {
      const ChannelHandle target = channels_.findByKey(limit.targetKey.c_str());
      if (target != kInvalidChannel) {
        outputs_.release(target, OutputHoldState::kTripped);
      }
      raise(limit, "safety limit released an output", /*severity=*/3);
      break;
    }

    case SafetyAction::kAlarmOnly:
      raise(limit, "safety limit reached", /*severity=*/3);
      break;
  }
}

void SafetyManager::tick(Micros now) {
  for (std::size_t i = 0; i < count_; ++i) {
    SafetyLimit& limit = limits_[i];
    if (!limit.active || !limit.enabled) continue;
    if (limit.latched) {
      // A latched limit is not re-evaluated — re-arming is a human decision.
      // But "not re-evaluated" must not become "no longer enforced": if this
      // limit tripped the rig and somebody cleared the trip without resetting
      // the limit, the outputs would be commandable again while the interlock
      // sits latched and blind.  So a latched trip keeps re-asserting itself.
      // Clearing the trip is then only possible by resetting the limit, and
      // resetting a limit whose cause is still there trips it again on the very
      // next pass.  That is the intended answer to "can I make it go away".
      if (limit.action == SafetyAction::kTripAll && !outputs_.tripped()) {
        outputs_.trip("a safety limit is still latched");
      }
      continue;
    }

    const ChannelHandle handle = channels_.findByKey(limit.channelKey.c_str());
    const ChannelValue* value =
        (handle != kInvalidChannel) ? channels_.value(handle) : nullptr;

    bool violating = false;
    Error reason = ok();

    if (value == nullptr) {
      // The channel this limit watches does not exist.  That is not a reason to
      // relax: an interlock whose sensor was deleted protects nothing.
      violating = limit.requireFreshInput;
      reason = fail(ErrorCode::kChannelNotFound, limit.channelKey.c_str());
    } else if (value->quality != ChannelQuality::kGood &&
               value->quality != ChannelQuality::kOutOfRange) {
      // STALE, FAULTED, SATURATED or never-sampled.  An interlock that can be
      // switched off by unplugging a thermocouple is not an interlock.
      // OUT_OF_RANGE is excluded on purpose: it means the sensor is working and
      // reporting something outside its declared band, which is exactly the
      // case this limit exists to catch.
      violating = limit.requireFreshInput;
      reason = fail(ErrorCode::kSafetyInterlock, "input is not trustworthy");
    } else {
      switch (limit.condition) {
        case SafetyCondition::kAbove:
          violating = value->processed > limit.high;
          break;
        case SafetyCondition::kBelow:
          violating = value->processed < limit.low;
          break;
        case SafetyCondition::kOutside:
          violating = value->processed < limit.low || value->processed > limit.high;
          break;
      }
      if (violating) reason = fail(ErrorCode::kSafetyInterlock, "limit exceeded");
    }

    if (!violating) {
      limit.violating = false;
      limit.violatingSinceUs = 0;
      continue;
    }

    if (!limit.violating) {
      limit.violating = true;
      limit.violatingSinceUs = now;
    }
    // Debounce delays; it never cancels.  One noisy sample must not stop an
    // eight-hour experiment, and a real excursion must not survive one.
    if (limit.forUs > 0 && now >= limit.violatingSinceUs &&
        (now - limit.violatingSinceUs) < limit.forUs) {
      continue;
    }
    act(limit, reason, now);
  }
}

}  // namespace lc
