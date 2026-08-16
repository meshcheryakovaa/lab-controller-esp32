#include "modules/sensors/Bmp280Driver.h"

#include <cstring>

namespace lc {
namespace modules {
namespace {

constexpr ParamOption kOversamplingOptions[] = {
    {"1", "x1"}, {"2", "x2"}, {"4", "x4"}, {"8", "x8"}, {"16", "x16"},
};

constexpr ParamOption kFilterOptions[] = {
    {"0", "Off"}, {"2", "2"}, {"4", "4"}, {"8", "8"}, {"16", "16"},
};

constexpr ParamSpec kParams[] = {
    ParamSpec{"bus", "I2C bus", ParamType::kBusRef, nullptr, nullptr,
              0, 1, 1, "0", nullptr, 0, PinUse::kBusSignal, true, false, nullptr},
    ParamSpec{"address", "I2C address", ParamType::kI2cAddress, nullptr,
              "0x76 with SDO to ground, 0x77 with SDO to VDD",
              0x76, 0x77, 1, "0x76", nullptr, 0, PinUse::kBusSignal, true, false,
              nullptr},
    ParamSpec{"oversampling_p", "Pressure oversampling", ParamType::kSelect,
              nullptr, "Higher values reduce noise and increase conversion time",
              0, 0, 0, "16", kOversamplingOptions, 5, PinUse::kBusSignal, false,
              true, nullptr},
    ParamSpec{"oversampling_t", "Temperature oversampling", ParamType::kSelect,
              nullptr, nullptr, 0, 0, 0, "2", kOversamplingOptions, 5,
              PinUse::kBusSignal, false, true, nullptr},
    ParamSpec{"filter", "IIR filter", ParamType::kSelect, nullptr,
              "Internal filter; suppresses short pressure transients",
              0, 0, 0, "16", kFilterOptions, 5, PinUse::kBusSignal, false, true,
              nullptr},
};

constexpr ChannelSpec kChannels[] = {
    ChannelSpec{"temperature", "Temperature", "degC", "temperature",
                ChannelDirection::kInput, -40.0f, 85.0f, 2, true},
    ChannelSpec{"pressure", "Pressure", "Pa", "pressure",
                ChannelDirection::kInput, 30000.0f, 110000.0f, 0, true},
};

constexpr ModuleManifest kManifest = {
    /*id*/ "bmp280",
    /*name*/ "BMP280 Pressure / Temperature",
    /*category*/ ModuleCategory::kSensor,
    /*description*/ "I2C barometric pressure and temperature sensor",
    /*bus*/ BusRequirement::kI2c,
    /*params*/ kParams,
    /*paramCount*/ 5,
    /*channels*/ kChannels,
    /*channelCount*/ 2,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 200000,  // 5 Hz
    /*minSampleIntervalUs*/ 20000,       // 50 Hz — above the part's ability
    /*schemaVersion*/ 1,
};

constexpr std::uint8_t kFailuresBeforeError = 3;

std::uint8_t oversamplingCode(std::uint8_t factor) {
  switch (factor) {
    case 1:  return 1;
    case 2:  return 2;
    case 4:  return 3;
    case 8:  return 4;
    case 16: return 5;
    default: return 5;
  }
}

std::uint8_t filterCode(std::uint8_t coefficient) {
  switch (coefficient) {
    case 0:  return 0;
    case 2:  return 1;
    case 4:  return 2;
    case 8:  return 3;
    case 16: return 4;
    default: return 4;
  }
}

std::uint16_t u16le(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
std::int16_t s16le(const std::uint8_t* p) {
  return static_cast<std::int16_t>(u16le(p));
}

}  // namespace

// ---------------------------------------------------------------------------
//  Calibration and compensation (pure)
// ---------------------------------------------------------------------------
Bmp280Calibration Bmp280Calibration::parse(const std::uint8_t raw[24]) {
  Bmp280Calibration c;
  c.t1 = u16le(raw + 0);
  c.t2 = s16le(raw + 2);
  c.t3 = s16le(raw + 4);
  c.p1 = u16le(raw + 6);
  c.p2 = s16le(raw + 8);
  c.p3 = s16le(raw + 10);
  c.p4 = s16le(raw + 12);
  c.p5 = s16le(raw + 14);
  c.p6 = s16le(raw + 16);
  c.p7 = s16le(raw + 18);
  c.p8 = s16le(raw + 20);
  c.p9 = s16le(raw + 22);
  return c;
}

bool Bmp280Calibration::plausible() const {
  // An all-zero or all-0xFF calibration block means the read failed or the part
  // is not a BMP280.  Publishing numbers computed from it would be worse than
  // reporting an error, because they look like measurements.
  if (t1 == 0 || p1 == 0) return false;
  if (t1 == 0xFFFF || p1 == 0xFFFF) return false;
  return true;
}

void Bmp280Protocol::decodeRaw(const std::uint8_t raw[6],
                               std::int32_t& adcPressure,
                               std::int32_t& adcTemperature) {
  adcPressure = static_cast<std::int32_t>(
      (static_cast<std::uint32_t>(raw[0]) << 12) |
      (static_cast<std::uint32_t>(raw[1]) << 4) |
      (static_cast<std::uint32_t>(raw[2]) >> 4));
  adcTemperature = static_cast<std::int32_t>(
      (static_cast<std::uint32_t>(raw[3]) << 12) |
      (static_cast<std::uint32_t>(raw[4]) << 4) |
      (static_cast<std::uint32_t>(raw[5]) >> 4));
}

std::int32_t Bmp280Protocol::compensateTemperature(
    std::int32_t adcTemperature, const Bmp280Calibration& calibration,
    std::int32_t& tFine) {
  // Bosch BMP280 datasheet, §3.11.3, integer variant.
  const std::int32_t var1 =
      ((((adcTemperature >> 3) - (static_cast<std::int32_t>(calibration.t1) << 1))) *
       static_cast<std::int32_t>(calibration.t2)) >> 11;
  const std::int32_t var2 =
      (((((adcTemperature >> 4) - static_cast<std::int32_t>(calibration.t1)) *
         ((adcTemperature >> 4) - static_cast<std::int32_t>(calibration.t1))) >> 12) *
       static_cast<std::int32_t>(calibration.t3)) >> 14;
  tFine = var1 + var2;
  return (tFine * 5 + 128) >> 8;
}

std::uint32_t Bmp280Protocol::compensatePressure(
    std::int32_t adcPressure, const Bmp280Calibration& calibration,
    std::int32_t tFine) {
  // Bosch BMP280 datasheet, §3.11.3, 64-bit integer variant.
  //
  // The datasheet writes these steps as left shifts.  Half of the operands are
  // signed and routinely negative — a cold sensor makes tFine - 128000 negative,
  // and p4/p5/p7 are signed trim values — and shifting a negative value left is
  // undefined behaviour in C++17.  Multiplication by the same power of two is
  // exactly equivalent for every value in range and is defined, so that is what
  // is written here.  Found by UBSan, not by a wrong reading: undefined
  // behaviour that happens to work today is a reading that changes when the
  // compiler does.
  std::int64_t var1 = static_cast<std::int64_t>(tFine) - 128000;
  std::int64_t var2 = var1 * var1 * static_cast<std::int64_t>(calibration.p6);
  var2 += (var1 * static_cast<std::int64_t>(calibration.p5)) * (INT64_C(1) << 17);
  var2 += static_cast<std::int64_t>(calibration.p4) * (INT64_C(1) << 35);
  var1 = ((var1 * var1 * static_cast<std::int64_t>(calibration.p3)) >> 8) +
         ((var1 * static_cast<std::int64_t>(calibration.p2)) * (INT64_C(1) << 12));
  var1 = (((static_cast<std::int64_t>(1) << 47) + var1) *
          static_cast<std::int64_t>(calibration.p1)) >> 33;
  if (var1 == 0) return 0;  // undefined; the caller must not publish this

  std::int64_t pressure = 1048576 - adcPressure;
  pressure = ((pressure * (INT64_C(1) << 31) - var2) * 3125) / var1;
  var1 = (static_cast<std::int64_t>(calibration.p9) * (pressure >> 13) *
          (pressure >> 13)) >> 25;
  var2 = (static_cast<std::int64_t>(calibration.p8) * pressure) >> 19;
  pressure = ((pressure + var1 + var2) >> 8) +
             (static_cast<std::int64_t>(calibration.p7) * 16);
  return static_cast<std::uint32_t>(pressure);
}

// ---------------------------------------------------------------------------
//  Driver
// ---------------------------------------------------------------------------
const ModuleManifest& Bmp280Driver::manifest() { return kManifest; }

Status Bmp280Driver::configure(const DeviceContext& context) {
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

  address_ = static_cast<std::uint8_t>(context.config->getInt("address", 0x76));
  oversamplingPressure_ =
      static_cast<std::uint8_t>(context.config->getInt("oversampling_p", 16));
  oversamplingTemperature_ =
      static_cast<std::uint8_t>(context.config->getInt("oversampling_t", 2));
  filter_ = static_cast<std::uint8_t>(context.config->getInt("filter", 16));

  return context.resources->claim(i2cAddressResource(busIndex, address_),
                                  context.self, "BMP280");
}

Status Bmp280Driver::begin() {
  if (bus_ == nullptr) return fail(ErrorCode::kBusNotConfigured, "no bus");

  const Result<std::uint8_t> chipId =
      bus_->readRegister(address_, Bmp280Protocol::kRegChipId);
  if (!chipId.ok()) {
    lastError_ = chipId.error();
    state_ = DeviceState::kError;
    return lastError_;
  }
  if (chipId.value() != Bmp280Protocol::kChipIdBmp280) {
    // This is the difference between a scan hint and a fact: the address said
    // "maybe BMP280", the chip ID says what it actually is.
    lastError_ = (chipId.value() == Bmp280Protocol::kChipIdBme280)
                     ? fail(ErrorCode::kDeviceConfigInvalid,
                            "this is a BME280, not a BMP280")
                     : fail(ErrorCode::kDeviceNotResponding,
                            "unexpected chip ID at this address");
    state_ = DeviceState::kError;
    return lastError_;
  }

  std::uint8_t raw[24] = {0};
  const Status read =
      bus_->readRegisters(address_, Bmp280Protocol::kRegCalibration, raw, sizeof(raw));
  if (!read.ok()) {
    lastError_ = read;
    state_ = DeviceState::kError;
    return read;
  }
  calibration_ = Bmp280Calibration::parse(raw);
  if (!calibration_.plausible()) {
    lastError_ = fail(ErrorCode::kDeviceCrcError, "implausible calibration data");
    state_ = DeviceState::kError;
    return lastError_;
  }

  // config: t_sb = 0 (0.5 ms), filter, no SPI 3-wire.
  Status status = bus_->writeRegister(
      address_, Bmp280Protocol::kRegConfig,
      static_cast<std::uint8_t>(filterCode(filter_) << 2));
  if (!status.ok()) {
    lastError_ = status;
    state_ = DeviceState::kError;
    return status;
  }

  // ctrl_meas: oversampling + normal mode.  Normal mode means the part
  // free-runs and poll() is a plain register read — no conversion to wait for,
  // so the acquisition path stays trivially non-blocking.
  const std::uint8_t ctrl =
      static_cast<std::uint8_t>((oversamplingCode(oversamplingTemperature_) << 5) |
                                (oversamplingCode(oversamplingPressure_) << 2) | 0x03);
  status = bus_->writeRegister(address_, Bmp280Protocol::kRegCtrlMeas, ctrl);
  if (!status.ok()) {
    lastError_ = status;
    state_ = DeviceState::kError;
    return status;
  }

  consecutiveFailures_ = 0;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
}

void Bmp280Driver::fault(const Error& error) {
  lastError_ = error;
  if (++consecutiveFailures_ >= kFailuresBeforeError) {
    state_ = DeviceState::kError;
    if (ctx_.events != nullptr) {
      Event event;
      event.type = EventType::kDeviceError;
      event.source = ctx_.self;
      event.code = error.code;
      event.detail = "BMP280 stopped responding";
      event.severity = 3;
      ctx_.events->publish(event);
    }
  } else {
    state_ = DeviceState::kWarning;
  }
}

void Bmp280Driver::poll(Micros now) {
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) return;

