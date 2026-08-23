#pragma once
#include "Arduino.h"
#include "WiFi.h"

class WiFiClientSecure {
 public:
  void setCACert(const char* rootCA);
  void setTimeout(uint32_t seconds);
  // Deliberately NOT declared: nothing in this firmware may call it, and a
  // stub that offered it would let such a call typecheck on the host.
  // void setInsecure();
};
