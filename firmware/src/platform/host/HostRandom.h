// =============================================================================
//  platform/host/HostRandom.h — std::random_device for the host build.
// =============================================================================
#pragma once

#include <random>

#include "core/IRandom.h"

namespace lc {
namespace platform {

class HostRandom final : public IRandom {
 public:
  void fill(std::uint8_t* out, std::size_t bytes) override {
    for (std::size_t i = 0; i < bytes; ++i) {
      out[i] = static_cast<std::uint8_t>(device_() & 0xffu);
    }
  }

 private:
  std::random_device device_;
};

}  // namespace platform
}  // namespace lc
