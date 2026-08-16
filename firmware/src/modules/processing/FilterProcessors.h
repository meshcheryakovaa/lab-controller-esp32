// =============================================================================
//  modules/processing/FilterProcessors.h — median, low-pass, deadband (§14).
//
//  Three filters that answer three different questions:
//
//    * MEDIAN removes single wild samples (an HX711 that catches a switching
//      transient) without smearing them into the neighbours the way an average
//      does.  A spike survives an average; it does not survive a median.
//    * LOW-PASS removes broadband noise with a stated time constant, so the
//      operator can say "this channel is smoothed with τ = 2 s" rather than
//      "there is a filter of some kind on it".
//    * DEADBAND removes *reporting* noise: a value that has not really moved
//      stops producing new points.  It is the only one of the three that
//      changes the value it emits on purpose, and it is documented as such.
//
//  All three are fixed-capacity and allocate nothing after configure().
// =============================================================================
#pragma once

#include "core/IModule.h"

namespace lc {
namespace modules {

// ---------------------------------------------------------------------------
//  Median — rejects outliers instead of averaging them in.
// ---------------------------------------------------------------------------
class MedianProcessor final : public IProcessor {
 public:
  // Odd windows only: an even window has no middle element, and averaging the
  // two central samples reintroduces exactly the outlier sensitivity a median
  // exists to remove.
  static constexpr std::size_t kMaxWindow = 31;

  static const ModuleManifest& manifest();
  static IProcessor* create() { return new MedianProcessor(); }

  const char* typeId() const override { return "median"; }
  Status configure(const IConfigView& config) override;
  float process(float input, Micros now, bool& valid) override;
  void reset() override;

  std::size_t window() const { return window_; }

 private:
  float buffer_[kMaxWindow] = {0.0f};
  float scratch_[kMaxWindow] = {0.0f};
  std::size_t window_ = 5;
  std::size_t head_ = 0;
  std::size_t filled_ = 0;
};

// ---------------------------------------------------------------------------
//  Low-pass — first-order IIR with a time constant in SECONDS.
// ---------------------------------------------------------------------------
//  The coefficient is derived from the actual interval between samples, not
//  assumed: a channel whose rate changes (or whose device stalls for a second)
//  would otherwise silently change its own cut-off frequency.
// ---------------------------------------------------------------------------
class LowPassProcessor final : public IProcessor {
 public:
  static const ModuleManifest& manifest();
  static IProcessor* create() { return new LowPassProcessor(); }

  const char* typeId() const override { return "low_pass"; }
  Status configure(const IConfigView& config) override;
  float process(float input, Micros now, bool& valid) override;
  void reset() override;

  float timeConstant() const { return tauSeconds_; }

 private:
  float tauSeconds_ = 1.0f;
  double state_ = 0.0;
  Micros lastUs_ = 0;
  bool primed_ = false;
};

// ---------------------------------------------------------------------------
//  Deadband — suppresses movement smaller than a threshold.
// ---------------------------------------------------------------------------
//  Honest about what it is: this stage DISTORTS the signal.  It is here to keep
//  a log from filling with the last digit of a stable reading, and it quantises
//  the output to the threshold in exchange.  Never put it before a control loop.
// ---------------------------------------------------------------------------
class DeadbandProcessor final : public IProcessor {
 public:
  static const ModuleManifest& manifest();
  static IProcessor* create() { return new DeadbandProcessor(); }

  const char* typeId() const override { return "deadband"; }
  Status configure(const IConfigView& config) override;
  float process(float input, Micros now, bool& valid) override;
  void reset() override;

 private:
  float threshold_ = 0.0f;
  float held_ = 0.0f;
  bool primed_ = false;
};

}  // namespace modules
}  // namespace lc
