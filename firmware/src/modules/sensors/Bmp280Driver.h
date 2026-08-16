// =============================================================================
//  modules/sensors/Bmp280Driver.h — BMP280 pressure and temperature over I²C.
//
//  The compensation arithmetic is the interesting part.  Bosch publishes an
//  integer implementation (fast, what ships) and a floating-point one
//  (readable, what the datasheet explains).  This file implements the integer
//  version; the test implements the floating-point version independently and
//  asserts the two agree to within 0.01 °C and 1 Pa across the whole ADC range.
//  Two structurally different algorithms agreeing is a real check — far
//  stronger than asserting a few remembered constants.
//
//  Everything below the driver is pure: give it 24 calibration bytes and a raw
//  reading and it produces a number, with no bus and no clock in sight.
// =============================================================================
#pragma once

#include "buses/IBusProvider.h"
#include "core/IModule.h"
#include "services/ChannelManager.h"

namespace lc {
namespace modules {

struct Bmp280Calibration {
  std::uint16_t t1 = 0;
  std::int16_t t2 = 0, t3 = 0;
  std::uint16_t p1 = 0;
  std::int16_t p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0, p7 = 0, p8 = 0, p9 = 0;

  // `raw` is the 24 bytes read from register 0x88, little-endian pairs.
  static Bmp280Calibration parse(const std::uint8_t raw[24]);
  bool plausible() const;
};

class Bmp280Protocol {
 public:
  static constexpr std::uint8_t kRegCalibration = 0x88;
  static constexpr std::uint8_t kRegChipId = 0xD0;
  static constexpr std::uint8_t kRegReset = 0xE0;
  static constexpr std::uint8_t kRegStatus = 0xF3;
  static constexpr std::uint8_t kRegCtrlMeas = 0xF4;
  static constexpr std::uint8_t kRegConfig = 0xF5;
  static constexpr std::uint8_t kRegData = 0xF7;
  static constexpr std::uint8_t kChipIdBmp280 = 0x58;
  static constexpr std::uint8_t kChipIdBme280 = 0x60;
  static constexpr std::uint8_t kResetMagic = 0xB6;

  // Temperature in hundredths of a degree Celsius.  `tFine` is the shared
  // intermediate the pressure compensation also needs.
  static std::int32_t compensateTemperature(std::int32_t adcTemperature,
                                            const Bmp280Calibration& calibration,
                                            std::int32_t& tFine);

  // Pressure in Q24.8 pascals (divide by 256 for Pa).  Returns 0 when the
  // calibration makes the result undefined, which the driver reports rather
  // than publishing a zero as if it were a measurement.
  static std::uint32_t compensatePressure(std::int32_t adcPressure,
                                          const Bmp280Calibration& calibration,
                                          std::int32_t tFine);

  // Unpacks the six data registers (0xF7..0xFC) into 20-bit raw readings.
  static void decodeRaw(const std::uint8_t raw[6], std::int32_t& adcPressure,
                        std::int32_t& adcTemperature);
};

class Bmp280Driver final : public IDevice {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new Bmp280Driver(); }

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;
  Status selfTest() override;

  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

  const Bmp280Calibration& calibration() const { return calibration_; }

 private:
  void fault(const Error& error);

  DeviceContext ctx_{};
  II2cBus* bus_ = nullptr;
  std::uint8_t address_ = 0x76;
  std::uint8_t oversamplingTemperature_ = 2;  // x2
  std::uint8_t oversamplingPressure_ = 5;     // x16
  std::uint8_t filter_ = 4;                   // coefficient 16

  Bmp280Calibration calibration_;
  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
  std::uint8_t consecutiveFailures_ = 0;
};

}  // namespace modules
}  // namespace lc
