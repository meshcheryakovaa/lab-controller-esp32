#include "modules/sensors/AdditionalSensors.h"

#include <cmath>
#include <cstring>
#include <cstdint>

#if defined(LC_TARGET_ESP32) || defined(LC_TARGET_ESP32S3)
#include <Arduino.h>
#include <VL53L0X.h>
#include <Wire.h>
#endif

namespace lc {
namespace modules {
namespace {

constexpr std::uint8_t kFailuresBeforeError = 3;

std::uint16_t u16le(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] |
                                    (static_cast<std::uint16_t>(p[1]) << 8));
}

std::int16_t s16le(const std::uint8_t* p) {
  return static_cast<std::int16_t>(u16le(p));
}

std::int16_t signExtend12(std::uint16_t value) {
  if ((value & 0x0800U) != 0U) value |= 0xF000U;
  return static_cast<std::int16_t>(value);
}

constexpr ParamSpec kBmeParams[] = {
    ParamSpec{"bus", "I2C bus", ParamType::kBusRef, nullptr, nullptr,
              0, 1, 1, "0", nullptr, 0, PinUse::kBusSignal, true, false, nullptr},
    ParamSpec{"address", "I2C address", ParamType::kI2cAddress, nullptr,
              "0x76 with SDO to ground, 0x77 with SDO to VDD",
              0x76, 0x77, 1, "0x76", nullptr, 0, PinUse::kBusSignal, true, false,
              nullptr},
};

constexpr ChannelSpec kBmeChannels[] = {
    ChannelSpec{"temperature", "Temperature", "degC", "temperature",
                ChannelDirection::kInput, -40.0f, 85.0f, 2, true},
    ChannelSpec{"humidity", "Relative humidity", "%RH", "humidity",
                ChannelDirection::kInput, 0.0f, 100.0f, 1, true},
    ChannelSpec{"pressure", "Pressure", "Pa", "pressure",
                ChannelDirection::kInput, 30000.0f, 110000.0f, 0, true},
};

constexpr ModuleManifest kBmeManifest = {
    "bme280", "BME280 Temperature / Humidity / Pressure",
    ModuleCategory::kSensor,
    "I2C environmental sensor with temperature, humidity and pressure",
    BusRequirement::kI2c, kBmeParams, 2, kBmeChannels, 3, 0,
    500000, 50000, 1};

constexpr ParamSpec kDsParams[] = {
    ParamSpec{"pin", "1-Wire data pin", ParamType::kGpio, nullptr,
              "HW-506 signal pin S", 0, 0, 0, "26", nullptr, 0,
              PinUse::kBusSignal, true, false, nullptr},
};

constexpr ChannelSpec kDsChannels[] = {
    ChannelSpec{"temperature", "Temperature", "degC", "temperature",
                ChannelDirection::kInput, -55.0f, 125.0f, 2, true},
};

constexpr ModuleManifest kDsManifest = {
    "ds18b20", "HW-506 / DS18B20 Temperature", ModuleCategory::kSensor,
    "Single DS18B20 on a dedicated 1-Wire GPIO", BusRequirement::kGpio,
    kDsParams, 1, kDsChannels, 1, 0, 500000, 250000, 1};

constexpr ParamSpec kDhtParams[] = {
    ParamSpec{"pin", "Data pin", ParamType::kGpio, nullptr,
              "HW-507 signal pin S", 0, 0, 0, "25", nullptr, 0,
              PinUse::kBusSignal, true, false, nullptr},
};

constexpr ChannelSpec kDhtChannels[] = {
    ChannelSpec{"temperature", "Temperature", "degC", "temperature",
                ChannelDirection::kInput, 0.0f, 50.0f, 1, true},
    ChannelSpec{"humidity", "Relative humidity", "%RH", "humidity",
                ChannelDirection::kInput, 20.0f, 90.0f, 1, true},
};

constexpr ModuleManifest kDhtManifest = {
    "dht11", "HW-507 / DHT11 Temperature / Humidity",
    ModuleCategory::kSensor, "DHT11 single-wire temperature and humidity module",
    BusRequirement::kGpio, kDhtParams, 1, kDhtChannels, 2, 0,
    2000000, 2000000, 1};

constexpr ParamSpec kMlxParams[] = {
    ParamSpec{"bus", "I2C bus", ParamType::kBusRef, nullptr,
              "Use a bus configured at 100 kHz", 0, 1, 1, "0", nullptr, 0,
              PinUse::kBusSignal, true, false, nullptr},
    ParamSpec{"address", "SMBus address", ParamType::kI2cAddress, nullptr,
              "Factory address is 0x5A", 0x5A, 0x5A, 1, "0x5A", nullptr, 0,
              PinUse::kBusSignal, true, true, nullptr},
    ParamSpec{"check_pec", "Check packet CRC", ParamType::kBool, nullptr,
              nullptr, 0, 0, 0, "true", nullptr, 0, PinUse::kBusSignal,
              false, true, nullptr},
};

constexpr ChannelSpec kMlxChannels[] = {
    ChannelSpec{"ambient_temperature", "Sensor temperature", "degC", "temperature",
                ChannelDirection::kInput, -40.0f, 125.0f, 2, true},
    ChannelSpec{"object_temperature", "Object temperature", "degC", "temperature",
                ChannelDirection::kInput, -70.0f, 380.0f, 2, true},
};

constexpr ModuleManifest kMlxManifest = {
    "mlx90614", "GY-906 / MLX90614 IR Thermometer", ModuleCategory::kSensor,
    "SMBus non-contact object and ambient temperature sensor",
    BusRequirement::kI2c, kMlxParams, 3, kMlxChannels, 2, 0,
    500000, 100000, 1};

constexpr ParamOption kRangeOptions[] = {
    {"short", "Short / standard"}, {"long", "Long range"},
};

constexpr ParamSpec kVlParams[] = {
    ParamSpec{"bus", "I2C bus", ParamType::kBusRef, nullptr, nullptr,
              0, 1, 1, "0", nullptr, 0, PinUse::kBusSignal, true, false, nullptr},
    ParamSpec{"address", "I2C address", ParamType::kI2cAddress, nullptr,
              "Default address is 0x29", 0x29, 0x29, 1, "0x29", nullptr, 0,
              PinUse::kBusSignal, true, true, nullptr},
    ParamSpec{"range", "Range mode", ParamType::kSelect, nullptr, nullptr,
              0, 0, 0, "short", kRangeOptions, 2, PinUse::kBusSignal,
              false, false, nullptr},
    ParamSpec{"timing_budget_ms", "Timing budget", ParamType::kInt, "ms",
              nullptr, 20, 200, 1, "50", nullptr, 0, PinUse::kBusSignal,
              false, true, nullptr},
};

constexpr ChannelSpec kVlChannels[] = {
    ChannelSpec{"distance", "Distance", "mm", "distance",
                ChannelDirection::kInput, 0.0f, 2000.0f, 0, true},
};

constexpr ModuleManifest kVlManifest = {
    "vl53l0x", "VL53L0X Time-of-Flight Distance", ModuleCategory::kSensor,
    "I2C laser time-of-flight distance sensor", BusRequirement::kI2c,
    kVlParams, 4, kVlChannels, 1, 0, 100000, 50000, 1};

void publishFaultEvent(const DeviceContext& ctx, const char* detail,
                       const Error& error) {
  if (ctx.events == nullptr) return;
  Event event;
  event.type = EventType::kDeviceError;
  event.source = ctx.self;
  event.code = error.code;
  event.detail = detail;
  event.severity = 3;
  ctx.events->publish(event);
}

}  // namespace

