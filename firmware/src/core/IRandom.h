// =============================================================================
//  core/IRandom.h — the only source of unpredictable bytes.
//
//  A port rather than a call to rand(): session tokens are the one thing in
//  this firmware whose value must not be guessable, and a host test that used
//  the same generator as the board would be testing nothing.  On the ESP32 this
//  is the hardware RNG; on the host it is std::random_device; in a test it is
//  whatever the test needs it to be.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace lc {

class IRandom {
 public:
  virtual ~IRandom() = default;
  virtual void fill(std::uint8_t* out, std::size_t bytes) = 0;
};

}  // namespace lc
