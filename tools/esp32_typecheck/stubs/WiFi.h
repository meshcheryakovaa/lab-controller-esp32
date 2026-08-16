#pragma once
#include "Arduino.h"
typedef enum {
  WL_NO_SHIELD = 255, WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6
} wl_status_t;
typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
#define WIFI_OFF WIFI_MODE_NULL
#define WIFI_STA WIFI_MODE_STA
#define WIFI_AP WIFI_MODE_AP
#define WIFI_AP_STA WIFI_MODE_APSTA
class WiFiClass {
 public:
  bool mode(wifi_mode_t m);
  wifi_mode_t getMode();
  bool setSleep(bool enable);
  wl_status_t begin(const char* ssid, const char* passphrase = nullptr);
  bool softAP(const char* ssid, const char* passphrase = nullptr,
              int channel = 1, int ssid_hidden = 0, int max_connection = 4);
  bool softAPdisconnect(bool wifioff = false);
  bool disconnect(bool wifioff = false, bool eraseap = false);
  wl_status_t status();
  IPAddress localIP();
  IPAddress softAPIP();
  String SSID();
  String macAddress();
  int RSSI();
  bool setHostname(const char* hostname);
  const char* getHostname();
  void setAutoReconnect(bool enable);
  void persistent(bool persistent);
  int softAPgetStationNum();
  bool softAPConfig(IPAddress local_ip, IPAddress gateway, IPAddress subnet);
  int scanNetworks(bool async = false, bool showHidden = false);
};
extern WiFiClass WiFi;