Bme280HumidityCalibration Bme280HumidityCalibration::parse(
    std::uint8_t h1Value, const std::uint8_t raw[7]) {
  Bme280HumidityCalibration c;
  c.h1 = h1Value;
  c.h2 = s16le(raw + 0);
  c.h3 = raw[2];
  c.h4 = signExtend12(static_cast<std::uint16_t>(raw[3]) << 4 |
                      (raw[4] & 0x0FU));
  c.h5 = signExtend12(static_cast<std::uint16_t>(raw[5]) << 4 |
                      (raw[4] >> 4));
  c.h6 = static_cast<std::int8_t>(raw[6]);
  return c;
}

bool Bme280HumidityCalibration::plausible() const {
  return !(h1 == 0xFFU && h2 == -1 && h3 == 0xFFU && h4 == -1 && h5 == -1 &&
           h6 == -1);
}

std::uint32_t Bme280Protocol::compensateHumidity(
    std::int32_t adcHumidity, const Bme280HumidityCalibration& c,
    std::int32_t tFine) {
  std::int32_t value = tFine - 76800;
  value = (((((adcHumidity << 14) -
              (static_cast<std::int32_t>(c.h4) * 1048576) -
              (static_cast<std::int32_t>(c.h5) * value)) + 16384) >> 15) *
           (((((((value * static_cast<std::int32_t>(c.h6)) >> 10) *
                 (((value * static_cast<std::int32_t>(c.h3)) >> 11) + 32768)) >>
                10) + 2097152) * static_cast<std::int32_t>(c.h2) + 8192) >> 14));
  value -= (((((value >> 15) * (value >> 15)) >> 7) *
             static_cast<std::int32_t>(c.h1)) >> 4);
  if (value < 0) value = 0;
  if (value > 419430400) value = 419430400;
  return static_cast<std::uint32_t>(value >> 12);
}

