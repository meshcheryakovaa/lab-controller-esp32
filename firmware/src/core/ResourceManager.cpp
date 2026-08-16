#include "core/ResourceManager.h"

#include <cstdio>

namespace lc {

const char* toString(ResourceKind kind) {
  switch (kind) {
    case ResourceKind::kGpio:        return "GPIO";
    case ResourceKind::kAdc1Channel: return "ADC1";
    case ResourceKind::kAdc2Channel: return "ADC2";
    case ResourceKind::kI2cBus:      return "I2C_BUS";
    case ResourceKind::kI2cAddress:  return "I2C_ADDRESS";
    case ResourceKind::kSpiBus:      return "SPI_BUS";
    case ResourceKind::kUartPort:    return "UART_PORT";
    case ResourceKind::kLedcChannel: return "PWM_CHANNEL";
    case ResourceKind::kLedcTimer:   return "PWM_TIMER";
    case ResourceKind::kCount:       break;
  }
  return "UNKNOWN";
}

Status ResourceManager::checkPinCapability(std::uint8_t pin, PinUse use) const {
  const GpioCapability* cap = chip_.pin(pin);
  if (cap == nullptr || !cap->exists) {
    return fail(ErrorCode::kGpioInvalid, "no such GPIO on this chip");
  }
  if (cap->reserved) {
    return fail(ErrorCode::kGpioReserved,
                cap->note != nullptr ? cap->note : "reserved by the module");
  }

  switch (use) {
    case PinUse::kDigitalOutput:
    case PinUse::kPwmOutput:
      if (cap->inputOnly) {
        return fail(ErrorCode::kGpioInputOnly,
                    "this pin has no output driver");
      }
      break;

    case PinUse::kAnalogInput:
      if (cap->adc1Channel < 0 && cap->adc2Channel < 0) {
        return fail(ErrorCode::kAdcChannelInvalid, "pin has no ADC channel");
      }
      // ADC2 shares its hardware with the Wi-Fi radio.  Rejecting it outright
      // is the honest behaviour for a networked instrument: readings would
      // silently fail whenever the radio is active.
      if (cap->adc1Channel < 0 && !chip_.adc2UsableWithWifi) {
        return fail(ErrorCode::kAdcChannelInvalid,
                    "ADC2 pin: unusable while Wi-Fi is enabled");
      }
      break;

    case PinUse::kDigitalInput:
    case PinUse::kBusSignal:
      break;
  }
  return ok();
}

const char* ResourceManager::pinAdvisory(std::uint8_t pin) const {
  const GpioCapability* cap = chip_.pin(pin);
  if (cap == nullptr || !cap->exists) return nullptr;
  if (cap->note != nullptr) return cap->note;
  if (cap->strapping) return "Strapping pin: affects boot mode";
  return nullptr;
}

const ResourceClaim* ResourceManager::find(const ResourceId& id) const {
  for (std::size_t i = 0; i < claimCount_; ++i) {
    if (claims_[i].id == id) return &claims_[i];
  }
  return nullptr;
}

Status ResourceManager::claim(const ResourceId& id, DeviceHandle owner,
                              const char* label, PinUse use) {
  const ResourceClaim* existing = find(id);
  if (existing != nullptr) {
    // The detail string is what the user actually reads, so spend the bytes to
    // make it specific: "already used by I2C0 SDA".
    char detail[limits::kDetailLength];
    std::snprintf(detail, sizeof(detail), "used by %s", existing->label.c_str());
    const ErrorCode code = (id.kind == ResourceKind::kI2cAddress)
                               ? ErrorCode::kI2cAddressBusy
                               : ErrorCode::kResourceBusy;
    return fail(code, detail);
  }

  if (claimCount_ >= limits::kMaxResourceClaims) {
    return fail(ErrorCode::kOutOfCapacity, "resource table full");
  }

  ResourceClaim& slot = claims_[claimCount_++];
  slot.id = id;
  slot.owner = owner;
  slot.use = use;
  slot.label.assign(label);
  return ok();
}

Status ResourceManager::claimPin(std::uint8_t pin, PinUse use,
                                 DeviceHandle owner, const char* label) {
  const Status capability = checkPinCapability(pin, use);
  if (!capability.ok()) return capability;
  return claim(gpioResource(pin), owner, label, use);
}

Status ResourceManager::release(const ResourceId& id) {
  for (std::size_t i = 0; i < claimCount_; ++i) {
    if (!(claims_[i].id == id)) continue;
    claims_[i] = claims_[claimCount_ - 1];
    --claimCount_;
    return ok();
  }
  return fail(ErrorCode::kNotFound, "resource is not claimed");
}

std::size_t ResourceManager::releaseAllOwnedBy(DeviceHandle owner) {
  std::size_t removed = 0;
  for (std::size_t i = 0; i < claimCount_;) {
    if (claims_[i].owner == owner) {
      claims_[i] = claims_[claimCount_ - 1];
      --claimCount_;
      ++removed;
    } else {
      ++i;
    }
  }
  return removed;
}

std::size_t ResourceManager::listOwnedBy(DeviceHandle owner, ResourceClaim* out,
                                         std::size_t capacity) const {
  if (out == nullptr) return 0;
  std::size_t written = 0;
  for (std::size_t i = 0; i < claimCount_ && written < capacity; ++i) {
    if (claims_[i].owner == owner) out[written++] = claims_[i];
  }
  return written;
}

}  // namespace lc
