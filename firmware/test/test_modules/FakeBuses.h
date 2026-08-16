// =============================================================================
//  Fake bus implementations for driver tests.
//
//  These are the reason every I²C driver in this project can be tested without
//  hardware: the driver only ever sees II2cBus / IGpioPort / IAdcPort, so a
//  test can hand it a scripted sensor — including one that NACKs, returns a bad
//  CRC, or is simply not there.  Those are the paths that matter and the ones
//  you cannot reproduce on demand with a real part.
// =============================================================================
#pragma once

#include <deque>
#include <map>
#include <set>
#include <vector>

#include "buses/IBusProvider.h"

namespace lc {
namespace test {

class FakeI2cBus final : public II2cBus {
 public:
  // --- scripting -----------------------------------------------------------
  void attach(std::uint8_t address) { present_.insert(address); }
  void detach(std::uint8_t address) { present_.erase(address); }

  // Register map, read through writeRead(reg) — how BMP280-style parts work.
  void setRegister(std::uint8_t address, std::uint8_t reg, std::uint8_t value) {
    registers_[address][reg] = value;
  }
  void setRegisters(std::uint8_t address, std::uint8_t reg,
                    const std::vector<std::uint8_t>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
      registers_[address][static_cast<std::uint8_t>(reg + i)] = values[i];
    }
  }

  // Register-less reads, queued in order — how AHT20-style parts work.
  void queueRead(std::uint8_t address, const std::vector<std::uint8_t>& bytes) {
    reads_[address].push_back(bytes);
  }

  // Make the next N transactions fail, to exercise the error paths.
  void failNext(std::size_t count) { failuresRemaining_ = count; }

  const std::vector<std::vector<std::uint8_t>>& writes() const { return writes_; }
  void clearWrites() { writes_.clear(); }

  // --- II2cBus -------------------------------------------------------------
  std::uint8_t index() const override { return 0; }

  bool probe(std::uint8_t address) override {
    return present_.count(address) > 0;
  }

  Status write(std::uint8_t address, const std::uint8_t* data,
               std::size_t length) override {
    if (Status failure = maybeFail(address); !failure.ok()) return failure;
    writes_.emplace_back(data, data + length);
    return ok();
  }

  Status read(std::uint8_t address, std::uint8_t* buffer,
              std::size_t length) override {
    if (Status failure = maybeFail(address); !failure.ok()) return failure;
    auto it = reads_.find(address);
    if (it == reads_.end() || it->second.empty()) {
      ++errors_;
      return fail(ErrorCode::kDeviceNotResponding, "nothing queued");
    }
    const std::vector<std::uint8_t> frame = it->second.front();
    it->second.pop_front();
    for (std::size_t i = 0; i < length; ++i) {
      buffer[i] = (i < frame.size()) ? frame[i] : 0;
    }
    return ok();
  }

  Status writeRead(std::uint8_t address, const std::uint8_t* out,
                   std::size_t outLength, std::uint8_t* in,
                   std::size_t inLength) override {
    if (Status failure = maybeFail(address); !failure.ok()) return failure;
    if (outLength != 1) return fail(ErrorCode::kInvalidArgument, "expected a register");
    const std::uint8_t reg = out[0];
    auto device = registers_.find(address);
    if (device == registers_.end()) {
      ++errors_;
      return fail(ErrorCode::kDeviceNotResponding, "no register map");
    }
    for (std::size_t i = 0; i < inLength; ++i) {
      const std::uint8_t key = static_cast<std::uint8_t>(reg + i);
      auto value = device->second.find(key);
      in[i] = (value == device->second.end()) ? 0 : value->second;
    }
    return ok();
  }

  std::uint32_t errorCount() const override { return errors_; }
  void resetErrorCount() override { errors_ = 0; }

 private:
  Status maybeFail(std::uint8_t address) {
    if (failuresRemaining_ > 0) {
      --failuresRemaining_;
      ++errors_;
      return fail(ErrorCode::kDeviceNotResponding, "scripted failure");
    }
    if (present_.count(address) == 0) {
      ++errors_;
      return fail(ErrorCode::kDeviceNotResponding, "NACK on address");
    }
    return ok();
  }

