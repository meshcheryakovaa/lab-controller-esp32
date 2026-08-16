#include "modules/processing/MathProcessors.h"

#include <cmath>
#include <cstring>

namespace lc {
namespace modules {
namespace {

constexpr ParamOption kPerUnitOptions[] = {
    {"s", "per second"},
    {"min", "per minute"},
    {"h", "per hour"},
};

float perUnitSeconds(const char* unit) {
  if (std::strcmp(unit, "min") == 0) return 60.0f;
  if (std::strcmp(unit, "h") == 0) return 3600.0f;
  return 1.0f;
}

// ---------------------------------------------------------------------------
//  Derivative
// ---------------------------------------------------------------------------
constexpr ParamSpec kDerivativeParams[] = {
    ParamSpec{"window", "Window", ParamType::kInt, "samples",
              "Samples between the two points differenced. Larger is quieter "
              "and lags more; 1 is the raw difference and is usually noise.",
              1.0f, static_cast<float>(DerivativeProcessor::kMaxWindow - 1),
              1.0f, "8",
              nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},
    ParamSpec{"per", "Rate unit", ParamType::kSelect, nullptr,
              "Denominator of the result: g per second or g per hour",
              0.0f, 0.0f, 0.0f, "s",
              kPerUnitOptions,
              static_cast<std::uint8_t>(sizeof(kPerUnitOptions) /
                                        sizeof(kPerUnitOptions[0])),
              PinUse::kDigitalInput, true, false, nullptr},
};

constexpr ModuleManifest kDerivativeManifest = {
    /*id*/ "derivative",
    /*name*/ "Derivative",
    /*category*/ ModuleCategory::kProcessing,
    /*description*/ "Rate of change over a window, using the real sample "
                    "timestamps — evaporation rate, heating rate, drift",
    /*bus*/ BusRequirement::kNone,
    /*params*/ kDerivativeParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kDerivativeParams) /
                                             sizeof(kDerivativeParams[0])),
    /*channels*/ nullptr,
    /*channelCount*/ 0,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 0,
    /*minSampleIntervalUs*/ 0,
    /*schemaVersion*/ 1,
};

// ---------------------------------------------------------------------------
//  Integral
// ---------------------------------------------------------------------------
constexpr ParamSpec kIntegralParams[] = {
    ParamSpec{"per", "Time unit", ParamType::kSelect, nullptr,
              "Unit the input rate is expressed in: mL/min integrates to mL",
              0.0f, 0.0f, 0.0f, "s",
              kPerUnitOptions,
              static_cast<std::uint8_t>(sizeof(kPerUnitOptions) /
                                        sizeof(kPerUnitOptions[0])),
              PinUse::kDigitalInput, true, false, nullptr},
    ParamSpec{"min", "Lower bound", ParamType::kFloat, nullptr,
              "Accumulator floor; leave equal to the upper bound for none",
              -1.0e9f, 1.0e9f, 0.1f, "0",
              nullptr, 0, PinUse::kDigitalInput, false, true, nullptr},
    ParamSpec{"max", "Upper bound", ParamType::kFloat, nullptr,
              "Accumulator ceiling; leave equal to the lower bound for none",
              -1.0e9f, 1.0e9f, 0.1f, "0",
              nullptr, 0, PinUse::kDigitalInput, false, true, nullptr},
};

constexpr ModuleManifest kIntegralManifest = {
    /*id*/ "integral",
    /*name*/ "Integral",
    /*category*/ ModuleCategory::kProcessing,
    /*description*/ "Trapezoidal accumulation over time — total dosed volume, "
                    "energy from power, charge from current",
    /*bus*/ BusRequirement::kNone,
    /*params*/ kIntegralParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kIntegralParams) /
                                             sizeof(kIntegralParams[0])),
    /*channels*/ nullptr,
    /*channelCount*/ 0,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 0,
    /*minSampleIntervalUs*/ 0,
    /*schemaVersion*/ 1,
};

// ---------------------------------------------------------------------------
//  Statistics
// ---------------------------------------------------------------------------
constexpr ParamOption kStatisticOptions[] = {
    {"mean", "Mean"},
    {"min", "Minimum"},
    {"max", "Maximum"},
    {"peak_to_peak", "Peak to peak"},
    {"stddev", "Standard deviation"},
};

constexpr ParamSpec kStatisticsParams[] = {
    ParamSpec{"statistic", "Statistic", ParamType::kSelect, nullptr,
              "Which number the channel reports",
              0.0f, 0.0f, 0.0f, "mean",
              kStatisticOptions,
              static_cast<std::uint8_t>(sizeof(kStatisticOptions) /
                                        sizeof(kStatisticOptions[0])),
              PinUse::kDigitalInput, true, false, nullptr},
    ParamSpec{"window", "Window", ParamType::kInt, "samples",
              "Number of samples the statistic covers",
              2.0f, static_cast<float>(StatisticsProcessor::kMaxWindow), 1.0f,
              "16",
              nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},
};

