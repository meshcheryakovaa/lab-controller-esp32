// =============================================================================
//  buses/IPwmOut.h — hardware PWM, mediated like every other peripheral.
//
//  On the ESP32 this is LEDC: sixteen channels shared between four timers, and
//  a channel is a resource exactly like a pin.  Drivers therefore ask for one
//  and get a handle; they never pick a channel number, never configure a timer
//  and never learn how many bits of resolution the board ended up with.
//
//  Duty is a float in 0..1 at this interface.  Resolution arithmetic — 8 bits
//  here, 12 there, and a different maximum on the S3 — is the port's problem,
//  and duplicating it in five drivers is how one of them ends up commanding
//  100 % as 255 on a 12-bit channel and delivering 6 %.
// =============================================================================
#pragma once

#include <cstdint>

#include "core/Error.h"
#include "core/Types.h"

namespace lc {

using PwmChannel = std::uint8_t;
inline constexpr PwmChannel kInvalidPwmChannel = 0xFF;

class IPwmOut {
 public:
  virtual ~IPwmOut() = default;

  // Claims a hardware channel and binds it to `pin`.  The caller must already
  // own the pin from ResourceManager; the ESP32 implementation asserts it.
  virtual Result<PwmChannel> attach(std::uint8_t pin, std::uint32_t frequencyHz,
                                    DeviceHandle owner) = 0;

  // `duty` is 0..1 and is clamped, never wrapped: a driver bug that asks for
  // 1.2 must produce full output, not 20 %.
  virtual Status write(PwmChannel channel, float duty) = 0;

  virtual void detach(PwmChannel channel) = 0;

  virtual std::uint8_t channelCount() const = 0;
  virtual std::uint8_t channelsInUse() const = 0;
};

}  // namespace lc
