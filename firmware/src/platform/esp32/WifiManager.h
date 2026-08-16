// =============================================================================
//  platform/esp32/WifiManager.h — network bring-up.
//
//  The rule for a lab instrument: it must ALWAYS be reachable.  If the
//  configured network is missing — the lab Wi-Fi changed its password, the
//  router is down, the rig was carried to a different building — the controller
//  falls back to its own access point rather than becoming a brick with a
//  serial port.
//
//  Credentials live in NVS, not in the LittleFS configuration: they have to
//  survive a filesystem format, and they must never appear in a configuration
//  export.
// =============================================================================
#pragma once

#include <Preferences.h>

#include "core/Error.h"
#include "core/EventBus.h"

namespace lc {
namespace platform {

class WifiManager {
 public:
  // How long to wait for the configured network before opening an AP.
  static constexpr std::uint32_t kStationTimeoutMs = 15000;
  static constexpr const char* kDefaultApSsid = "LAB-CONTROLLER";
  static constexpr const char* kDefaultHostname = "lab-controller";

  explicit WifiManager(EventBus& events) : events_(events) {}

  // Connects in station mode if credentials exist, otherwise (or on timeout)
  // starts the access point.  Never fails in a way that leaves no network.
  Status begin();

  Status saveCredentials(const char* ssid, const char* password);
  Status clearCredentials();
  bool hasCredentials() const;

  bool connected() const;
  bool accessPointActive() const { return apActive_; }

  // Called periodically: re-attempts the station connection while the AP is up,
  // so a rig that lost the lab network rejoins it on its own once it returns.
  void tick(std::uint32_t nowMs);

 private:
  Status startAccessPoint();
  // Opens the NVS namespace read/write; see the comment in the .cpp for why
  // "read-only" is the wrong flag even for the calls that only read.
  bool openPreferences() const;

  EventBus& events_;
  mutable Preferences preferences_;
  bool apActive_ = false;
  std::uint32_t lastRetryMs_ = 0;
};

}  // namespace platform
}  // namespace lc
