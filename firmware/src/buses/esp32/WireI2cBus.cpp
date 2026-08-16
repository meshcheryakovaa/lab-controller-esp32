#include "buses/esp32/WireI2cBus.h"

namespace lc {
namespace platform {

Status WireI2cBus::begin(std::uint8_t sda, std::uint8_t scl,
                         std::uint32_t frequency) {
  sda_ = sda;
  scl_ = scl;
  frequency_ = frequency;
  if (!wire_.begin(sda, scl, frequency)) {
    return fail(ErrorCode::kBusNotConfigured, "Wire.begin failed");
  }
  // Without a timeout a stuck slave holding SDA low would hang the control task
  // forever, and the watchdog would reboot the instrument mid-experiment.
  wire_.setTimeOut(25);  // ms
  configured_ = true;
  return ok();
}

void WireI2cBus::end() {
  if (!configured_) return;
  wire_.end();
  configured_ = false;
}

Status WireI2cBus::translate(std::uint8_t code) {
  switch (code) {
    case 0: return ok();
    case 1: ++errors_; return fail(ErrorCode::kPayloadTooLarge, "I2C buffer");
    case 2: ++errors_; return fail(ErrorCode::kDeviceNotResponding, "NACK on address");
    case 3: ++errors_; return fail(ErrorCode::kDeviceNotResponding, "NACK on data");
    case 5: ++errors_; return fail(ErrorCode::kTimeout, "I2C timeout");
    default: ++errors_; return fail(ErrorCode::kInternal, "I2C error");
  }
}

bool WireI2cBus::probe(std::uint8_t address) {
  if (!configured_) return false;
  wire_.beginTransmission(address);
  // A probe that finds nothing is not an error: the scanner walks 112 addresses
  // and would otherwise report 110 failures every time.
  return wire_.endTransmission(true) == 0;
}

Status WireI2cBus::write(std::uint8_t address, const std::uint8_t* data,
                         std::size_t length) {
  if (!configured_) return fail(ErrorCode::kBusNotConfigured, "I2C bus");
  wire_.beginTransmission(address);
  if (length > 0 && data != nullptr) {
    wire_.write(data, length);
  }
  return translate(wire_.endTransmission(true));
}

Status WireI2cBus::read(std::uint8_t address, std::uint8_t* buffer,
                        std::size_t length) {
  if (!configured_) return fail(ErrorCode::kBusNotConfigured, "I2C bus");
  if (buffer == nullptr || length == 0) {
    return fail(ErrorCode::kInvalidArgument, "no buffer");
  }
  const std::size_t received = wire_.requestFrom(address, length, true);
  if (received != length) {
    ++errors_;
    return fail(ErrorCode::kDeviceNotResponding, "short I2C read");
  }
  for (std::size_t i = 0; i < length; ++i) {
    buffer[i] = static_cast<std::uint8_t>(wire_.read());
  }
  return ok();
}

Status WireI2cBus::writeRead(std::uint8_t address, const std::uint8_t* out,
                             std::size_t outLength, std::uint8_t* in,
                             std::size_t inLength) {
  if (!configured_) return fail(ErrorCode::kBusNotConfigured, "I2C bus");
  if (in == nullptr || inLength == 0) {
    return fail(ErrorCode::kInvalidArgument, "no buffer");
  }

  wire_.beginTransmission(address);
  if (outLength > 0 && out != nullptr) wire_.write(out, outLength);
  // endTransmission(false) issues a repeated START.  A separate STOP/START pair
  // would let another master slip a register write in between, and the read
  // would silently return a different register's contents.
  const std::uint8_t status = wire_.endTransmission(false);
  if (status != 0) {
    wire_.endTransmission(true);  // release the bus
    return translate(status);
  }

  const std::size_t received = wire_.requestFrom(address, inLength, true);
  if (received != inLength) {
    ++errors_;
    return fail(ErrorCode::kDeviceNotResponding, "short I2C read");
  }
  for (std::size_t i = 0; i < inLength; ++i) {
    in[i] = static_cast<std::uint8_t>(wire_.read());
  }
  return ok();
}

}  // namespace platform
}  // namespace lc
