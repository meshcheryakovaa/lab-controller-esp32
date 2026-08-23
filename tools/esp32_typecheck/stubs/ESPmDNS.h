#pragma once
// Enough of ESPmDNS for M16's hostname advertising to typecheck.
#include "Arduino.h"

class MDNSResponder {
 public:
  bool begin(const char* hostname);
  void end();
  void addService(const char* service, const char* proto, uint16_t port);
};

extern MDNSResponder MDNS;
