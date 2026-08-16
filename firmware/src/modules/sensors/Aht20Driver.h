// =============================================================================
//  modules/sensors/Aht20Driver.h — AHT20 temperature and humidity over I²C.
//
//  Two parts, deliberately separated:
//
//    Aht20Protocol   pure functions: CRC-8, decoding the 7-byte response.
//                    No bus, no clock, no state — unit-tested against byte
//                    sequences taken from the datasheet.
//
//    Aht20Driver     the state machine that talks to the part.  It owns no
//                    timing loop: a conversion takes ~80 ms, so poll() starts
//                    one, notes the deadline, and returns.  Nothing blocks.
//
//  The part has no configurable address and no identification register, so
//  "is it really an AHT20?" can only be answered by the calibration bit in the
//  status byte.  The driver says exactly that rather than pretending to have
//  identified the chip.
// =============================================================================
#pragma once

#include "buses/IBusProvider.h"
#include "core/IModule.h"
#include "services/ChannelManager.h"

namespace lc {
namespace modules {

struct Aht20Reading {
  float humidityPercent = 0.0f;
  float temperatureCelsius = 0.0f;
};

class Aht20Protocol {
 public:
  static constexpr std::uint8_t kDefaultAddress = 0x38;
  static constexpr std::uint8_t kCmdStatus = 0x71;
  static constexpr std::uint8_t kCmdInitialise = 0xBE;
  static constexpr std::uint8_t kCmdMeasure = 0xAC;
  static constexpr std::uint8_t kCmdSoftReset = 0xBA;
  static constexpr std::uint8_t kStatusBusy = 0x80;
  static constexpr std::uint8_t kStatusCalibrated = 0x08;
  static constexpr Micros kMeasurementTimeUs = 80000;

  // CRC-8, polynomial 0x31, initial value 0xFF (datasheet §5.4.5).
  static std::uint8_t crc8(const std::uint8_t* data, std::size_t length);

  // `raw` is the 7-byte response: status, 5 data bytes, CRC.
  // Returns false on CRC mismatch or a still-busy status byte.
  static bool decode(const std::uint8_t raw[7], Aht20Reading& out);
};

class Aht20Driver final : public IDevice {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new Aht20Driver(); }

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;
  Status selfTest() override;

  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

 private:
  enum class Phase : std::uint8_t { kIdle = 0, kConverting };

  void fault(const Error& error);

  DeviceContext ctx_{};
  II2cBus* bus_ = nullptr;
  std::uint8_t address_ = Aht20Protocol::kDefaultAddress;

  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
  Phase phase_ = Phase::kIdle;
  Micros conversionDueUs_ = 0;
  std::uint8_t initialiseAttempts_ = 0;
  std::uint8_t consecutiveFailures_ = 0;
};

}  // namespace modules
}  // namespace lc
