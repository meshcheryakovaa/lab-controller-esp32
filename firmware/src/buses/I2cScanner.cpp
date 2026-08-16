#include "buses/I2cScanner.h"

namespace lc {
namespace {

constexpr ModuleHint kHints0x29[] = {
    {"vl53l0x", "VL53L0X distance sensor", HintConfidence::kLikely},
};
constexpr ModuleHint kHints0x38[] = {
    {"aht20", "AHT20 temperature/humidity", HintConfidence::kLikely},
};
constexpr ModuleHint kHints0x40[] = {
    {"ina219", "INA219 current sensor", HintConfidence::kPossible},
    {"sht31", "SHT3x temperature/humidity", HintConfidence::kPossible},
};
constexpr ModuleHint kHints0x48[] = {
    {"ads1115", "ADS111x ADC", HintConfidence::kPossible},
    {"lm75", "LM75 temperature", HintConfidence::kPossible},
};
constexpr ModuleHint kHints0x68[] = {
    {"ds3231", "DS3231 RTC", HintConfidence::kPossible},
    {"mpu6050", "MPU6050 IMU", HintConfidence::kPossible},
};
constexpr ModuleHint kHints0x76[] = {
    {"bmp280", "BMP280 pressure/temperature", HintConfidence::kPossible},
    {"bme280", "BME280 pressure/temperature/humidity", HintConfidence::kPossible},
};
constexpr ModuleHint kHints0x77[] = {
    {"bmp280", "BMP280 pressure/temperature (SDO high)", HintConfidence::kPossible},
    {"bme280", "BME280 (SDO high)", HintConfidence::kPossible},
    {"bmp180", "BMP180 pressure/temperature", HintConfidence::kPossible},
};

struct HintTableEntry {
  std::uint8_t address;
  const ModuleHint* hints;
  std::uint8_t count;
};

constexpr HintTableEntry kHintTable[] = {
    {0x29, kHints0x29, 1},
    {0x38, kHints0x38, 1},
    {0x40, kHints0x40, 2},
    {0x48, kHints0x48, 2},
    {0x68, kHints0x68, 2},
    {0x76, kHints0x76, 2},
    {0x77, kHints0x77, 3},
};

}  // namespace

const ModuleHint* I2cScanner::hintsFor(std::uint8_t address,
                                       std::uint8_t& count) {
  for (const HintTableEntry& entry : kHintTable) {
    if (entry.address == address) {
      count = entry.count;
      return entry.hints;
    }
  }
  count = 0;
  return nullptr;
}

std::size_t I2cScanner::scan(II2cBus& bus, I2cScanEntry* out,
                             std::size_t capacity) {
  if (out == nullptr || capacity == 0) return 0;

  std::size_t found = 0;
  for (std::uint16_t address = kFirstAddress;
       address <= kLastAddress && found < capacity; ++address) {
    const std::uint8_t candidate = static_cast<std::uint8_t>(address);
    if (!bus.probe(candidate)) continue;

    I2cScanEntry& entry = out[found++];
    entry.address = candidate;
    entry.hints = hintsFor(candidate, entry.hintCount);
  }
  return found;
}

}  // namespace lc
