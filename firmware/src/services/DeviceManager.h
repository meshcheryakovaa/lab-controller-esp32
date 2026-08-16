// =============================================================================
//  services/DeviceManager.h — owns Device instances and their lifecycle.
//
//  This is where "the user adds a sensor in a browser" actually happens:
//
//     validate(manifest, config)      pure, touches no hardware
//        → factory()                  driver object exists
//        → create channels            from the manifest's ChannelSpec[]
//        → configure(ctx)             driver claims GPIO / I²C addresses
//        → begin()                    driver talks to the part (retryable)
//        → schedule poll()            at the device's acquisition rate
//
//  Every failure path unwinds completely: the driver is deleted, its channels
//  are removed, and `resources.releaseAllOwnedBy(handle)` drops every claim the
//  driver managed to take.  A half-added device leaving GPIO21 busy forever is
//  the single most common bug in systems like this, and it is impossible here.
//
//  Handles are stable: `handle == index + 1` into a fixed array, and removal
//  leaves a hole rather than renumbering.  Scheduler callbacks therefore hold a
//  DeviceRecord* that stays valid.
// =============================================================================
#pragma once

#include "buses/IBusProvider.h"
#include "core/Clock.h"
#include "core/ConfigView.h"
#include "core/Error.h"
#include "core/EventBus.h"
#include "core/ModuleRegistry.h"
#include "core/ResourceManager.h"
#include "core/Scheduler.h"
#include "services/ChannelManager.h"
#include "services/OutputManager.h"

namespace lc {

// Per-channel user overrides.  Anything left empty falls back to the manifest,
// so a stored configuration only carries what the user actually changed.
struct ChannelOverride {
  FixedString<16> specId;  // which ChannelSpec this applies to ("temperature")
  KeyString key;           // empty -> "<deviceKey>.<specId>"
  NameString name;
  UnitString unit;
  float minimum = 0.0f;
  float maximum = 0.0f;
  bool hasRange = false;
  std::uint8_t precision = 0xFF;  // 0xFF -> from manifest
  std::uint32_t color = 0;        // 0 -> palette default
  bool logged = true;
  bool visible = true;
  bool hasGeometry = false;
  Geometry geometry;
};

struct DeviceSpec {
  KeyString key;    // "hx711_01" — stable, referenced by configuration
  NameString name;  // "Sample balance"
  bool enabled = true;
  Micros sampleIntervalUs = 0;  // 0 -> manifest default
  Geometry geometry;

  static constexpr std::size_t kMaxChannelsPerDevice = 8;
  ChannelOverride overrides[kMaxChannelsPerDevice];
  std::uint8_t overrideCount = 0;

  const ChannelOverride* findOverride(const char* specId) const;
};

class DeviceManager;

struct DeviceRecord {
  bool active = false;
  // The scheduler hands a callback nothing but a void*, and a driver that
  // faults mid-poll has to be able to reach the manager to be noticed.
  DeviceManager* owner = nullptr;
  // The scheduler callback receives only a void*, so the record carries the
  // clock it needs to timestamp samples.  Cheaper and safer than a second
  // indirection through the manager.
  const IClock* clock = nullptr;
  DeviceHandle handle = kInvalidDevice;
  KeyString key;
  NameString name;
  const ModuleManifest* manifest = nullptr;
  IDevice* instance = nullptr;
  DeviceState state = DeviceState::kDisabled;
  Error lastError;
  Micros sampleIntervalUs = 0;
  TaskId pollTask = kInvalidTask;
  ChannelHandle channels[DeviceSpec::kMaxChannelsPerDevice] = {kInvalidChannel};
  // Resolved once, in createChannels, and read by BOTH the driver (through
  // DeviceContext) and the safety layer.  One number, one resolution.
  float channelSafeValues[DeviceSpec::kMaxChannelsPerDevice] = {0.0f};
  std::uint8_t channelCount = 0;
  Geometry geometry;

  // Consecutive begin() retries; after the limit the device goes to ERROR
  // instead of retrying forever and hiding a wiring fault.
  std::uint8_t initAttempts = 0;

  // Set while the record is being torn down, so the poll callback that may
  // still be queued in this scheduler pass does nothing.
  bool tearingDown = false;
};

class DeviceManager {
 public:
  static constexpr std::uint8_t kMaxInitAttempts = 50;  // ~0.5 s at 10 ms

  DeviceManager(const IClock& clock, ModuleRegistry& registry,
                ResourceManager& resources, ChannelManager& channels,
                Scheduler& scheduler, EventBus& events);
  ~DeviceManager();

