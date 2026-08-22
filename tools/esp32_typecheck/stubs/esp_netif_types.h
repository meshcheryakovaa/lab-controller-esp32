#pragma once
// PsychicHttp 3.x reports the peer address of a client and pulls the esp_netif
// address types in for it.  esp_netif.h already declares what this file needs.
#include "esp_netif.h"

typedef struct {
  std::uint32_t addr[4];
} esp_ip6_addr_t;

typedef struct {
  union {
    esp_ip6_addr_t ip6;
    esp_ip4_addr_t ip4;
  } u_addr;
  std::uint8_t type;
} esp_ip_addr_t;
