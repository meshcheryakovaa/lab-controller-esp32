// =============================================================================
//  platform/esp32/Esp32Clock.h — IClock on the device.
//
//  esp_timer_get_time() is a genuine 64-bit monotonic microsecond counter, so
//  unlike millis()/micros() it does not wrap after 71 minutes.  Every timing
//  decision in the firmware ultimately reads this.
// =============================================================================
#pragma once

#include <esp_timer.h>
#include <sys/time.h>

#include "core/Clock.h"

namespace lc {
namespace platform {

class Esp32Clock final : public IClock {
 public:
  Micros nowMicros() const override {
    return static_cast<Micros>(esp_timer_get_time());
  }

  EpochMs epochMillis() const override {
    timeval tv{};
    if (gettimeofday(&tv, nullptr) != 0) return 0;
    // Before SNTP has run, the RTC reads a few seconds past the epoch.  Report
    // 0 in that case so data logs can honestly record "no wall clock".
    if (tv.tv_sec < 1600000000) return 0;  // 2020-09-13
    return static_cast<EpochMs>(tv.tv_sec) * 1000ULL +
           static_cast<EpochMs>(tv.tv_usec) / 1000ULL;
  }
};

}  // namespace platform
}  // namespace lc
