#pragma once
#include "Arduino.h"
class TwoWire : public Stream {
 public:
  bool begin(int sda = -1, int scl = -1, std::uint32_t frequency = 0);
  bool end();
  bool setClock(std::uint32_t frequency);
  void setTimeOut(std::uint16_t timeOutMillis);
  std::uint16_t getTimeOut();
  void beginTransmission(std::uint8_t address);
  std::uint8_t endTransmission(bool sendStop = true);
  std::size_t requestFrom(std::uint8_t address, std::size_t size,
                          bool sendStop = true);
  std::size_t write(std::uint8_t) override;
  std::size_t write(const std::uint8_t* buf, std::size_t size) override;
  int available() override;
  int read() override;
  int peek() override;
};
extern TwoWire Wire;
extern TwoWire Wire1;
