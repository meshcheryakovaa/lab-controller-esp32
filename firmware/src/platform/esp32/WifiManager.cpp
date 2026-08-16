#include "platform/esp32/WifiManager.h"

#include <Arduino.h>
#include <WiFi.h>

namespace lc {
namespace platform {
namespace {
constexpr std::uint32_t kRetryIntervalMs = 60000;
constexpr const char* kNamespace = "lc-wifi";
}

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

}  // namespace platform
}  // namespace lc
