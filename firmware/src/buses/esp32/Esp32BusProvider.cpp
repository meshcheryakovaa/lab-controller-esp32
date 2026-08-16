#include "buses/esp32/Esp32BusProvider.h"

#include <Arduino.h>
#include <esp_adc_cal.h>

#include <cstdio>

namespace lc {
namespace platform {
namespace {

int toArduinoMode(PinMode mode) {
  switch (mode) {
    case PinMode::kInput:        return INPUT;
    case PinMode::kInputPullup:  return INPUT_PULLUP;
    case PinMode::kInputPulldown:return INPUT_PULLDOWN;
    case PinMode::kOutput:       return OUTPUT;
    case PinMode::kOpenDrain:    return OUTPUT_OPEN_DRAIN;
  }
  return INPUT;
}

adc_attenuation_t toArduinoAttenuation(AdcAttenuation attenuation) {
  switch (attenuation) {
    case AdcAttenuation::kDb0:   return ADC_0db;
    case AdcAttenuation::kDb2_5: return ADC_2_5db;
    case AdcAttenuation::kDb6:   return ADC_6db;
    case AdcAttenuation::kDb11:  return ADC_11db;
  }
  return ADC_11db;
}

}  // namespace

// ---------------------------------------------------------------------------
//  GPIO
// ---------------------------------------------------------------------------
Status Esp32GpioPort::verifyOwnership(std::uint8_t pin,
                                      DeviceHandle owner) const {
  const ResourceClaim* claim = resources_.find(gpioResource(pin));
  if (claim == nullptr) {
    return fail(ErrorCode::kInvalidState, "pin was never claimed");
  }
  if (claim->owner != owner) {
    return fail(ErrorCode::kResourceBusy, claim->label.c_str());
  }
  return ok();
}

Status Esp32GpioPort::configure(std::uint8_t pin, PinMode mode,
                                DeviceHandle owner) {
  const Status owned = verifyOwnership(pin, owner);
  if (!owned.ok()) return owned;
  pinMode(pin, toArduinoMode(mode));
  return ok();
}

Status Esp32GpioPort::write(std::uint8_t pin, bool high) {
  digitalWrite(pin, high ? HIGH : LOW);
  return ok();
}

Result<bool> Esp32GpioPort::read(std::uint8_t pin) {
  return digitalRead(pin) == HIGH;
}

void Esp32GpioPort::delayMicros(std::uint32_t microseconds) const {
  delayMicroseconds(microseconds);
}

// ---------------------------------------------------------------------------
//  ADC
// ---------------------------------------------------------------------------
Status Esp32AdcPort::configure(std::uint8_t pin, AdcAttenuation attenuation,
                               DeviceHandle owner) {
  const ResourceClaim* claim = resources_.find(gpioResource(pin));
  if (claim == nullptr || claim->owner != owner) {
    return fail(ErrorCode::kInvalidState, "ADC pin was never claimed");
  }
  analogSetPinAttenuation(pin, toArduinoAttenuation(attenuation));
  analogReadResolution(12);

  // Two-point / Vref calibration lives in eFuse and is absent on some early
  // chips.  We record the fact instead of silently reporting made-up volts.
  esp_adc_cal_characteristics_t characteristics{};
  const esp_adc_cal_value_t source = esp_adc_cal_characterize(
      ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &characteristics);
  calibrated_ = (source != ESP_ADC_CAL_VAL_DEFAULT_VREF);
  return ok();
}

Result<std::uint16_t> Esp32AdcPort::readRaw(std::uint8_t pin) {
  const int value = analogRead(pin);
  if (value < 0) return fail(ErrorCode::kAdcChannelInvalid, "analogRead failed");
  return static_cast<std::uint16_t>(value);
}

Result<float> Esp32AdcPort::readMillivolts(std::uint8_t pin) {
  const std::uint32_t value = analogReadMilliVolts(pin);
  return static_cast<float>(value);
}

// ---------------------------------------------------------------------------
//  Provider
// ---------------------------------------------------------------------------
Status Esp32BusProvider::configureI2c(std::uint8_t index, std::uint8_t sda,
                                      std::uint8_t scl,
                                      std::uint32_t frequency) {
  if (index >= kI2cBusCount) {
    return fail(ErrorCode::kBusNotConfigured, "no such I2C controller");
  }
  if (sda == scl) {
    return fail(ErrorCode::kInvalidArgument, "SDA and SCL are the same pin");
  }

  WireI2cBus& bus = buses_[index];
  if (bus.configured()) {
    return fail(ErrorCode::kInvalidState, "bus already configured");
  }

  char sdaLabel[limits::kLabelLength];
  char sclLabel[limits::kLabelLength];
  std::snprintf(sdaLabel, sizeof(sdaLabel), "I2C%u SDA", index);
  std::snprintf(sclLabel, sizeof(sclLabel), "I2C%u SCL", index);

  // Owned by the system (handle 0), not by any device: a bus outlives the
  // devices hanging off it.
  Status status = resources_.claimPin(sda, PinUse::kBusSignal, kInvalidDevice,
                                      sdaLabel);
  if (!status.ok()) return status;
  status = resources_.claimPin(scl, PinUse::kBusSignal, kInvalidDevice, sclLabel);
  if (!status.ok()) {
    resources_.release(gpioResource(sda));
    return status;
  }
  status = resources_.claim(ResourceId{ResourceKind::kI2cBus, index, 0},
                            kInvalidDevice, sdaLabel);
  if (!status.ok()) {
    resources_.release(gpioResource(sda));
    resources_.release(gpioResource(scl));
    return status;
  }

  status = bus.begin(sda, scl, frequency);
  if (!status.ok()) {
    resources_.release(gpioResource(sda));
    resources_.release(gpioResource(scl));
    resources_.release(ResourceId{ResourceKind::kI2cBus, index, 0});
    return status;
  }
  return ok();
}

II2cBus* Esp32BusProvider::i2c(std::uint8_t index) {
  if (index >= kI2cBusCount) return nullptr;
  return buses_[index].configured() ? &buses_[index] : nullptr;
}

// ---------------------------------------------------------------------------
//  Esp32PwmOut — LEDC
// ---------------------------------------------------------------------------
std::uint8_t Esp32PwmOut::resolutionFor(std::uint32_t frequencyHz) {
  // The LEDC timer counts an 80 MHz source, so resolution and frequency trade
  // against each other: bits <= log2(80e6 / f).  Asking for 12 bits at 25 kHz
  // does not fail, it silently gives a duty that is not the one requested.
  if (frequencyHz <= 5000) return 12;
  if (frequencyHz <= 20000) return 10;
  return 8;
}

Result<PwmChannel> Esp32PwmOut::attach(std::uint8_t pin,
                                       std::uint32_t frequencyHz,
                                       DeviceHandle owner) {
  // The pin must already be owned by the caller.  Without this check the
  // ResourceManager is advisory, and an advisory resource manager is decoration.
  const ResourceClaim* claim = resources_.find(gpioResource(pin));
  if (claim == nullptr || claim->owner != owner) {
    return fail(ErrorCode::kInvalidState, "pin was not claimed by this device");
  }

  for (std::uint8_t channel = 0; channel < kChannels; ++channel) {
    if (used_[channel]) continue;
    const std::uint8_t bits = resolutionFor(frequencyHz);
    if (ledcSetup(channel, static_cast<double>(frequencyHz), bits) == 0) {
      return fail(ErrorCode::kDeviceInitFailed, "LEDC rejected this frequency");
    }
    ledcAttachPin(pin, channel);
    ledcWrite(channel, 0);
    used_[channel] = true;
    pins_[channel] = pin;
    bits_[channel] = bits;
    ++inUse_;
    return channel;
  }
  return fail(ErrorCode::kPwmChannelExhausted,
              "all 16 LEDC channels are in use");
}

Status Esp32PwmOut::write(PwmChannel channel, float duty) {
  if (channel >= kChannels || !used_[channel]) {
    return fail(ErrorCode::kInvalidState, "PWM channel is not attached");
  }
  const float bounded = duty < 0.0f ? 0.0f : (duty > 1.0f ? 1.0f : duty);
  const std::uint32_t maximum = (1u << bits_[channel]) - 1u;
  ledcWrite(channel, static_cast<std::uint32_t>(bounded * maximum + 0.5f));
  return ok();
}

void Esp32PwmOut::detach(PwmChannel channel) {
  if (channel >= kChannels || !used_[channel]) return;
  ledcWrite(channel, 0);
  ledcDetachPin(pins_[channel]);
  used_[channel] = false;
  if (inUse_ > 0) --inUse_;
}

}  // namespace platform
}  // namespace lc
