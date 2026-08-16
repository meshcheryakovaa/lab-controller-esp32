#pragma once
#include "Arduino.h"
class MD5Builder {
 public:
  void begin();
  void add(const std::uint8_t* data, std::uint16_t len);
  void add(const char* data);
  void add(const String& data);
  void calculate();
  String toString();
};
