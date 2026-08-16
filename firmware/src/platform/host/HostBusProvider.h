// =============================================================================
//  platform/host/HostBusProvider.h — a board with nothing wired to it.
//
//  Used by tools/host_server so the web interface can be developed against the
//  real firmware API on a PC.  It deliberately behaves like an ESP32 with no
//  sensors attached rather than pretending everything works:
//
//    * GPIO inputs read HIGH  → an HX711 added here really does end up in
//      ERROR with "DOUT never went low; check wiring and power", which is
//      exactly what the operator would see with a loose cable;
//    * the I²C scan finds nothing  → the scan panel shows its empty state;
//    * the ADC returns mid-scale.
//
//  Faking working hardware here would make the UI look good and teach nothing.
//  The simulator modules already exist for producing believable data.
// =============================================================================
#pragma once

#include <chrono>
#include <map>
#include <thread>

#include "buses/IBusProvider.h"

namespace lc {
namespace platform {

class HostGpioPort final : public IGpioPort {
 public:
  Status configure(std::uint8_t pin, PinMode mode, DeviceHandle) override {
    modes_[pin] = mode;
    return ok();
  }
  Status write(std::uint8_t pin, bool high) override {
    levels_[pin] = high;
    return ok();
  }
  Result<bool> read(std::uint8_t pin) override {
    const auto it = levels_.find(pin);
    // Nothing connected: an input with a pull-up floats high.
    return it == levels_.end() ? true : it->second;
  }
  void delayMicros(std::uint32_t microseconds) const override {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
  }

 private:
  std::map<std::uint8_t, PinMode> modes_;
  std::map<std::uint8_t, bool> levels_;
};

// A PWM peripheral that accepts duty cycles and remembers them.  Unlike the
// input ports above this does NOT pretend nothing is connected — there is
// nothing to pretend about.  An actuator on a PC simply has nowhere to act,
// and recording the commanded duty is what lets the output controls, the
// safety layer and the command deadline be exercised without a board.
class HostPwmOut final : public IPwmOut {
 public:
  static constexpr std::uint8_t kChannels = 16;

  Result<PwmChannel> attach(std::uint8_t pin, std::uint32_t frequencyHz,
                            DeviceHandle) override {
    for (std::uint8_t i = 0; i < kChannels; ++i) {
      if (used_[i]) continue;
      used_[i] = true;
      pins_[i] = pin;
      frequencies_[i] = frequencyHz;
      duties_[i] = 0.0f;
      ++inUse_;
      return i;
    }
    return fail(ErrorCode::kPwmChannelExhausted, "no free PWM channel");
  }

  Status write(PwmChannel channel, float duty) override {
    if (channel >= kChannels || !used_[channel]) {
      return fail(ErrorCode::kInvalidState, "PWM channel is not attached");
    }
    duties_[channel] = duty < 0.0f ? 0.0f : (duty > 1.0f ? 1.0f : duty);
    return ok();
  }

  void detach(PwmChannel channel) override {
    if (channel >= kChannels || !used_[channel]) return;
    used_[channel] = false;
    if (inUse_ > 0) --inUse_;
  }

  std::uint8_t channelCount() const override { return kChannels; }
  std::uint8_t channelsInUse() const override { return inUse_; }

  float dutyOf(PwmChannel channel) const {
    return (channel < kChannels && used_[channel]) ? duties_[channel] : 0.0f;
  }

 private:
  bool used_[kChannels] = {false};
  std::uint8_t pins_[kChannels] = {0};
  std::uint32_t frequencies_[kChannels] = {0};
  float duties_[kChannels] = {0.0f};
  std::uint8_t inUse_ = 0;
};

class HostAdcPort final : public IAdcPort {
 public:
  Status configure(std::uint8_t, AdcAttenuation, DeviceHandle) override { return ok(); }
  Result<std::uint16_t> readRaw(std::uint8_t) override { return std::uint16_t{2048}; }
  Result<float> readMillivolts(std::uint8_t) override { return 1225.0f; }
  // Honest: a PC has no eFuse calibration data, and the driver should say so.
  bool calibrated() const override { return false; }
};

class HostI2cBus final : public II2cBus {
 public:
  std::uint8_t index() const override { return 0; }
  bool probe(std::uint8_t) override { return false; }
  Status write(std::uint8_t, const std::uint8_t*, std::size_t) override {
    ++errors_;
    return fail(ErrorCode::kDeviceNotResponding, "no device on this bus");
  }
  Status read(std::uint8_t, std::uint8_t*, std::size_t) override {
    ++errors_;
    return fail(ErrorCode::kDeviceNotResponding, "no device on this bus");
  }
  Status writeRead(std::uint8_t, const std::uint8_t*, std::size_t, std::uint8_t*,
                   std::size_t) override {
    ++errors_;
    return fail(ErrorCode::kDeviceNotResponding, "no device on this bus");
  }
  std::uint32_t errorCount() const override { return errors_; }
  void resetErrorCount() override { errors_ = 0; }

 private:
  std::uint32_t errors_ = 0;
};

class HostBusProvider final : public IBusProvider {
 public:
  II2cBus* i2c(std::uint8_t index) override { return index == 0 ? &bus_ : nullptr; }
  std::uint8_t i2cBusCount() const override { return 1; }
  IGpioPort* gpio() override { return &gpio_; }
  IAdcPort* adc() override { return &adc_; }
  IPwmOut* pwm() override { return &pwm_; }

 private:
  HostI2cBus bus_;
  HostGpioPort gpio_;
  HostAdcPort adc_;
  HostPwmOut pwm_;
};

}  // namespace platform
}  // namespace lc
