// =============================================================================
//  modules/outputs/DigitalOutputs.h — a pin that is on or off, and a relay.
//
//  These are the first modules in the catalogue that CHANGE the rig rather than
//  observe it, and both carry a constraint that a plain digitalWrite() does not.
//
//  DIGITAL OUTPUT is the general case: a pin, optionally inverted, driven to a
//  declared safe level whenever nothing is commanding it.
//
//  RELAY is a digital output plus contact life.  A mechanical relay is good for
//  something like 10^5 switching operations; a control loop that toggles it
//  once a second reaches that in a day and a half, and the failure shows up as
//  a contact that welds itself closed — with the load on.  So a relay enforces
//  a minimum interval between changes and REPORTS the refusal rather than
//  quietly dropping it, because a command that appeared to work and did not is
//  worse than one that was rejected out loud.
// =============================================================================
#pragma once

#include "buses/IBusProvider.h"
#include "core/IModule.h"
#include "services/ChannelManager.h"

namespace lc {
namespace modules {

// Shared behaviour of anything that drives one pin high or low.
class DigitalOutputBase : public IOutputDevice {
 public:
  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;

  Status write(ChannelHandle channel, float value, float* applied) override;
  void failSafe() override;

  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

  bool level() const { return level_; }

 protected:
  // Minimum microseconds between two changes of level; 0 for none.
  virtual Micros minimumSwitchIntervalUs() const { return 0; }
  virtual const char* switchTooFastDetail() const { return "switching too fast"; }

  Status applyLevel(bool level, Micros now);

  DeviceContext ctx_{};
  IGpioPort* gpio_ = nullptr;
  std::uint8_t pin_ = 0;
  bool invert_ = false;
  bool safeLevel_ = false;
  bool level_ = false;
  Micros lastChangeUs_ = 0;
  bool everApplied_ = false;

  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
};

class DigitalOutputDriver final : public DigitalOutputBase {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new DigitalOutputDriver(); }
  const char* typeId() const { return "digital_out"; }
};

class RelayDriver final : public DigitalOutputBase {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new RelayDriver(); }

  Status configure(const DeviceContext& context) override;
  std::uint32_t refusedSwitches() const { return refused_; }

 protected:
  Micros minimumSwitchIntervalUs() const override { return minIntervalUs_; }
  const char* switchTooFastDetail() const override {
    return "relay contacts are protected by a minimum switching interval";
  }

 private:
  Micros minIntervalUs_ = 1000000;  // 1 s
  std::uint32_t refused_ = 0;
};

}  // namespace modules
}  // namespace lc