const ModuleManifest& Bme280Driver::manifest() { return kBmeManifest; }

Status Bme280Driver::configure(const DeviceContext& context) {
  if (context.buses == nullptr || context.channels == nullptr ||
      context.resources == nullptr || context.channelCount < 3) {
    return fail(ErrorCode::kNotSupported, "incomplete BME280 device context");
  }
  ctx_ = context;
  const std::uint8_t busIndex =
      static_cast<std::uint8_t>(context.config->getInt("bus", 0));
  bus_ = context.buses->i2c(busIndex);
  if (bus_ == nullptr) return fail(ErrorCode::kBusNotConfigured, "I2C bus");
  address_ = static_cast<std::uint8_t>(context.config->getInt("address", 0x76));
  return context.resources->claim(i2cAddressResource(busIndex, address_),
                                  context.self, "BME280");
}

Status Bme280Driver::begin() {
  const Result<std::uint8_t> id =
      bus_->readRegister(address_, Bmp280Protocol::kRegChipId);
  if (!id.ok()) return id.error();
  if (id.value() != Bmp280Protocol::kChipIdBme280) {
    state_ = DeviceState::kError;
    lastError_ = fail(ErrorCode::kDeviceConfigInvalid,
                      id.value() == Bmp280Protocol::kChipIdBmp280
                          ? "this is a BMP280, not a BME280"
                          : "unexpected BME280 chip ID");
    return lastError_;
  }

  std::uint8_t baseRaw[24] = {0};
  Status status = bus_->readRegisters(address_, 0x88, baseRaw, sizeof(baseRaw));
  if (!status.ok()) return status;
  calibration_ = Bmp280Calibration::parse(baseRaw);
  if (!calibration_.plausible()) {
    return fail(ErrorCode::kDeviceCrcError, "bad BME280 base calibration");
  }

  const Result<std::uint8_t> h1 = bus_->readRegister(address_, 0xA1);
  if (!h1.ok()) return h1.error();
  std::uint8_t humidityRaw[7] = {0};
  status = bus_->readRegisters(address_, 0xE1, humidityRaw, sizeof(humidityRaw));
  if (!status.ok()) return status;
  humidityCalibration_ = Bme280HumidityCalibration::parse(h1.value(), humidityRaw);
  if (!humidityCalibration_.plausible()) {
    return fail(ErrorCode::kDeviceCrcError, "bad BME280 humidity calibration");
  }

  status = bus_->writeRegister(address_, 0xF2, 0x01);  // humidity x1
  if (!status.ok()) return status;
  status = bus_->writeRegister(address_, 0xF5, 0xA0);  // 1000 ms standby
  if (!status.ok()) return status;
  status = bus_->writeRegister(address_, 0xF4, 0x27);  // T x1, P x1, normal
  if (!status.ok()) return status;

  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
}

