#include "modules/processing/FilterProcessors.h"

#include <cmath>

namespace lc {
namespace modules {
namespace {

// ---------------------------------------------------------------------------
//  Manifests
// ---------------------------------------------------------------------------
constexpr ParamSpec kMedianParams[] = {
    ParamSpec{"window", "Window", ParamType::kInt, "samples",
              "Number of samples the median is taken over; must be odd",
              3.0f, static_cast<float>(MedianProcessor::kMaxWindow), 2.0f, "5",
              nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},
};

constexpr ModuleManifest kMedianManifest = {
    /*id*/ "median",
    /*name*/ "Median filter",
    /*category*/ ModuleCategory::kProcessing,
    /*description*/ "Removes isolated outliers without smearing them into "
                    "neighbouring samples the way an average does",
    /*bus*/ BusRequirement::kNone,
    /*params*/ kMedianParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kMedianParams) /
                                             sizeof(kMedianParams[0])),
    /*channels*/ nullptr,
    /*channelCount*/ 0,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 0,
    /*minSampleIntervalUs*/ 0,
    /*schemaVersion*/ 1,
};

constexpr ParamSpec kLowPassParams[] = {
    ParamSpec{"tau_s", "Time constant", ParamType::kFloat, "s",
              "Time to reach 63% of a step. Larger is smoother and slower.",
              0.001f, 3600.0f, 0.1f, "1.0",
              nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},
};

constexpr ModuleManifest kLowPassManifest = {
    /*id*/ "low_pass",
    /*name*/ "Low-pass filter",
    /*category*/ ModuleCategory::kProcessing,
    /*description*/ "First-order smoothing with a stated time constant; the "
                    "coefficient follows the real sample interval",
    /*bus*/ BusRequirement::kNone,
    /*params*/ kLowPassParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kLowPassParams) /
                                             sizeof(kLowPassParams[0])),
    /*channels*/ nullptr,
    /*channelCount*/ 0,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 0,
    /*minSampleIntervalUs*/ 0,
    /*schemaVersion*/ 1,
};

constexpr ParamSpec kDeadbandParams[] = {
    ParamSpec{"threshold", "Threshold", ParamType::kFloat, nullptr,
              "Movement smaller than this is not reported. In the channel's "
              "own units, and it quantises the output by the same amount.",
              0.0f, 1.0e6f, 0.01f, "0.0",
              nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},
};

constexpr ModuleManifest kDeadbandManifest = {
    /*id*/ "deadband",
    /*name*/ "Deadband",
    /*category*/ ModuleCategory::kProcessing,
    /*description*/ "Holds the last value until the signal moves further than "
                    "the threshold. Distorts the signal on purpose — for logs, "
                    "not for control loops.",
    /*bus*/ BusRequirement::kNone,
    /*params*/ kDeadbandParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kDeadbandParams) /
                                             sizeof(kDeadbandParams[0])),
    /*channels*/ nullptr,
    /*channelCount*/ 0,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 0,
    /*minSampleIntervalUs*/ 0,
    /*schemaVersion*/ 1,
};

}  // namespace

// ---------------------------------------------------------------------------
//  Median
// ---------------------------------------------------------------------------
const ModuleManifest& MedianProcessor::manifest() { return kMedianManifest; }

Status MedianProcessor::configure(const IConfigView& config) {
  const std::int32_t window = config.getInt("window", 5);
  if (window < 3 || static_cast<std::size_t>(window) > kMaxWindow) {
    return fail(ErrorCode::kInvalidArgument, "window must be 3..31");
  }
  if ((window % 2) == 0) {
    return fail(ErrorCode::kInvalidArgument, "window must be odd");
  }
  window_ = static_cast<std::size_t>(window);
  reset();
  return ok();
}

float MedianProcessor::process(float input, Micros now, bool& valid) {
  (void)now;
  buffer_[head_] = input;
  head_ = (head_ + 1) % window_;
  if (filled_ < window_) ++filled_;

  // A partial window has no median worth the name: with two samples of five
  // the "middle" one is just one of the two.  Suppress until it is full —
  // this is exactly what the `valid` flag is for.
  if (filled_ < window_) {
    valid = false;
    return input;
  }

  for (std::size_t i = 0; i < window_; ++i) scratch_[i] = buffer_[i];

  // Insertion sort: window_ <= 31, so this is at most ~500 comparisons and
  // runs on a channel sampled at tens of hertz.  A partial selection would be
  // faster and much easier to get subtly wrong.
  for (std::size_t i = 1; i < window_; ++i) {
    const float key = scratch_[i];
    std::size_t j = i;
    while (j > 0 && scratch_[j - 1] > key) {
      scratch_[j] = scratch_[j - 1];
      --j;
    }
    scratch_[j] = key;
  }
  return scratch_[window_ / 2];
}

void MedianProcessor::reset() {
  head_ = 0;
  filled_ = 0;
  for (std::size_t i = 0; i < kMaxWindow; ++i) buffer_[i] = 0.0f;
}

// ---------------------------------------------------------------------------
//  Low-pass
// ---------------------------------------------------------------------------
const ModuleManifest& LowPassProcessor::manifest() { return kLowPassManifest; }

Status LowPassProcessor::configure(const IConfigView& config) {
  const float tau = config.getFloat("tau_s", 1.0f);
  if (!(tau > 0.0f) || tau > 3600.0f) {
    return fail(ErrorCode::kInvalidArgument, "tau_s must be 0.001..3600");
  }
  tauSeconds_ = tau;
  reset();
  return ok();
}

float LowPassProcessor::process(float input, Micros now, bool& valid) {
  (void)valid;
  if (!primed_) {
    // Seed with the first sample rather than with zero: starting from zero
    // makes every channel ramp up from nothing for the first few τ, which
    // looks exactly like a real transient and has fooled people.
    state_ = input;
    lastUs_ = now;
    primed_ = true;
    return input;
  }

  const double dt = (now > lastUs_)
                        ? static_cast<double>(now - lastUs_) * 1e-6
                        : 0.0;
  lastUs_ = now;
  if (dt <= 0.0) return static_cast<float>(state_);

  // alpha from the REAL interval.  After a one-second stall alpha approaches
  // 1 and the filter simply catches up, which is the honest answer: it has no
  // information about what happened during the gap.
  const double alpha = 1.0 - std::exp(-dt / static_cast<double>(tauSeconds_));
  state_ += alpha * (static_cast<double>(input) - state_);
  return static_cast<float>(state_);
}

void LowPassProcessor::reset() {
  state_ = 0.0;
  lastUs_ = 0;
  primed_ = false;
}

// ---------------------------------------------------------------------------
//  Deadband
// ---------------------------------------------------------------------------
const ModuleManifest& DeadbandProcessor::manifest() { return kDeadbandManifest; }

Status DeadbandProcessor::configure(const IConfigView& config) {
  const float threshold = config.getFloat("threshold", 0.0f);
  if (threshold < 0.0f) {
    return fail(ErrorCode::kInvalidArgument, "threshold must not be negative");
  }
  threshold_ = threshold;
  reset();
  return ok();
}

float DeadbandProcessor::process(float input, Micros now, bool& valid) {
  (void)now;
  (void)valid;
  if (!primed_) {
    held_ = input;
    primed_ = true;
    return held_;
  }
  if (std::fabs(input - held_) > threshold_) held_ = input;
  return held_;
}

void DeadbandProcessor::reset() {
  held_ = 0.0f;
  primed_ = false;
}

}  // namespace modules
}  // namespace lc
