// =============================================================================
//  buses/esp32/Esp32BusProvider.h — the device-side IBusProvider.
//
//  Owns the two I²C controllers, the GPIO port and the ADC port, and is the
//  only place that turns a bus configuration into claimed pins.  A driver
//  therefore cannot start a bus, cannot reconfigure one someone else is using,
//  and cannot drive a pin it did not claim.
// =============================================================================
#pragma once

#include "buses/IBusProvider.h"
#include "buses/esp32/WireI2cBus.h"
#include "core/ResourceManager.h"

namespace lc {
namespace platform {

class Esp32GpioPort final : public IGpioPort {
 public:
  explicit Esp32GpioPort(ResourceManager& resources) : resources_(resources) {}

  Status configure(std::uint8_t pin, PinMode mode, DeviceHandle owner) override;
  Status write(std::uint8_t pin, bool high) override;
  Result<bool> read(std::uint8_t pin) override;
  void delayMicros(std::uint32_t microseconds) const override;

 private:
  // Every call checks that the caller actually owns the pin.  Without this the
  // ResourceManager is advisory, and an advisory resource manager is decoration.
  Status verifyOwnership(std::uint8_t pin, DeviceHandle owner) const;

  ResourceManager& resources_;
};

class Esp32AdcPort final : public IAdcPort {
 public:
  explicit Esp32AdcPort(ResourceManager& resources) : resources_(resources) {}

  Status configure(std::uint8_t pin, AdcAttenuation attenuation,
                   DeviceHandle owner) override;
  Result<std::uint16_t> readRaw(std::uint8_t pin) override;
  Result<float> readMillivolts(std::uint8_t pin) override;
  bool calibrated() const override { return calibrated_; }

 private:
  ResourceManager& resources_;
  bool calibrated_ = false;
};

// LEDC: sixteen channels shared between four timers on the classic ESP32.
// A channel is a resource exactly like a pin, so it is allocated here and the
// driver never learns which number it got.
class Esp32PwmOut final : public IPwmOut {
 public:
  static constexpr std::uint8_t kChannels = 16;
  // 12 bits at 1 kHz, 8 bits above 20 kHz: the LEDC timer trades resolution
  // for frequency, and asking for more of both than the peripheral can give
  // silently returns a duty that is not the one requested.
  static std::uint8_t resolutionFor(std::uint32_t frequencyHz);

  explicit Esp32PwmOut(ResourceManager& resources) : resources_(resources) {}

  Result<PwmChannel> attach(std::uint8_t pin, std::uint32_t frequencyHz,
                            DeviceHandle owner) override;
  Status write(PwmChannel channel, float duty) override;
  void detach(PwmChannel channel) override;
  std::uint8_t channelCount() const override { return kChannels; }
  std::uint8_t channelsInUse() const override { return inUse_; }

 private:
  ResourceManager& resources_;
  bool used_[kChannels] = {false};
  std::uint8_t pins_[kChannels] = {0};
  std::uint8_t bits_[kChannels] = {0};
  std::uint8_t inUse_ = 0;
};

class Esp32BusProvider final : public IBusProvider, public IBusConfigurator {
 public:
  explicit Esp32BusProvider(ResourceManager& resources)
      : resources_(resources), gpio_(resources), adc_(resources), pwm_(resources) {}

  // Claims SDA/SCL for the system and starts the controller.  Called by
  // SystemManager from the "buses" section of system.json.
  Status configureI2c(std::uint8_t index, std::uint8_t sda, std::uint8_t scl,
                      std::uint32_t frequency) override;

  IPwmOut* pwm() override { return &pwm_; }

  II2cBus* i2c(std::uint8_t index) override;
  std::uint8_t i2cBusCount() const override { return kI2cBusCount; }
  IGpioPort* gpio() override { return &gpio_; }
  IAdcPort* adc() override { return &adc_; }

 private:
  static constexpr std::uint8_t kI2cBusCount = 2;

  ResourceManager& resources_;
  Esp32GpioPort gpio_;
  Esp32AdcPort adc_;
  Esp32PwmOut pwm_;
  WireI2cBus buses_[kI2cBusCount]{{Wire, 0}, {Wire1, 1}};
};

}  // namespace platform
}  // namespace lc
