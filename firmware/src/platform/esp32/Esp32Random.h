// =============================================================================
//  platform/esp32/Esp32Random.h — the SoC's hardware RNG.
//
//  esp_random() is only guaranteed to be a true RNG once RF (Wi-Fi or BT) is
//  running; before that it is a PRNG seeded at boot.  Sessions are created long
//  after the network is up — a login has to arrive over it — so this is sound,
//  and saying it here is cheaper than rediscovering it.
// =============================================================================
#pragma once

#include <esp_random.h>

#include "core/IRandom.h"

namespace lc {
namespace platform {

class Esp32Random final : public IRandom {
 public:
  void fill(std::uint8_t* out, std::size_t bytes) override {
    esp_fill_random(out, bytes);
  }
};

}  // namespace platform
}  // namespace lc
