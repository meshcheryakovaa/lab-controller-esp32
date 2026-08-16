#include "modules/outputs/AnalogOutputs.h"

#include <cmath>

namespace lc {
namespace modules {
namespace {

float clampPercent(float percent) {
  if (percent < 0.0f) return 0.0f;
  if (percent > 100.0f) return 100.0f;
  return percent;
}

// --- pwm_out -----------------------------------------------------------------
constexpr ParamSpec kPwmParams[] = {
    ParamSpec{"pin", "Output pin", ParamType::kGpio, nullptr,
              "Must be output-capable", 0.0f, 0.0f, 0.0f, nullptr,
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
    ParamSpec{"frequency", "Frequency", ParamType::kInt, "Hz",
              "1 kHz suits MOSFET loads; a few tens of hertz suits an SSR "
              "driving mains, which can only switch at the zero crossing",
              1.0f, 40000.0f, 1.0f, "1000",
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
    ParamSpec{"safe_value", "Safe duty", ParamType::kFloat, "%",
              "Where the output goes at boot and whenever nothing commands it",
              0.0f, 100.0f, 1.0f, "0",
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
    ParamSpec{"invert", "Invert", ParamType::kBool, nullptr,
              "For drivers that idle high", 0.0f, 0.0f, 0.0f, "false",
              nullptr, 0, PinUse::kPwmOutput, false, true, nullptr},
    ParamSpec{"hold_s", "Command expires after", ParamType::kFloat, "s",
              "0 means the duty stays until something changes it",
              0.0f, 86400.0f, 1.0f, "0",
              nullptr, 0, PinUse::kPwmOutput, false, false, nullptr},
};

constexpr ChannelSpec kPwmChannels[] = {
    ChannelSpec{"duty", "Duty", "%", "ratio", ChannelDirection::kOutput,
                0.0f, 100.0f, 1, true, /*safeValue=*/0.0f, /*safeValueFixed=*/false,
                /*defaultHoldSeconds=*/0.0f},
};

constexpr ModuleManifest kPwmManifest = {
    /*id*/ "pwm_out",
    /*name*/ "PWM output",
    /*category*/ ModuleCategory::kOutput,
    /*description*/ "A duty cycle on a hardware PWM channel",
    /*bus*/ BusRequirement::kPwm,
    /*params*/ kPwmParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kPwmParams) / sizeof(kPwmParams[0])),
    /*channels*/ kPwmChannels,
    /*channelCount*/ 1,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 1000000,
    /*minSampleIntervalUs*/ 100000,
    /*schemaVersion*/ 1,
};

// --- heater ------------------------------------------------------------------
constexpr ParamSpec kHeaterParams[] = {
    ParamSpec{"pin", "Output pin", ParamType::kGpio, nullptr,
              "Drives the MOSFET gate or the SSR input",
              0.0f, 0.0f, 0.0f, nullptr,
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
    ParamSpec{"frequency", "Frequency", ParamType::kInt, "Hz",
              "A few hertz for a solid-state relay on mains; 1 kHz for a MOSFET",
              1.0f, 40000.0f, 1.0f, "10",
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
    ParamSpec{"max_duty", "Power limit", ParamType::kFloat, "%",
              "The most this heater may ever be driven to, whatever asks for "
              "it. The one parameter that turns a runaway control loop into a "
              "slow warm-up.",
              1.0f, 100.0f, 1.0f, "100",
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
    ParamSpec{"hold_s", "Command expires after", ParamType::kFloat, "s",
              "A heater is never left commanded indefinitely: if nothing "
              "renews the command within this time it switches off.",
              1.0f, 3600.0f, 10.0f, "600",
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
};

constexpr ChannelSpec kHeaterChannels[] = {
    ChannelSpec{"power", "Power", "%", "power", ChannelDirection::kOutput,
                0.0f, 100.0f, 1, true,
                // Not configurable, on purpose.  See the header.
                /*safeValue=*/0.0f, /*safeValueFixed=*/true,
                /*defaultHoldSeconds=*/600.0f},
};

constexpr ModuleManifest kHeaterManifest = {
    /*id*/ "heater",
    /*name*/ "Heater",
    /*category*/ ModuleCategory::kOutput,
    /*description*/ "A heating element with a hard power limit, a safe state "
                    "fixed at zero, and a command that expires",
    /*bus*/ BusRequirement::kPwm,
    /*params*/ kHeaterParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kHeaterParams) /
                                             sizeof(kHeaterParams[0])),
    /*channels*/ kHeaterChannels,
    /*channelCount*/ 1,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 1000000,
    /*minSampleIntervalUs*/ 100000,
    /*schemaVersion*/ 1,
};

// --- fan ---------------------------------------------------------------------
constexpr ParamSpec kFanParams[] = {
    ParamSpec{"pin", "Output pin", ParamType::kGpio, nullptr, nullptr,
              0.0f, 0.0f, 0.0f, nullptr,
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
    ParamSpec{"frequency", "Frequency", ParamType::kInt, "Hz",
              "25 kHz is the 4-wire fan standard and is inaudible",
              1.0f, 40000.0f, 1.0f, "25000",
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
    ParamSpec{"min_duty", "Minimum running duty", ParamType::kFloat, "%",
              "Below this a fan stalls: it draws current, moves no air, and "
              "looks exactly like a fan that is running. Anything above zero "
              "and below this is raised to it.",
              0.0f, 100.0f, 1.0f, "20",
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
    ParamSpec{"kickstart_ms", "Kick-start", ParamType::kInt, "ms",
              "Full power for this long when starting from rest, to break "
              "stiction. 0 disables it.",
              0.0f, 5000.0f, 50.0f, "400",
              nullptr, 0, PinUse::kPwmOutput, false, true, nullptr},
    ParamSpec{"safe_value", "Safe duty", ParamType::kFloat, "%",
              "On a hot enclosure the safe state of the fan is not zero",
              0.0f, 100.0f, 1.0f, "0",
              nullptr, 0, PinUse::kPwmOutput, true, false, nullptr},
    ParamSpec{"hold_s", "Command expires after", ParamType::kFloat, "s",
              "0 means it keeps running until something changes it",
              0.0f, 86400.0f, 1.0f, "0",
              nullptr, 0, PinUse::kPwmOutput, false, false, nullptr},
};

constexpr ChannelSpec kFanChannels[] = {
    ChannelSpec{"speed", "Speed", "%", "ratio", ChannelDirection::kOutput,
                0.0f, 100.0f, 0, true, /*safeValue=*/0.0f, /*safeValueFixed=*/false,
                /*defaultHoldSeconds=*/0.0f},
};

constexpr ModuleManifest kFanManifest = {
    /*id*/ "fan",
    /*name*/ "Fan",
    /*category*/ ModuleCategory::kOutput,
    /*description*/ "A PWM fan, with the minimum duty below which it stalls "
                    "and a kick-start to get it turning",
    /*bus*/ BusRequirement::kPwm,
    /*params*/ kFanParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kFanParams) / sizeof(kFanParams[0])),
    /*channels*/ kFanChannels,
    /*channelCount*/ 1,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 1000000,
    /*minSampleIntervalUs*/ 100000,
    /*schemaVersion*/ 1,
};

}  // namespace

// ---------------------------------------------------------------------------
//  PwmOutputBase
// ---------------------------------------------------------------------------
Status PwmOutputBase::configure(const DeviceContext& context) {
  ctx_ = context;
  if (context.buses == nullptr || context.buses->pwm() == nullptr) {
    return fail(ErrorCode::kNotSupported,
                "the hardware layer this driver needs is not available");
  }
  pwm_ = context.buses->pwm();
  gpio_ = context.buses->gpio();

  const std::int32_t pin = context.config->getInt("pin", -1);
  if (pin < 0) return fail(ErrorCode::kInvalidArgument, "pin is required");
  pin_ = static_cast<std::uint8_t>(pin);

  const std::int32_t frequency = context.config->getInt("frequency", 1000);
  if (frequency < 1 || frequency > 40000) {
    return fail(ErrorCode::kDeviceConfigInvalid, "frequency must be 1..40000 Hz");
  }
  frequencyHz_ = static_cast<std::uint32_t>(frequency);

  invert_ = context.config->getBool("invert", false);
  // Resolved once by DeviceManager and shared with the safety layer.
  safePercent_ = clampPercent(
      (context.channelSafeValues != nullptr && context.channelCount > 0)
          ? context.channelSafeValues[0] : 0.0f);
  duty_ = safePercent_;

  const Status claimed = context.resources->claimPin(
      pin_, PinUse::kPwmOutput, context.self, "pwm output");
  if (!claimed.ok()) return claimed;
  return ok();
}

Status PwmOutputBase::begin() {
  const Result<PwmChannel> attached = pwm_->attach(pin_, frequencyHz_, ctx_.self);
  if (!attached.ok()) {
    state_ = DeviceState::kError;
    lastError_ = attached.error();
    return lastError_;
  }
  channel_ = attached.value();

  // Safe duty first, before anything can command it.
  const Status applied = applyPercent(safePercent_);
  if (!applied.ok()) {
    state_ = DeviceState::kError;
    lastError_ = applied;
    return applied;
  }

  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
}

void PwmOutputBase::poll(Micros now) { (void)now; }

void PwmOutputBase::end() {
  if (pwm_ != nullptr && channel_ != kInvalidPwmChannel) {
    pwm_->write(channel_, invert_ ? 1.0f - safePercent_ / 100.0f
                                  : safePercent_ / 100.0f);
    pwm_->detach(channel_);
    channel_ = kInvalidPwmChannel;
  }
  state_ = DeviceState::kDisabled;
}

Status PwmOutputBase::applyPercent(float percent) {
  const float bounded = clampPercent(percent);
  const float fraction = bounded / 100.0f;
  const Status written =
      pwm_->write(channel_, invert_ ? (1.0f - fraction) : fraction);
  if (!written.ok()) return written;
  duty_ = bounded;
  return ok();
}

Result<float> PwmOutputBase::shape(float percent, Micros now) {
  (void)now;
  return clampPercent(percent);
}

Status PwmOutputBase::write(ChannelHandle channel, float percent,
                            float* applied) {
  (void)channel;
  if (!std::isfinite(percent)) {
    return fail(ErrorCode::kInvalidArgument, "value is not a number");
  }
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) {
    return fail(ErrorCode::kInvalidState, "device is not running");
  }
  const Result<float> shaped = shape(percent, ctx_.clock->nowMicros());
  if (!shaped.ok()) return shaped.error();
  const Status written = applyPercent(shaped.value());
  if (!written.ok()) return written;
  if (applied != nullptr) *applied = duty_;
  return ok();
}

void PwmOutputBase::failSafe() {
  if (pwm_ == nullptr || channel_ == kInvalidPwmChannel) return;
  const float fraction = safePercent_ / 100.0f;
  pwm_->write(channel_, invert_ ? (1.0f - fraction) : fraction);
  duty_ = safePercent_;
}

const ModuleManifest& PwmOutputDriver::manifest() { return kPwmManifest; }

// ---------------------------------------------------------------------------
//  Heater
// ---------------------------------------------------------------------------
const ModuleManifest& HeaterDriver::manifest() { return kHeaterManifest; }

Status HeaterDriver::configure(const DeviceContext& context) {
  const Status base = PwmOutputBase::configure(context);
  if (!base.ok()) return base;

  const float maximum = context.config->getFloat("max_duty", 100.0f);
  if (!(maximum > 0.0f) || maximum > 100.0f) {
    return fail(ErrorCode::kDeviceConfigInvalid, "max_duty must be 1..100 %");
  }
  maxPercent_ = maximum;
  return ok();
}

Result<float> HeaterDriver::shape(float percent, Micros now) {
  (void)now;
  const float bounded = clampPercent(percent);
  if (bounded <= maxPercent_) return bounded;
  // Limited rather than refused, and counted so the operator can see it
  // happening: refusing would leave the heater at whatever it was, which is
  // the opposite of what a power limit is for.
  ++limited_;
  return maxPercent_;
}

// ---------------------------------------------------------------------------
//  Fan
// ---------------------------------------------------------------------------
const ModuleManifest& FanDriver::manifest() { return kFanManifest; }

Status FanDriver::configure(const DeviceContext& context) {
  const Status base = PwmOutputBase::configure(context);
  if (!base.ok()) return base;

  minRunPercent_ = clampPercent(context.config->getFloat("min_duty", 20.0f));
  const std::int32_t kick = context.config->getInt("kickstart_ms", 400);
  if (kick < 0 || kick > 5000) {
    return fail(ErrorCode::kDeviceConfigInvalid, "kickstart_ms must be 0..5000");
  }
  kickstartUs_ = static_cast<Micros>(kick) * 1000ULL;
  return ok();
}

Result<float> FanDriver::shape(float percent, Micros now) {
  const float requested = clampPercent(percent);

  if (requested <= 0.0f) {
    spinning_ = false;
    kickstartUntilUs_ = 0;
    return 0.0f;
  }

  // Anything the fan cannot actually turn at is raised to the point where it
  // can.  A fan commanded to 5 % that sits still is reported as running.
  const float effective = (requested < minRunPercent_) ? minRunPercent_ : requested;

  if (!spinning_) {
    spinning_ = true;
    kickstartUntilUs_ = (kickstartUs_ > 0) ? now + kickstartUs_ : 0;
  }
  if (kickstartUntilUs_ != 0 && now < kickstartUntilUs_) return 100.0f;
  kickstartUntilUs_ = 0;
  return effective;
}

}  // namespace modules
}  // namespace lc
