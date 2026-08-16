#include "services/ChannelManager.h"

#include <cmath>

namespace lc {

namespace {
// A channel is considered stale after this many missed acquisition periods.
constexpr Micros kStaleIntervalMultiplier = 3;
}  // namespace

ChannelManager::Slot* ChannelManager::slotFor(ChannelHandle handle) {
  if (handle == kInvalidChannel || handle > limits::kMaxChannels) return nullptr;
  Slot& slot = slots_[handle - 1];
  return slot.active ? &slot : nullptr;
}

const ChannelManager::Slot* ChannelManager::slotFor(ChannelHandle handle) const {
  return const_cast<ChannelManager*>(this)->slotFor(handle);
}

Result<ChannelHandle> ChannelManager::create(const ChannelDescriptor& descriptor) {
  if (descriptor.key.empty()) {
    return fail(ErrorCode::kInvalidArgument, "channel key is empty");
  }
  if (findByKey(descriptor.key.c_str()) != kInvalidChannel) {
    return fail(ErrorCode::kAlreadyExists, descriptor.key.c_str());
  }

  for (std::size_t i = 0; i < limits::kMaxChannels; ++i) {
    if (slots_[i].active) continue;
    Slot& slot = slots_[i];
    slot = Slot{};
    slot.active = true;
    slot.descriptor = descriptor;
    slot.declared.unit = descriptor.unit;
    slot.declared.precision = descriptor.precision;
    slot.declared.minimum = descriptor.minimum;
    slot.declared.maximum = descriptor.maximum;
    slot.value = ChannelValue{};
    ++activeCount_;
    const ChannelHandle handle = static_cast<ChannelHandle>(i + 1);
    notifyLifecycle(handle, /*created=*/true);
    return handle;
  }
  return fail(ErrorCode::kOutOfCapacity, "channel table full");
}

Status ChannelManager::remove(ChannelHandle handle) {
  Slot* slot = slotFor(handle);
  if (slot == nullptr) return fail(ErrorCode::kChannelNotFound);
  // Listeners run BEFORE the slot is cleared, so ProcessingManager can still
  // see which channel it is destroying processors for.
  notifyLifecycle(handle, /*created=*/false);
  *slot = Slot{};  // leaves a hole: handles of other channels stay valid
  --activeCount_;
  return ok();
}

std::size_t ChannelManager::removeAllFrom(DeviceHandle device) {
  std::size_t removed = 0;
  for (std::size_t i = 0; i < limits::kMaxChannels; ++i) {
    if (!slots_[i].active) continue;
    if (slots_[i].descriptor.source != device) continue;
    notifyLifecycle(static_cast<ChannelHandle>(i + 1), /*created=*/false);
    slots_[i] = Slot{};
    --activeCount_;
    ++removed;
  }
  return removed;
}

Status ChannelManager::addLifecycleListener(ChannelLifecycleListener listener,
                                            void* context) {
  if (listener == nullptr) return fail(ErrorCode::kInvalidArgument);
  if (lifecycleListenerCount_ >= kMaxLifecycleListeners) {
    return fail(ErrorCode::kOutOfCapacity, "lifecycle listener table full");
  }
  lifecycleListeners_[lifecycleListenerCount_].fn = listener;
  lifecycleListeners_[lifecycleListenerCount_].context = context;
  ++lifecycleListenerCount_;
  return ok();
}

void ChannelManager::notifyLifecycle(ChannelHandle handle, bool created) {
  for (std::size_t i = 0; i < lifecycleListenerCount_; ++i) {
    if (lifecycleListeners_[i].fn != nullptr) {
      lifecycleListeners_[i].fn(handle, created, lifecycleListeners_[i].context);
    }
  }
}

ChannelHandle ChannelManager::findByKey(const char* key) const {
  if (key == nullptr) return kInvalidChannel;
  for (std::size_t i = 0; i < limits::kMaxChannels; ++i) {
    if (slots_[i].active && slots_[i].descriptor.key.equals(key)) {
      return static_cast<ChannelHandle>(i + 1);
    }
  }
  return kInvalidChannel;
}

bool ChannelManager::exists(ChannelHandle handle) const {
  return slotFor(handle) != nullptr;
}

const ChannelDescriptor* ChannelManager::descriptor(ChannelHandle handle) const {
  const Slot* slot = slotFor(handle);
  return (slot != nullptr) ? &slot->descriptor : nullptr;
}

const ChannelValue* ChannelManager::value(ChannelHandle handle) const {
  const Slot* slot = slotFor(handle);
  return (slot != nullptr) ? &slot->value : nullptr;
}

ChannelQuality ChannelManager::classify(const ChannelDescriptor& descriptor,
                                        float sample) const {
  if (std::isnan(sample) || std::isinf(sample)) return ChannelQuality::kFaulted;
  // A channel with minimum == maximum has no declared range; skip the check
  // rather than flagging every sample.
  if (descriptor.minimum < descriptor.maximum) {
    if (sample < descriptor.minimum || sample > descriptor.maximum) {
      return ChannelQuality::kOutOfRange;
    }
  }
  return ChannelQuality::kGood;
}

void ChannelManager::commit(Slot& slot, ChannelHandle handle, Micros now) {
  slot.value.timestampUs = now;
  slot.value.epochMs = clock_.epochMillis();
  ++slot.value.sequence;
  slot.value.quality = classify(slot.descriptor, slot.value.processed);
  ++publishedSamples_;
  notify(handle, slot);
}

void ChannelManager::notify(ChannelHandle handle, const Slot& slot) {
  for (std::size_t i = 0; i < listenerCount_; ++i) {
    if (listeners_[i].fn != nullptr) {
      listeners_[i].fn(handle, slot.value, listeners_[i].context);
    }
  }
}

bool ChannelManager::publishRaw(ChannelHandle handle, float raw, Micros now) {
  Slot* slot = slotFor(handle);
  if (slot == nullptr) return false;

  slot->value.raw = raw;

  float current = raw;
  float calibrated = raw;
  bool valid = true;

  for (std::uint8_t i = 0; i < slot->stageCount; ++i) {
    IProcessor* stage = slot->stages[i];
    if (stage == nullptr) continue;
    current = stage->process(current, now, valid);
    if (slot->calibrationStage >= 0 &&
        i == static_cast<std::uint8_t>(slot->calibrationStage)) {
      calibrated = current;
    }
    if (!valid) break;
  }

  if (!valid) {
    // The pipeline is still warming up (median window, moving average).  The
    // raw value is retained so diagnostics can show the sensor is alive.
    ++suppressedSamples_;
    return false;
  }

  slot->value.calibrated = (slot->calibrationStage >= 0) ? calibrated : raw;
  slot->value.processed = current;
  commit(*slot, handle, now);
  return true;
}

bool ChannelManager::publishProcessed(ChannelHandle handle, float processed,
                                      Micros now) {
  Slot* slot = slotFor(handle);
  if (slot == nullptr) return false;
  slot->value.raw = processed;
  slot->value.calibrated = processed;
  slot->value.processed = processed;
  commit(*slot, handle, now);
  return true;
}

Status ChannelManager::write(ChannelHandle handle, float commanded) {
  Slot* slot = slotFor(handle);
  if (slot == nullptr) return fail(ErrorCode::kChannelNotFound);
  if (slot->descriptor.direction != ChannelDirection::kOutput) {
    return fail(ErrorCode::kChannelTypeMismatch, "channel is not an output");
  }
  if (outputSink_ == nullptr) {
    return fail(ErrorCode::kInvalidState, "no output sink installed");
  }

  float applied = commanded;
  const Status status =
      outputSink_(handle, commanded, &applied, outputSinkContext_);
  if (!status.ok()) return status;

  // Reflect what the actuator ACTUALLY did back into the channel, so dashboards,
  // logging and rules see actuator state through exactly the same mechanism as
  // sensor readings — and see the effect of any limit the driver applied.
  publishProcessed(handle, applied, clock_.nowMicros());
  return ok();
}

void ChannelManager::setOutputSink(OutputSink sink, void* context) {
  outputSink_ = sink;
  outputSinkContext_ = context;
}

Status ChannelManager::setPipeline(ChannelHandle handle,
                                   IProcessor* const* stages,
                                   std::uint8_t stageCount,
                                   std::int8_t calibrationStage) {
  Slot* slot = slotFor(handle);
  if (slot == nullptr) return fail(ErrorCode::kChannelNotFound);
  if (stageCount > limits::kMaxProcessorsPerChannel) {
    return fail(ErrorCode::kProcessorChainTooLong);
  }
  if (calibrationStage >= static_cast<std::int8_t>(stageCount)) {
    return fail(ErrorCode::kInvalidArgument, "calibration stage out of range");
  }
  slot->stages = stages;
  slot->stageCount = stageCount;
  slot->calibrationStage = calibrationStage;
  return ok();
}

Status ChannelManager::clearPipeline(ChannelHandle handle) {
  Slot* slot = slotFor(handle);
  if (slot == nullptr) return fail(ErrorCode::kChannelNotFound);
  slot->stages = nullptr;
  slot->stageCount = 0;
  slot->calibrationStage = -1;
  return ok();
}

Status ChannelManager::addListener(ChannelListener listener, void* context) {
  if (listener == nullptr) return fail(ErrorCode::kInvalidArgument);
  if (listenerCount_ >= limits::kMaxChannelSubscribers) {
    return fail(ErrorCode::kOutOfCapacity, "channel listener table full");
  }
  listeners_[listenerCount_].fn = listener;
  listeners_[listenerCount_].context = context;
  ++listenerCount_;
  return ok();
}

Status ChannelManager::removeListener(ChannelListener listener, void* context) {
  for (std::size_t i = 0; i < listenerCount_; ++i) {
    if (listeners_[i].fn == listener && listeners_[i].context == context) {
      listeners_[i] = listeners_[listenerCount_ - 1];
      --listenerCount_;
      return ok();
    }
  }
  return fail(ErrorCode::kNotFound, "listener not registered");
}

void ChannelManager::tick(Micros now) {
  for (std::size_t i = 0; i < limits::kMaxChannels; ++i) {
    Slot& slot = slots_[i];
    if (!slot.active) continue;
    const Micros expected = slot.descriptor.expectedIntervalUs;
    if (expected == 0) continue;
    if (slot.value.quality == ChannelQuality::kUnknown ||
        slot.value.quality == ChannelQuality::kFaulted ||
        slot.value.quality == ChannelQuality::kStale) {
      continue;
    }
    if (now > slot.value.timestampUs &&
        (now - slot.value.timestampUs) > expected * kStaleIntervalMultiplier) {
      slot.value.quality = ChannelQuality::kStale;
      // Listeners MUST be told.  Going stale means samples stopped arriving, so
      // there will be no next sample to carry the news: without this the
      // telemetry batcher never marks the channel dirty and the browser keeps
      // painting the last value as if it were fresh — precisely the failure the
      // quality field exists to prevent.
      notify(static_cast<ChannelHandle>(i + 1), slot);
    }
  }
}

Status ChannelManager::setPresentation(ChannelHandle handle, const char* unit,
                                       std::uint8_t precision, float minimum,
                                       float maximum) {
  Slot* slot = slotFor(handle);
  if (slot == nullptr) return fail(ErrorCode::kChannelNotFound);

  if (unit != nullptr) {
    if (!slot->descriptor.unit.assign(unit)) {
      return fail(ErrorCode::kInvalidArgument, "unit is too long");
    }
  }
  if (precision > 0) {
    // Six decimals is already past what a 24-bit converter can justify; more
    // is an invitation to read noise as signal.
    if (precision > 6) return fail(ErrorCode::kInvalidArgument, "precision > 6");
    slot->descriptor.precision = precision;
  }
  slot->descriptor.minimum = minimum;
  slot->descriptor.maximum = maximum;
  return ok();
}

Status ChannelManager::resetPresentation(ChannelHandle handle) {
  Slot* slot = slotFor(handle);
  if (slot == nullptr) return fail(ErrorCode::kChannelNotFound);
  slot->descriptor.unit = slot->declared.unit;
  slot->descriptor.precision = slot->declared.precision;
  slot->descriptor.minimum = slot->declared.minimum;
  slot->descriptor.maximum = slot->declared.maximum;
  return ok();
}

void ChannelManager::setSourceQuality(DeviceHandle device,
                                      ChannelQuality quality) {
  for (std::size_t i = 0; i < limits::kMaxChannels; ++i) {
    Slot& slot = slots_[i];
    if (!slot.active || slot.descriptor.source != device) continue;
    if (slot.value.quality == quality) continue;
    slot.value.quality = quality;
    // Same reasoning as in tick(): a device that just failed has had its poll
    // task disabled, so this is the last chance to tell anyone.
    notify(static_cast<ChannelHandle>(i + 1), slot);
  }
}

}  // namespace lc
