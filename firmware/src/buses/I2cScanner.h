// =============================================================================
//  buses/I2cScanner.h — "what is actually plugged into this bus?" (§9)
//
//  Scanning is the single most useful diagnostic in a lab rig: it answers
//  "is it wired correctly" before the user has configured anything.
//
//  The scanner also suggests which module a responding address might be —
//  and is deliberately careful about it.  0x76 is BMP280, BME280, and a
//  handful of unrelated parts; 0x68 is DS3231, MPU6050 and others.  A guess is
//  a starting point for the form, never a fact.  Confirmation comes only from a
//  driver reading the chip's identification register in begin().
//
//  Pure logic over II2cBus, so it is unit-testable against a fake bus.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "buses/II2cBus.h"

namespace lc {

enum class HintConfidence : std::uint8_t {
  kPossible = 0,  // the address is shared by several unrelated parts
  kLikely,        // the address is characteristic of this part
};

struct ModuleHint {
  const char* moduleId = nullptr;  // manifest id, e.g. "bmp280"
  const char* label = nullptr;     // human name for the scan result
  HintConfidence confidence = HintConfidence::kPossible;
};

struct I2cScanEntry {
  std::uint8_t address = 0;
  const ModuleHint* hints = nullptr;
  std::uint8_t hintCount = 0;
};

class I2cScanner {
 public:
  static constexpr std::uint8_t kFirstAddress = 0x08;
  static constexpr std::uint8_t kLastAddress = 0x77;
  static constexpr std::size_t kMaxResults = 24;

  // Probes every valid 7-bit address.  Returns how many entries were written.
  static std::size_t scan(II2cBus& bus, I2cScanEntry* out, std::size_t capacity);

  // Candidate modules for an address, or nullptr.  Exposed separately so the
  // REST layer can annotate an address the user typed in by hand.
  static const ModuleHint* hintsFor(std::uint8_t address, std::uint8_t& count);
};

}  // namespace lc
