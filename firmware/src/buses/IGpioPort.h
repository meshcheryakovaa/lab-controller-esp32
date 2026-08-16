// =============================================================================
//  buses/IGpioPort.h — pin-level access, mediated so it can be tested and
//  audited.
//
//  A driver never calls pinMode()/digitalWrite() directly.  Going through this
//  interface buys two things:
//    * the ESP32 implementation asserts the pin was actually claimed from
//      ResourceManager by the caller — a driver cannot quietly drive a pin it
//      does not own;
//    * a fake port in the tests can record the exact pulse sequence a bit-banged
//      driver produces, which is how the HX711 gain selection is verified
//      without an oscilloscope.
//
//  delayMicros() lives here rather than in IClock on purpose: it is a hardware
//  timing primitive for bit-banging, not a scheduling mechanism, and having it
//  on the port keeps the distinction visible.  Callers must keep the total under
//  a few tens of microseconds — this is still a cooperative system.
// =============================================================================
#pragma once

#include <cstdint>

#include "core/Error.h"
#include "core/Types.h"

namespace lc {

enum class PinMode : std::uint8_t {
  kInput = 0,
  kInputPullup,
  kInputPulldown,
  kOutput,
  kOpenDrain,
};

class IGpioPort {
 public:
  virtual ~IGpioPort() = default;

  virtual Status configure(std::uint8_t pin, PinMode mode,
                           DeviceHandle owner) = 0;
  virtual Status write(std::uint8_t pin, bool high) = 0;
  virtual Result<bool> read(std::uint8_t pin) = 0;

  // Busy-wait.  Only for bit-banged protocols, only for short pulses.
  virtual void delayMicros(std::uint32_t microseconds) const = 0;
};

}  // namespace lc
