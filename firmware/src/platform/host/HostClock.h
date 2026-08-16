// =============================================================================
//  platform/host/HostClock.h — IClock backed by std::chrono, for the native
//  test build and for desktop prototyping of the services layer.
// =============================================================================
#pragma once

#include "core/Clock.h"

namespace lc {
namespace platform {

class HostClock final : public IClock {
 public:
  HostClock();
  Micros nowMicros() const override;
  EpochMs epochMillis() const override;

 private:
  std::uint64_t originNs_;
};

}  // namespace platform
}  // namespace lc
