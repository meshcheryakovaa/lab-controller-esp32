// =============================================================================
//  modules/processing/CalibrationProcessor.h — first stage of a pipeline (§12).
//
//  Calibration is a processor like any other, which is the point: it can be
//  reordered, disabled, or applied to a virtual channel just as easily as to a
//  load cell.  ChannelManager records the value at this stage's output as the
//  channel's "calibrated" value.
//
//  Configuration (see docs/channels.md):
//    { "type": "polynomial", "coefficients": [...], "x_center": ..,
//      "x_scale": .. }
//    { "type": "table", "table_x": [...], "table_y": [...] }
// =============================================================================
#pragma once

#include "core/IModule.h"
#include "services/CalibrationSolver.h"

namespace lc {
namespace modules {

class CalibrationProcessor final : public IProcessor {
 public:
  enum class Mode : std::uint8_t { kIdentity = 0, kPolynomial, kTable };

  static constexpr std::size_t kMaxTablePoints = kMaxCalibrationPoints;

  static const ModuleManifest& manifest();
  static IProcessor* create() { return new CalibrationProcessor(); }

  const char* typeId() const override { return "calibration"; }
  Status configure(const IConfigView& config) override;
  float process(float input, Micros now, bool& valid) override;
  void reset() override {}

  // Direct programmatic setup, used by CalibrationManager right after a fit
  // and by unit tests.
  void applyFit(const PolynomialFit& fit);

  Mode mode() const { return mode_; }

 private:
  Mode mode_ = Mode::kIdentity;
  PolynomialFit fit_;
  float tableX_[kMaxTablePoints] = {0.0f};
  float tableY_[kMaxTablePoints] = {0.0f};
  std::size_t tableCount_ = 0;

  float interpolate(float x) const;
};

}  // namespace modules
}  // namespace lc
