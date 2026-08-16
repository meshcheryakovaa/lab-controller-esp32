#include "platform/host/HostClock.h"

#include <chrono>

namespace lc {
namespace platform {

namespace {
std::uint64_t steadyNanos() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}
}  // namespace

HostClock::HostClock() : originNs_(steadyNanos()) {}

Micros HostClock::nowMicros() const {
  return static_cast<Micros>((steadyNanos() - originNs_) / 1000ULL);
}

EpochMs HostClock::epochMillis() const {
  using namespace std::chrono;
  return static_cast<EpochMs>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace platform
}  // namespace lc
