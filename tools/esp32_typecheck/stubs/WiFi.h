#pragma once
#include "Arduino.h"
typedef enum {
  WL_NO_SHIELD = 255, WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6
} wl_status_t;
typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;

// M16: asynchronous scanning and disconnect reasons.
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED  (-2)

typedef enum {
  WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK,
  WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_WPA2_ENTERPRISE,
  WIFI_AUTH_WPA3_PSK, WIFI_AUTH_MAX
} wifi_auth_mode_t;

enum {
  WIFI_REASON_AUTH_EXPIRE = 2, WIFI_REASON_AUTH_LEAVE = 3,
  WIFI_REASON_ASSOC_EXPIRE = 4, WIFI_REASON_NOT_AUTHED = 6,
  WIFI_REASON_NOT_ASSOCED = 7, WIFI_REASON_BEACON_TIMEOUT = 200,
  WIFI_REASON_NO_AP_FOUND = 201, WIFI_REASON_AUTH_FAIL = 202,
  WIFI_REASON_ASSOC_FAIL = 203, WIFI_REASON_HANDSHAKE_TIMEOUT = 204,
};

typedef enum { ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 0, ARDUINO_EVENT_MAX } arduino_event_id_t;
typedef arduino_event_id_t WiFiEvent_t;

typedef union {
  struct { std::uint8_t reason; } wifi_sta_disconnected;
} WiFiEventInfo_t;
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
  int scanComplete();
  void scanDelete();
  String SSID(int index);
  int RSSI(int index);
  int channel(int index);
  wifi_auth_mode_t encryptionType(int index);
  void macAddress(std::uint8_t* mac);
  bool reconnect();
  void onEvent(void (*callback)(WiFiEvent_t, WiFiEventInfo_t),
               arduino_event_id_t event = ARDUINO_EVENT_MAX);
  // Templated so a lambda binds, and DEFINED so the linker-visible instantiation
  // exists: -fsyntax-only still diagnoses a used-but-never-defined template.
  template <typename F>
  void onEvent(F, arduino_event_id_t = ARDUINO_EVENT_MAX) {}
};
extern WiFiClass WiFi;
