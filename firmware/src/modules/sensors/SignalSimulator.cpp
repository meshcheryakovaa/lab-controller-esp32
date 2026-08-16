#include "modules/sensors/SignalSimulator.h"

#include <cmath>
#include <cstring>

namespace lc {
namespace modules {
namespace {

constexpr ParamOption kWaveformOptions[] = {
    {"sine", "Sine"},
    {"ramp", "Ramp (sawtooth)"},
    {"square", "Square"},
    {"triangle", "Triangle"},
    {"constant", "Constant"},
    {"random_walk", "Random walk"},
};

constexpr ParamSpec kParams[] = {
    ParamSpec{"waveform", "Waveform", ParamType::kSelect, nullptr,
              "Shape of the generated signal",
              0.0f, 0.0f, 0.0f, "sine",
              kWaveformOptions,
              static_cast<std::uint8_t>(sizeof(kWaveformOptions) /
                                        sizeof(kWaveformOptions[0])),
              PinUse::kDigitalInput, true, false, nullptr},

    ParamSpec{"amplitude", "Amplitude", ParamType::kFloat, nullptr,
              "Peak deviation from the offset",
              -1.0e6f, 1.0e6f, 0.1f, "1.0",
              nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},

    ParamSpec{"offset", "Offset", ParamType::kFloat, nullptr,
              "Value the waveform oscillates around",
              -1.0e6f, 1.0e6f, 0.1f, "0.0",
              nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},

    ParamSpec{"period_s", "Period", ParamType::kFloat, "s",
              "Duration of one full cycle",
              0.05f, 3600.0f, 0.05f, "10.0",
              nullptr, 0, PinUse::kDigitalInput, true, false,
              "waveform!=constant"},

    ParamSpec{"noise", "Noise amplitude", ParamType::kFloat, nullptr,
              "Uniform noise added to every sample",
              0.0f, 1.0e6f, 0.01f, "0.0",
              nullptr, 0, PinUse::kDigitalInput, false, true, nullptr},

    ParamSpec{"unit", "Unit", ParamType::kText, nullptr,
              "Unit reported by the generated channel",
              0.0f, 0.0f, 0.0f, "",
              nullptr, 0, PinUse::kDigitalInput, false, true, nullptr},
};

constexpr ChannelSpec kChannels[] = {
    ChannelSpec{"value", "Simulated value", "", "raw",
                ChannelDirection::kInput, 0.0f, 0.0f, 3, true},
};

constexpr ModuleManifest kManifest = {
    /*id*/ "sim_signal",
    /*name*/ "Signal Simulator",
    /*category*/ ModuleCategory::kSensor,
    /*description*/ "Software signal source for developing dashboards, "
                    "controllers and experiments without hardware",
    /*bus*/ BusRequirement::kNone,
    /*params*/ kParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kParams) / sizeof(kParams[0])),
    /*channels*/ kChannels,
    /*channelCount*/ static_cast<std::uint8_t>(sizeof(kChannels) / sizeof(kChannels[0])),
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 100000,  // 10 Hz
    /*minSampleIntervalUs*/ 1000,        // 1 kHz
    /*schemaVersion*/ 1,
};

SignalSimulator::Waveform parseWaveform(const char* text) {
  if (text == nullptr) return SignalSimulator::Waveform::kSine;
  if (std::strcmp(text, "ramp") == 0) return SignalSimulator::Waveform::kRamp;
  if (std::strcmp(text, "square") == 0) return SignalSimulator::Waveform::kSquare;
  if (std::strcmp(text, "triangle") == 0) return SignalSimulator::Waveform::kTriangle;
  if (std::strcmp(text, "constant") == 0) return SignalSimulator::Waveform::kConstant;
  if (std::strcmp(text, "random_walk") == 0) return SignalSimulator::Waveform::kRandomWalk;
  return SignalSimulator::Waveform::kSine;
}

}  // namespace

const ModuleManifest& SignalSimulator::manifest() { return kManifest; }

Status SignalSimulator::configure(const DeviceContext& context) {
  if (context.config == nullptr || context.channels == nullptr ||
      context.clock == nullptr || context.channelCount < 1) {
    return fail(ErrorCode::kInvalidArgument, "incomplete device context");
  }
  ctx_ = context;

  waveform_ = parseWaveform(context.config->getString("waveform", "sine"));
  amplitude_ = context.config->getFloat("amplitude", 1.0f);
  offset_ = context.config->getFloat("offset", 0.0f);
  periodSeconds_ = context.config->getFloat("period_s", 10.0f);
  noise_ = context.config->getFloat("noise", 0.0f);

  if (periodSeconds_ <= 0.0f) {
    return fail(ErrorCode::kDeviceConfigInvalid, "period_s must be > 0");
  }
  if (noise_ < 0.0f) {
    return fail(ErrorCode::kDeviceConfigInvalid, "noise must be >= 0");
  }

  // A simulator claims no hardware resources; that is exactly why it can run
  // alongside any real configuration without conflicting with it.
  state_ = DeviceState::kConfigured;
  lastError_ = ok();
  return ok();
}

Status SignalSimulator::begin() {
  if (state_ != DeviceState::kConfigured && state_ != DeviceState::kError) {
    return fail(ErrorCode::kInvalidState, "configure() first");
  }
  startedAtUs_ = ctx_.clock->nowMicros();
  walkState_ = offset_;
  state_ = DeviceState::kRunning;
  return ok();
}

float SignalSimulator::nextNoise() {
  // xorshift32: deterministic, four instructions, and reproducible across
  // hosts — which matters when a test asserts on generated data.
  rngState_ ^= rngState_ << 13;
  rngState_ ^= rngState_ >> 17;
  rngState_ ^= rngState_ << 5;
  const float unit = static_cast<float>(rngState_ & 0xFFFFFFu) / 8388607.5f;
  return unit - 1.0f;  // [-1, 1]
}

float SignalSimulator::evaluate(Micros t) {
  const float elapsed =
      static_cast<float>(t - startedAtUs_) * 1.0e-6f;  // seconds
  const float phase = (periodSeconds_ > 0.0f)
                          ? std::fmod(elapsed / periodSeconds_, 1.0f)
                          : 0.0f;

  float shape = 0.0f;
  switch (waveform_) {
    case Waveform::kSine:
      shape = std::sin(phase * 6.2831853f);
      break;
    case Waveform::kRamp:
      shape = 2.0f * phase - 1.0f;
      break;
    case Waveform::kSquare:
      shape = (phase < 0.5f) ? 1.0f : -1.0f;
      break;
    case Waveform::kTriangle:
      shape = (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
      break;
    case Waveform::kConstant:
      shape = 0.0f;
      break;
    case Waveform::kRandomWalk:
      walkState_ += amplitude_ * 0.02f * nextNoise();
      // Soft pull towards the offset keeps the walk from drifting away for
      // hours, which would make it useless as a stand-in for a real signal.
      walkState_ += (offset_ - walkState_) * 0.001f;
      return walkState_ + noise_ * nextNoise();
  }

  return offset_ + amplitude_ * shape + noise_ * nextNoise();
}

void SignalSimulator::poll(Micros now) {
  if (state_ != DeviceState::kRunning) return;
  ctx_.channels->publishRaw(ctx_.channelHandles[0], evaluate(now), now);
}

void SignalSimulator::end() { state_ = DeviceState::kDisabled; }

}  // namespace modules
}  // namespace lc