void Bme280Driver::fault(const Error& error) {
  lastError_ = error;
  if (++consecutiveFailures_ >= kFailuresBeforeError) {
    state_ = DeviceState::kError;
    publishFaultEvent(ctx_, "BME280 stopped responding", error);
  } else {
    state_ = DeviceState::kWarning;
  }
}

void Bme280Driver::poll(Micros now) {
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) return;
  std::uint8_t raw[8] = {0};
  const Status status = bus_->readRegisters(address_, 0xF7, raw, sizeof(raw));
  if (!status.ok()) {
    fault(status);
    return;
  }

  std::int32_t adcPressure = 0;
  std::int32_t adcTemperature = 0;
  Bmp280Protocol::decodeRaw(raw, adcPressure, adcTemperature);
  const std::int32_t adcHumidity =
      static_cast<std::int32_t>((static_cast<std::uint16_t>(raw[6]) << 8) | raw[7]);
  if (adcTemperature == 0x80000 || adcPressure == 0x80000) return;

  std::int32_t tFine = 0;
  const std::int32_t temperature =
      Bmp280Protocol::compensateTemperature(adcTemperature, calibration_, tFine);
  const std::uint32_t pressure =
      Bmp280Protocol::compensatePressure(adcPressure, calibration_, tFine);
  const std::uint32_t humidity =
      Bme280Protocol::compensateHumidity(adcHumidity, humidityCalibration_, tFine);

  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  ctx_.channels->publishRaw(ctx_.channelHandles[0], temperature * 0.01f, now);
  ctx_.channels->publishRaw(ctx_.channelHandles[1], humidity / 1024.0f, now);
  if (pressure != 0) {
    ctx_.channels->publishRaw(ctx_.channelHandles[2], pressure / 256.0f, now);
  }
}

void Bme280Driver::end() {
  if (bus_ != nullptr) bus_->writeRegister(address_, 0xF4, 0x00);
  state_ = DeviceState::kDisabled;
}

Status Bme280Driver::selfTest() {
  if (bus_ == nullptr) return fail(ErrorCode::kBusNotConfigured, "no bus");
  const Result<std::uint8_t> id = bus_->readRegister(address_, 0xD0);
  return id.ok() && id.value() == 0x60
             ? ok()
             : fail(ErrorCode::kDeviceNotResponding, "BME280 chip ID");
}

const ModuleManifest& Ds18b20Driver::manifest() { return kDsManifest; }

Status Ds18b20Driver::configure(const DeviceContext& context) {
  if (context.buses == nullptr || context.buses->gpio() == nullptr ||
      context.resources == nullptr || context.channels == nullptr ||
      context.channelCount < 1) {
    return fail(ErrorCode::kNotSupported, "incomplete DS18B20 device context");
  }
  ctx_ = context;
  gpio_ = context.buses->gpio();
  pin_ = static_cast<std::uint8_t>(context.config->getInt("pin", 26));
  return context.resources->claimPin(pin_, PinUse::kBusSignal, context.self,
                                     "DS18B20 data");
}

bool Ds18b20Driver::resetBus() {
  gpio_->configure(pin_, PinMode::kOpenDrain, ctx_.self);
  gpio_->write(pin_, false);
  gpio_->delayMicros(480);
  gpio_->write(pin_, true);
  gpio_->delayMicros(70);
  const Result<bool> level = gpio_->read(pin_);
  gpio_->delayMicros(410);
  return level.ok() && !level.value();
}

void Ds18b20Driver::writeBit(bool value) {
  gpio_->write(pin_, false);
  gpio_->delayMicros(value ? 6 : 60);
  gpio_->write(pin_, true);
  gpio_->delayMicros(value ? 64 : 10);
}

bool Ds18b20Driver::readBit() {
  gpio_->write(pin_, false);
  gpio_->delayMicros(3);
  gpio_->write(pin_, true);
  gpio_->delayMicros(10);
  const Result<bool> value = gpio_->read(pin_);
  gpio_->delayMicros(53);
  return value.ok() && value.value();
}

