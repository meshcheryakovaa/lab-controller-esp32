#include "core/ModuleRegistry.h"

#include <cstring>

namespace lc {

const char* toString(ModuleCategory category) {
  switch (category) {
    case ModuleCategory::kSensor:     return "sensor";
    case ModuleCategory::kOutput:     return "output";
    case ModuleCategory::kProcessing: return "processing";
    case ModuleCategory::kControl:    return "control";
    case ModuleCategory::kVirtual:    return "virtual";
    case ModuleCategory::kSystem:     return "system";
    case ModuleCategory::kCount:      break;
  }
  return "unknown";
}

const ParamSpec* ModuleManifest::findParam(const char* key) const {
  if (key == nullptr || params == nullptr) return nullptr;
  for (std::uint8_t i = 0; i < paramCount; ++i) {
    if (params[i].key != nullptr && std::strcmp(params[i].key, key) == 0) {
      return &params[i];
    }
  }
  return nullptr;
}

const ChannelSpec* ModuleManifest::findChannel(const char* channelId) const {
  if (channelId == nullptr || channels == nullptr) return nullptr;
  for (std::uint8_t i = 0; i < channelCount; ++i) {
    if (channels[i].id != nullptr && std::strcmp(channels[i].id, channelId) == 0) {
      return &channels[i];
    }
  }
  return nullptr;
}

Status ModuleRegistry::add(const ModuleDescriptor& descriptor) {
  if (descriptor.manifest == nullptr || descriptor.manifest->id == nullptr) {
    return fail(ErrorCode::kInvalidArgument, "manifest or manifest id is null");
  }
  if (findById(descriptor.manifest->id) != nullptr) {
    return fail(ErrorCode::kAlreadyExists, descriptor.manifest->id);
  }
  if (count_ >= limits::kMaxModuleTypes) {
    return fail(ErrorCode::kOutOfCapacity, "module registry full");
  }

  // A module must be constructible in exactly one way, matching its category.
  const bool hasFactory = descriptor.createDevice != nullptr ||
                          descriptor.createProcessor != nullptr ||
                          descriptor.createController != nullptr;
  if (!hasFactory) {
    return fail(ErrorCode::kInvalidArgument, "module has no factory");
  }

  items_[count_++] = descriptor;
  return ok();
}

const ModuleDescriptor* ModuleRegistry::findById(const char* id) const {
  if (id == nullptr) return nullptr;
  for (std::size_t i = 0; i < count_; ++i) {
    const ModuleManifest* manifest = items_[i].manifest;
    if (manifest != nullptr && manifest->id != nullptr &&
        std::strcmp(manifest->id, id) == 0) {
      return &items_[i];
    }
  }
  return nullptr;
}

const ModuleManifest* ModuleRegistry::manifestById(const char* id) const {
  const ModuleDescriptor* descriptor = findById(id);
  return (descriptor != nullptr) ? descriptor->manifest : nullptr;
}

std::size_t ModuleRegistry::countByCategory(ModuleCategory category) const {
  std::size_t total = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    if (items_[i].manifest != nullptr && items_[i].manifest->category == category) {
      ++total;
    }
  }
  return total;
}

ModuleRegistry& ModuleRegistry::instance() {
  static ModuleRegistry registry;
  return registry;
}

}  // namespace lc
