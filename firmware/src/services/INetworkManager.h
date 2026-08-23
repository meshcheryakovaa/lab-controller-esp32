// =============================================================================
//  services/INetworkManager.h — what the API is allowed to know about the
//  network (M16).
//
//  WHY AN INTERFACE AND NOT JUST WifiManager.
//  RestApi is compiled on the host, where there is no WiFi.h, no Preferences
//  and no esp_netif.  Every other subsystem the API talks to is already behind
//  a contract for exactly that reason, and the network is the subsystem where
//  it matters most: the rules worth testing here — a password is never returned,
//  a second connect attempt is refused, an invalid hostname is rejected — are
//  rules about POLICY, not about radios.  Behind this interface they are
//  ordinary host tests; behind WiFi.h they would be things nobody can check
//  without standing next to the board.
//
//  THE PROMISE THIS FILE EXISTS TO KEEP (ADR-0022).
//  Changing the network settings must never make the instrument unreachable.
//  That is why `testCredentials()` is not called `setCredentials()`: new details
//  are PROVED before they replace working ones, the fallback access point stays
//  up while they are being proved, and a wrong password costs the operator a
//  retry rather than a walk to the bench with a USB cable.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/Error.h"
#include "core/Types.h"

namespace lc {

/**
 * Where the network is, as a state rather than a pair of booleans.
 *
 * "AP is up" and "station is connected" are not independent — the combinations
 * mean different things to an operator, and the interface has to say which one
 * is happening.  kApStationFallback in particular is NOT an error: it is the
 * instrument holding its own door open while it keeps knocking on the router's.
 */
enum class NetworkState : std::uint8_t {
  kOff = 0,
  kApOnly,              // our own access point, and nothing configured
  kStationConnecting,   // joining a configured network
  kStationConnected,    // on the house network, with an address
  kApStationFallback,   // both: retrying the router, reachable meanwhile
  kError,               // neither could be started — see lastError
};

const char* toString(NetworkState state);

/** Longest SSID IEEE 802.11 allows, plus the terminator. */
inline constexpr std::size_t kSsidLength = 33;
/** Hostname without the ".local": 1..31 characters. */
inline constexpr std::size_t kHostnameLength = 32;
/** "255.255.255.255". */
inline constexpr std::size_t kIpTextLength = 16;
/** How many scan results are ever reported.  A busy building sees far more;
 *  the list is for choosing a network, not for surveying the spectrum. */
inline constexpr std::size_t kMaxScanResults = 20;

struct NetworkStatus {
  NetworkState state = NetworkState::kOff;
  // Credentials are stored — not necessarily working right now.
  bool configured = false;
  bool stationConnected = false;
  bool accessPointActive = false;
  // True while credentials are being proved.  The interface polls on this.
  bool testing = false;
  FixedString<kSsidLength> ssid;              // the configured network
  FixedString<kSsidLength> accessPointSsid;
  FixedString<kHostnameLength> hostname;
  FixedString<kIpTextLength> stationIp;
  FixedString<kIpTextLength> accessPointIp;
  std::int32_t rssi = 0;
  // Counters, because "it dropped out twice overnight" is the question an
  // instrument on a house network actually gets asked.
  std::uint32_t reconnects = 0;
  std::uint32_t disconnects = 0;
  FixedString<limits::kDetailLength> lastDisconnectReason;
  Error lastError;
};

struct NetworkCandidate {
  FixedString<kSsidLength> ssid;
  std::int32_t rssi = 0;
  std::uint8_t channel = 0;
  bool secured = false;
};

/** What a scan is doing, so the page can say so rather than spin forever. */
enum class ScanState : std::uint8_t { kIdle = 0, kRunning, kComplete, kFailed };

const char* toString(ScanState state);

class INetworkManager {
 public:
  virtual ~INetworkManager() = default;

  virtual NetworkStatus status() const = 0;

  /** Start an asynchronous scan.  Calling it while one is running is not an
   *  error and does not start a second — it simply reports the current one. */
  virtual Status beginScan() = 0;
  virtual ScanState scanState() const = 0;
  virtual std::size_t scanResults(NetworkCandidate* out,
                                  std::size_t capacity) const = 0;

  /**
   * Prove these credentials, then adopt them if they work.
   *
   * Returns as soon as the attempt is ACCEPTED — never after it finishes.  The
   * caller is an HTTP handler running on the web server's task; blocking it for
   * the fifteen seconds a join can take would stall every other request,
   * including the one the page uses to poll for the result.
   */
  virtual Status testCredentials(const char* ssid, const char* password) = 0;

  /** Forget the house network and go back to our own access point. */
  virtual Status clearCredentials() = 0;

  /** Set the name the device answers to on the local network (no ".local"). */
  virtual Status setHostname(const char* hostname) = 0;

  /** Shared by the firmware and the API so the rules cannot drift apart:
   *  1..31 characters of [a-z0-9-], not starting or ending with a hyphen. */
  static bool hostnameIsValid(const char* hostname);
};

}  // namespace lc
