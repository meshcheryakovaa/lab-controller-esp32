// =============================================================================
//  core/ConfigView.h — read-only access to a module's configuration object.
//
//  Modules never see ArduinoJson.  They see this interface, which storage/ and
//  api/ implement over a JsonObjectConst.  Two payoffs:
//    * every driver and processor can be unit-tested on a host with a trivial
//      map-backed implementation;
//    * if the JSON library is ever replaced, no driver changes.
// =============================================================================
#pragma once

#include <cstdint>

namespace lc {

class IConfigView {
 public:
  virtual ~IConfigView() = default;

  virtual bool has(const char* key) const = 0;
  virtual std::int32_t getInt(const char* key, std::int32_t fallback) const = 0;
  virtual float getFloat(const char* key, float fallback) const = 0;
  virtual bool getBool(const char* key, bool fallback) const = 0;
  virtual const char* getString(const char* key, const char* fallback) const = 0;

  // Array access, used for calibration point tables and processor chains.
  virtual std::size_t arraySize(const char* key) const = 0;
  virtual float getFloatAt(const char* key, std::size_t index,
                           float fallback) const = 0;

  // Key enumeration.  Optional: an implementation that cannot enumerate returns
  // 0 and DeviceManager simply skips the "unknown key" check.  Implementing it
  // is worth the effort — a typo like "clock_pln" is otherwise silently ignored
  // and the driver quietly runs on a default pin.
  virtual std::size_t keyCount() const { return 0; }
  virtual const char* keyAt(std::size_t index) const { return nullptr; }
};

// Always-empty view: handy as a default argument and in tests.
class EmptyConfigView final : public IConfigView {
 public:
  bool has(const char*) const override { return false; }
  std::int32_t getInt(const char*, std::int32_t fallback) const override { return fallback; }
  float getFloat(const char*, float fallback) const override { return fallback; }
  bool getBool(const char*, bool fallback) const override { return fallback; }
  const char* getString(const char*, const char* fallback) const override { return fallback; }
  std::size_t arraySize(const char*) const override { return 0; }
  float getFloatAt(const char*, std::size_t, float fallback) const override { return fallback; }
};

}  // namespace lc
