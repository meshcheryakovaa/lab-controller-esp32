#include "modules/sensors/Hx711Driver.h"

namespace lc {
namespace modules {
namespace {

constexpr ParamOption kGainOptions[] = {
    {"128", "128 (channel A)"},
    {"64", "64 (channel A)"},
    {"32", "32 (channel B)"},
};

constexpr ParamOption kRateOptions[] = {
    {"10", "10 Hz (RATE pin low)"},
    {"80", "80 Hz (RATE pin high)"},
};

constexpr ParamSpec kParams[] = {
    ParamSpec{"data_pin", "DOUT pin", ParamType::kGpio, nullptr,
              "Data output of the HX711; any input-capable pin",
              0, 0, 0, nullptr, nullptr, 0, PinUse::kDigitalInput, true, false,
              nullptr},
    ParamSpec{"clock_pin", "SCK pin", ParamType::kGpio, nullptr,
              "Clock input of the HX711; must be output-capable",
              0, 0, 0, nullptr, nullptr, 0, PinUse::kDigitalOutput, true, false,
              nullptr},
    ParamSpec{"gain", "Gain", ParamType::kSelect, nullptr,
              "128 and 64 read channel A, 32 reads channel B",
              0, 0, 0, "128", kGainOptions, 3, PinUse::kDigitalInput, true, false,
              nullptr},
    ParamSpec{"rate_hz", "Conversion rate", ParamType::kSelect, "Hz",
              "Set by the RATE pin on the module; used here to detect a dead sensor",
              0, 0, 0, "10", kRateOptions, 2, PinUse::kDigitalInput, false, true,
              nullptr},
};

constexpr ChannelSpec kChannels[] = {
    // Unit and quantity describe what the channel becomes AFTER calibration.
    // Until a calibration is attached the value is raw ADC counts, and the UI
    // marks the channel as uncalibrated.
    ChannelSpec{"mass", "Mass", "g", "mass", ChannelDirection::kInput,
                0.0f, 0.0f, 3, true},
};

constexpr ModuleManifest kManifest = {
    /*id*/ "hx711",
    /*name*/ "HX711 Load Cell",
    /*category*/ ModuleCategory::kSensor,
    /*description*/ "24-bit load-cell amplifier. Publishes raw counts; attach a "
                    "calibration to convert them to a physical quantity.",
    /*bus*/ BusRequirement::kGpio,
    /*params*/ kParams,
    /*paramCount*/ 4,
    /*channels*/ kChannels,
    /*channelCount*/ 1,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 100000,  // 10 Hz; poll faster than the part
    /*minSampleIntervalUs*/ 5000,        // 200 Hz polling for an 80 Hz part
    /*schemaVersion*/ 1,
};

// Clock pulse timing.  The datasheet requires PD_SCK high for 0.2..50 µs; one
// microsecond each way is comfortable and keeps a full read near 60 µs.
constexpr std::uint32_t kPulseMicros = 1;

// SCK held high for >60 µs puts the part into power-down.  Every pulse here is
// bounded, but the limit is why this loop must not be interrupted by a long
// task — one more reason acquisition runs at its own scheduler priority.
constexpr std::uint32_t kPowerDownMicros = 60;

}  // namespace

std::uint8_t Hx711Protocol::pulsesForGain(std::uint8_t gain) {
  switch (gain) {
    case 128: return 1;  // channel A
    case 32:  return 2;  // channel B
    case 64:  return 3;  // channel A
    default:  return 1;
  }
}

std::int32_t Hx711Protocol::signExtend24(std::uint32_t raw) {
  raw &= 0x00FFFFFFu;
  if ((raw & 0x00800000u) != 0) raw |= 0xFF000000u;
  return static_cast<std::int32_t>(raw);
}

const ModuleManifest& Hx711Driver::manifest() { return kManifest; }

Status Hx711Driver::configure(const DeviceContext& context) {
  if (context.buses == nullptr || context.channels == nullptr ||
      context.resources == nullptr || context.channelCount < 1) {
    return fail(ErrorCode::kNotSupported,
                "the hardware layer this driver needs is not available");
  }
  ctx_ = context;
  gpio_ = context.buses->gpio();
  if (gpio_ == nullptr) return fail(ErrorCode::kNotSupported, "no GPIO port");

  dataPin_ = static_cast<std::uint8_t>(context.config->getInt("data_pin", 0));
  clockPin_ = static_cast<std::uint8_t>(context.config->getInt("clock_pin", 0));
  gain_ = static_cast<std::uint8_t>(context.config->getInt("gain", 128));

  const std::int32_t rate = context.config->getInt("rate_hz", 10);
  // Three missed conversions is a fault.  At 10 Hz that is 300 ms, which is
  // long enough to survive a jittery scheduler and short enough that a
  // disconnected cell is obvious to the operator.
  timeoutUs_ = static_cast<Micros>(3 * 1000000 / (rate > 0 ? rate : 10));
  if (timeoutUs_ < 200000) timeoutUs_ = 200000;

  Status status = context.resources->claimPin(dataPin_, PinUse::kDigitalInput,
                                              context.self, "HX711 DOUT");
  if (!status.ok()) return status;
  status = context.resources->claimPin(clockPin_, PinUse::kDigitalOutput,
                                       context.self, "HX711 SCK");
  if (!status.ok()) return status;

  state_ = DeviceState::kConfigured;
  return ok();
}

Status Hx711Driver::begin() {
  Status status = gpio_->configure(clockPin_, PinMode::kOutput, ctx_.self);
  if (!status.ok()) return status;
  status = gpio_->configure(dataPin_, PinMode::kInputPullup, ctx_.self);
  if (!status.ok()) return status;

  // Reset: SCK high for >60 µs powers the part down, then low wakes it with a
  // known channel/gain selection.
  gpio_->write(clockPin_, true);
  gpio_->delayMicros(kPowerDownMicros + 10);
  gpio_->write(clockPin_, false);

  lastSampleUs_ = ctx_.clock->nowMicros();
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
}

bool Hx711Driver::readOnce(std::int32_t& counts) {
  std::uint32_t raw = 0;
  for (std::uint8_t bit = 0; bit < 24; ++bit) {
    gpio_->write(clockPin_, true);
    gpio_->delayMicros(kPulseMicros);
    const Result<bool> level = gpio_->read(dataPin_);
    gpio_->write(clockPin_, false);
    gpio_->delayMicros(kPulseMicros);
    if (!level.ok()) return false;
    raw = (raw << 1) | (level.value() ? 1u : 0u);
  }

  // The extra pulses both latch the result and select the channel and gain for
  // the NEXT conversion.
  const std::uint8_t extra = Hx711Protocol::pulsesForGain(gain_);
  for (std::uint8_t pulse = 0; pulse < extra; ++pulse) {
    gpio_->write(clockPin_, true);
    gpio_->delayMicros(kPulseMicros);
    gpio_->write(clockPin_, false);
    gpio_->delayMicros(kPulseMicros);
  }

  counts = Hx711Protocol::signExtend24(raw);
  return true;
}

void Hx711Driver::fault(const Error& error) {
  lastError_ = error;
  state_ = DeviceState::kError;
  if (ctx_.events != nullptr) {
    Event event;
    event.type = EventType::kDeviceError;
    event.source = ctx_.self;
    event.code = error.code;
    event.detail = "HX711 is not producing conversions";
    event.severity = 3;
    ctx_.events->publish(event);
  }
}

void Hx711Driver::poll(Micros now) {
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) return;