  std::set<std::uint8_t> present_;
  std::map<std::uint8_t, std::map<std::uint8_t, std::uint8_t>> registers_;
  std::map<std::uint8_t, std::deque<std::vector<std::uint8_t>>> reads_;
  std::vector<std::vector<std::uint8_t>> writes_;
  std::size_t failuresRemaining_ = 0;
  std::uint32_t errors_ = 0;
};

class FakeGpioPort final : public IGpioPort {
 public:
  // Levels returned by read(), in order.  When the queue for a pin runs out the
  // static level is used.
  void queueReads(std::uint8_t pin, const std::vector<bool>& levels) {
    for (bool level : levels) reads_[pin].push_back(level);
  }
  void setLevel(std::uint8_t pin, bool level) { levels_[pin] = level; }

  std::uint32_t risingEdges(std::uint8_t pin) const {
    auto it = risingEdges_.find(pin);
    return it == risingEdges_.end() ? 0 : it->second;
  }
  std::uint32_t totalDelayMicros() const { return delayMicros_; }
  PinMode modeOf(std::uint8_t pin) const { return modes_.at(pin); }

  Status configure(std::uint8_t pin, PinMode mode, DeviceHandle) override {
    modes_[pin] = mode;
    return ok();
  }

  Status write(std::uint8_t pin, bool high) override {
    if (high && !levels_[pin]) ++risingEdges_[pin];
    levels_[pin] = high;
    return ok();
  }

  Result<bool> read(std::uint8_t pin) override {
    auto it = reads_.find(pin);
    if (it != reads_.end() && !it->second.empty()) {
      const bool level = it->second.front();
      it->second.pop_front();
      return level;
    }
    return levels_[pin];
  }

  void delayMicros(std::uint32_t microseconds) const override {
    delayMicros_ += microseconds;
  }

 private:
  std::map<std::uint8_t, bool> levels_;
  std::map<std::uint8_t, std::deque<bool>> reads_;
  std::map<std::uint8_t, std::uint32_t> risingEdges_;
  std::map<std::uint8_t, PinMode> modes_;
  mutable std::uint32_t delayMicros_ = 0;
};

class FakeAdcPort final : public IAdcPort {
 public:
  void setRaw(std::uint16_t value) { raw_ = value; }
  void queueRaw(const std::vector<std::uint16_t>& values) {
    for (std::uint16_t value : values) queue_.push_back(value);
  }
  void setCalibrated(bool value) { calibrated_ = value; }
  std::size_t readCount() const { return readCount_; }

  Status configure(std::uint8_t, AdcAttenuation, DeviceHandle) override {
    return ok();
  }

  Result<std::uint16_t> readRaw(std::uint8_t) override {
    ++readCount_;
    if (!queue_.empty()) {
      const std::uint16_t value = queue_.front();
      queue_.pop_front();
      return value;
    }
    return raw_;
  }

  Result<float> readMillivolts(std::uint8_t pin) override {
    const Result<std::uint16_t> counts = readRaw(pin);
    if (!counts.ok()) return counts.error();
    // 12 bits over a nominal 2450 mV span at 11 dB.
    return static_cast<float>(counts.value()) * 2450.0f / 4095.0f;
  }

  bool calibrated() const override { return calibrated_; }

 private:
  std::uint16_t raw_ = 0;
  std::deque<std::uint16_t> queue_;
  bool calibrated_ = true;
  std::size_t readCount_ = 0;
};

class FakeBusProvider final : public IBusProvider {
 public:
  FakeI2cBus i2cBus;
  FakeGpioPort gpioPort;
  FakeAdcPort adcPort;
  bool i2cConfigured = true;

  II2cBus* i2c(std::uint8_t index) override {
    return (index == 0 && i2cConfigured) ? &i2cBus : nullptr;
  }
  std::uint8_t i2cBusCount() const override { return 1; }
  IGpioPort* gpio() override { return &gpioPort; }
  IAdcPort* adc() override { return &adcPort; }
};

}  // namespace test
}  // namespace lc
