// =============================================================================
//  modules/sensors/Hx711Driver.h — HX711 24-bit load-cell ADC.
//
//  The HX711 has no bus and no registers: data comes out on a bit-banged clock,
//  and DOUT going low is the only "ready" signal there is.  Two consequences
//  shape this driver:
//
//  1. poll() NEVER waits for DOUT.  If the part is not ready the call returns
//     immediately and tries again next tick.  Waiting for a 12.5 ms conversion
//     inside the acquisition path would stall every other device on the rig —
//     and if the wire falls off, it would stall them forever.
//
//  2. A missing sensor is indistinguishable from "not ready yet" except by
//     elapsed time, so the driver keeps a deadline and reports
//     DEVICE_NOT_RESPONDING rather than sitting silent (§40).
//
//  The driver publishes RAW COUNTS.  Converting counts to grams is the
//  calibration stage's job (§12) — that is why "tare" is an offset in the
//  calibration, not a hidden variable inside this class.
// =============================================================================
#pragma once

#include "buses/IBusProvider.h"
#include "core/IModule.h"
#include "services/ChannelManager.h"

namespace lc {
namespace modules {

class Hx711Protocol {
 public:
  // Channel A gain 128 / 64, channel B gain 32 — selected by the number of
  // extra clock pulses after the 24 data bits.
  static std::uint8_t pulsesForGain(std::uint8_t gain);

  // The part returns 24-bit two's complement.  Sign-extending it is the single
  // most commonly botched line in HX711 code: a naive cast makes anything below
  // zero read as +8 million.
  static std::int32_t signExtend24(std::uint32_t raw);
};

class Hx711Driver final : public IDevice {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new Hx711Driver(); }

  Status configure(const DeviceContext& context) override;
  Status begin() override;
  void poll(Micros now) override;
  void end() override;
  Status selfTest() override;

  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

 private:
  bool readOnce(std::int32_t& counts);
  void fault(const Error& error);

  DeviceContext ctx_{};
  IGpioPort* gpio_ = nullptr;
  std::uint8_t dataPin_ = 0;
  std::uint8_t clockPin_ = 0;
  std::uint8_t gain_ = 128;

  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
  Micros lastSampleUs_ = 0;
  Micros timeoutUs_ = 1000000;
};

}  // namespace modules
}  // namespace lc