constexpr ModuleManifest kStatisticsManifest = {
    /*id*/ "statistics",
    /*name*/ "Statistics",
    /*category*/ ModuleCategory::kProcessing,
    /*description*/ "Rolling mean, extremes, span or standard deviation — "
                    "peak-to-peak is how you measure noise you cannot see",
    /*bus*/ BusRequirement::kNone,
    /*params*/ kStatisticsParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kStatisticsParams) /
                                             sizeof(kStatisticsParams[0])),
    /*channels*/ nullptr,
    /*channelCount*/ 0,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 0,
    /*minSampleIntervalUs*/ 0,
    /*schemaVersion*/ 1,
};

// ---------------------------------------------------------------------------
//  Clamp
// ---------------------------------------------------------------------------
constexpr ParamOption kClampModeOptions[] = {
    {"saturate", "Hold at the bound"},
    {"invalidate", "Discard the sample"},
};

constexpr ParamSpec kClampParams[] = {
    ParamSpec{"min", "Lower bound", ParamType::kFloat, nullptr, nullptr,
              -1.0e9f, 1.0e9f, 0.1f, "0",
              nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},
    ParamSpec{"max", "Upper bound", ParamType::kFloat, nullptr, nullptr,
              -1.0e9f, 1.0e9f, 0.1f, "100",
              nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},
    ParamSpec{"mode", "Outside the bounds", ParamType::kSelect, nullptr,
              "Saturating hides the excursion from the channel's own range "
              "check; discarding keeps it visible as a gap.",
              0.0f, 0.0f, 0.0f, "saturate",
              kClampModeOptions,
              static_cast<std::uint8_t>(sizeof(kClampModeOptions) /
                                        sizeof(kClampModeOptions[0])),
              PinUse::kDigitalInput, true, false, nullptr},
};

constexpr ModuleManifest kClampManifest = {
    /*id*/ "clamp",
    /*name*/ "Clamp",
    /*category*/ ModuleCategory::kProcessing,
    /*description*/ "Bounds a value, or marks it invalid outside the bounds",
    /*bus*/ BusRequirement::kNone,
    /*params*/ kClampParams,
    /*paramCount*/ static_cast<std::uint8_t>(sizeof(kClampParams) /
                                             sizeof(kClampParams[0])),
    /*channels*/ nullptr,
    /*channelCount*/ 0,
    /*maxInstances*/ 0,
    /*defaultSampleIntervalUs*/ 0,
    /*minSampleIntervalUs*/ 0,
    /*schemaVersion*/ 1,
};

}  // namespace

// ---------------------------------------------------------------------------
//  Derivative
// ---------------------------------------------------------------------------
const ModuleManifest& DerivativeProcessor::manifest() {
  return kDerivativeManifest;
}

Status DerivativeProcessor::configure(const IConfigView& config) {
  const std::int32_t window = config.getInt("window", 8);
  if (window < 1 || static_cast<std::size_t>(window) >= kMaxWindow) {
    return fail(ErrorCode::kInvalidArgument, "window must be 1..63");
  }
  window_ = static_cast<std::size_t>(window);
  perSeconds_ = perUnitSeconds(config.getString("per", "s"));
  reset();
  return ok();
}

float DerivativeProcessor::process(float input, Micros now, bool& valid) {
  values_[head_] = input;
  times_[head_] = now;
  const std::size_t current = head_;
  head_ = (head_ + 1) % kMaxWindow;
  if (filled_ <= window_) ++filled_;

  // Until the window has filled there is no second point to difference
  // against.  Reporting 0 would be a lie that looks like a steady signal.
  if (filled_ <= window_) {
    valid = false;
    return 0.0f;
  }

  const std::size_t past = (current + kMaxWindow - window_) % kMaxWindow;
  const Micros then = times_[past];
  if (now <= then) {
    // Clock did not advance between the two samples: no rate is defined.
    valid = false;
    return 0.0f;
  }

  const double dt = static_cast<double>(now - then) * 1e-6;
  const double slope = (static_cast<double>(input) - values_[past]) / dt;
  return static_cast<float>(slope * static_cast<double>(perSeconds_));
}

void DerivativeProcessor::reset() {
  head_ = 0;
  filled_ = 0;
  for (std::size_t i = 0; i < kMaxWindow; ++i) {
    values_[i] = 0.0f;
    times_[i] = 0;
  }
}

// ---------------------------------------------------------------------------
//  Integral
// ---------------------------------------------------------------------------
const ModuleManifest& IntegralProcessor::manifest() { return kIntegralManifest; }

Status IntegralProcessor::configure(const IConfigView& config) {
  perSeconds_ = perUnitSeconds(config.getString("per", "s"));
  minimum_ = config.getFloat("min", 0.0f);
  maximum_ = config.getFloat("max", 0.0f);
  if (maximum_ < minimum_) {
    return fail(ErrorCode::kInvalidArgument, "max must not be below min");
  }
  clamped_ = (maximum_ > minimum_);
  reset();
  return ok();
}