void Ds18b20Driver::writeByte(std::uint8_t value) {
  for (std::uint8_t bit = 0; bit < 8; ++bit) {
    writeBit((value & (1U << bit)) != 0);
  }
}

std::uint8_t Ds18b20Driver::readByte() {
  std::uint8_t value = 0;
  for (std::uint8_t bit = 0; bit < 8; ++bit) {
    if (readBit()) value |= static_cast<std::uint8_t>(1U << bit);
  }
  return value;
}

std::uint8_t Ds18b20Driver::crc8(const std::uint8_t* data, std::size_t length) {
  std::uint8_t crc = 0;
  while (length-- > 0) {
    std::uint8_t value = *data++;
    for (std::uint8_t i = 0; i < 8; ++i) {
      const std::uint8_t mix = static_cast<std::uint8_t>((crc ^ value) & 0x01U);
      crc >>= 1;
      if (mix != 0) crc ^= 0x8CU;
      value >>= 1;
    }
  }
  return crc;
}

Status Ds18b20Driver::begin() {
  gpio_->configure(pin_, PinMode::kOpenDrain, ctx_.self);
  gpio_->write(pin_, true);
  if (!resetBus()) {
    state_ = DeviceState::kError;
    lastError_ = fail(ErrorCode::kDeviceNotResponding, "no DS18B20 presence pulse");
    return lastError_;
  }
  phase_ = Phase::kIdle;
  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
}

void Ds18b20Driver::fault(const Error& error) {
  lastError_ = error;
  phase_ = Phase::kIdle;
  if (++consecutiveFailures_ >= kFailuresBeforeError) {
    state_ = DeviceState::kError;
    publishFaultEvent(ctx_, "DS18B20 stopped responding", error);
  } else {
    state_ = DeviceState::kWarning;
  }
}

void Ds18b20Driver::poll(Micros now) {
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) return;
  if (phase_ == Phase::kIdle) {
    if (!resetBus()) {
      fault(fail(ErrorCode::kDeviceNotResponding, "DS18B20 reset failed"));
      return;
    }
    writeByte(0xCC);  // Skip ROM: one sensor per configured pin.
    writeByte(0x44);  // Convert T.
    conversionDueUs_ = now + 750000;
    phase_ = Phase::kConverting;
    return;
  }
  if (now < conversionDueUs_) return;
  if (!resetBus()) {
    fault(fail(ErrorCode::kDeviceNotResponding, "DS18B20 read reset failed"));
    return;
  }
  writeByte(0xCC);
  writeByte(0xBE);
  std::uint8_t scratchpad[9] = {0};
  for (std::uint8_t i = 0; i < 9; ++i) scratchpad[i] = readByte();
  phase_ = Phase::kIdle;
  if (crc8(scratchpad, 8) != scratchpad[8]) {
    fault(fail(ErrorCode::kDeviceCrcError, "DS18B20 scratchpad CRC"));
    return;
  }
  const std::int16_t raw = static_cast<std::int16_t>(
      static_cast<std::uint16_t>(scratchpad[0]) |
      (static_cast<std::uint16_t>(scratchpad[1]) << 8));
  const float temperature = static_cast<float>(raw) / 16.0f;
  if (!std::isfinite(temperature) || temperature < -55.0f ||
      temperature > 125.0f || raw == 0x0550) {
    fault(fail(ErrorCode::kDeviceOutOfRange, "invalid DS18B20 temperature"));
    return;
  }
  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  ctx_.channels->publishRaw(ctx_.channelHandles[0], temperature, now);
}

void Ds18b20Driver::end() {
  if (gpio_ != nullptr) gpio_->write(pin_, true);
  phase_ = Phase::kIdle;
  state_ = DeviceState::kDisabled;
}

Status Ds18b20Driver::selfTest() {
  return resetBus() ? ok()
                    : fail(ErrorCode::kDeviceNotResponding, "no presence pulse");
}

const ModuleManifest& Dht11Driver::manifest() { return kDhtManifest; }

