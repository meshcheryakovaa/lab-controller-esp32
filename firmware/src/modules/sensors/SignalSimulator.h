// =============================================================================
//  modules/sensors/SignalSimulator.h — software signal source (§59).
//
//  Not a toy.  This is the module that lets dashboards, PID tuning, the rule
//  engine and experiment scripts be developed and regression-tested with no
//  hardware attached, and it doubles as the reference implementation of the
//  IDevice contract: manifest, configure/begin/poll/end, resource claiming
//  (none), channel publishing.
//
//  Deliberately free of Arduino includes so it compiles in the host test build.
// =============================================================================
#pragma once

#include "core/IModule.h"
#include "services/ChannelManager.h"

namespace lc {
namespace modules {

class SignalSimulator final : public IDevice {
 public:
  enum class Waveform : std::uint8_t {
    kSine = 0,
    kRamp,
    kSquare,
    kTriangle,
    kConstant,
    kRandomWalk,
  };

  static const ModuleManifest& manifest();
  static IDevice* create() { return new SignalSimulator(); }

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;

  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }
  Status selfTest() override { return ok(); }

  // Exposed for unit tests: the value the module would emit at time `t`.
  float evaluate(Micros t);

 private:
  DeviceContext ctx_{};
  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};

  Waveform waveform_ = Waveform::kSine;
  float amplitude_ = 1.0f;
  float offset_ = 0.0f;
  float periodSeconds_ = 10.0f;
  float noise_ = 0.0f;
  Micros startedAtUs_ = 0;
  float walkState_ = 0.0f;

  std::uint32_t rngState_ = 0x1234ABCDu;
  float nextNoise();  // uniform in [-1, 1]
};

}  // namespace modules
}  // namespace lc