float IntegralProcessor::process(float input, Micros now, bool& valid) {
  (void)valid;
  if (!primed_) {
    previous_ = input;
    lastUs_ = now;
    primed_ = true;
    return static_cast<float>(total_);
  }

  if (now > lastUs_) {
    const double dt = static_cast<double>(now - lastUs_) * 1e-6;
    // Trapezoid, not rectangle: on a ramp the rectangle rule accumulates an
    // error proportional to the run length, and a dosing total that drifts by
    // 3% over an hour is worse than no total at all.
    total_ += 0.5 * (previous_ + static_cast<double>(input)) * dt /
              static_cast<double>(perSeconds_);
    if (clamped_) {
      if (total_ < minimum_) total_ = minimum_;
      if (total_ > maximum_) total_ = maximum_;
    }
  }
  previous_ = input;
  lastUs_ = now;
  return static_cast<float>(total_);
}

void IntegralProcessor::reset() {
  total_ = 0.0;
  previous_ = 0.0;
  lastUs_ = 0;
  primed_ = false;
}

// ---------------------------------------------------------------------------
//  Statistics
// ---------------------------------------------------------------------------
const ModuleManifest& StatisticsProcessor::manifest() {
  return kStatisticsManifest;
}

Status StatisticsProcessor::configure(const IConfigView& config) {
  const char* statistic = config.getString("statistic", "mean");
  if (std::strcmp(statistic, "mean") == 0) {
    statistic_ = Statistic::kMean;
  } else if (std::strcmp(statistic, "min") == 0) {
    statistic_ = Statistic::kMin;
  } else if (std::strcmp(statistic, "max") == 0) {
    statistic_ = Statistic::kMax;
  } else if (std::strcmp(statistic, "peak_to_peak") == 0) {
    statistic_ = Statistic::kPeakToPeak;
  } else if (std::strcmp(statistic, "stddev") == 0) {
    statistic_ = Statistic::kStdDev;
  } else {
    return fail(ErrorCode::kInvalidArgument, "unknown statistic");
  }

  const std::int32_t window = config.getInt("window", 16);
  if (window < 2 || static_cast<std::size_t>(window) > kMaxWindow) {
    return fail(ErrorCode::kInvalidArgument, "window must be 2..64");
  }
  window_ = static_cast<std::size_t>(window);
  reset();
  return ok();
}

float StatisticsProcessor::process(float input, Micros now, bool& valid) {
  (void)now;
  buffer_[head_] = input;
  head_ = (head_ + 1) % window_;
  if (filled_ < window_) ++filled_;

  if (filled_ < 2) {
    valid = false;
    return input;
  }

  double sum = 0.0;
  float minimum = buffer_[0];
  float maximum = buffer_[0];
  for (std::size_t i = 0; i < filled_; ++i) {
    sum += buffer_[i];
    if (buffer_[i] < minimum) minimum = buffer_[i];
    if (buffer_[i] > maximum) maximum = buffer_[i];
  }
  const double mean = sum / static_cast<double>(filled_);

  switch (statistic_) {
    case Statistic::kMean:
      return static_cast<float>(mean);
    case Statistic::kMin:
      return minimum;
    case Statistic::kMax:
      return maximum;
    case Statistic::kPeakToPeak:
      return maximum - minimum;
    case Statistic::kStdDev: {
      // Two-pass, deliberately.  The one-pass E[x²]-E[x]² form subtracts two
      // nearly equal large numbers, and on a 20 °C signal with 0.01 °C noise
      // it returns garbage — sometimes a negative variance.
      double sumSquares = 0.0;
      for (std::size_t i = 0; i < filled_; ++i) {
        const double deviation = static_cast<double>(buffer_[i]) - mean;
        sumSquares += deviation * deviation;
      }
      // Sample standard deviation: this is an estimate from a window, not the
      // whole population.
      return static_cast<float>(
          std::sqrt(sumSquares / static_cast<double>(filled_ - 1)));
    }
  }
  return static_cast<float>(mean);
}

void StatisticsProcessor::reset() {
  head_ = 0;
  filled_ = 0;
  for (std::size_t i = 0; i < kMaxWindow; ++i) buffer_[i] = 0.0f;
}

// ---------------------------------------------------------------------------
//  Clamp
// ---------------------------------------------------------------------------
const ModuleManifest& ClampProcessor::manifest() { return kClampManifest; }

Status ClampProcessor::configure(const IConfigView& config) {
  minimum_ = config.getFloat("min", 0.0f);
  maximum_ = config.getFloat("max", 100.0f);
  if (!(maximum_ > minimum_)) {
    return fail(ErrorCode::kInvalidArgument, "max must be above min");
  }
  const char* mode = config.getString("mode", "saturate");
  if (std::strcmp(mode, "invalidate") == 0) {
    mode_ = Mode::kInvalidate;
  } else if (std::strcmp(mode, "saturate") == 0) {
    mode_ = Mode::kSaturate;
  } else {
    return fail(ErrorCode::kInvalidArgument, "unknown clamp mode");
  }
  return ok();
}

float ClampProcessor::process(float input, Micros now, bool& valid) {
  (void)now;
  if (input >= minimum_ && input <= maximum_) return input;
  if (mode_ == Mode::kInvalidate) {
    valid = false;
    return input;
  }
  return (input < minimum_) ? minimum_ : maximum_;
}

}  // namespace modules
}  // namespace lc
