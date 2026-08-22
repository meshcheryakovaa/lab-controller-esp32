#pragma once
// Enough of esp_netif for WifiManager::interfaceReady() to typecheck.  The real
// header comes from ESP-IDF; what matters here is that the signatures match, so
// that a change to how readiness is asked about fails on the host rather than on
// the bench.
#include <cstdint>

#include "esp_err.h"

struct esp_netif_obj;
typedef struct esp_netif_obj esp_netif_t;

typedef struct {
  std::uint32_t addr;
} esp_ip4_addr_t;

typedef struct {
  esp_ip4_addr_t ip;
  esp_ip4_addr_t netmask;
  esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

extern "C" {
esp_netif_t* esp_netif_get_handle_from_ifkey(const char* if_key);
bool esp_netif_is_netif_up(esp_netif_t* esp_netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t* esp_netif, esp_netif_ip_info_t* ip_info);
}
