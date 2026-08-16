#include "services/ProcessingManager.h"

#include <cstring>

namespace lc {

ProcessingManager::ProcessingManager(ModuleRegistry& registry,
                                     ChannelManager& channels)
    : registry_(registry), channels_(channels) {}

ProcessingManager::~ProcessingManager() { clearAll(); }

Status ProcessingManager::begin() {
  return channels_.addLifecycleListener(lifecycleTrampoline, this);
}

void ProcessingManager::lifecycleTrampoline(ChannelHandle handle, bool created,
                                            void* context) {
  if (created) return;
  static_cast<ProcessingManager*>(context)->clear(handle);
}

void ProcessingManager::destroy(Chain& chain) {
  for (std::uint8_t i = 0; i < chain.count; ++i) {
    delete chain.stages[i];
    chain.stages[i] = nullptr;
  }
  chain.count = 0;
  chain.calibrationStage = -1;
}

Status ProcessingManager::apply(ChannelHandle handle,
                                const IPipelineSource& source) {
  if (handle == kInvalidChannel || handle > limits::kMaxChannels) {
    return fail(ErrorCode::kChannelNotFound);
  }
  if (!channels_.exists(handle)) return fail(ErrorCode::kChannelNotFound);

  const std::size_t count = source.stageCount();
  if (count > limits::kMaxProcessorsPerChannel) {
    return fail(ErrorCode::kProcessorChainTooLong);
  }

  // Build the whole chain into a scratch structure first.  If stage 4 has a bad
  // window size we throw the scratch away and the channel keeps working with
  // its previous pipeline.
  Chain built;
  for (std::size_t i = 0; i < count; ++i) {
    const char* typeId = source.stageType(i);
    const ModuleDescriptor* descriptor = registry_.findById(typeId);
    if (descriptor == nullptr || descriptor->createProcessor == nullptr) {
      destroy(built);
      return fail(ErrorCode::kDriverNotRegistered,
                  typeId != nullptr ? typeId : "processor");
    }

    IProcessor* stage = descriptor->createProcessor();
    if (stage == nullptr) {
      destroy(built);
      return fail(ErrorCode::kInternal, "processor factory returned null");
    }
    built.stages[built.count++] = stage;

    const Status configured = stage->configure(source.stageConfig(i));
    if (!configured.ok()) {
      destroy(built);
      return configured;
    }
  }

  std::int8_t calibrationStage = source.calibrationStage();
  if (calibrationStage == IPipelineSource::kAutoCalibrationStage) {
    calibrationStage = -1;
    for (std::uint8_t i = 0; i < built.count; ++i) {
      if (std::strcmp(built.stages[i]->typeId(), "calibration") == 0) {
        calibrationStage = static_cast<std::int8_t>(i);
        break;
      }
    }
  }
  if (calibrationStage >= static_cast<std::int8_t>(built.count)) {
    destroy(built);
    return fail(ErrorCode::kInvalidArgument, "calibration stage out of range");
  }
  built.calibrationStage = calibrationStage;

  // Detach the old chain before destroying it: ChannelManager must never hold
  // a pointer to a deleted processor, not even for one statement.
  channels_.clearPipeline(handle);
  Chain& slot = chains_[handle - 1];
  destroy(slot);
  slot = built;

  if (slot.count == 0) return ok();

  const Status installed = channels_.setPipeline(
      handle, slot.stages, slot.count, slot.calibrationStage);
  if (!installed.ok()) {
    destroy(slot);
    return installed;
  }
  return ok();
}

Status ProcessingManager::clear(ChannelHandle handle) {
  if (handle == kInvalidChannel || handle > limits::kMaxChannels) {
    return fail(ErrorCode::kChannelNotFound);
  }
  channels_.clearPipeline(handle);
  destroy(chains_[handle - 1]);
  return ok();
}

void ProcessingManager::clearAll() {
  for (std::size_t i = 0; i < limits::kMaxChannels; ++i) {
    if (chains_[i].count == 0) continue;
    channels_.clearPipeline(static_cast<ChannelHandle>(i + 1));
    destroy(chains_[i]);
  }
}

std::size_t ProcessingManager::stageCount(ChannelHandle handle) const {
  if (handle == kInvalidChannel || handle > limits::kMaxChannels) return 0;
  return chains_[handle - 1].count;
}

const char* ProcessingManager::stageType(ChannelHandle handle,
                                         std::size_t index) const {
  if (handle == kInvalidChannel || handle > limits::kMaxChannels) return nullptr;
  const Chain& chain = chains_[handle - 1];
  if (index >= chain.count || chain.stages[index] == nullptr) return nullptr;
  return chain.stages[index]->typeId();
}

std::size_t ProcessingManager::totalStages() const {
  std::size_t total = 0;
  for (std::size_t i = 0; i < limits::kMaxChannels; ++i) total += chains_[i].count;
  return total;
}

}  // namespace lc
