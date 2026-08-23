#include "platform/esp32/WifiManager.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <sys/time.h>

#include <cstdio>
#include <cstring>

#include "core/Format.h"

namespace lc {
namespace platform {
namespace {

constexpr const char* kNamespace = "lc-wifi";

// NVS keys are limited to 15 characters, which is why these are abbreviated
// rather than spelled out.  `p*` are the credentials being PROVED; they are
// promoted to `ssid`/`pass` only once a router has handed out an address, and
// deleted on failure — so a wrong password can never displace a working one.
constexpr const char* kKeySsid    = "ssid";
constexpr const char* kKeyPass    = "pass";
constexpr const char* kKeyPendSsid = "pssid";
constexpr const char* kKeyPendPass = "ppass";
constexpr const char* kKeyHost    = "host";

/**
 * The three conditions PsychicHttp checks before it will start a server.
 *
 * Deliberately the same three, and in the same order: a readiness test that
 * asked an easier question than the library would still let setup() start the
 * server too early, which is the failure 0.15.1 fixed.
 */
bool netifReady(const char* key) {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey(key);
  if (netif == nullptr) return false;
  if (!esp_netif_is_netif_up(netif)) return false;

  esp_netif_ip_info_t info{};
  if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
  // A station that has associated but has no lease yet reports 0.0.0.0.
  return info.ip.addr != 0;
}

/** Why the station went away, in words an operator can act on. */
const char* disconnectReason(std::uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:      return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:       return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:     return "ASSOC_EXPIRE";
    case WIFI_REASON_NOT_AUTHED:       return "NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED:      return "NOT_ASSOCED";
    case WIFI_REASON_BEACON_TIMEOUT:   return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:      return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:       return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    default:                           return "DISCONNECTED";
  }
}

void ipToText(const IPAddress& address, FixedString<kIpTextLength>& out) {
  char text[kIpTextLength];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u",
                static_cast<unsigned>(address[0]), static_cast<unsigned>(address[1]),
                static_cast<unsigned>(address[2]), static_cast<unsigned>(address[3]));
  out.assign(text);
}

}  // namespace

// Every open is READ/WRITE, including the ones that only read.
//
// Milestone 12, from a factory-fresh board: Preferences::begin(name, true) maps
// onto nvs_open(..., NVS_READONLY), and opening a namespace that has never been
// written fails with ESP_ERR_NVS_NOT_FOUND.  On first boot the log therefore
// read "nvs_open failed: NOT_FOUND" three times before the access point came up
// — an error message describing the completely normal state of a device nobody
// has configured yet.  A read/write open creates the namespace, so the first
// boot is quiet and the code no longer has to distinguish "no credentials" from
// "no namespace".
bool WifiManager::openPreferences() const {
  return preferences_.begin(kNamespace, /*readOnly=*/false);
}

bool WifiManager::hasCredentials() const {
  if (!openPreferences()) return false;
  const bool present = preferences_.isKey(kKeySsid);
  preferences_.end();
  return present;
}

Status WifiManager::saveCredentials(const char* ssid, const char* password) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return fail(ErrorCode::kInvalidArgument, "SSID is empty");
  }
  if (!openPreferences()) {
    return fail(ErrorCode::kStorageFailure, "NVS is not available");
  }
  preferences_.putString(kKeySsid, ssid);
  preferences_.putString(kKeyPass, password != nullptr ? password : "");
  preferences_.end();
  activeSsid_.assign(ssid);
  return ok();
}

Status WifiManager::clearCredentials() {
  if (!openPreferences()) {
    return fail(ErrorCode::kStorageFailure, "NVS is not available");
  }
  preferences_.remove(kKeySsid);
  preferences_.remove(kKeyPass);
  preferences_.remove(kKeyPendSsid);
  preferences_.remove(kKeyPendPass);
  preferences_.end();

  activeSsid_.assign("");
  pendingSsid_.assign("");
  testing_ = false;

  // Our own access point first, THEN the station is torn down.  The other order
  // leaves a window with no interface at all, and the browser that asked for
  // this is on one of them.
  startAccessPoint();
  WiFi.disconnect(/*wifioff=*/false);
  WiFi.mode(WIFI_AP);
  enter(NetworkState::kApOnly, 2, "home Wi-Fi credentials cleared",
        ErrorCode::kOk);
  return ok();
}

void WifiManager::defaultHostname(char* out, std::size_t capacity) const {
  // The last three bytes of the MAC, so two controllers on one bench do not
  // answer to the same name.
  std::uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  std::size_t used = 0;
  appendFormat(out, capacity, used, "%s-%02x%02x%02x", kHostnamePrefix,
               mac[3], mac[4], mac[5]);
}