Status Dht11Driver::configure(const DeviceContext& context) {
  if (context.buses == nullptr || context.buses->gpio() == nullptr ||
      context.resources == nullptr || context.channels == nullptr ||
      context.channelCount < 2) {
    return fail(ErrorCode::kNotSupported, "incomplete DHT11 device context");
  }
  ctx_ = context;
  gpio_ = context.buses->gpio();
  pin_ = static_cast<std::uint8_t>(context.config->getInt("pin", 25));
  return context.resources->claimPin(pin_, PinUse::kBusSignal, context.self,
                                     "DHT11 data");
}

Status Dht11Driver::begin() {
  const Status status = gpio_->configure(pin_, PinMode::kInputPullup, ctx_.self);
  if (!status.ok()) return status;
  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
}

bool Dht11Driver::waitWhile(bool level, std::uint32_t timeoutUs,
                            std::uint32_t* durationUs) {
  std::uint32_t elapsed = 0;
  while (elapsed < timeoutUs) {
    const Result<bool> current = gpio_->read(pin_);
    if (!current.ok()) return false;
    if (current.value() != level) {
      if (durationUs != nullptr) *durationUs = elapsed;
      return true;
    }
    gpio_->delayMicros(1);
    ++elapsed;
  }
  if (durationUs != nullptr) *durationUs = elapsed;
  return false;
}

bool Dht11Driver::readFrame(std::uint8_t out[5]) {
  gpio_->configure(pin_, PinMode::kOpenDrain, ctx_.self);
  gpio_->write(pin_, false);
  gpio_->delayMicros(18000);
  gpio_->write(pin_, true);
  gpio_->delayMicros(30);
  gpio_->configure(pin_, PinMode::kInputPullup, ctx_.self);

  if (!waitWhile(true, 100) || !waitWhile(false, 100) ||
      !waitWhile(true, 100)) {
    return false;
  }

  for (std::uint8_t bit = 0; bit < 40; ++bit) {
    if (!waitWhile(false, 100)) return false;
    std::uint32_t highUs = 0;
    if (!waitWhile(true, 100, &highUs)) return false;
    out[bit / 8] = static_cast<std::uint8_t>(out[bit / 8] << 1);
    if (highUs > 40) out[bit / 8] |= 1U;
  }
  return true;
}

void Dht11Driver::fault(const Error& error) {
  lastError_ = error;
  if (++consecutiveFailures_ >= kFailuresBeforeError) {
    state_ = DeviceState::kError;
    publishFaultEvent(ctx_, "DHT11 stopped responding", error);
  } else {
    state_ = DeviceState::kWarning;
  }
}

void Dht11Driver::poll(Micros now) {
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) return;
  std::uint8_t raw[5] = {0};
  if (!readFrame(raw)) {
    fault(fail(ErrorCode::kDeviceNotResponding, "DHT11 pulse timeout"));
    return;
  }
  const std::uint8_t checksum =
      static_cast<std::uint8_t>(raw[0] + raw[1] + raw[2] + raw[3]);
  if (checksum != raw[4]) {
    fault(fail(ErrorCode::kDeviceCrcError, "DHT11 checksum"));
    return;
  }
  const float humidity = raw[0] + raw[1] * 0.1f;
  float temperature = (raw[2] & 0x7FU) + raw[3] * 0.1f;
  if ((raw[2] & 0x80U) != 0) temperature = -temperature;
  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  ctx_.channels->publishRaw(ctx_.channelHandles[0], temperature, now);
  ctx_.channels->publishRaw(ctx_.channelHandles[1], humidity, now);
}

void Dht11Driver::end() {
  if (gpio_ != nullptr) gpio_->configure(pin_, PinMode::kInputPullup, ctx_.self);
  state_ = DeviceState::kDisabled;
}

Status Dht11Driver::selfTest() {
  std::uint8_t raw[5] = {0};
  return readFrame(raw) ? ok()
                        : fail(ErrorCode::kDeviceNotResponding, "DHT11 timeout");
}

const ModuleManifest& Mlx90614Driver::manifest() { return kMlxManifest; }

