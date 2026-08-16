// =============================================================================
//  api/SystemMetrics.h — the platform numbers the diagnostics page needs.
//
//  Heap, filesystem and Wi-Fi figures are the most ESP32-specific things in the
//  whole API, so they come in through an interface.  The host build gets a stub
//  and the diagnostics endpoint stays testable; the device gets the real thing.
// =============================================================================
#pragma once

#include <cstdint>

namespace lc {

struct HeapMetrics {
  std::uint32_t freeBytes = 0;
  std::uint32_t minFreeBytes = 0;   // low-water mark since boot — the number
                                    // that actually predicts a crash
  std::uint32_t largestBlock = 0;   // fragmentation indicator
  std::uint32_t totalBytes = 0;
  std::uint32_t psramFreeBytes = 0;
  bool hasPsram = false;
};

class ISystemMetrics {
 public:
  virtual ~ISystemMetrics() = default;

  virtual HeapMetrics heap() const = 0;
  virtual std::uint32_t filesystemUsedBytes() const = 0;
  virtual std::uint32_t filesystemTotalBytes() const = 0;
  virtual std::uint32_t sketchUsedBytes() const = 0;
  virtual std::uint32_t sketchTotalBytes() const = 0;

  virtual const char* networkMode() const = 0;   // "STA", "AP", "AP+STA", "OFF"
  virtual const char* ipAddress() const = 0;
  virtual const char* hostname() const = 0;
  virtual std::int32_t wifiRssi() const = 0;     // dBm, 0 when not connected
};

// Reports zeroes and "OFF".  Used by the host tests, and as a safe default
// before the network layer exists.
class NullSystemMetrics final : public ISystemMetrics {
 public:
  HeapMetrics heap() const override { return HeapMetrics{}; }
  std::uint32_t filesystemUsedBytes() const override { return 0; }
  std::uint32_t filesystemTotalBytes() const override { return 0; }
  std::uint32_t sketchUsedBytes() const override { return 0; }
  std::uint32_t sketchTotalBytes() const override { return 0; }
  const char* networkMode() const override { return "OFF"; }
  const char* ipAddress() const override { return "0.0.0.0"; }
  const char* hostname() const override { return "lab-controller"; }
  std::int32_t wifiRssi() const override { return 0; }
};

}  // namespace lc
