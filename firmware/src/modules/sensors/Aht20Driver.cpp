#include "modules/sensors/Aht20Driver.h"

namespace lc {
namespace modules {
namespace {

constexpr ParamSpec kParams[] = {
    ParamSpec{"bus", "I2C bus", ParamType::kBusRef, nullptr,
              "Which I2C controller the sensor is wired to",
              0, 1, 1, "0", nullptr, 0, PinUse::kBusSignal, true, false, nullptr},
    ParamSpec{"address", "I2C address", ParamType::kI2cAddress, nullptr,
              "The AHT20 has a fixed address of 0x38",
              0x38, 0x38, 1, "0x38", nullptr, 0, PinUse::kBusSignal, true, true,
              nullptr},
};

constexpr ChannelSpec kChannels[] = {
    ChannelSpec{"temperature", "Temperature", "degC", "temperature",
                ChannelDirection::kInput, -40.0f, 85.0f, 2, true},
    ChannelSpec{"humidity", "Relative humidity", "%RH", "humidity",
                ChannelDirection::kInput, 0.0f, 100.0f, 1, true},
};

constexpr ModuleManifest kManifest = {
    /*id*/ "aht20",
    /*name*/ "AHT20 Temperature / Humidity",
    /*category*/ ModuleCategory::kSensor,
    /*description*/ "I2C temperature and relative humidity sensor",
    /*bus*/ BusRequirement::kI2c,
    /*params*/ kParams,
    /*paramCount*/ 2,
    /*channels*/ kChannels,
    /*channelCount*/ 2,
    /*maxInstances*/ 0,
    // The part needs ~80 ms per conversion and self-heats if polled harder;
    // 1 Hz is the sensible default and 5 Hz the honest floor.
    /*defaultSampleIntervalUs*/ 1000000,
    /*minSampleIntervalUs*/ 200000,
    /*schemaVersion*/ 1,
};

// Three failures in a row is a fault; one is a rattled cable.
constexpr std::uint8_t kFailuresBeforeError = 3;
constexpr std::uint8_t kMaxInitialiseAttempts = 20;

}  // namespace

// ---------------------------------------------------------------------------
//  Protocol (pure)
// ---------------------------------------------------------------------------
std::uint8_t Aht20Protocol::crc8(const std::uint8_t* data, std::size_t length) {
  std::uint8_t crc = 0xFF;
  for (std::size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (std::uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<std::uint8_t>((crc << 1) ^ 0x31)
                         : static_cast<std::uint8_t>(crc << 1);
    }
  }
  return crc;
}

bool Aht20Protocol::decode(const std::uint8_t raw[7], Aht20Reading& out) {
  if ((raw[0] & kStatusBusy) != 0) return false;
  if (crc8(raw, 6) != raw[6]) return false;

  // Humidity is the upper 20 bits of bytes 1..3, temperature the lower 20 bits
  // of bytes 3..5 — they share the nibble in byte 3.
  const std::uint32_t humidity = (static_cast<std::uint32_t>(raw[1]) << 12) |
                                 (static_cast<std::uint32_t>(raw[2]) << 4) |
                                 (static_cast<std::uint32_t>(raw[3]) >> 4);
  const std::uint32_t temperature =
      ((static_cast<std::uint32_t>(raw[3]) & 0x0F) << 16) |
      (static_cast<std::uint32_t>(raw[4]) << 8) |
      static_cast<std::uint32_t>(raw[5]);

  constexpr float kScale = 1.0f / 1048576.0f;  // 2^20
  out.humidityPercent = static_cast<float>(humidity) * kScale * 100.0f;
  out.temperatureCelsius = static_cast<float>(temperature) * kScale * 200.0f - 50.0f;
  return true;
}

// ---------------------------------------------------------------------------
//  Driver
// ---------------------------------------------------------------------------
const ModuleManifest& Aht20Driver::manifest() { return kManifest; }

Status Aht20Driver::configure(const DeviceContext& context) {
  if (context.buses == nullptr || context.channels == nullptr ||
      context.resources == nullptr || context.channelCount < 2) {
    return fail(ErrorCode::kNotSupported,
                "the hardware layer this driver needs is not available");
  }
  ctx_ = context;

  const std::uint8_t busIndex =
      static_cast<std::uint8_t>(context.config->getInt("bus", 0));
  bus_ = context.buses->i2c(busIndex);
  if (bus_ == nullptr) {
    return fail(ErrorCode::kBusNotConfigured, "I2C bus is not configured");
  }

  address_ = static_cast<std::uint8_t>(
      context.config->getInt("address", Aht20Protocol::kDefaultAddress));

  // The address on this particular bus is the resource, not the address alone:
  // two AHT20s on two buses is a legal rig.
  const Status claimed = context.resources->claim(
      i2cAddressResource(busIndex, address_), context.self, "AHT20");
  if (!claimed.ok()) return claimed;

  state_ = DeviceState::kConfigured;
  lastError_ = ok();
  return ok();
}

Status Aht20Driver::begin() {
  if (bus_ == nullptr) return fail(ErrorCode::kBusNotConfigured, "no bus");

  if (!bus_->probe(address_)) {
    lastError_ = fail(ErrorCode::kDeviceNotResponding, "no ACK from 0x38");
    state_ = DeviceState::kError;
    return lastError_;
  }

  std::uint8_t status = 0;
  const Status read = bus_->read(address_, &status, 1);
  if (!read.ok()) {
    lastError_ = read;
    state_ = DeviceState::kError;
    return read;
  }

  if ((status & Aht20Protocol::kStatusCalibrated) == 0) {
    // Not calibrated yet: kick off initialisation and come back next tick
    // rather than sleeping 10 ms inside begin().
    if (++initialiseAttempts_ > kMaxInitialiseAttempts) {
      lastError_ = fail(ErrorCode::kDeviceInitFailed,
                        "sensor never reports calibrated");
      state_ = DeviceState::kError;
      return lastError_;
    }
    bus_->writeCommand(address_, Aht20Protocol::kCmdInitialise, 0x08, 0x00);
    return fail(ErrorCode::kTimeout);
  }

  phase_ = Phase::kIdle;
  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
}

void Aht20Driver::fault(const Error& error) {
  lastError_ = error;
  if (++consecutiveFailures_ >= kFailuresBeforeError) {
    state_ = DeviceState::kError;
    if (ctx_.events != nullptr) {
      Event event;
      event.type = EventType::kDeviceError;
      event.source = ctx_.self;
      event.code = error.code;
      event.detail = "AHT20 stopped responding";
      event.severity = 3;
      ctx_.events->publish(event);
    }
  } else {
    // One bad read is not a dead sensor.  WARNING keeps the data flowing and
    // still tells the operator that something is marginal.
    state_ = DeviceState::kWarning;
  }
  phase_ = Phase::kIdle;
}

void Aht20Driver::poll(Micros now) {
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) return;

