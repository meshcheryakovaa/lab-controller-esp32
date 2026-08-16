// =============================================================================
//  buses/IBusProvider.h — how a driver reaches the hardware it needs.
//
//  Handed to every device through DeviceContext.  A driver asks for the bus its
//  manifest declared and gets either a working bus or nullptr; it never
//  constructs one, never configures pins for one, and never guesses which
//  peripheral instance it is on.
// =============================================================================
#pragma once

#include <cstdint>

#include "buses/IAdcPort.h"
#include "buses/IGpioPort.h"
#include "buses/II2cBus.h"
#include "buses/IPwmOut.h"

namespace lc {

class IBusProvider {
 public:
  virtual ~IBusProvider() = default;

  // nullptr when the bus index does not exist or was never configured.
  virtual II2cBus* i2c(std::uint8_t index) = 0;
  virtual std::uint8_t i2cBusCount() const = 0;

  virtual IGpioPort* gpio() = 0;
  virtual IAdcPort* adc() = 0;

  // nullptr on a build with no PWM peripheral.  Declared like every other
  // port so a module can state it needs one (BusRequirement::kPwm) and be
  // refused during validate() rather than during configure().
  virtual IPwmOut* pwm() { return nullptr; }
};

// Turning a bus description from system.json into a running controller.
// Separate from IBusProvider because drivers must be able to USE a bus without
// being able to RECONFIGURE one out from under their neighbours.
class IBusConfigurator {
 public:
  virtual ~IBusConfigurator() = default;

  // Claims SDA/SCL for the system and starts the controller.
  virtual Status configureI2c(std::uint8_t index, std::uint8_t sda,
                              std::uint8_t scl, std::uint32_t frequency) = 0;
};

}  // namespace lc
