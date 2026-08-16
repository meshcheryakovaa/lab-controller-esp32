// =============================================================================
//  core/Clock.h — the only way the core layer learns what time it is.
//
//  Injecting the clock is what makes Scheduler, RuleEngine, ExperimentManager
//  and the filter chain testable on a host: a test can advance time by an hour
//  in a microsecond.  Nothing in core/ or services/ may call millis()/micros()
//  directly.
// =============================================================================
#pragma once

#include "core/Types.h"

namespace lc {

class IClock {
 public:
  virtual ~IClock() = default;

  // Monotonic since boot.  Must never go backwards and must not be affected
  // by NTP corrections.
  virtual Micros nowMicros() const = 0;

  // Wall-clock milliseconds since the Unix epoch, or 0 when the device has no
  // trustworthy time yet.  Data logs must record which of the two they used.
  virtual EpochMs epochMillis() const = 0;

  bool epochValid() const { return epochMillis() != 0; }

  Millis nowMillis() const {
    return static_cast<Millis>(nowMicros() / 1000ULL);
  }
};

// Deterministic clock for unit tests: time only moves when a test moves it.
class ManualClock final : public IClock {
 public:
  Micros nowMicros() const override { return micros_; }
  EpochMs epochMillis() const override { return epoch_; }

  void advanceMicros(Micros delta) { micros_ += delta; }
  void advanceMillis(Millis delta) { micros_ += static_cast<Micros>(delta) * 1000ULL; }
  void setEpochMillis(EpochMs epoch) { epoch_ = epoch; }

 private:
  Micros micros_ = 0;
  EpochMs epoch_ = 0;
};

}  // namespace lc
