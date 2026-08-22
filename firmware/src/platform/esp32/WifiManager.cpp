#include "platform/esp32/WifiManager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>

namespace lc {
namespace platform {
namespace {
constexpr std::uint32_t kRetryIntervalMs = 60000;
constexpr const char* kNamespace = "lc-wifi";

/**
 * The three conditions PsychicHttp checks before it will start a server.
 *
 * Deliberately the same three, and in the same order: a readiness test that
 * asked an easier question than the library would still let setup() start the
 * server too early, which is the whole failure being fixed.
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

}  // namespace

// Every open is READ/WRITE, including the ones that only read.
//
// Milestone 12, from a factory-fresh board: Preferences::begin(name, true) maps
// onto nvs_open(..., NVS_READONLY), and opening a namespace that has never been
// written fails with ESP_ERR_NVS_NOT_FOUND.  On first boot the log therefore
// read
//
//     nvs_open failed: NOT_FOUND
//
// three times before the access point came up — an error message describing the
// completely normal state of a device nobody has configured yet.  A read/write
// open creates the namespace, so the first boot is quiet and the code no longer
// has to distinguish "no credentials" from "no namespace".
bool WifiManager::openPreferences() const {
  return preferences_.begin(kNamespace, /*readOnly=*/false);
}

bool WifiManager::hasCredentials() const {
  if (!openPreferences()) return false;
  const bool present = preferences_.isKey("ssid");
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
  preferences_.putString("ssid", ssid);
  preferences_.putString("pass", password != nullptr ? password : "");
  preferences_.end();
  return ok();
}

Status WifiManager::clearCredentials() {
  if (!openPreferences()) {
    return fail(ErrorCode::kStorageFailure, "NVS is not available");
  }
  preferences_.clear();
  preferences_.end();
  return ok();
}

Status WifiManager::begin() {
  WiFi.setHostname(kDefaultHostname);
  // Sleep saves ~30 mA and costs tens of milliseconds of latency on every
  // request.  For a bench instrument on mains power that is the wrong trade.
  WiFi.setSleep(false);

  if (!hasCredentials()) return startAccessPoint();

  if (!openPreferences()) return startAccessPoint();
  const String ssid = preferences_.getString("ssid", "");
  const String password = preferences_.getString("pass", "");
  preferences_.end();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  const std::uint32_t deadline = millis() + kStationTimeoutMs;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(100);  // start-up only; nothing else is running yet
  }

  if (WiFi.status() == WL_CONNECTED) {
    Event event;
    event.type = EventType::kSystemMessage;
    event.severity = 1;
    event.detail = "Wi-Fi connected";
    events_.publish(event);
    return ok();
  }

  // Could not join.  An instrument that is unreachable is useless, so open our
  // own network and keep trying the configured one in the background.
  return startAccessPoint();
}

Status WifiManager::startAccessPoint() {
  WiFi.mode(WIFI_AP);
  const bool started = WiFi.softAP(kDefaultApSsid);
  apActive_ = started;

  Event event;
  event.type = EventType::kSystemMessage;
  event.severity = started ? 2 : 4;
  event.code = started ? ErrorCode::kOk : ErrorCode::kInternal;
  event.detail = started ? "access point started (192.168.4.1)"
                         : "could not start the access point";
  events_.publish(event);

  return started ? ok() : fail(ErrorCode::kInternal, "softAP failed");
}

void WifiManager::tick(std::uint32_t nowMs) {
  if (!apActive_ || !hasCredentials()) return;
  if (nowMs - lastRetryMs_ < kRetryIntervalMs) return;
  lastRetryMs_ = nowMs;

  // Non-blocking retry: if the lab network comes back, the controller rejoins
  // it without anyone having to power-cycle the rig.
  if (!openPreferences()) return;
  const String ssid = preferences_.getString("ssid", "");
  const String password = preferences_.getString("pass", "");
  preferences_.end();

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
}

bool WifiManager::connected() const { return WiFi.status() == WL_CONNECTED; }

bool WifiManager::interfaceReady() const {
  const wifi_mode_t mode = WiFi.getMode();

  // Either interface will do: the point is that SOMETHING is up with an
  // address, because that is all the HTTP server needs to bind.
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
  // initialisation happened to take a little longer, which is precisely how
  // this bug behaved in the first place.
  while (millis() - startedMs < timeoutMs) {
    if (interfaceReady()) return true;
    delay(20);  // start-up only; the scheduler does not own the loop yet
  }
  return false;
}

}  // namespace platform
}  // namespace lc
