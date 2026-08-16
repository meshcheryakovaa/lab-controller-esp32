// =============================================================================
//  core/ResourceManager.h — single source of truth for hardware ownership (§8).
//
//  Every scarce hardware resource is claimed here before a Device touches it:
//  GPIO pins, ADC channels, I²C buses and addresses on those buses, SPI buses
//  and chip-select lines, UART ports, LEDC (PWM) channels and timers.
//
//  Two jobs:
//    1. CAPABILITY CHECK — is this pin even able to do what you're asking?
//       ("GPIO34 has no output driver", "GPIO6 is wired to the flash chip".)
//    2. CONFLICT CHECK — is someone already using it, and who?
//       The error carries the current owner's label so the UI can say
//       "GPIO21 is already used by I2C0 SDA" instead of "error".
//
//  Validation happens BEFORE a Device is started, so a bad configuration is
//  rejected at the API boundary and never reaches a driver.
// =============================================================================
#pragma once

#include <cstdint>

#include "core/ChipProfile.h"
#include "core/Error.h"
#include "core/Types.h"

namespace lc {

enum class ResourceKind : std::uint8_t {
  kGpio = 0,
  kAdc1Channel,
  kAdc2Channel,
  kI2cBus,
  kI2cAddress,  // index = bus number, sub = 7-bit address
  kSpiBus,
  kUartPort,
  kLedcChannel,
  kLedcTimer,
  kCount
};

const char* toString(ResourceKind kind);

struct ResourceId {
  ResourceKind kind = ResourceKind::kGpio;
  std::uint16_t index = 0;
  std::uint16_t sub = 0;

  bool operator==(const ResourceId& other) const {
    return kind == other.kind && index == other.index && sub == other.sub;
  }
};

inline ResourceId gpioResource(std::uint8_t pin) {
  return ResourceId{ResourceKind::kGpio, pin, 0};
}
inline ResourceId i2cAddressResource(std::uint8_t bus, std::uint8_t address) {
  return ResourceId{ResourceKind::kI2cAddress, bus, address};
}

// How a pin is going to be driven.  Determines which capability checks apply.
enum class PinUse : std::uint8_t {
  kDigitalInput = 0,
  kDigitalOutput,
  kAnalogInput,
  kPwmOutput,
  kBusSignal,  // SDA/SCL/MOSI/MISO/SCK/CS — bidirectional or push-pull
};

struct ResourceClaim {
  ResourceId id;
  DeviceHandle owner = kInvalidDevice;  // kInvalidDevice == owned by the system
  LabelString label;                    // "I2C0 SDA", "HX711 #1 DOUT"
  PinUse use = PinUse::kDigitalInput;
};

class ResourceManager {
 public:
  explicit ResourceManager(const ChipProfile& chip = ChipProfile::current())
      : chip_(chip) {}

  // --- capability only, no ownership involved ------------------------------
  // Answers "could this pin ever be used this way on this chip?".
  Status checkPinCapability(std::uint8_t pin, PinUse use) const;

  // Non-fatal note for the UI ("Strapping pin: must be HIGH at reset"), or
  // nullptr.  A claim on such a pin succeeds; the UI is expected to warn.
  const char* pinAdvisory(std::uint8_t pin) const;

  // --- ownership -----------------------------------------------------------
  // Claims a resource for `owner`.  Fails with RESOURCE_BUSY (or
  // I2C_ADDRESS_BUSY) if it is taken; `detail` then names the current owner.
  Status claim(const ResourceId& id, DeviceHandle owner, const char* label,
               PinUse use = PinUse::kDigitalInput);

  // Convenience wrapper: capability check + claim in one call.
  Status claimPin(std::uint8_t pin, PinUse use, DeviceHandle owner,
                  const char* label);

  Status release(const ResourceId& id);

  // Called when a device is deleted or reconfigured.  Returns how many claims
  // were dropped.  This is the only correct way to undo a partial claim
  // sequence, so DeviceManager always calls it on a failed begin().
  std::size_t releaseAllOwnedBy(DeviceHandle owner);

  const ResourceClaim* find(const ResourceId& id) const;
  bool isFree(const ResourceId& id) const { return find(id) == nullptr; }

  // --- introspection for the UI -------------------------------------------
  std::size_t claimCount() const { return claimCount_; }
  const ResourceClaim& claimAt(std::size_t i) const { return claims_[i]; }
  const ChipProfile& chip() const { return chip_; }

  // Fills `out` with every claim owned by `owner`; returns how many were
  // written (never more than `capacity`).
  std::size_t listOwnedBy(DeviceHandle owner, ResourceClaim* out,
                          std::size_t capacity) const;

 private:
  const ChipProfile& chip_;
  ResourceClaim claims_[limits::kMaxResourceClaims];
  std::size_t claimCount_ = 0;
};

}  // namespace lc