Status Mlx90614Driver::configure(const DeviceContext& context) {
  if (context.buses == nullptr || context.channels == nullptr ||
      context.resources == nullptr || context.channelCount < 2) {
    return fail(ErrorCode::kNotSupported, "incomplete MLX90614 device context");
  }
  ctx_ = context;
  const std::uint8_t busIndex =
      static_cast<std::uint8_t>(context.config->getInt("bus", 0));
  bus_ = context.buses->i2c(busIndex);
  if (bus_ == nullptr) return fail(ErrorCode::kBusNotConfigured, "I2C bus");
  address_ = static_cast<std::uint8_t>(context.config->getInt("address", 0x5A));
  checkPec_ = context.config->getBool("check_pec", true);
  return context.resources->claim(i2cAddressResource(busIndex, address_),
                                  context.self, "MLX90614");
}

std::uint8_t Mlx90614Driver::crc8(const std::uint8_t* data, std::size_t length) {
  std::uint8_t crc = 0;
  for (std::size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (std::uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80U) != 0U
                ? static_cast<std::uint8_t>((crc << 1) ^ 0x07U)
                : static_cast<std::uint8_t>(crc << 1);
    }
  }
  return crc;
}

Result<float> Mlx90614Driver::readTemperature(std::uint8_t command) {
  std::uint8_t raw[3] = {0};
  const Status status = bus_->writeRead(address_, &command, 1, raw, sizeof(raw));
  if (!status.ok()) return status;
  if (checkPec_) {
    const std::uint8_t packet[5] = {
        static_cast<std::uint8_t>(address_ << 1), command,
        static_cast<std::uint8_t>((address_ << 1) | 1U), raw[0], raw[1]};
    if (crc8(packet, sizeof(packet)) != raw[2]) {
      return fail(ErrorCode::kDeviceCrcError, "MLX90614 PEC");
    }
  }
  const std::uint16_t value = u16le(raw);
  if ((value & 0x8000U) != 0U) {
    return fail(ErrorCode::kDeviceOutOfRange, "MLX90614 error flag");
  }
  return static_cast<float>(value) * 0.02f - 273.15f;
}

Status Mlx90614Driver::begin() {
  if (!bus_->probe(address_)) {
    state_ = DeviceState::kError;
    lastError_ = fail(ErrorCode::kDeviceNotResponding, "no MLX90614 ACK");
    return lastError_;
  }
  const Result<float> ambient = readTemperature(0x06);
  if (!ambient.ok()) {
    state_ = DeviceState::kError;
    lastError_ = ambient.error();
    return lastError_;
  }
  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
}

void Mlx90614Driver::fault(const Error& error) {
  lastError_ = error;
  if (++consecutiveFailures_ >= kFailuresBeforeError) {
    state_ = DeviceState::kError;
    publishFaultEvent(ctx_, "MLX90614 stopped responding", error);
  } else {
    state_ = DeviceState::kWarning;
  }
}

void Mlx90614Driver::poll(Micros now) {
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) return;
  const Result<float> ambient = readTemperature(0x06);
  if (!ambient.ok()) {
    fault(ambient.error());
    return;
  }
  const Result<float> object = readTemperature(0x07);
  if (!object.ok()) {
    fault(object.error());
    return;
  }
  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  ctx_.channels->publishRaw(ctx_.channelHandles[0], ambient.value(), now);
  ctx_.channels->publishRaw(ctx_.channelHandles[1], object.value(), now);
}

void Mlx90614Driver::end() { state_ = DeviceState::kDisabled; }

Status Mlx90614Driver::selfTest() {
  const Result<float> value = readTemperature(0x06);
  return value.ok() ? ok() : value.error();
}

const ModuleManifest& Vl53l0xDriver::manifest() { return kVlManifest; }

Vl53l0xDriver::~Vl53l0xDriver() { end(); }

Status Vl53l0xDriver::configure(const DeviceContext& context) {
  if (context.buses == nullptr || context.channels == nullptr ||
      context.resources == nullptr || context.channelCount < 1) {
    return fail(ErrorCode::kNotSupported, "incomplete VL53L0X device context");
  }
  ctx_ = context;
  busIndex_ = static_cast<std::uint8_t>(context.config->getInt("bus", 0));
  bus_ = context.buses->i2c(busIndex_);
  if (bus_ == nullptr) return fail(ErrorCode::kBusNotConfigured, "I2C bus");
  address_ = static_cast<std::uint8_t>(context.config->getInt("address", 0x29));
  timingBudgetMs_ = static_cast<std::uint16_t>(
      context.config->getInt("timing_budget_ms", 50));
  longRange_ = std::strcmp(context.config->getString("range", "short"), "long") == 0;
  return context.resources->claim(i2cAddressResource(busIndex_, address_),
                                  context.self, "VL53L0X");
}