void WifiManager::loadHostname() {
  char stored[kHostnameLength] = {0};
  if (openPreferences()) {
    const String value = preferences_.getString(kKeyHost, "");
    preferences_.end();
    if (value.length() > 0 && value.length() < kHostnameLength) {
      std::snprintf(stored, sizeof(stored), "%s", value.c_str());
    }
  }
  if (!INetworkManager::hostnameIsValid(stored)) {
    defaultHostname(stored, sizeof(stored));
  }
  hostname_.assign(stored);

  char ap[kSsidLength];
  std::uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  std::size_t used = 0;
  appendFormat(ap, sizeof(ap), used, "%s-%02X%02X%02X", kApSsidPrefix,
               mac[3], mac[4], mac[5]);
  apSsid_.assign(ap);
}

Status WifiManager::setHostname(const char* hostname) {
  if (!INetworkManager::hostnameIsValid(hostname)) {
    return fail(ErrorCode::kInvalidArgument,
                "1-31 characters of a-z, 0-9 and '-', not starting or ending "
                "with '-'");
  }
  if (!openPreferences()) {
    return fail(ErrorCode::kStorageFailure, "NVS is not available");
  }
  preferences_.putString(kKeyHost, hostname);
  preferences_.end();
  hostname_.assign(hostname);

  // mDNS caches the name at begin(); restarting it is how the new one is
  // advertised without rebooting the instrument.
  if (mdnsStarted_) {
    MDNS.end();
    mdnsStarted_ = false;
  }
  if (state_ == NetworkState::kStationConnected
      || state_ == NetworkState::kApStationFallback) {
    startMdns();
  }
  return ok();
}