  if (phase_ == Phase::kIdle) {
    const Status started =
        bus_->writeCommand(address_, Aht20Protocol::kCmdMeasure, 0x33, 0x00);
    if (!started.ok()) {
      fault(started);
      return;
    }
    conversionDueUs_ = now + Aht20Protocol::kMeasurementTimeUs;
    phase_ = Phase::kConverting;
    return;  // come back when it is ready; do not wait here
  }

  if (now < conversionDueUs_) return;

  std::uint8_t raw[7] = {0};
  const Status read = bus_->read(address_, raw, sizeof(raw));
  if (!read.ok()) {
    fault(read);
    return;
  }

  Aht20Reading reading;
  if (!Aht20Protocol::decode(raw, reading)) {
    fault(fail(ErrorCode::kDeviceCrcError, "bad CRC or still busy"));
    return;
  }

  consecutiveFailures_ = 0;
  if (state_ == DeviceState::kWarning) state_ = DeviceState::kRunning;
  lastError_ = ok();

  ctx_.channels->publishRaw(ctx_.channelHandles[0], reading.temperatureCelsius, now);
  ctx_.channels->publishRaw(ctx_.channelHandles[1], reading.humidityPercent, now);
  phase_ = Phase::kIdle;
}

void Aht20Driver::end() {
  phase_ = Phase::kIdle;
  state_ = DeviceState::kDisabled;
}

Status Aht20Driver::selfTest() {
  if (bus_ == nullptr) return fail(ErrorCode::kBusNotConfigured, "no bus");
  if (!bus_->probe(address_)) {
    return fail(ErrorCode::kDeviceNotResponding, "no ACK from the sensor");
  }
  std::uint8_t status = 0;
  const Status read = bus_->read(address_, &status, 1);
  if (!read.ok()) return read;
  if ((status & Aht20Protocol::kStatusCalibrated) == 0) {
    return fail(ErrorCode::kDeviceInitFailed, "sensor reports not calibrated");
  }
  return ok();
}

}  // namespace modules
}  // namespace lc
