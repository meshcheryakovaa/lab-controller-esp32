// =============================================================================
//  buses/esp32/WireI2cBus.h — II2cBus on the ESP32's hardware I²C controllers.
// =============================================================================
#pragma once

#include <Wire.h>

#include "buses/II2cBus.h"

namespace lc {
namespace platform {

class WireI2cBus final : public II2cBus {
 public:
  WireI2cBus(TwoWire& wire, std::uint8_t index) : wire_(wire), index_(index) {}

  // Pins must already be claimed from ResourceManager by the caller.
  Status begin(std::uint8_t sda, std::uint8_t scl, std::uint32_t frequency);
  void end();
  bool configured() const { return configured_; }

  std::uint8_t sda() const { return sda_; }
  std::uint8_t scl() const { return scl_; }
  std::uint32_t frequency() const { return frequency_; }

  std::uint8_t index() const override { return index_; }
  bool probe(std::uint8_t address) override;
  Status write(std::uint8_t address, const std::uint8_t* data,
               std::size_t length) override;
  Status read(std::uint8_t address, std::uint8_t* buffer,
              std::size_t length) override;
  Status writeRead(std::uint8_t address, const std::uint8_t* out,
                   std::size_t outLength, std::uint8_t* in,
                   std::size_t inLength) override;

  std::uint32_t errorCount() const override { return errors_; }
  void resetErrorCount() override { errors_ = 0; }

 private:
  Status translate(std::uint8_t code);

  TwoWire& wire_;
  std::uint8_t index_;
  std::uint8_t sda_ = 0;
  std::uint8_t scl_ = 0;
  std::uint32_t frequency_ = 400000;
  std::uint32_t errors_ = 0;
  bool configured_ = false;
};

}  // namespace platform
}  // namespace lc
