#include "modules/processing/MovingAverageProcessor.h"

namespace lc {
namespace modules {
namespace {

constexpr ParamSpec kParams[] = {
    ParamSpec{"window", "Window size", ParamType::kInt, "samples",
              "Number of samples averaged; larger is smoother but slower",
              1.0f, static_cast<float>(MovingAverageProcessor::kMaxWindow), 1.0f,
              "8", nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},
};

constexpr ModuleManifest kManifest = {
    /*id*/ "moving_average",
    /*name*/ "Moving Average",
    /*category*/ ModuleCategory::kProcessing,
    /*description*/ "Boxcar average over the last N samples",
    /*bus*/ BusRequirement::kNone,
    /*params*/ kParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kParams) / sizeof(kParams[0])),
    /*channels*/ nullptr,
    /*channelCount*/ 0,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 0,
    /*minSampleIntervalUs*/ 0,
    /*schemaVersion*/ 1,
};

}  // namespace

const ModuleManifest& MovingAverageProcessor::manifest() { return kManifest; }

Status MovingAverageProcessor::configure(const IConfigView& config) {
  const std::int32_t requested = config.getInt("window", 8);
  if (requested < 1 || requested > static_cast<std::int32_t>(kMaxWindow)) {
    return fail(ErrorCode::kInvalidArgument, "window out of range");
  }
  window_ = static_cast<std::size_t>(requested);
  reset();
  return ok();
}

void MovingAverageProcessor::reset() {
  head_ = 0;
  filled_ = 0;
  sum_ = 0.0;
  for (std::size_t i = 0; i < kMaxWindow; ++i) buffer_[i] = 0.0f;
}

float MovingAverageProcessor::process(float input, Micros now, bool& valid) {
  (void)now;
  (void)valid;
  if (filled_ == window_) {
    sum_ -= buffer_[head_];
  } else {
    ++filled_;
  }
  buffer_[head_] = input;
  sum_ += input;
  head_ = (head_ + 1) % window_;
  return static_cast<float>(sum_ / static_cast<double>(filled_));
}

}  // namespace modules
}  // namespace lc
