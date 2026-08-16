#include "services/OutputManager.h"

#include <cmath>

namespace lc {

const char* toString(OutputHoldState state) {
  switch (state) {
    case OutputHoldState::kSafe:        return "SAFE";
    case OutputHoldState::kCommanded:   return "COMMANDED";
    case OutputHoldState::kExpired:     return "EXPIRED";
    case OutputHoldState::kDeviceFault: return "DEVICE_FAULT";
    case OutputHoldState::kTripped:     return "TRIPPED";
  }
  return "SAFE";
}

OutputRecord* OutputManager::mutableFind(ChannelHandle channel) {
  for (std::size_t i = 0; i < count_; ++i) {
    if (records_[i].active && records_[i].channel == channel) return &records_[i];
  }
  return nullptr;
}

const OutputRecord* OutputManager::find(ChannelHandle channel) const {
  return const_cast<OutputManager*>(this)->mutableFind(channel);
}

Status OutputManager::registerOutput(ChannelHandle channel, DeviceHandle device,
                                     float safeValue, Micros holdTimeoutUs) {
  if (channel == kInvalidChannel) {
    return fail(ErrorCode::kInvalidArgument, "no channel");
  }
  if (!std::isfinite(safeValue)) {
    // A NaN safe value would propagate into the actuator on the very path that
    // exists to make things safe.
    return fail(ErrorCode::kInvalidArgument, "safe value is not a number");
  }

  OutputRecord* existing = mutableFind(channel);
  OutputRecord* record = existing;
  if (record == nullptr) {
    if (count_ >= capacity()) {
      return fail(ErrorCode::kOutOfCapacity, "too many outputs");
    }
    record = &records_[count_];
    ++count_;
  }

  *record = OutputRecord{};
  record->active = true;
  record->published = false;
  record->channel = channel;
  record->device = device;
  record->safeValue = safeValue;
  record->holdTimeoutUs = holdTimeoutUs;
  record->commanded = safeValue;
  record->applied = safeValue;
  record->state = OutputHoldState::kSafe;
  return ok();
}

void OutputManager::forgetChannel(ChannelHandle channel) {
  for (std::size_t i = 0; i < count_; ++i) {
    if (!records_[i].active || records_[i].channel != channel) continue;
    records_[i] = records_[count_ - 1];
    --count_;
    return;
  }
}

void OutputManager::forgetDevice(DeviceHandle device) {
  for (std::size_t i = count_; i-- > 0;) {
    if (records_[i].device != device) continue;
    records_[i] = records_[count_ - 1];
    --count_;
  }
}

Status OutputManager::begin(Micros periodUs) {
  if (task_ != kInvalidTask) return ok();
  // kSafety: this task runs before acquisition, processing or telemetry can
  // spend the pass budget.  An output that should have been released must not
  // wait behind a WebSocket frame.
  const Result<TaskId> added = scheduler_.addPeriodic(
      "safety.outputs", periodUs, TaskPriority::kSafety, tickTrampoline, this);
  if (!added.ok()) return added.error();
  task_ = added.value();
  return ok();
}

void OutputManager::tickTrampoline(void* context) {
  OutputManager* self = static_cast<OutputManager*>(context);
  self->tick(self->clock_.nowMicros());
}

Status OutputManager::drive(OutputRecord& record, float value,
                            OutputHoldState state) {
  // Through ChannelManager, so an actuator's value appears in the channel
  // exactly like a sensor reading: the same telemetry, the same log, the same
  // chart.  An output nobody can see on the dashboard is an output nobody
  // notices is stuck.
  const Status written = channels_.write(record.channel, value);
  if (!written.ok()) return written;

  record.state = state;
  // What the actuator ACTUALLY did, read back rather than assumed.  A heater
  // limited to 60 % and a fan raised to its minimum duty both mean the request
  // and the result differ, and this record is what the API reports.
  const ChannelValue* actual = channels_.value(record.channel);
  record.applied = (actual != nullptr) ? actual->processed : value;
  return ok();
}

void OutputManager::publish(const OutputRecord& record, const char* detail,
                            std::uint8_t severity) {
  Event event;
  event.type = EventType::kSafetyTripped;
  event.source = record.channel;
  event.number = record.safeValue;
  event.integer = static_cast<std::int32_t>(record.state);
  event.severity = severity;
  event.detail = detail;  // static lifetime only
  event.timestamp = clock_.nowMicros();
  events_.publish(event);
}

Status OutputManager::command(ChannelHandle channel, float value) {
  if (tripped_) {
    return fail(ErrorCode::kSafetyInterlock, tripReason_);
  }
  OutputRecord* record = mutableFind(channel);
  if (record == nullptr) {
    // Not a refusal to be helpful: an output that never declared a safe state
    // cannot be released safely either, so it must not be commandable.
    return fail(ErrorCode::kNotFound, "channel is not a registered output");
  }
  if (!std::isfinite(value)) {
    return fail(ErrorCode::kInvalidArgument, "value is not a number");
  }
  if (record->state == OutputHoldState::kDeviceFault) {
    return fail(ErrorCode::kInvalidState, "device is not running");
  }

  record->commanded = value;
  record->commandedAtUs = clock_.nowMicros();
  return drive(*record, value, OutputHoldState::kCommanded);
}

Status OutputManager::renew(ChannelHandle channel) {
  if (tripped_) return fail(ErrorCode::kSafetyInterlock, tripReason_);
  OutputRecord* record = mutableFind(channel);
  if (record == nullptr) return fail(ErrorCode::kNotFound, "output");
  if (record->state != OutputHoldState::kCommanded) {
    // Renewing an expired command would let a browser that woke up after the
    // deadline silently switch the heater back on without anybody deciding to.
    return fail(ErrorCode::kInvalidState, "nothing is commanded");
  }
  record->commandedAtUs = clock_.nowMicros();
  return ok();
}

Status OutputManager::release(ChannelHandle channel, OutputHoldState reason) {
  OutputRecord* record = mutableFind(channel);
  if (record == nullptr) return fail(ErrorCode::kNotFound, "output");
  record->commanded = record->safeValue;
  return drive(*record, record->safeValue, reason);
}

void OutputManager::trip(const char* reason) {
  tripped_ = true;
  tripReason_ = (reason != nullptr) ? reason : "safety trip";
  ++trips_;
  for (std::size_t i = 0; i < count_; ++i) {
    OutputRecord& record = records_[i];
    if (!record.active) continue;
    record.commanded = record.safeValue;
    drive(record, record.safeValue, OutputHoldState::kTripped);
    publish(record, tripReason_, /*severity=*/4);
  }
}

void OutputManager::clearTrip() {
  if (!tripped_) return;
  tripped_ = false;
  tripReason_ = "";
  // Outputs stay at their safe values.  Clearing a trip means "you may command
  // again", never "resume whatever you were doing" — the operator decides what
  // comes back on, one output at a time.
  for (std::size_t i = 0; i < count_; ++i) {
    if (records_[i].active) records_[i].state = OutputHoldState::kSafe;
  }
}

void OutputManager::onDeviceStateChanged(DeviceHandle device, bool healthy) {
  for (std::size_t i = 0; i < count_; ++i) {
    OutputRecord& record = records_[i];
    if (!record.active || record.device != device) continue;

    if (!healthy) {
      record.commanded = record.safeValue;
      record.state = OutputHoldState::kDeviceFault;
      record.applied = record.safeValue;
      // Deliberately NOT written through the driver: the device is in error,
      // so the write would fail anyway.  The record is what the API reports,
      // and it must not claim the actuator is still carrying a command.
      publish(record, "output device stopped running", /*severity=*/3);
      continue;
    }
    if (record.state == OutputHoldState::kDeviceFault || !record.published) {
      // Either recovered, or running for the first time.  Both come back SAFE,
      // never commanded: a sensor that dropped off the bus for a minute must
      // not bring the heater back with it.
      //
      // The first publish also puts the safe value into the CHANNEL, so a fan
      // whose safe state is 30 % reads 30 % on the dashboard from the moment it
      // starts, instead of reading 0 while turning.
      record.state = OutputHoldState::kSafe;
      if (drive(record, record.safeValue, OutputHoldState::kSafe).ok()) {
        record.published = true;
      }
    }
  }
}

void OutputManager::tick(Micros now) {
  if (tripped_) return;

  for (std::size_t i = 0; i < count_; ++i) {
    OutputRecord& record = records_[i];
    if (!record.active) continue;
    if (record.state != OutputHoldState::kCommanded) continue;
    if (record.holdTimeoutUs == 0) continue;  // deadline waived, deliberately
    if (now < record.commandedAtUs) continue;  // clock stepped backwards
    if (now - record.commandedAtUs < record.holdTimeoutUs) continue;

    // The deadline is the answer to "the browser was closed / the tablet went
    // out of Wi-Fi range / the operator went to lunch".
    record.commanded = record.safeValue;
    drive(record, record.safeValue, OutputHoldState::kExpired);
    ++expiries_;
    publish(record, "command expired; output released to its safe value",
            /*severity=*/3);
  }
}

}  // namespace lc
