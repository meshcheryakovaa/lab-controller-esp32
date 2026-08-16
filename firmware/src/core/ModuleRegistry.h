// =============================================================================
//  core/ModuleRegistry.h — the catalogue of module TYPES compiled into the
//  firmware (§6).
//
//  Types are static; instances are created by the user at runtime.  The
//  registry is the bridge: it maps a manifest id from a stored configuration
//  ("hx711") to the factory that can build the object.
//
//  REGISTRATION IS EXPLICIT, not static-initialiser magic.  A single
//  registerBuiltinModules() in modules/BuiltinModules.cpp lists every module.
//  Self-registering globals are tempting, but on an archived static-library
//  build the linker is entitled to discard translation units nothing refers
//  to, and the failure mode ("HX711 disappeared in the release build only") is
//  miserable to debug.  One explicit list costs nothing and is greppable.
// =============================================================================
#pragma once

#include "core/Error.h"
#include "core/IModule.h"
#include "core/ModuleManifest.h"

namespace lc {

using DeviceFactory = IDevice* (*)();
using ProcessorFactory = IProcessor* (*)();
using ControllerFactory = IController* (*)();

struct ModuleDescriptor {
  const ModuleManifest* manifest = nullptr;
  DeviceFactory createDevice = nullptr;
  ProcessorFactory createProcessor = nullptr;
  ControllerFactory createController = nullptr;
};

class ModuleRegistry {
 public:
  Status add(const ModuleDescriptor& descriptor);

  const ModuleDescriptor* findById(const char* id) const;
  const ModuleManifest* manifestById(const char* id) const;

  std::size_t size() const { return count_; }
  const ModuleDescriptor& at(std::size_t index) const { return items_[index]; }
  std::size_t countByCategory(ModuleCategory category) const;

  // Process-wide catalogue.  A singleton is justified here: it is immutable
  // after start-up and every subsystem needs it.
  static ModuleRegistry& instance();

 private:
  ModuleDescriptor items_[limits::kMaxModuleTypes];
  std::size_t count_ = 0;
};

// Implemented in modules/BuiltinModules.cpp.
void registerBuiltinModules(ModuleRegistry& registry);

}  // namespace lc
