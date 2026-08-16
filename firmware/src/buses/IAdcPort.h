// =============================================================================
//  buses/IAdcPort.h — analogue input.
//
//  The ESP32's ADC is not a nice ADC: it is non-linear, noisy, and needs the
//  factory calibration data in eFuse to produce anything resembling volts.
//  Hiding all of that behind two calls keeps the mess in one place, and lets an
//  analogue driver be tested against a scripted sequence of counts.
// =============================================================================
#pragma once

#include <cstdint>

#include "core/Error.h"
#include "core/Types.h"

namespace lc {

// Input range.  Higher attenuation = wider range, worse linearity near the ends.
enum class AdcAttenuation : std::uint8_t {
  kDb0 = 0,    // ~0.10 .. 0.95 V
  kDb2_5,      // ~0.10 .. 1.25 V
  kDb6,        // ~0.15 .. 1.75 V
  kDb11,       // ~0.15 .. 2.45 V  (default; the usable range is NOT 0..3.3 V)
};

class IAdcPort {
 public:
  virtual ~IAdcPort() = default;

  virtual Status configure(std::uint8_t pin, AdcAttenuation attenuation,
                           DeviceHandle owner) = 0;

  // Raw converter counts (12-bit on the ESP32).
  virtual Result<std::uint16_t> readRaw(std::uint8_t pin) = 0;

  // Millivolts at the pin, using the chip's factory calibration when present.
  // Reported honestly: if the chip has no calibration data, the implementation
  // says so once at start-up rather than pretending to be accurate.
  virtual Result<float> readMillivolts(std::uint8_t pin) = 0;

  virtual bool calibrated() const = 0;
};

}  // namespace lc
