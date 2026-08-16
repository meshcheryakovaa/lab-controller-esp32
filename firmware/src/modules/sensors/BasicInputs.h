// =============================================================================
//  modules/sensors/BasicInputs.h — analogue and digital input.
//
//  The two least glamorous modules in the catalogue and among the most used:
//  a thermistor divider, a 4–20 mA loop through a shunt, a limit switch, a lid
//  interlock.  Everything downstream — calibration, virtual channels, rules —
//  works on them exactly as it does on a BMP280, which is the whole point of
//  the channel abstraction.
// =============================================================================
#pragma once

#include "buses/IBusProvider.h"
#include "core/IModule.h"
#include "services/ChannelManager.h"

namespace lc {
namespace modules {

class AnalogInputDriver final : public IDevice {
 public:
  static constexpr std::uint8_t kMaxAverage = 64;

  static const ModuleManifest& manifest();
  static IDevice* create() { return new AnalogInputDriver(); }

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;
  Status selfTest() override;

  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

 private:
  DeviceContext ctx_{};
  IAdcPort* adc_ = nullptr;
  std::uint8_t pin_ = 0;
  std::uint8_t samples_ = 8;
  bool reportMillivolts_ = true;
  AdcAttenuation attenuation_ = AdcAttenuation::kDb11;

  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
};

class DigitalInputDriver final : public IDevice {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new DigitalInputDriver(); }

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;

  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

 private:
  DeviceContext ctx_{};
  IGpioPort* gpio_ = nullptr;
  std::uint8_t pin_ = 0;
  PinMode mode_ = PinMode::kInputPullup;
  bool invert_ = false;
  Micros debounceUs_ = 20000;

  // Debounce state: a candidate level has to hold for the whole window before
  // it is published.  Without it a mechanical switch produces a dozen channel
  // updates per press, and any rule watching it fires a dozen times.
  bool stableLevel_ = false;
  bool candidateLevel_ = false;
  Micros candidateSinceUs_ = 0;
  bool published_ = false;

  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
};

}  // namespace modules
}  // namespace lc
