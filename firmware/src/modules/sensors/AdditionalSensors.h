#pragma once

#include "buses/IBusProvider.h"
#include "core/IModule.h"
#include "modules/sensors/Bmp280Driver.h"
#include "services/ChannelManager.h"

namespace lc {
namespace modules {

struct Bme280HumidityCalibration {
  std::uint8_t h1 = 0;
  std::int16_t h2 = 0;
  std::uint8_t h3 = 0;
  std::int16_t h4 = 0;
  std::int16_t h5 = 0;
  std::int8_t h6 = 0;

  static Bme280HumidityCalibration parse(std::uint8_t h1,
                                         const std::uint8_t raw[7]);
  bool plausible() const;
};

class Bme280Protocol {
 public:
  static std::uint32_t compensateHumidity(
      std::int32_t adcHumidity, const Bme280HumidityCalibration& calibration,
      std::int32_t tFine);
};

class Bme280Driver final : public IDevice {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new Bme280Driver(); }

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;
  Status selfTest() override;
  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

 private:
  void fault(const Error& error);

  DeviceContext ctx_{};
  II2cBus* bus_ = nullptr;
  std::uint8_t address_ = 0x76;
  Bmp280Calibration calibration_{};
  Bme280HumidityCalibration humidityCalibration_{};
  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
  std::uint8_t consecutiveFailures_ = 0;
};

class Ds18b20Driver final : public IDevice {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new Ds18b20Driver(); }

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;
  Status selfTest() override;
  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

 private:
  bool resetBus();
  void writeBit(bool value);
  bool readBit();
  void writeByte(std::uint8_t value);
  std::uint8_t readByte();
  static std::uint8_t crc8(const std::uint8_t* data, std::size_t length);
  void fault(const Error& error);

  enum class Phase : std::uint8_t { kIdle = 0, kConverting };
  DeviceContext ctx_{};
  IGpioPort* gpio_ = nullptr;
  std::uint8_t pin_ = 26;
  Phase phase_ = Phase::kIdle;
  Micros conversionDueUs_ = 0;
  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
  std::uint8_t consecutiveFailures_ = 0;
};

class Dht11Driver final : public IDevice {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new Dht11Driver(); }

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;
  Status selfTest() override;
  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

 private:
  bool waitWhile(bool level, std::uint32_t timeoutUs,
                 std::uint32_t* durationUs = nullptr);
  bool readFrame(std::uint8_t out[5]);
  void fault(const Error& error);

  DeviceContext ctx_{};
  IGpioPort* gpio_ = nullptr;
  std::uint8_t pin_ = 25;
  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
  std::uint8_t consecutiveFailures_ = 0;
};

class Mlx90614Driver final : public IDevice {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new Mlx90614Driver(); }

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;
  Status selfTest() override;
  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

 private:
  Result<float> readTemperature(std::uint8_t command);
  static std::uint8_t crc8(const std::uint8_t* data, std::size_t length);
  void fault(const Error& error);

  DeviceContext ctx_{};
  II2cBus* bus_ = nullptr;
  std::uint8_t address_ = 0x5A;
  bool checkPec_ = true;
  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
  std::uint8_t consecutiveFailures_ = 0;
};

class Vl53l0xDriver final : public IDevice {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new Vl53l0xDriver(); }
  ~Vl53l0xDriver() override;

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;
  Status selfTest() override;
  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

 private:
  void fault(const Error& error);

  DeviceContext ctx_{};
  II2cBus* bus_ = nullptr;
  std::uint8_t busIndex_ = 0;
  std::uint8_t address_ = 0x29;
  std::uint16_t timingBudgetMs_ = 50;
  bool longRange_ = false;
  void* sensor_ = nullptr;
  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
  std::uint8_t consecutiveFailures_ = 0;
};

}  // namespace modules
}  // namespace lc