void WifiManager::startMdns() {
  if (mdnsStarted_ || hostname_.empty()) return;
  if (!MDNS.begin(hostname_.c_str())) {
    // Not fatal, and deliberately not an error state: the DHCP address still
    // works, and the interface shows it precisely so that a machine which
    // cannot resolve .local is not left without a way in.
    lastError_ = fail(ErrorCode::kInternal, "mDNS did not start");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  mdnsStarted_ = true;
}

// M17.  A wall clock is not a convenience here, it is a precondition: TLS
// rejects a certificate whose validity window does not contain "now", OAuth
// token expiry is an absolute time, and a log directory named from a zero epoch
// would put every session in 1970.  Esp32Clock reports 0 until the RTC is
// plausible, and CloudManager waits for that rather than guessing — so
// something has to actually set it, and nothing did before this.
//
// Started only with a station: the fallback AP has no route anywhere, and an
// SNTP client pointed at an unreachable server is only a retry timer.  Called
// again on a later reconnect while the clock is still implausible, because the
// first attempt can land before DNS is usable.
void WifiManager::startTimeSync() {
  if (timeSynced()) return;  // already set; restarting SNTP would only step it
  // UTC, no daylight offset: every timestamp this firmware writes — CSV rows,
  // segment names, token expiry — is epoch UTC, and a local offset here would
  // silently shift all of them.  time.yandex.ru first because the device that
  // needs this clock most is talking to Yandex Disk anyway.
  configTime(0, 0, "time.yandex.ru", "pool.ntp.org", "time.cloudflare.com");
}

bool WifiManager::timeSynced() const {
  timeval tv{};
  if (gettimeofday(&tv, nullptr) != 0) return false;
  return tv.tv_sec >= 1600000000;  // the same 2020-09-13 floor as Esp32Clock
}

void WifiManager::enter(NetworkState state, std::uint8_t severity,
                        const char* detail, ErrorCode code) {
  const bool changed = state_ != state;
  state_ = state;
  if (!changed && code == ErrorCode::kOk) return;

  Event event;
  event.type = EventType::kNetworkStateChanged;
  event.severity = severity;
  event.code = code;
  event.detail = detail;  // static lifetime only
  events_.publish(event);
}

Status WifiManager::startAccessPoint() {
  if (apActive_) return ok();
  // AP_STA rather than AP when a station is configured: the fallback exists so
  // that the operator keeps a way in WHILE the router is being retried, and
  // dropping to AP-only would abandon the retry.
  WiFi.mode(activeSsid_.empty() && pendingSsid_.empty() ? WIFI_AP : WIFI_AP_STA);
  const bool started = WiFi.softAP(apSsid_.c_str());
  apActive_ = started;
  if (!started) {
    lastError_ = fail(ErrorCode::kInternal, "softAP failed");
    return fail(ErrorCode::kInternal, "softAP failed");
  }
  return ok();
}

void WifiManager::stopAccessPoint() {
  if (!apActive_) return;
  WiFi.softAPdisconnect(/*wifioff=*/false);
  WiFi.mode(WIFI_STA);
  apActive_ = false;
}

void WifiManager::beginStationAttempt(const char* ssid, const char* password,
                                      bool isTest, std::uint32_t nowMs) {
  testing_ = isTest;
  connectStartedMs_ = nowMs;
  if (isTest) pendingSsid_.assign(ssid);
  // AP_STA, always, for an attempt the operator asked for: taking the access
  // point down to try a network that might not accept us is how a device
  // disappears mid-configuration.
  WiFi.mode(apActive_ ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(ssid, password);
}

void WifiManager::adoptPendingCredentials() {
  if (!openPreferences()) return;
  const String ssid = preferences_.getString(kKeyPendSsid, "");
  const String pass = preferences_.getString(kKeyPendPass, "");
  if (ssid.length() > 0) {
    preferences_.putString(kKeySsid, ssid);
    preferences_.putString(kKeyPass, pass);
    activeSsid_.assign(ssid.c_str());
  }
  preferences_.remove(kKeyPendSsid);
  preferences_.remove(kKeyPendPass);
  preferences_.end();
  pendingSsid_.assign("");
  testing_ = false;
}

void WifiManager::discardPendingCredentials() {
  if (openPreferences()) {
    preferences_.remove(kKeyPendSsid);
    preferences_.remove(kKeyPendPass);
    preferences_.end();
  }
  pendingSsid_.assign("");
  testing_ = false;
}

Status WifiManager::begin() {
  WiFi.setHostname(kHostnamePrefix);
  // Sleep saves ~30 mA and costs tens of milliseconds of latency on every
  // request.  For a bench instrument on mains power that is the wrong trade.
  WiFi.setSleep(false);
  // Arduino's auto-reconnect races this state machine: both would call
  // WiFi.begin(), and the retry timing would stop being ours to reason about.
  WiFi.setAutoReconnect(false);

  loadHostname();

  // Disconnect events are the only way to learn WHY the station went away, and
  // "why" is the difference between a wrong password and a router that rebooted.
  WiFi.onEvent([this](WiFiEvent_t, WiFiEventInfo_t info) {
    lastDisconnectReason_.assign(
        disconnectReason(info.wifi_sta_disconnected.reason));
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  if (!hasCredentials()) {
    const Status ap = startAccessPoint();
    enter(ap.ok() ? NetworkState::kApOnly : NetworkState::kError,
          ap.ok() ? 2 : 4,
          ap.ok() ? "access point started (192.168.4.1)"
                  : "could not start the access point",
          ap.code);
    return ap;
  }

  if (!openPreferences()) {
    const Status ap = startAccessPoint();
    enter(NetworkState::kApOnly, 2, "NVS unavailable; access point started",
          ErrorCode::kOk);
    return ap;
  }
  const String ssid = preferences_.getString(kKeySsid, "");
  const String password = preferences_.getString(kKeyPass, "");
  preferences_.end();
  activeSsid_.assign(ssid.c_str());

  // NOT blocking any more.  begin() used to wait here for up to fifteen
  // seconds; now it starts the attempt and lets tick() finish it, so setup()
  // reaches the HTTP server immediately and the instrument is reachable on the
  // fallback AP while the join is still in progress.
  startAccessPoint();
  beginStationAttempt(ssid.c_str(), password.c_str(), /*isTest=*/false, millis());
  enter(NetworkState::kStationConnecting, 1, "connecting to configured Wi-Fi",
        ErrorCode::kOk);
  return ok();
}

void WifiManager::onStationConnected(std::uint32_t nowMs) {
  stationStableSinceMs_ = nowMs;
  disconnectedSinceMs_ = 0;
  ++reconnects_;
  if (testing_) adoptPendingCredentials();
  startMdns();
  startTimeSync();
  enter(NetworkState::kStationConnected, 1, "station connected", ErrorCode::kOk);
}

void WifiManager::tick(std::uint32_t nowMs) {
  collectScanResults();

  const bool up = WiFi.status() == WL_CONNECTED;

  switch (state_) {
    case NetworkState::kStationConnecting: {
      if (up) {
        onStationConnected(nowMs);
        // The AP stays up for a grace period even on a successful join: the
        // browser that asked for this is very likely still on it.
        break;
      }
      if (nowMs - connectStartedMs_ < kStationTimeoutMs) break;

      // Timed out.  A PROVED attempt failing means the operator mistyped
      // something; the previously working credentials are untouched, which is
      // the entire point of proving them separately.
      if (testing_) {
        discardPendingCredentials();
        lastError_ = fail(ErrorCode::kTimeout,
                          "could not join that network; check the password");
        WiFi.disconnect(/*wifioff=*/false);
        startAccessPoint();
        enter(activeSsid_.empty() ? NetworkState::kApOnly
                                  : NetworkState::kApStationFallback,
              3, "station test failed; access point still available",
              ErrorCode::kTimeout);
        break;
      }
      startAccessPoint();
      disconnectedSinceMs_ = nowMs;
      lastRetryMs_ = nowMs;
      enter(NetworkState::kApStationFallback, 3,
            "station unavailable; fallback AP started", ErrorCode::kTimeout);
      break;
    }

    case NetworkState::kStationConnected: {
      if (up) {
        // Stable for long enough — the fallback AP can go.  Only now, and only
        // after the grace period: dropping it the instant the station returns
        // would disconnect whoever is watching this happen.
        if (apActive_ && !activeSsid_.empty()
            && nowMs - stationStableSinceMs_ >= kApShutdownGraceMs) {
          stopAccessPoint();
        }
        break;
      }
      ++disconnects_;
      disconnectedSinceMs_ = nowMs;
      lastRetryMs_ = nowMs;
      enter(NetworkState::kStationConnecting, 2,
            "station lost; reconnecting", ErrorCode::kOk);
      connectStartedMs_ = nowMs;
      WiFi.reconnect();
      break;
    }

    case NetworkState::kApStationFallback: {
      if (up) {
        onStationConnected(nowMs);
        break;
      }
      if (nowMs - lastRetryMs_ < kRetryIntervalMs) break;
      lastRetryMs_ = nowMs;
      if (activeSsid_.empty()) break;
      // Retry with the ACTIVE credentials — never with pending ones, which by
      // now have been discarded precisely because they did not work.
      if (!openPreferences()) break;
      const String password = preferences_.getString(kKeyPass, "");
      preferences_.end();
      connectStartedMs_ = nowMs;
      WiFi.begin(activeSsid_.c_str(), password.c_str());
      break;
    }

    case NetworkState::kApOnly: {
      if (up) onStationConnected(nowMs);
      break;
    }

    case NetworkState::kOff:
    case NetworkState::kError:
      break;
  }

  // The grace period between losing the station and opening the door.  Kept
  // outside the switch because it applies to every state in which the station
  // is meant to be up but is not.
  if (!up && !apActive_ && disconnectedSinceMs_ != 0
      && nowMs - disconnectedSinceMs_ >= kDisconnectGraceMs) {
    startAccessPoint();
    enter(NetworkState::kApStationFallback, 3,
          "station unavailable; fallback AP started", ErrorCode::kTimeout);
  }
}

Status WifiManager::testCredentials(const char* ssid, const char* password) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return fail(ErrorCode::kInvalidArgument, "SSID is empty");
  }
  if (std::strlen(ssid) >= kSsidLength) {
    return fail(ErrorCode::kPayloadTooLarge, "SSID is longer than 32 bytes");
  }
  if (testing_) {
    return fail(ErrorCode::kResourceBusy,
                "a connection test is already running");
  }
  if (scanState_ == ScanState::kRunning) {
    return fail(ErrorCode::kResourceBusy,
                "a scan is running; try again in a moment");
  }

  // Written as PENDING.  If this attempt fails they are deleted and the working
  // credentials are exactly where they were (ADR-0022).
  if (!openPreferences()) {
    return fail(ErrorCode::kStorageFailure, "NVS is not available");
  }
  preferences_.putString(kKeyPendSsid, ssid);
  preferences_.putString(kKeyPendPass, password != nullptr ? password : "");
  preferences_.end();

  lastError_ = Error{};
  startAccessPoint();
  beginStationAttempt(ssid, password != nullptr ? password : "",
                      /*isTest=*/true, millis());
  enter(NetworkState::kStationConnecting, 1, "testing new Wi-Fi credentials",
        ErrorCode::kOk);
  return ok();
}

Status WifiManager::beginScan() {
  if (scanState_ == ScanState::kRunning) return ok();  // not an error: idempotent
  if (testing_) {
    return fail(ErrorCode::kResourceBusy,
                "a connection test is running; try again in a moment");
  }
  scanCount_ = 0;
  // Asynchronous: a synchronous scan takes seconds and would block whichever
  // task called it — which is the HTTP task.
  const int started = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
  if (started == WIFI_SCAN_FAILED) {
    scanState_ = ScanState::kFailed;
    return fail(ErrorCode::kInternal, "the scan did not start");
  }
  scanState_ = ScanState::kRunning;
  return ok();
}

ScanState WifiManager::scanState() const { return scanState_; }

/** Drains a finished scan into our own bounded array and frees the library's. */
void WifiManager::collectScanResults() {
  if (scanState_ != ScanState::kRunning) return;
  const int found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) return;
  if (found < 0) {
    scanState_ = ScanState::kFailed;
    return;
  }

  scanCount_ = 0;
  for (int i = 0; i < found && scanCount_ < kMaxScanResults; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0 || ssid.length() >= kSsidLength) continue;

    // One entry per network name.  A mesh or a dual-band router answers on
    // several BSSIDs, and a list with "HomeWiFi" five times is a list nobody
    // can choose from — so the strongest of each name wins.
    bool merged = false;
    for (std::size_t j = 0; j < scanCount_; ++j) {
      if (!scan_[j].ssid.equals(ssid.c_str())) continue;
      if (WiFi.RSSI(i) > scan_[j].rssi) {
        scan_[j].rssi = WiFi.RSSI(i);
        scan_[j].channel = static_cast<std::uint8_t>(WiFi.channel(i));
      }
      merged = true;
      break;
    }
    if (merged) continue;

    NetworkCandidate& entry = scan_[scanCount_++];
    entry.ssid.assign(ssid.c_str());
    entry.rssi = WiFi.RSSI(i);
    entry.channel = static_cast<std::uint8_t>(WiFi.channel(i));
    entry.secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }

  // Strongest first: the network the operator wants is almost always the one
  // they are standing next to.
  for (std::size_t i = 1; i < scanCount_; ++i) {
    NetworkCandidate key = scan_[i];
    std::size_t j = i;
    while (j > 0 && scan_[j - 1].rssi < key.rssi) {
      scan_[j] = scan_[j - 1];
      --j;
    }
    scan_[j] = key;
  }

  WiFi.scanDelete();
  scanState_ = ScanState::kComplete;
}

