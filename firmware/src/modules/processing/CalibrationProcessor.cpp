#include "modules/processing/CalibrationProcessor.h"

#include <cstring>

namespace lc {
namespace modules {
namespace {

constexpr ParamOption kModeOptions[] = {
    {"identity", "None (pass through)"},
    {"polynomial", "Polynomial / linear / offset"},
    {"table", "Piecewise linear table"},
};

constexpr ParamSpec kParams[] = {
    ParamSpec{"type", "Calibration type", ParamType::kSelect, nullptr,
              "Offset and linear calibrations are polynomials of order 0 and 1",
              0.0f, 0.0f, 0.0f, "identity",
              kModeOptions,
              static_cast<std::uint8_t>(sizeof(kModeOptions) / sizeof(kModeOptions[0])),
              PinUse::kDigitalInput, true, false, nullptr},
};

constexpr ModuleManifest kManifest = {
    /*id*/ "calibration",
    /*name*/ "Calibration",
    /*category*/ ModuleCategory::kProcessing,
    /*description*/ "Maps a raw reading to a physical quantity using "
                    "coefficients fitted from reference points",
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

const ModuleManifest& CalibrationProcessor::manifest() { return kManifest; }

void CalibrationProcessor::applyFit(const PolynomialFit& fit) {
  fit_ = fit;
  mode_ = Mode::kPolynomial;
}

Status CalibrationProcessor::configure(const IConfigView& config) {
  const char* type = config.getString("type", "identity");

  if (std::strcmp(type, "identity") == 0) {
    mode_ = Mode::kIdentity;
    return ok();
  }

  if (std::strcmp(type, "polynomial") == 0) {
    const std::size_t count = config.arraySize("coefficients");
    if (count == 0 || count > kMaxPolynomialOrder + 1) {
      return fail(ErrorCode::kInvalidArgument,
                  "coefficients: 1..5 values expected");
    }
    fit_ = PolynomialFit{};
    fit_.order = count - 1;
    for (std::size_t i = 0; i < count; ++i) {
      fit_.coefficients[i] = config.getFloatAt("coefficients", i, 0.0f);
    }
    fit_.xCenter = config.getFloat("x_center", 0.0f);
    fit_.xScale = config.getFloat("x_scale", 1.0f);
    if (fit_.xScale == 0.0) {
      return fail(ErrorCode::kInvalidArgument, "x_scale must not be zero");
    }
    mode_ = Mode::kPolynomial;
    return ok();
  }

  if (std::strcmp(type, "table") == 0) {
    const std::size_t count = config.arraySize("table_x");
    if (count < 2 || count != config.arraySize("table_y")) {
      return fail(ErrorCode::kCalibrationInsufficientPoints,
                  "table_x/table_y must match and hold >= 2 points");
    }
    tableCount_ = (count > kMaxTablePoints) ? kMaxTablePoints : count;
    for (std::size_t i = 0; i < tableCount_; ++i) {
      tableX_[i] = config.getFloatAt("table_x", i, 0.0f);
      tableY_[i] = config.getFloatAt("table_y", i, 0.0f);
    }
    // Interpolation assumes a strictly increasing abscissa; verifying it here
    // turns a silently wrong measurement into a configuration error.
    for (std::size_t i = 1; i < tableCount_; ++i) {
      if (!(tableX_[i] > tableX_[i - 1])) {
        return fail(ErrorCode::kInvalidArgument,
                    "table_x must strictly increase");
      }
    }
    mode_ = Mode::kTable;
    return ok();
  }

  return fail(ErrorCode::kInvalidArgument, "unknown calibration type");
}

float CalibrationProcessor::interpolate(float x) const {
  if (tableCount_ == 0) return x;
  // Clamp rather than extrapolate: extrapolating a lookup table is how a load
  // cell reports 900 g when the wire falls off.
  if (x <= tableX_[0]) return tableY_[0];
  if (x >= tableX_[tableCount_ - 1]) return tableY_[tableCount_ - 1];

  for (std::size_t i = 1; i < tableCount_; ++i) {
    if (x <= tableX_[i]) {
      const float span = tableX_[i] - tableX_[i - 1];
      const float t = (span != 0.0f) ? ((x - tableX_[i - 1]) / span) : 0.0f;
      return tableY_[i - 1] + t * (tableY_[i] - tableY_[i - 1]);
    }
  }
  return tableY_[tableCount_ - 1];
}

float CalibrationProcessor::process(float input, Micros now, bool& valid) {
  (void)now;
  (void)valid;
  switch (mode_) {
    case Mode::kIdentity:
      return input;
    case Mode::kPolynomial:
      return static_cast<float>(fit_.evaluate(static_cast<double>(input)));
    case Mode::kTable:
      return interpolate(input);
  }
  return input;
}

}  // namespace modules
}  // namespace lc
