// =============================================================================
//  api/esp32/Esp32SystemMetrics.h — the real numbers behind /api/v1/diagnostics.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cstdio>

#include "api/SystemMetrics.h"

namespace lc {
namespace platform {

class Esp32SystemMetrics final : public ISystemMetrics {
 public:
  HeapMetrics heap() const override {
    HeapMetrics metrics;
    metrics.freeBytes = ESP.getFreeHeap();
    // The low-water mark is the number that predicts a crash; the current free
    // heap only says how lucky you are right now.
    metrics.minFreeBytes = ESP.getMinFreeHeap();
    metrics.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    metrics.totalBytes = ESP.getHeapSize();
#if defined(BOARD_HAS_PSRAM)
    metrics.hasPsram = ESP.getPsramSize() > 0;
    metrics.psramFreeBytes = ESP.getFreePsram();
#endif
    return metrics;
  }

  std::uint32_t filesystemUsedBytes() const override { return LittleFS.usedBytes(); }
  std::uint32_t filesystemTotalBytes() const override { return LittleFS.totalBytes(); }
  std::uint32_t sketchUsedBytes() const override { return ESP.getSketchSize(); }
  std::uint32_t sketchTotalBytes() const override {
    return ESP.getSketchSize() + ESP.getFreeSketchSpace();
  }

  const char* networkMode() const override {
    const wifi_mode_t mode = WiFi.getMode();
    switch (mode) {
      case WIFI_MODE_STA:   return "STA";
      case WIFI_MODE_AP:    return "AP";
      case WIFI_MODE_APSTA: return "AP+STA";
      default:              return "OFF";
    }
  }

  const char* ipAddress() const override {
    static char buffer[16];
    const IPAddress address =
        (WiFi.getMode() == WIFI_MODE_AP) ? WiFi.softAPIP() : WiFi.localIP();
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", address[0], address[1],
                  address[2], address[3]);
    return buffer;
  }

  const char* hostname() const override { return WiFi.getHostname(); }

  std::int32_t wifiRssi() const override {
    return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  }

  /**
   * From the eFuse MAC: burned at the factory, identical across reflashes and
   * OTA updates, and unaffected by which network the board is on.  That last
   * part is the point — in access-point mode every controller is 192.168.4.1,
   * so a browser storing recordings per origin needs something else to tell
   * two rigs apart (§M14).
   */
  const char* controllerId() const override {
    static char buffer[16];
    if (buffer[0] == '\0') {
      const std::uint64_t mac = ESP.getEfuseMac();
      std::snprintf(buffer, sizeof(buffer), "lc-%012llx",
                    static_cast<unsigned long long>(mac & 0xFFFFFFFFFFFFULL));
    }
    return buffer;
  }
};

}  // namespace platform
}  // namespace lc
