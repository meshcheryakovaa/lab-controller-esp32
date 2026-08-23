#pragma once
// Enough of HTTPClient for the M17 cloud clients to typecheck.  The real one
// comes from arduino-esp32; what matters here is that the signatures agree.
#include "Arduino.h"
#include "WiFiClientSecure.h"

#define HTTPC_DISABLE_FOLLOW_REDIRECTS 0
typedef int followRedirects_t;

class HTTPClient {
 public:
  bool begin(WiFiClientSecure& client, const char* url);
  bool begin(WiFiClientSecure& client, const String& url);
  void end();
  void setTimeout(uint16_t timeout);
  void setFollowRedirects(followRedirects_t follow);
  void addHeader(const char* name, const char* value);
  void collectHeaders(const char** headerKeys, size_t count);
  String header(const char* name);
  int POST(const uint8_t* payload, size_t size);
  int sendRequest(const char* type);
  int sendRequest(const char* type, Stream* stream, size_t size);
  int getSize();
  String getString();
};
