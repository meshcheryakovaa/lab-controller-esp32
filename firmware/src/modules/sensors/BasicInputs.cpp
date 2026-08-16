#include "modules/sensors/BasicInputs.h"

#include <cstring>

namespace lc {
namespace modules {
namespace {

// ---------------------------------------------------------------------------
//  Analogue input
// ---------------------------------------------------------------------------
constexpr ParamOption kAttenuationOptions[] = {
    {"0", "0 dB — 0.10..0.95 V"},
    {"2.5", "2.5 dB — 0.10..1.25 V"},
    {"6", "6 dB — 0.15..1.75 V"},
    {"11", "11 dB — 0.15..2.45 V"},
};

constexpr ParamOption kOutputOptions[] = {
    {"mv", "Millivolts"},
    {"raw", "Raw ADC counts"},
};

constexpr ParamSpec kAnalogParams[] = {
    ParamSpec{"pin", "Input pin", ParamType::kGpio, nullptr,
              "Only ADC1 pins are offered: ADC2 stops working when Wi-Fi is on",
              0, 0, 0, nullptr, nullptr, 0, PinUse::kAnalogInput, true, false,
              nullptr},
    ParamSpec{"attenuation", "Input range", ParamType::kSelect, "dB",
              "The ESP32 ADC does not reach 0 V or 3.3 V at any setting",
              0, 0, 0, "11", kAttenuationOptions, 4, PinUse::kAnalogInput, false,
              false, nullptr},
    ParamSpec{"samples", "Samples to average", ParamType::kInt, nullptr,
              "Simple averaging in the driver; use a filter stage for anything cleverer",
              1, AnalogInputDriver::kMaxAverage, 1, "8", nullptr, 0,
              PinUse::kAnalogInput, false, false, nullptr},
    ParamSpec{"output", "Reported value", ParamType::kSelect, nullptr,
              "Raw counts are the honest choice when a calibration follows",
              0, 0, 0, "mv", kOutputOptions, 2, PinUse::kAnalogInput, false, false,
              nullptr},
};

constexpr ChannelSpec kAnalogChannels[] = {
    ChannelSpec{"value", "Analog value", "mV", "voltage", ChannelDirection::kInput,
                0.0f, 0.0f, 1, true},
};

constexpr ModuleManifest kAnalogManifest = {
    /*id*/ "analog_in",
    /*name*/ "Analog Input",
    /*category*/ ModuleCategory::kSensor,
    /*description*/ "ADC input with averaging. Attach a calibration to turn it "
                    "into a physical quantity.",
    /*bus*/ BusRequirement::kAdc,
    /*params*/ kAnalogParams,
    /*paramCount*/ 4,
    /*channels*/ kAnalogChannels,
    /*channelCount*/ 1,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 100000,
    /*minSampleIntervalUs*/ 1000,
    /*schemaVersion*/ 1,
};

AdcAttenuation parseAttenuation(const char* text) {
  if (text == nullptr) return AdcAttenuation::kDb11;
  if (std::strcmp(text, "0") == 0) return AdcAttenuation::kDb0;
  if (std::strcmp(text, "2.5") == 0) return AdcAttenuation::kDb2_5;
  if (std::strcmp(text, "6") == 0) return AdcAttenuation::kDb6;
  return AdcAttenuation::kDb11;
}

// ---------------------------------------------------------------------------
//  Digital input
// ---------------------------------------------------------------------------
constexpr ParamOption kPullOptions[] = {
    {"up", "Pull-up"},
    {"down", "Pull-down"},
    {"none", "None (external)"},
};

constexpr ParamSpec kDigitalParams[] = {
    ParamSpec{"pin", "Input pin", ParamType::kGpio, nullptr, nullptr,
              0, 0, 0, nullptr, nullptr, 0, PinUse::kDigitalInput, true, false,
              nullptr},
    ParamSpec{"pull", "Internal pull", ParamType::kSelect, nullptr,
              "GPIO34..39 have no internal pulls; use an external resistor",
              0, 0, 0, "up", kPullOptions, 3, PinUse::kDigitalInput, false, false,
              nullptr},
    ParamSpec{"invert", "Invert", ParamType::kBool, nullptr,
              "Switch to ground with a pull-up reads as 1 when closed",
              0, 0, 0, "true", nullptr, 0, PinUse::kDigitalInput, false, false,
              nullptr},
    ParamSpec{"debounce_ms", "Debounce", ParamType::kInt, "ms",
              "A level must hold this long before it is published",
              0, 1000, 1, "20", nullptr, 0, PinUse::kDigitalInput, false, false,
              nullptr},
};

constexpr ChannelSpec kDigitalChannels[] = {
    ChannelSpec{"state", "State", "", "count", ChannelDirection::kInput,
                0.0f, 1.0f, 0, true},
};

constexpr ModuleManifest kDigitalManifest = {
    /*id*/ "digital_in",
    /*name*/ "Digital Input",
    /*category*/ ModuleCategory::kSensor,
    /*description*/ "Debounced logic-level input: switches, interlocks, flags",
    /*bus*/ BusRequirement::kGpio,
    /*params*/ kDigitalParams,
    /*paramCount*/ 4,
    /*channels*/ kDigitalChannels,
    /*channelCount*/ 1,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 10000,  // 100 Hz: fast enough for a limit switch
    /*minSampleIntervalUs*/ 1000,
    /*schemaVersion*/ 1,
};

PinMode parsePull(const char* text) {
  if (text != nullptr && std::strcmp(text, "down") == 0) return PinMode::kInputPulldown;
  if (text != nullptr && std::strcmp(text, "none") == 0) return PinMode::kInput;
  return PinMode::kInputPullup;
}

}  // namespace

// ===========================================================================
//  AnalogInputDriver
// ===========================================================================
const ModuleManifest& AnalogInputDriver::manifest() { return kAnalogManifest; }

Status AnalogInputDriver::configure(const DeviceContext& context) {
  if (context.buses == nullptr || context.channels == nullptr ||
      context.resources == nullptr || context.channelCount < 1) {
    return fail(ErrorCode::kNotSupported,
                "the hardware layer this driver needs is not available");
  }
  ctx_ = context;
  adc_ = context.buses->adc();
  if (adc_ == nullptr) return fail(ErrorCode::kNotSupported, "no ADC port");

  pin_ = static_cast<std::uint8_t>(context.config->getInt("pin", 0));
  attenuation_ = parseAttenuation(context.config->getString("attenuation", "11"));
  const std::int32_t samples = context.config->getInt("samples", 8);
  samples_ = static_cast<std::uint8_t>(
      (samples < 1) ? 1 : ((samples > kMaxAverage) ? kMaxAverage : samples));
  reportMillivolts_ =
      std::strcmp(context.config->getString("output", "mv"), "raw") != 0;

  return context.resources->claimPin(pin_, PinUse::kAnalogInput, context.self,
                                     "Analog input");
}

Status AnalogInputDriver::begin() {
  const Status status = adc_->configure(pin_, attenuation_, ctx_.self);
  if (!status.ok()) {
    lastError_ = status;
    state_ = DeviceState::kError;
    return status;
  }
  // Missing factory calibration is a real limitation, not a detail to hide:
  // millivolts will be several percent off and the operator should know.
  if (reportMillivolts_ && !adc_->calibrated()) {
    lastError_ = fail(ErrorCode::kNotSupported,
                      "chip has no ADC calibration; mV are approximate");
    state_ = DeviceState::kWarning;
  } else {
    lastError_ = ok();
    state_ = DeviceState::kRunning;
  }
  return ok();
}

void AnalogInputDriver::poll(Micros now) {
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) return;

