// =============================================================================
//  modules/processing/MovingAverageProcessor.h — boxcar filter (§14).
//
//  Fixed-capacity ring buffer, running sum, O(1) per sample.  While the window
//  is filling, the stage reports the partial average and stays valid; that is
//  the right behaviour for a moving average (unlike a median, where a partial
//  window would be misleading).
// =============================================================================
#pragma once

#include "core/IModule.h"

namespace lc {
namespace modules {

class MovingAverageProcessor final : public IProcessor {
 public:
  static constexpr std::size_t kMaxWindow = 64;

  static const ModuleManifest& manifest();
  static IProcessor* create() { return new MovingAverageProcessor(); }

  const char* typeId() const override { return "moving_average"; }
  Status configure(const IConfigView& config) override;
  float process(float input, Micros now, bool& valid) override;
  void reset() override;

  std::size_t window() const { return window_; }
  std::size_t filled() const { return filled_; }

 private:
  float buffer_[kMaxWindow] = {0.0f};
  std::size_t window_ = 8;
  std::size_t head_ = 0;
  std::size_t filled_ = 0;
  double sum_ = 0.0;  // double: a float accumulator drifts over hours at 80 Hz
};

}  // namespace modules
}  // namespace lc
