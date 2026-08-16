#include "modules/outputs/DigitalOutputs.h"

#include <cmath>
#include <cstring>

namespace lc {
namespace modules {
namespace {

constexpr ParamOption kSafeLevelOptions[] = {
    {"low", "Low (off)"},
    {"high", "High (on)"},
};

// --- digital_out -------------------------------------------------------------
constexpr ParamSpec kDigitalParams[] = {
    ParamSpec{"pin", "Output pin", ParamType::kGpio, nullptr,
              "Must be output-capable; input-only pins are greyed out",
              0.0f, 0.0f, 0.0f, nullptr,
              nullptr, 0, PinUse::kDigitalOutput, true, false, nullptr},
    ParamSpec{"safe_level", "Safe state", ParamType::kSelect, nullptr,
              "Where the pin is driven at boot and whenever nothing is "
              "commanding it. There is no unspecified safe state.",
              0.0f, 0.0f, 0.0f, "low",
              kSafeLevelOptions, 2, PinUse::kDigitalOutput, true, false, nullptr},
    ParamSpec{"invert", "Active low", ParamType::kBool, nullptr,
              "The pin is driven LOW to switch the load ON",
              0.0f, 0.0f, 0.0f, "false",
              nullptr, 0, PinUse::kDigitalOutput, false, false, nullptr},
    ParamSpec{"hold_s", "Command expires after", ParamType::kFloat, "s",
              "The output returns to its safe state this long after the last "
              "command, unless renewed. 0 means it stays until told otherwise.",
              0.0f, 86400.0f, 1.0f, "0",
              nullptr, 0, PinUse::kDigitalOutput, false, false, nullptr},
};

constexpr ChannelSpec kDigitalChannels[] = {
    ChannelSpec{"state", "State", "", "state", ChannelDirection::kOutput,
                0.0f, 1.0f, 0, true, /*safeValue=*/0.0f, /*safeValueFixed=*/false,
                /*defaultHoldSeconds=*/0.0f},
};

constexpr ModuleManifest kDigitalManifest = {
    /*id*/ "digital_out",
    /*name*/ "Digital output",
    /*category*/ ModuleCategory::kOutput,
    /*description*/ "One pin, on or off, with a declared safe state",
    /*bus*/ BusRequirement::kGpio,
    /*params*/ kDigitalParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kDigitalParams) /
                                             sizeof(kDigitalParams[0])),
    /*channels*/ kDigitalChannels,
    /*channelCount*/ 1,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 1000000,
    /*minSampleIntervalUs*/ 100000,
    /*schemaVersion*/ 1,
};

// --- relay -------------------------------------------------------------------
constexpr ParamSpec kRelayParams[] = {
    ParamSpec{"pin", "Coil pin", ParamType::kGpio, nullptr,
              "The pin driving the relay module's input",
              0.0f, 0.0f, 0.0f, nullptr,
              nullptr, 0, PinUse::kDigitalOutput, true, false, nullptr},
    ParamSpec{"safe_level", "Safe state", ParamType::kSelect, nullptr,
              "Almost always OFF. A relay that closes on its own after a power "
              "cut is how equipment gets left running overnight.",
              0.0f, 0.0f, 0.0f, "low",
              kSafeLevelOptions, 2, PinUse::kDigitalOutput, true, false, nullptr},
    ParamSpec{"invert", "Active low", ParamType::kBool, nullptr,
              "Most relay boards energise on a LOW input — check yours",
              0.0f, 0.0f, 0.0f, "true",
              nullptr, 0, PinUse::kDigitalOutput, false, false, nullptr},
    ParamSpec{"min_switch_s", "Minimum time between switches", ParamType::kFloat, "s",
              "Contact life is finite: once a second is 10^5 operations in a "
              "day and a half. Commands that arrive sooner are refused.",
              0.05f, 3600.0f, 0.05f, "1.0",
              nullptr, 0, PinUse::kDigitalOutput, true, false, nullptr},
    ParamSpec{"hold_s", "Command expires after", ParamType::kFloat, "s",
              "Returns to the safe state this long after the last command",
              0.0f, 86400.0f, 1.0f, "0",
              nullptr, 0, PinUse::kDigitalOutput, false, false, nullptr},
};

constexpr ChannelSpec kRelayChannels[] = {
    ChannelSpec{"state", "State", "", "state", ChannelDirection::kOutput,
                0.0f, 1.0f, 0, true, /*safeValue=*/0.0f, /*safeValueFixed=*/false,
                /*defaultHoldSeconds=*/0.0f},
};

constexpr ModuleManifest kRelayManifest = {
    /*id*/ "relay",
    /*name*/ "Relay",
    /*category*/ ModuleCategory::kOutput,
    /*description*/ "A mechanical relay, with its contacts protected against "
                    "being switched faster than they can survive",
    /*bus*/ BusRequirement::kGpio,
    /*params*/ kRelayParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kRelayParams) /
                                             sizeof(kRelayParams[0])),
    /*channels*/ kRelayChannels,
    /*channelCount*/ 1,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 1000000,
    /*minSampleIntervalUs*/ 100000,
    /*schemaVersion*/ 1,
};

}  // namespace

