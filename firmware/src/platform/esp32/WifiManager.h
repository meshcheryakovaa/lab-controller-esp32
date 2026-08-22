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

  /**
   * Is there a network interface an HTTP server can actually bind to?
   *
   * 0.15.1-m15.  After a software reset the boot log said, in this order:
   *
   *     access point started (192.168.4.1)
   *     [psychic] Server start failed - no network interface available
   *
   * which reads like a contradiction and is not one.  `WiFi.softAP()` returns as
   * soon as it has ASKED esp-idf for an access point; the esp_netif behind it
   * comes up asynchronously, a little later.  PsychicHttp checks for a handle
   * that exists, is UP, and holds a non-zero address before it will start —
   * quite rightly, since the alternative is binding to nothing — so the window
   * between those two moments is a window in which the web server refuses to
   * start and the controller is then unreachable until somebody power-cycles it.
   *
   * A cold boot usually gets through the window by accident, because so much
   * else happens first.  SW_CPU_RESET does not: everything is already warm, and
   * setup() reaches the HTTP server sooner.  That is why this only appeared
   * after a crash — which made it look like part of the crash, and it is not.
   *
   * This asks the same three questions PsychicHttp asks, so waiting on it means
   * waiting for the actual precondition rather than for a guessed number of
   * milliseconds.
   */
  bool interfaceReady() const;

  /** Blocks until interfaceReady(), or the timeout.  Start-up only: it is
   *  called from setup(), before the scheduler owns the loop. */
  bool waitUntilReady(std::uint32_t timeoutMs = 3000) const;

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
