// =============================================================================
//  storage/JsonConfigView.h — IConfigView over an ArduinoJson object.
//
//  This is the only place in the firmware where a driver's configuration meets
//  the JSON library.  Drivers see IConfigView and nothing else, which is why
//  they can be unit-tested with a three-line map-backed implementation.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include <cstdlib>

#include "core/ConfigView.h"

namespace lc {

class JsonConfigView final : public IConfigView {
 public:
  JsonConfigView() = default;
  explicit JsonConfigView(JsonObjectConst object) : object_(object) {}

  void bind(JsonObjectConst object) { object_ = object; }

  bool has(const char* key) const override {
    return !object_.isNull() && !object_[key].isNull();
  }

  std::int32_t getInt(const char* key, std::int32_t fallback) const override {
    JsonVariantConst value = object_[key];
    if (value.isNull()) return fallback;
    // Accept "0x76" as well as 118: the UI shows I²C addresses in hex and it
    // would be silly to force it to convert them before storing.
    if (value.is<const char*>()) {
      const char* text = value.as<const char*>();
      if (text == nullptr) return fallback;
      return static_cast<std::int32_t>(std::strtol(text, nullptr, 0));
    }
    return value.as<std::int32_t>();
  }

  float getFloat(const char* key, float fallback) const override {
    JsonVariantConst value = object_[key];
    if (value.isNull()) return fallback;
    if (value.is<const char*>()) {
      const char* text = value.as<const char*>();
      return (text != nullptr) ? std::strtof(text, nullptr) : fallback;
    }
    return value.as<float>();
  }

  bool getBool(const char* key, bool fallback) const override {
    JsonVariantConst value = object_[key];
    return value.isNull() ? fallback : value.as<bool>();
  }

  const char* getString(const char* key, const char* fallback) const override {
    JsonVariantConst value = object_[key];
    if (!value.is<const char*>()) return fallback;
    const char* text = value.as<const char*>();
    return (text != nullptr) ? text : fallback;
  }

  std::size_t arraySize(const char* key) const override {
    JsonArrayConst array = object_[key];
    return array.isNull() ? 0 : array.size();
  }

  float getFloatAt(const char* key, std::size_t index,
                   float fallback) const override {
    JsonArrayConst array = object_[key];
    if (array.isNull() || index >= array.size()) return fallback;
    return array[index].as<float>();
  }

  std::size_t keyCount() const override {
    return object_.isNull() ? 0 : object_.size();
  }

  const char* keyAt(std::size_t index) const override {
    if (object_.isNull()) return nullptr;
    std::size_t i = 0;
    for (JsonPairConst pair : object_) {
      if (i++ == index) return pair.key().c_str();
    }
    return nullptr;
  }

 private:
  JsonObjectConst object_;
};

}  // namespace lc