std::size_t WifiManager::scanResults(NetworkCandidate* out,
                                     std::size_t capacity) const {
  const std::size_t count = scanCount_ < capacity ? scanCount_ : capacity;
  for (std::size_t i = 0; i < count; ++i) out[i] = scan_[i];
  return count;
}

NetworkStatus WifiManager::status() const {
  NetworkStatus out;
  out.state = state_;
  out.configured = !activeSsid_.empty();
  out.stationConnected = WiFi.status() == WL_CONNECTED;
  out.accessPointActive = apActive_;
  out.testing = testing_;
  out.ssid = activeSsid_;
  out.accessPointSsid = apSsid_;
  out.hostname = hostname_;
  out.rssi = out.stationConnected ? WiFi.RSSI() : 0;
  out.reconnects = reconnects_;
  out.disconnects = disconnects_;
  out.lastDisconnectReason = lastDisconnectReason_;
  out.lastError = lastError_;
  if (out.stationConnected) ipToText(WiFi.localIP(), out.stationIp);
  if (apActive_) ipToText(WiFi.softAPIP(), out.accessPointIp);
  // The password is not a member of this structure, and that is deliberate:
  // there is no code path that can accidentally serialise what does not exist.
  return out;
}

bool WifiManager::connected() const { return WiFi.status() == WL_CONNECTED; }

bool WifiManager::interfaceReady() const {
  const wifi_mode_t mode = WiFi.getMode();
  if ((mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) &&
      netifReady("WIFI_AP_DEF")) {
    return true;
  }
  if ((mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) &&
      netifReady("WIFI_STA_DEF")) {
    return true;
  }
  return false;
}

bool WifiManager::waitUntilReady(std::uint32_t timeoutMs) const {
  const std::uint32_t startedMs = millis();
  // Polling rather than an event handler: this runs in setup(), where blocking
  // is what is wanted, and an interface that never comes up has to end in a
  // timeout rather than a callback that is never delivered.
  //
  // A fixed delay() here instead of the check would be the tempting fix and the
  // wrong one — it would work on the bench and fail on the boot where
  // initialisation happened to take a little longer, which is precisely how the
  // 0.15.1 bug behaved.
  while (millis() - startedMs < timeoutMs) {
    if (interfaceReady()) return true;
    delay(20);  // start-up only; the scheduler does not own the loop yet
  }
  return false;
}

}  // namespace platform
}  // namespace lc