Status Vl53l0xDriver::begin() {
#if defined(LC_TARGET_ESP32) || defined(LC_TARGET_ESP32S3)
  if (!bus_->probe(address_)) {
    state_ = DeviceState::kError;
    lastError_ = fail(ErrorCode::kDeviceNotResponding, "no VL53L0X ACK");
    return lastError_;
  }
  if (address_ != 0x29) {
    return fail(ErrorCode::kDeviceConfigInvalid,
                "VL53L0X must start at address 0x29");
  }
  if (sensor_ == nullptr) sensor_ = new VL53L0X();
  VL53L0X& sensor = *static_cast<VL53L0X*>(sensor_);
  sensor.setBus(busIndex_ == 0 ? &Wire : &Wire1);
  sensor.setTimeout(100);
  if (!sensor.init(true)) {
    state_ = DeviceState::kError;
    lastError_ = fail(ErrorCode::kDeviceInitFailed, "VL53L0X init failed");
    return lastError_;
  }
  if (longRange_) {
    sensor.setSignalRateLimit(0.1f);
    sensor.setVcselPulsePeriod(VL53L0X::VcselPeriodPreRange, 18);
    sensor.setVcselPulsePeriod(VL53L0X::VcselPeriodFinalRange, 14);
  }
  if (!sensor.setMeasurementTimingBudget(
          static_cast<std::uint32_t>(timingBudgetMs_) * 1000U)) {
    return fail(ErrorCode::kDeviceConfigInvalid, "VL53L0X timing budget");
  }
  sensor.startContinuous();
  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
#else
  state_ = DeviceState::kError;
  lastError_ = fail(ErrorCode::kNotSupported, "VL53L0X requires ESP32 build");
  return lastError_;
#endif
}

void Vl53l0xDriver::fault(const Error& error) {
  lastError_ = error;
  if (++consecutiveFailures_ >= kFailuresBeforeError) {
    state_ = DeviceState::kError;
    publishFaultEvent(ctx_, "VL53L0X stopped responding", error);
  } else {
    state_ = DeviceState::kWarning;
  }
}

void Vl53l0xDriver::poll(Micros now) {
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) return;
#if defined(LC_TARGET_ESP32) || defined(LC_TARGET_ESP32S3)
  const Result<std::uint8_t> ready = bus_->readRegister(address_, 0x13);
  if (!ready.ok()) {
    fault(ready.error());
    return;
  }
  if ((ready.value() & 0x07U) == 0U) return;
  VL53L0X& sensor = *static_cast<VL53L0X*>(sensor_);
  const std::uint16_t millimeters = sensor.readRangeContinuousMillimeters();
  if (sensor.timeoutOccurred() || millimeters == 0 || millimeters >= 8190) {
    fault(fail(ErrorCode::kDeviceOutOfRange, "VL53L0X invalid range"));
    return;
  }
  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  ctx_.channels->publishRaw(ctx_.channelHandles[0], millimeters, now);
#else
  (void)now;
#endif
}

void Vl53l0xDriver::end() {
#if defined(LC_TARGET_ESP32) || defined(LC_TARGET_ESP32S3)
  if (sensor_ != nullptr) {
    static_cast<VL53L0X*>(sensor_)->stopContinuous();
    delete static_cast<VL53L0X*>(sensor_);
    sensor_ = nullptr;
  }
#endif
  state_ = DeviceState::kDisabled;
}

Status Vl53l0xDriver::selfTest() {
  return bus_ != nullptr && bus_->probe(address_)
             ? ok()
             : fail(ErrorCode::kDeviceNotResponding, "no VL53L0X ACK");
}

}  // namespace modules
}  // namespace lc