  const Result<bool> ready = gpio_->read(dataPin_);
  if (!ready.ok()) {
    fault(ready.error());
    return;
  }

  if (ready.value()) {
    // DOUT still high: no conversion available.  Return — never spin here.
    if (now > lastSampleUs_ && (now - lastSampleUs_) > timeoutUs_) {
      fault(fail(ErrorCode::kDeviceNotResponding,
                 "DOUT never went low; check wiring and power"));
    }
    return;
  }

  std::int32_t counts = 0;
  if (!readOnce(counts)) {
    fault(fail(ErrorCode::kDeviceNotResponding, "failed to read DOUT"));
    return;
  }

  // 0x7FFFFF and -0x800000 are the saturation rails: the cell is overloaded or
  // the bridge is disconnected.  Reporting the number as a measurement would
  // hide a physical problem.
  if (counts >= 0x7FFFFF || counts <= -0x800000) {
    lastError_ = fail(ErrorCode::kDeviceOutOfRange, "load cell saturated");
    state_ = DeviceState::kWarning;
  } else {
    lastError_ = ok();
    state_ = DeviceState::kRunning;
  }

  lastSampleUs_ = now;
  ctx_.channels->publishRaw(ctx_.channelHandles[0],
                            static_cast<float>(counts), now);
}

void Hx711Driver::end() {
  if (gpio_ != nullptr && state_ != DeviceState::kDisabled) {
    gpio_->write(clockPin_, true);  // power down
  }
  state_ = DeviceState::kDisabled;
}

Status Hx711Driver::selfTest() {
  if (gpio_ == nullptr) return fail(ErrorCode::kNotSupported, "no GPIO port");
  // A working HX711 pulls DOUT low at least once per conversion period.  If it
  // is high for well over that, the cable, the power or the part is at fault.
  const Result<bool> level = gpio_->read(dataPin_);
  if (!level.ok()) return level.error();
  if (level.value()) {
    return fail(ErrorCode::kDeviceNotResponding,
                "DOUT is high; no conversion ready");
  }
  return ok();
}

}  // namespace modules
}  // namespace lc