  DeviceManager(const DeviceManager&) = delete;
  DeviceManager& operator=(const DeviceManager&) = delete;

  // Registers the retry task and installs the output sink.  Call once.
  Status begin();

  // Optional, and installed rather than consulted: DeviceManager registers
  // every output channel with the safety layer ITSELF, from the manifest and
  // the stored configuration.  A driver therefore cannot forget to declare its
  // safe state, because declaring it was never the driver's job (ADR-0016).
  void setOutputManager(OutputManager* outputs) { outputs_ = outputs; }
  OutputManager* outputManager() const { return outputs_; }

  // Optional: without it, only modules that need no hardware bus can run.
  // Injected rather than taken in the constructor so that the software-only
  // test rigs stay a two-line setup.
  void setBusProvider(IBusProvider* buses) { buses_ = buses; }
  IBusProvider* busProvider() const { return buses_; }

  // --- validation ----------------------------------------------------------
  // Pure: no hardware touched, nothing created.  `offendingField` (optional)
  // receives the configuration key that failed, so the REST layer can point the
  // form at the right input.  `ignoreOwner` lets reconfigure() ignore the claims
  // the device already holds.
  Status validate(const char* moduleId, const IConfigView& config,
                  LabelString* offendingField = nullptr,
                  DeviceHandle ignoreOwner = kInvalidDevice) const;

  // --- lifecycle -----------------------------------------------------------
  Result<DeviceHandle> add(const char* moduleId, const DeviceSpec& spec,
                           const IConfigView& config,
                           LabelString* offendingField = nullptr);
  Status remove(DeviceHandle handle);
  Status setEnabled(DeviceHandle handle, bool enabled);

  // Full replace: tears the device down and rebuilds it under the same key.
  // On failure the old device is NOT restored — the caller re-applies the
  // stored configuration.  Documented rather than silently half-done.
  Status reconfigure(DeviceHandle handle, const char* moduleId,
                     const DeviceSpec& spec, const IConfigView& config,
                     LabelString* offendingField = nullptr);

  void removeAll();

  // --- lookup --------------------------------------------------------------
  const DeviceRecord* find(DeviceHandle handle) const;
  const DeviceRecord* findByKey(const char* key) const;
  DeviceHandle handleForChannel(ChannelHandle channel) const;
  std::size_t activeCount() const { return activeCount_; }
  static constexpr std::size_t capacity() { return limits::kMaxDevices; }
  const DeviceRecord& slot(std::size_t index) const { return records_[index]; }

  // --- diagnostics ---------------------------------------------------------
  std::size_t countInState(DeviceState state) const;

 private:
  Status validateParam(const ParamSpec& param, const IConfigView& config,
                       DeviceHandle ignoreOwner, std::uint8_t* pinsSeen,
                       std::uint8_t& pinsSeenCount, LabelString* field) const;

  DeviceRecord* mutableFind(DeviceHandle handle);
  DeviceRecord* allocate();
  void teardown(DeviceRecord& record);
  Status createChannels(DeviceRecord& record, const DeviceSpec& spec);
  Status resolveSafeValues(DeviceRecord& record, const IConfigView& config);
  Status registerOutputs(DeviceRecord& record, const IConfigView& config);
  void setState(DeviceRecord& record, DeviceState state, const Error& error);
  void tryBegin(DeviceRecord& record);

  // A driver changes its own state during poll() — it goes to WARNING after a
  // bad CRC, to ERROR after a cable falls off.  Without this, that change
  // stayed inside the driver object: the record still said RUNNING, the API
  // reported RUNNING, the dashboard showed a healthy sensor, and the channels
  // kept their last value looking fresh.  Every poll ends by reconciling.
  void syncDriverState(DeviceRecord& record);

  static void pollTrampoline(void* context);
  static void retryTrampoline(void* context);
  static Status outputSinkTrampoline(ChannelHandle handle, float value,
                                     float* applied, void* context);

  const IClock& clock_;
  ModuleRegistry& registry_;
  ResourceManager& resources_;
  ChannelManager& channels_;
  Scheduler& scheduler_;
  EventBus& events_;
  OutputManager* outputs_ = nullptr;

  IBusProvider* buses_ = nullptr;
  DeviceRecord records_[limits::kMaxDevices];
  std::size_t activeCount_ = 0;
  TaskId retryTask_ = kInvalidTask;
  bool started_ = false;
};

}  // namespace lc
