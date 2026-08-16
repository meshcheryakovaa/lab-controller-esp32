// =============================================================================
//  core/ChipProfile.h — what the silicon can actually do.
//
//  Keeping the pin map as data (not #ifdefs scattered through drivers) is what
//  lets ResourceManager reject "GPIO34 as heater output" with a precise reason,
//  lets the UI grey out impossible pins, and lets the whole thing be unit
//  tested on a host without an ESP32 attached.
//
//  Adding a new chip = adding a table here.  No other file changes.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace lc {

struct GpioCapability {
  bool exists = false;
  bool inputOnly = false;   // ESP32 GPIO34..39 — no output driver at all
  bool reserved = false;    // wired to flash/PSRAM; claiming it bricks the boot
  bool strapping = false;   // usable, but affects boot mode — warn the user
  bool touchCapable = false;
  std::int8_t adc1Channel = -1;
  std::int8_t adc2Channel = -1;  // unusable while Wi-Fi is on — see note below
  const char* note = nullptr;    // shown verbatim in the UI when non-null
};

// A chip profile is a static table plus a few scalar limits.
struct ChipProfile {
  const char* name = "unknown";
  const GpioCapability* gpio = nullptr;
  std::uint8_t gpioCount = 0;
  std::uint8_t i2cBusCount = 0;
  std::uint8_t spiBusCount = 0;   // user-available host controllers
  std::uint8_t uartPortCount = 0;
  std::uint8_t ledcChannelCount = 0;
  std::uint8_t ledcTimerCount = 0;
  bool adc2UsableWithWifi = false;

  const GpioCapability* pin(std::uint8_t number) const {
    return (number < gpioCount) ? &gpio[number] : nullptr;
  }

  static const ChipProfile& esp32();
  static const ChipProfile& esp32s3();

  // Profile matching the firmware's build target; used by production code.
  static const ChipProfile& current();
};

}  // namespace lc
