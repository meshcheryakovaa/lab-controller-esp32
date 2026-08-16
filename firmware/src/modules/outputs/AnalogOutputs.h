// =============================================================================
//  modules/outputs/AnalogOutputs.h — PWM, and the two things people drive with it.
//
//  PWM OUTPUT is the general case: a duty cycle in per cent on a hardware
//  channel, with a declared safe duty.
//
//  HEATER is PWM with the two constraints that matter on a heater and nowhere
//  else.  Its safe state is fixed at 0 % and cannot be configured to anything
//  else — a "safe" heater state of 40 % is not a configuration, it is an
//  accident waiting for a power cut.  And it takes a MAXIMUM DUTY: the single
//  most useful safety parameter on a laboratory heater, because it converts
//  "the control loop went mad" from a fire into a slow warm-up.  The command
//  deadline is mandatory too, with a bounded maximum.
//
//  FAN is PWM with a minimum running duty and a kick-start.  Below roughly a
//  fifth of full power most fans stall: they draw current, report nothing, and
//  move no air — which on a cooling fan reads exactly like "cooling is on".
// =============================================================================
#pragma once

#include "buses/IBusProvider.h"
#include "core/IModule.h"
#include "services/ChannelManager.h"

namespace lc {
namespace modules {

class PwmOutputBase : public IOutputDevice {
 public:
  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;

  Status write(ChannelHandle channel, float percent, float* applied) override;
  void failSafe() override;

  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

  float duty() const { return duty_; }

 protected:
  // Hook for the constraints each kind of load brings with it.  Returns the
  // per cent actually to be applied, or an error to refuse the command.
  virtual Result<float> shape(float percent, Micros now);

  Status applyPercent(float percent);

  DeviceContext ctx_{};
  IPwmOut* pwm_ = nullptr;
  IGpioPort* gpio_ = nullptr;
  PwmChannel channel_ = kInvalidPwmChannel;
  std::uint8_t pin_ = 0;
  std::uint32_t frequencyHz_ = 1000;
  bool invert_ = false;
  float safePercent_ = 0.0f;
  float duty_ = 0.0f;

  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
};

class PwmOutputDriver final : public PwmOutputBase {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new PwmOutputDriver(); }
};

class HeaterDriver final : public PwmOutputBase {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new HeaterDriver(); }

  Status configure(const DeviceContext& context) override;
  float maximumDuty() const { return maxPercent_; }
  std::uint32_t limitedCommands() const { return limited_; }

 protected:
  Result<float> shape(float percent, Micros now) override;

 private:
  float maxPercent_ = 100.0f;
  std::uint32_t limited_ = 0;
};

class FanDriver final : public PwmOutputBase {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new FanDriver(); }

  Status configure(const DeviceContext& context) override;

 protected:
  Result<float> shape(float percent, Micros now) override;

 private:
  float minRunPercent_ = 20.0f;
  Micros kickstartUs_ = 400000;
  Micros kickstartUntilUs_ = 0;
  bool spinning_ = false;
};

}  // namespace modules
}  // namespace lc
