// =============================================================================
//  platform/esp32/NvsBootCounter.h — boot failure streak in NVS.
//
//  NVS, not LittleFS: the counter has to survive a filesystem that will not
//  mount, and it has to be readable before anything else is initialised.
//
//  "Before anything else" has a floor, and Milestone 12 found it.  This class
//  used to open NVS in its CONSTRUCTOR, and the object is at namespace scope in
//  main.cpp — so it ran before Arduino's initArduino() had called
//  nvs_flash_init().  Every boot printed
//
//      nvs_open failed: ESP_ERR_NVS_NOT_INITIALIZED
//
//  and the counter silently read zero forever, which means the safe-mode
//  mechanism that exists to rescue a board stuck in a boot loop was itself
//  never armed.  A failure that hides the recovery path is the worst kind.
//
//  So: the constructor touches nothing, and begin() — called from setup(), once
//  the platform is actually up — opens the namespace.  Until then the counter
//  answers zero and refuses to write, which is the honest answer for "I have
//  not been able to look yet".
// =============================================================================
#pragma once

#include <Preferences.h>

#include "app/BootCounter.h"

namespace lc {
namespace platform {

class NvsBootCounter final : public IBootCounter {
 public:
  NvsBootCounter() = default;

  ~NvsBootCounter() override {
    if (open_) preferences_.end();
  }

  // Read/write: opening a namespace that has never been written read-only fails
  // with NOT_FOUND on a factory-fresh board, which is not an error — it is what
  // "first boot" looks like.  See WifiManager for the same lesson.
  bool begin() {
    if (open_) return true;
    open_ = preferences_.begin("lc-boot", /*readOnly=*/false);
    if (open_) failures_ = preferences_.getUChar("fails", 0);
    return open_;
  }

  bool ready() const { return open_; }

  std::uint8_t consecutiveFailures() const override { return failures_; }

  void markAttempt() override {
    if (failures_ < 0xFF) ++failures_;
    if (open_) preferences_.putUChar("fails", failures_);
  }

  void markSuccess() override {
    if (failures_ == 0) return;  // avoid a pointless NVS write every boot
    failures_ = 0;
    if (open_) preferences_.putUChar("fails", 0);
  }

 private:
  mutable Preferences preferences_;
  std::uint8_t failures_ = 0;
  bool open_ = false;
};

}  // namespace platform
}  // namespace lc