  std::uint8_t raw[6] = {0};
  const Status read =
      bus_->readRegisters(address_, Bmp280Protocol::kRegData, raw, sizeof(raw));
  if (!read.ok()) {
    fault(read);
    return;
  }

  std::int32_t adcPressure = 0;
  std::int32_t adcTemperature = 0;
  Bmp280Protocol::decodeRaw(raw, adcPressure, adcTemperature);

  // 0x80000 in both fields is the reset value: the first conversion has not
  // finished yet.  Publishing it would put -140 °C on the dashboard.
  if (adcTemperature == 0x80000 || adcPressure == 0x80000) return;

  std::int32_t tFine = 0;
  const std::int32_t temperature =
      Bmp280Protocol::compensateTemperature(adcTemperature, calibration_, tFine);
  const std::uint32_t pressure =
      Bmp280Protocol::compensatePressure(adcPressure, calibration_, tFine);

  consecutiveFailures_ = 0;
  if (state_ == DeviceState::kWarning) state_ = DeviceState::kRunning;
  lastError_ = ok();

  ctx_.channels->publishRaw(ctx_.channelHandles[0],
                            static_cast<float>(temperature) * 0.01f, now);
  if (pressure != 0) {
    ctx_.channels->publishRaw(ctx_.channelHandles[1],
                              static_cast<float>(pressure) / 256.0f, now);
  }
}

void Bmp280Driver::end() {
  if (bus_ != nullptr && state_ == DeviceState::kRunning) {
    // Sleep mode: a device that has been disabled must stop converting, or it
    // keeps self-heating and skewing the neighbouring temperature sensor.
    bus_->writeRegister(address_, Bmp280Protocol::kRegCtrlMeas, 0x00);
  }
  state_ = DeviceState::kDisabled;
}

Status Bmp280Driver::selfTest() {
  if (bus_ == nullptr) return fail(ErrorCode::kBusNotConfigured, "no bus");
  const Result<std::uint8_t> chipId =
      bus_->readRegister(address_, Bmp280Protocol::kRegChipId);
  if (!chipId.ok()) return chipId.error();
  if (chipId.value() != Bmp280Protocol::kChipIdBmp280) {
    return fail(ErrorCode::kDeviceNotResponding, "unexpected chip ID");
  }
  return calibration_.plausible()
             ? ok()
             : fail(ErrorCode::kDeviceCrcError, "implausible calibration data");
}

}  // namespace modules
}  // namespace lc
