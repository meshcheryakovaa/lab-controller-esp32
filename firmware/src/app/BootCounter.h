// =============================================================================
//  app/BootCounter.h — how the firmware notices it keeps crashing.
//
//  A configuration can be valid JSON, pass validation, and still hang the board
//  (a driver that spins on a bus that is shorted, for instance).  Without a
//  boot counter the only cure is a serial cable and a flash erase — which for a
//  lab instrument in a rack is a bad afternoon.
//
//  The counter is incremented BEFORE devices are started and cleared once the
//  system has been up long enough to be considered healthy.  Too many failures
//  in a row and the next boot comes up in safe mode: network and API only,
//  no devices, configuration editable.
// =============================================================================
#pragma once

#include <cstdint>

namespace lc {

class IBootCounter {
 public:
  virtual ~IBootCounter() = default;

  virtual std::uint8_t consecutiveFailures() const = 0;
  virtual void markAttempt() = 0;
  virtual void markSuccess() = 0;
};

// For the host tests and for boards without persistent storage.
class MemoryBootCounter final : public IBootCounter {
 public:
  explicit MemoryBootCounter(std::uint8_t initial = 0) : failures_(initial) {}

  std::uint8_t consecutiveFailures() const override { return failures_; }
  void markAttempt() override {
    if (failures_ < 0xFF) ++failures_;
  }
  void markSuccess() override { failures_ = 0; }

 private:
  std::uint8_t failures_ = 0;
};

}  // namespace lc
