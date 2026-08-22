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

  /**
   * A name for THIS controller that survives a reflash and does not depend on
   * the network (§M14).
   *
   * The IP address cannot serve: every board in access-point mode answers on
   * 192.168.4.1, so a browser keeping data per origin would pour two different
   * rigs into one archive and never notice.  This comes from the eFuse MAC,
   * contains no secret, and is the key local recordings are filed under.
   */
  virtual const char* controllerId() const = 0;
};

// Reports zeroes and "OFF".  Used by the host tests, and as a safe default
// before the network layer exists.
//
// Not `final`: tools/host_server derives from it to give each simulated rig its
// own controllerId, and overriding one stub beats restating seven.
class NullSystemMetrics : public ISystemMetrics {
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
  // Deliberately a constant, and deliberately not blank: a client filing data
  // under "" would merge every host build ever run.  Tools that host several
  // simulated rigs override this.
  const char* controllerId() const override { return "lc-000000000000"; }
};

}  // namespace lc
