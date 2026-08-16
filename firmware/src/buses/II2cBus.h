// =============================================================================
//  buses/II2cBus.h — what a driver is allowed to do with an I²C bus.
//
//  Free of Arduino includes, and that is the whole point: with this interface a
//  BMP280 driver is a pure function of the bytes the part returns, so it can be
//  unit-tested on a host against a recorded register map.  Every I²C driver in
//  this firmware is therefore testable without an ESP32, a breadboard, or a
//  working sensor — including its error paths, which are the ones you can never
//  reproduce on demand with real hardware.
//
//  Transactions are synchronous and short.  A 6-byte read at 400 kHz is ~150 µs,
//  which is acceptable inside a cooperative poll().  Anything that requires
//  waiting (a conversion, a reset) must be handled by the driver's state
//  machine, not by blocking here.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/Error.h"

namespace lc {

class II2cBus {
 public:
  virtual ~II2cBus() = default;

  virtual std::uint8_t index() const = 0;

  // True if the address ACKs.  Used by the scanner and by begin().
  virtual bool probe(std::uint8_t address) = 0;

  virtual Status write(std::uint8_t address, const std::uint8_t* data,
                       std::size_t length) = 0;
  virtual Status read(std::uint8_t address, std::uint8_t* buffer,
                      std::size_t length) = 0;

  // Write-then-read with a repeated START.  This is the correct primitive for
  // register reads; a separate write and read can be interleaved by another
  // master and silently return the wrong register.
  virtual Status writeRead(std::uint8_t address, const std::uint8_t* out,
                           std::size_t outLength, std::uint8_t* in,
                           std::size_t inLength) = 0;

  // NACK / timeout / arbitration counters, per bus.  A single NACK is normal;
  // a hundred a minute is a bad solder joint, and the diagnostics page should
  // be able to say so.
  virtual std::uint32_t errorCount() const = 0;
  virtual void resetErrorCount() = 0;

  // --- convenience, implemented in terms of the above ----------------------
  Status writeRegister(std::uint8_t address, std::uint8_t reg,
                       std::uint8_t value) {
    const std::uint8_t payload[2] = {reg, value};
    return write(address, payload, 2);
  }

  Status readRegisters(std::uint8_t address, std::uint8_t reg,
                       std::uint8_t* buffer, std::size_t length) {
    return writeRead(address, &reg, 1, buffer, length);
  }

  Result<std::uint8_t> readRegister(std::uint8_t address, std::uint8_t reg) {
    std::uint8_t value = 0;
    const Status status = readRegisters(address, reg, &value, 1);
    if (!status.ok()) return status;
    return value;
  }

  Status writeCommand(std::uint8_t address, std::uint8_t a, std::uint8_t b,
                      std::uint8_t c) {
    const std::uint8_t payload[3] = {a, b, c};
    return write(address, payload, 3);
  }
};

}  // namespace lc
