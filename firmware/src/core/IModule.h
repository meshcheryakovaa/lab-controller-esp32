// =============================================================================
//  core/IModule.h — the contracts every runtime object implements.
//
//  Lifecycle of a Device (mirrors DeviceState):
//
//     [ configuration submitted ]
//               |
//               v
//     validate(manifest, config)         <- pure, no hardware touched
//               |
//               v
//     configure(ctx)   claims resources  --(error)--> ERROR (resources rolled back)
//               |                                        ^
//               v                                        |
//          CONFIGURED                                    |
//               |  begin()                               |
//               v                                        |
//        INITIALIZING  --(retry over several ticks)------+
//               |
//               v
//            RUNNING  <---> WARNING
//               |
//               |  end()
//               v
//           DISABLED
//
//  Hard rules for implementers:
//    * poll() must never block.  No delay(), no busy-wait longer than a few
//      microseconds, no while(!ready).  If the part needs 80 ms to convert,
//      remember the deadline and return.
//    * begin() may also be re-entered: return kTimeout to be called again on
//      the next tick rather than spinning.
//    * a driver never creates its own channels and never touches GPIO it did
//      not claim through the context's ResourceManager.
// =============================================================================
#pragma once

#include "core/Clock.h"
#include "core/ConfigView.h"
#include "core/Error.h"
#include "core/EventBus.h"
#include "core/ModuleManifest.h"
#include "core/ResourceManager.h"
#include "core/Types.h"

namespace lc {

class ChannelManager;
class IOutputDevice;
// Defined in buses/IBusProvider.h.  Forward-declared here so core/ keeps its
// "depends on nothing" property: DeviceContext only ever holds a pointer.
class IBusProvider;

// Everything a module is allowed to reach.  Passed by const reference; the
// module stores it (it is valid for the module's whole lifetime).
struct DeviceContext {
  DeviceHandle self = kInvalidDevice;
  const ModuleManifest* manifest = nullptr;
  const IConfigView* config = nullptr;
  const IClock* clock = nullptr;
  ResourceManager* resources = nullptr;
  ChannelManager* channels = nullptr;
  EventBus* events = nullptr;

  // I²C / GPIO / ADC access.  nullptr on a system with no bus layer wired up
  // (the software modules do not need one), so drivers must check.
  IBusProvider* buses = nullptr;

  // Channels pre-created by DeviceManager from manifest->channels, in the same
  // order.  A driver publishes with channelHandles[i].
  const ChannelHandle* channelHandles = nullptr;
  std::uint8_t channelCount = 0;

  // OUTPUTS: the safe value of each channel, ALREADY RESOLVED from the manifest
  // and the stored configuration.  A driver must read it from here and must
  // never re-derive it from `config`: the safety layer was handed this exact
  // number, and two readers of one value are two chances to disagree about what
  // "safe" means (ADR-0016).
  const float* channelSafeValues = nullptr;
};

// ---------------------------------------------------------------------------
//  IDevice — a configured instance of a hardware module.
// ---------------------------------------------------------------------------
class IDevice {
 public:
  virtual ~IDevice() = default;

  // Claim GPIO/bus resources and pre-compute whatever the driver needs.
  // Must not talk to the hardware yet.  On failure, DeviceManager calls
  // resources.releaseAllOwnedBy(self) — do not attempt manual rollback.
  virtual Status configure(const DeviceContext& context) = 0;

  // Talk to the part: probe, reset, set resolution, etc.
  // Return kTimeout to be called again on the next scheduler tick.
  virtual Status begin() = 0;

  // Called at the device's acquisition rate.  Non-blocking.
  virtual void poll(Micros now) = 0;

  // Release hardware (not resources — DeviceManager does that).
  virtual void end() = 0;

  virtual DeviceState state() const = 0;
  virtual const Error& lastError() const = 0;

  // Optional: a self-test the Diagnostics page can trigger (§58).
  virtual Status selfTest() { return fail(ErrorCode::kNotSupported); }

  // Downcast to the output interface without RTTI (exceptions and RTTI are
  // disabled in the firmware build).  IOutputDevice overrides this to return
  // `this`; everything else keeps the nullptr default.  ChannelManager's output
  // sink uses it to route a write to the right driver.
  virtual IOutputDevice* asOutput() { return nullptr; }
};

// ---------------------------------------------------------------------------
//  IOutputDevice — anything the system can command (§27).
// ---------------------------------------------------------------------------
class IOutputDevice : public IDevice {
 public:
  // `value` is in the channel's declared unit (%, °C, PWM duty, 0/1 ...).
  // `applied` receives what the driver actually did with it: a power limit, a
  // minimum running duty or a kick-start all mean the two differ, and the
  // channel must report the second, not the first.
  virtual Status write(ChannelHandle channel, float value, float* applied) = 0;

  // Driven to the configured fail-safe state.  Must be safe to call from the
  // SafetyManager at any moment, including from an error path.
  virtual void failSafe() = 0;

  IOutputDevice* asOutput() final { return this; }
};

// ---------------------------------------------------------------------------
//  IProcessor — one stage of a channel's processing pipeline (§14).
//  Stateful, cheap, and strictly single-input/single-output.
// ---------------------------------------------------------------------------
class IProcessor {
 public:
  virtual ~IProcessor() = default;
  virtual const char* typeId() const = 0;
  virtual Status configure(const IConfigView& config) = 0;

  // `valid` starts true; a stage may set it false to suppress the sample
  // (a median filter that has not filled its window yet, for example).
  virtual float process(float input, Micros now, bool& valid) = 0;

  virtual void reset() = 0;
};

// ---------------------------------------------------------------------------
//  IController — PID, thermostat, threshold, timer, state machine (§28).
// ---------------------------------------------------------------------------
class IController {
 public:
  virtual ~IController() = default;
  virtual const char* typeId() const = 0;
  virtual Status configure(const IConfigView& config, ChannelManager& channels) = 0;
  virtual void update(Micros now) = 0;
  virtual void setEnabled(bool enabled) = 0;
  virtual bool enabled() const = 0;
};

}  // namespace lc
