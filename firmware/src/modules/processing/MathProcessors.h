// =============================================================================
//  modules/processing/MathProcessors.h — derivative, integral, statistics,
//  clamp (§14).
//
//  These are the stages that turn a measurement into a different measurement:
//  a mass channel becomes an evaporation rate, a power channel becomes energy.
//  They are where a laboratory rig stops being a set of sensors and starts
//  answering the question the experiment actually asked.
//
//  All four are rate-aware: they take their time base from the sample
//  timestamps, never from an assumed interval, because a channel whose device
//  stalls for a second would otherwise report a derivative that never happened.
// =============================================================================
#pragma once

#include "core/IModule.h"

namespace lc {
namespace modules {

// ---------------------------------------------------------------------------
//  Derivative — d(value)/dt over a window, in units per second/minute/hour.
// ---------------------------------------------------------------------------
class DerivativeProcessor final : public IProcessor {
 public:
  static constexpr std::size_t kMaxWindow = 64;

  static const ModuleManifest& manifest();
  static IProcessor* create() { return new DerivativeProcessor(); }

  const char* typeId() const override { return "derivative"; }
  Status configure(const IConfigView& config) override;
  float process(float input, Micros now, bool& valid) override;
  void reset() override;

 private:
  float values_[kMaxWindow] = {0.0f};
  Micros times_[kMaxWindow] = {0};
  std::size_t window_ = 8;   // samples between the two points differenced
  std::size_t head_ = 0;
  std::size_t filled_ = 0;
  float perSeconds_ = 1.0f;  // 1 / 60 / 3600
};

// ---------------------------------------------------------------------------
//  Integral — trapezoidal accumulation with an explicit reset.
// ---------------------------------------------------------------------------
class IntegralProcessor final : public IProcessor {
 public:
  static const ModuleManifest& manifest();
  static IProcessor* create() { return new IntegralProcessor(); }

  const char* typeId() const override { return "integral"; }
  Status configure(const IConfigView& config) override;
  float process(float input, Micros now, bool& valid) override;
  void reset() override;

  double total() const { return total_; }

 private:
  double total_ = 0.0;
  double previous_ = 0.0;
  Micros lastUs_ = 0;
  bool primed_ = false;
  float perSeconds_ = 1.0f;
  float minimum_ = 0.0f;
  float maximum_ = 0.0f;
  bool clamped_ = false;
};

// ---------------------------------------------------------------------------
//  Statistics — one rolling statistic over a window.
// ---------------------------------------------------------------------------
class StatisticsProcessor final : public IProcessor {
 public:
  static constexpr std::size_t kMaxWindow = 64;

  enum class Statistic : std::uint8_t {
    kMean = 0,
    kMin,
    kMax,
    kPeakToPeak,
    kStdDev,
  };

  static const ModuleManifest& manifest();
  static IProcessor* create() { return new StatisticsProcessor(); }

  const char* typeId() const override { return "statistics"; }
  Status configure(const IConfigView& config) override;
  float process(float input, Micros now, bool& valid) override;
  void reset() override;

 private:
  float buffer_[kMaxWindow] = {0.0f};
  std::size_t window_ = 16;
  std::size_t head_ = 0;
  std::size_t filled_ = 0;
  Statistic statistic_ = Statistic::kMean;
};

// ---------------------------------------------------------------------------
//  Clamp — bounds a value, or marks it invalid outside them.
// ---------------------------------------------------------------------------
class ClampProcessor final : public IProcessor {
 public:
  enum class Mode : std::uint8_t { kSaturate = 0, kInvalidate };

  static const ModuleManifest& manifest();
  static IProcessor* create() { return new ClampProcessor(); }

  const char* typeId() const override { return "clamp"; }
  Status configure(const IConfigView& config) override;
  float process(float input, Micros now, bool& valid) override;
  void reset() override {}

 private:
  float minimum_ = 0.0f;
  float maximum_ = 0.0f;
  Mode mode_ = Mode::kSaturate;
};

}  // namespace modules
}  // namespace lc