  float accumulator = 0.0f;
  for (std::uint8_t i = 0; i < samples_; ++i) {
    if (reportMillivolts_) {
      const Result<float> value = adc_->readMillivolts(pin_);
      if (!value.ok()) {
        lastError_ = value.error();
        state_ = DeviceState::kError;
        return;
      }
      accumulator += value.value();
    } else {
      const Result<std::uint16_t> value = adc_->readRaw(pin_);
      if (!value.ok()) {
        lastError_ = value.error();
        state_ = DeviceState::kError;
        return;
      }
      accumulator += static_cast<float>(value.value());
    }
  }

  ctx_.channels->publishRaw(ctx_.channelHandles[0],
                            accumulator / static_cast<float>(samples_), now);
}

void AnalogInputDriver::end() { state_ = DeviceState::kDisabled; }

Status AnalogInputDriver::selfTest() {
  if (adc_ == nullptr) return fail(ErrorCode::kNotSupported, "no ADC port");
  const Result<std::uint16_t> value = adc_->readRaw(pin_);
  return value.ok() ? ok() : value.error();
}

// ===========================================================================
//  DigitalInputDriver
// ===========================================================================
const ModuleManifest& DigitalInputDriver::manifest() { return kDigitalManifest; }

Status DigitalInputDriver::configure(const DeviceContext& context) {
  if (context.buses == nullptr || context.channels == nullptr ||
      context.resources == nullptr || context.channelCount < 1) {
    return fail(ErrorCode::kNotSupported,
                "the hardware layer this driver needs is not available");
  }
  ctx_ = context;
  gpio_ = context.buses->gpio();
  if (gpio_ == nullptr) return fail(ErrorCode::kNotSupported, "no GPIO port");

  pin_ = static_cast<std::uint8_t>(context.config->getInt("pin", 0));
  mode_ = parsePull(context.config->getString("pull", "up"));
  invert_ = context.config->getBool("invert", true);
  debounceUs_ =
      static_cast<Micros>(context.config->getInt("debounce_ms", 20)) * 1000ULL;

  return context.resources->claimPin(pin_, PinUse::kDigitalInput, context.self,
                                     "Digital input");
}

Status DigitalInputDriver::begin() {
  const Status status = gpio_->configure(pin_, mode_, ctx_.self);
  if (!status.ok()) {
    lastError_ = status;
    state_ = DeviceState::kError;
    return status;
  }
  published_ = false;
  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
}

void DigitalInputDriver::poll(Micros now) {
  if (state_ != DeviceState::kRunning) return;

  const Result<bool> level = gpio_->read(pin_);
  if (!level.ok()) {
    lastError_ = level.error();
    state_ = DeviceState::kError;
    return;
  }
  const bool observed = invert_ ? !level.value() : level.value();

  if (observed != candidateLevel_) {
    candidateLevel_ = observed;
    candidateSinceUs_ = now;
    return;
  }
  if (observed == stableLevel_ && published_) return;
  if (debounceUs_ > 0 && (now - candidateSinceUs_) < debounceUs_) return;

  stableLevel_ = observed;
  published_ = true;
  ctx_.channels->publishRaw(ctx_.channelHandles[0], stableLevel_ ? 1.0f : 0.0f,
                            now);
}

void DigitalInputDriver::end() { state_ = DeviceState::kDisabled; }

}  // namespace modules
}  // namespace lc