// ---------------------------------------------------------------------------
//  DigitalOutputBase
// ---------------------------------------------------------------------------
Status DigitalOutputBase::configure(const DeviceContext& context) {
  ctx_ = context;
  if (context.buses == nullptr || context.buses->gpio() == nullptr) {
    return fail(ErrorCode::kNotSupported,
                "the hardware layer this driver needs is not available");
  }
  gpio_ = context.buses->gpio();

  const std::int32_t pin = context.config->getInt("pin", -1);
  if (pin < 0) return fail(ErrorCode::kInvalidArgument, "pin is required");
  pin_ = static_cast<std::uint8_t>(pin);

  invert_ = context.config->getBool("invert", false);
  // From the resolved array, not from the configuration: the safety layer was
  // handed this exact number, and re-deriving it here is how the two come to
  // disagree about what "safe" means (ADR-0016).  `safe_level` is translated
  // into the channel's numeric safe value by the manifest resolution above.
  safeLevel_ = (context.channelSafeValues != nullptr &&
                context.channelCount > 0 &&
                context.channelSafeValues[0] >= 0.5f);
  level_ = safeLevel_;

  const Status claimed = context.resources->claimPin(
      pin_, PinUse::kDigitalOutput, context.self, "output");
  if (!claimed.ok()) return claimed;

  return ok();
}

Status DigitalOutputBase::begin() {
  const Status configured =
      gpio_->configure(pin_, PinMode::kOutput, ctx_.self);
  if (!configured.ok()) {
    state_ = DeviceState::kError;
    lastError_ = configured;
    return configured;
  }

  // The pin is driven to the safe level as the very first thing that happens
  // to it, before the scheduler has run a single acquisition pass.  A pin left
  // floating between reset and the first command is a pin whose load is in an
  // unknown state.
  const Status applied = applyLevel(safeLevel_, ctx_.clock->nowMicros());
  if (!applied.ok()) {
    state_ = DeviceState::kError;
    lastError_ = applied;
    return applied;
  }
  everApplied_ = true;

  state_ = DeviceState::kRunning;
  lastError_ = ok();
  return ok();
}

void DigitalOutputBase::poll(Micros now) {
  (void)now;
  // Outputs have nothing to sample.  The channel already carries the commanded
  // value, written back by ChannelManager, so the dashboard and the log show an
  // actuator exactly as they show a sensor.
}

void DigitalOutputBase::end() {
  if (gpio_ != nullptr && everApplied_) {
    // Last thing on the way out, and deliberately not conditional on state:
    // a device being removed must not leave its load energised.
    gpio_->write(pin_, invert_ ? !safeLevel_ : safeLevel_);
  }
  state_ = DeviceState::kDisabled;
}

Status DigitalOutputBase::applyLevel(bool level, Micros now) {
  // Going TO the safe level is never rate-limited, and that exemption is not a
  // convenience — it is the priority order (§49).  Contact life is Reliability;
  // releasing an output to its safe state is Safety, and a wear-protection
  // mechanism that can block a safety mechanism is a defect, not a trade-off.
  // Found the first time the master stop was raised on a relay that had just
  // been switched: the trip was refused with RESOURCE_BUSY and the pump stayed
  // on.  Chattering is still bounded, because leaving the safe level is not
  // exempt.
  const Micros minimum = minimumSwitchIntervalUs();
  if (minimum > 0 && everApplied_ && level != level_ && level != safeLevel_ &&
      now >= lastChangeUs_ && (now - lastChangeUs_) < minimum) {
    return fail(ErrorCode::kResourceBusy, switchTooFastDetail());
  }

  const Status written = gpio_->write(pin_, invert_ ? !level : level);
  if (!written.ok()) return written;

  if (level != level_ || !everApplied_) lastChangeUs_ = now;
  level_ = level;
  return ok();
}

Status DigitalOutputBase::write(ChannelHandle channel, float value,
                                float* applied) {
  (void)channel;
  if (!std::isfinite(value)) {
    return fail(ErrorCode::kInvalidArgument, "value is not a number");
  }
  if (state_ != DeviceState::kRunning && state_ != DeviceState::kWarning) {
    return fail(ErrorCode::kInvalidState, "device is not running");
  }
  // Anything above the midpoint is ON.  Being generous here is right: a rule
  // computing 0.9 means on, and refusing it on a technicality helps nobody.
  const bool level = value >= 0.5f;
  const Status written = applyLevel(level, ctx_.clock->nowMicros());
  if (!written.ok()) return written;
  if (applied != nullptr) *applied = level ? 1.0f : 0.0f;
  return ok();
}

void DigitalOutputBase::failSafe() {
  if (gpio_ == nullptr) return;
  // No interval check and no error path: this is the call that must always
  // work, including from inside a failure.
  gpio_->write(pin_, invert_ ? !safeLevel_ : safeLevel_);
  level_ = safeLevel_;
  if (ctx_.clock != nullptr) lastChangeUs_ = ctx_.clock->nowMicros();
}

const ModuleManifest& DigitalOutputDriver::manifest() { return kDigitalManifest; }

// ---------------------------------------------------------------------------
//  Relay
// ---------------------------------------------------------------------------
const ModuleManifest& RelayDriver::manifest() { return kRelayManifest; }

Status RelayDriver::configure(const DeviceContext& context) {
  const Status base = DigitalOutputBase::configure(context);
  if (!base.ok()) return base;

  const float seconds = context.config->getFloat("min_switch_s", 1.0f);
  if (!(seconds > 0.0f)) {
    return fail(ErrorCode::kDeviceConfigInvalid,
                "min_switch_s must be greater than zero");
  }
  minIntervalUs_ = static_cast<Micros>(seconds * 1000000.0f);
  return ok();
}

}  // namespace modules
}  // namespace lc
