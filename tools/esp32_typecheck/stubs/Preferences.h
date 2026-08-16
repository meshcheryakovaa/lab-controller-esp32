#pragma once
#include "Arduino.h"
class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false,
             const char* partition_label = nullptr);
  void end();
  bool clear();
  bool remove(const char* key);
  bool isKey(const char* key);
  std::size_t putUChar(const char* key, std::uint8_t value);
  std::size_t putUInt(const char* key, std::uint32_t value);
  std::size_t putString(const char* key, const char* value);
  std::size_t putString(const char* key, const String& value);
  std::size_t putBool(const char* key, bool value);
  std::uint8_t getUChar(const char* key, std::uint8_t defaultValue = 0);
  std::uint32_t getUInt(const char* key, std::uint32_t defaultValue = 0);
  String getString(const char* key, const String& defaultValue = String());
  bool getBool(const char* key, bool defaultValue = false);
};
