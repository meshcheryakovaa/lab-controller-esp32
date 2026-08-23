// =============================================================================
//  platform/esp32/WifiManager.h — network bring-up (M16).
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
//
//  MILESTONE 16 — WHAT CHANGED AND WHY.
//
//  1. NOTHING BLOCKS ANY MORE.  begin() used to sit in a `while (…) delay(100)`
//     for up to fifteen seconds.  That was survivable when it only ever ran
//     once at boot, and became unacceptable the moment the operator could ask
//     for a connection from a web page: the request arrives on the HTTP task,
//     and a handler that waits fifteen seconds stalls every other request —
//     including the very poll the page uses to watch for the result.  Every
//     transition now happens in tick().
//
//  2. NEW CREDENTIALS ARE PROVED BEFORE THEY ARE ADOPTED.  They are written as
//     `pending`, tried with the fallback AP still up, and only promoted to the
//     active ssid/pass once a router has actually handed out an address.  A
//     typo in a password therefore costs a retry, not a trip to the bench with
//     a USB cable — which is the whole of ADR-0022.
//
//  3. LOSING THE ROUTER IS A STATE, NOT A FAILURE.  Thirty seconds without the
//     station brings the fallback AP up alongside the retries, so the
//     instrument stays reachable while the router reboots; the AP is taken down
//     again only after the station has been stable for a further minute.  The
//     grace period is what stops a flapping router turning the interface on and
//     off underneath whoever is looking at it.
// =============================================================================
#pragma once

#include <Preferences.h>

#include "core/Error.h"
#include "core/EventBus.h"
#include "services/INetworkManager.h"

namespace lc {
namespace platform {

class WifiManager final : public INetworkManager {
 public:
  // How long a join is given before it is treated as failed.  Fifteen seconds
  // covers a DHCP lease on a busy home router; beyond that something is wrong
  // and the operator is better served by being told.
  static constexpr std::uint32_t kStationTimeoutMs = 15000;
  // How long the station may be missing before the fallback AP comes up.  Short
  // enough that a real outage is covered quickly; long enough that a router
  // rebooting after a firmware update does not flap the interface.
  static constexpr std::uint32_t kDisconnectGraceMs = 30000;
  // How often a lost station is retried.
  static constexpr std::uint32_t kRetryIntervalMs = 60000;
  // How long the station must stay up before the fallback AP is taken down.
  // Taking it down the instant the station returns would drop whoever is
  // connected to it — including, quite possibly, the person watching the
  // network page to see whether the reconnection worked.
  static constexpr std::uint32_t kApShutdownGraceMs = 60000;

  static constexpr const char* kApSsidPrefix = "LAB-CONTROLLER";
  static constexpr const char* kHostnamePrefix = "lab-controller";

  explicit WifiManager(EventBus& events) : events_(events) {}

  /**
   * Bring the network up.  Returns as soon as something is LISTENING — an
   * access point, or a station attempt in progress — never after waiting for a
   * join.  The rest happens in tick().
   */
  Status begin();

  Status saveCredentials(const char* ssid, const char* password);
  bool hasCredentials() const;

  bool connected() const;
  bool accessPointActive() const { return apActive_; }

  /** Is there a network interface an HTTP server can actually bind to?
   *
   *  0.15.1-m15.  WiFi.softAP() returns as soon as it has ASKED esp-idf for an
   *  access point; the esp_netif behind it comes up asynchronously.  PsychicHttp
   *  checks for a handle that exists, is UP and holds a non-zero address before
   *  it will start, so the window between those two moments is a window in which
   *  the web server refuses to start and the controller is unreachable until
   *  somebody power-cycles it.  This asks the library's own three questions. */
  bool interfaceReady() const;

  /** Blocks until interfaceReady(), or the timeout.  Start-up only. */
  bool waitUntilReady(std::uint32_t timeoutMs = 3000) const;

  /** The whole state machine.  Called from loop(); never blocks. */
  void tick(std::uint32_t nowMs);

  // --- INetworkManager -------------------------------------------------------
  NetworkStatus status() const override;
  Status beginScan() override;
  ScanState scanState() const override;
  std::size_t scanResults(NetworkCandidate* out,
                          std::size_t capacity) const override;
  Status testCredentials(const char* ssid, const char* password) override;
  Status clearCredentials() override;
  Status setHostname(const char* hostname) override;

 private:
  Status startAccessPoint();
  void stopAccessPoint();
  void beginStationAttempt(const char* ssid, const char* password,
                           bool isTest, std::uint32_t nowMs);
  void adoptPendingCredentials();
  void discardPendingCredentials();
  void onStationConnected(std::uint32_t nowMs);
  void startMdns();
  /** Point SNTP at a server and let it run.  M17: the cloud uploader cannot
   *  start without a real wall clock, and nothing else sets one. */
  void startTimeSync();
  bool timeSynced() const;
  void enter(NetworkState state, std::uint8_t severity, const char* detail,
             ErrorCode code);
  void collectScanResults();
  // Opens the NVS namespace read/write; see the comment in the .cpp for why
  // "read-only" is the wrong flag even for the calls that only read.
  bool openPreferences() const;
  void loadHostname();
  void defaultHostname(char* out, std::size_t capacity) const;

  EventBus& events_;
  mutable Preferences preferences_;

  NetworkState state_ = NetworkState::kOff;
  bool apActive_ = false;
  // Set while testCredentials() is being proved, so a second request can be
  // refused rather than trampling the first one's pending keys.
  bool testing_ = false;

  std::uint32_t connectStartedMs_ = 0;
  std::uint32_t disconnectedSinceMs_ = 0;
  std::uint32_t stationStableSinceMs_ = 0;
  std::uint32_t lastRetryMs_ = 0;

  std::uint32_t reconnects_ = 0;
  std::uint32_t disconnects_ = 0;
  FixedString<limits::kDetailLength> lastDisconnectReason_;
  Error lastError_;

  FixedString<kSsidLength> activeSsid_;
  FixedString<kSsidLength> pendingSsid_;
  FixedString<kSsidLength> apSsid_;
  FixedString<kHostnameLength> hostname_;
  bool mdnsStarted_ = false;

  ScanState scanState_ = ScanState::kIdle;
  NetworkCandidate scan_[kMaxScanResults];
  std::size_t scanCount_ = 0;
};

}  // namespace platform
}  // namespace lc
