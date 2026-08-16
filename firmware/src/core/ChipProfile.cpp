#include "core/ChipProfile.h"

namespace lc {
namespace {

// Helper so the tables below stay readable.
constexpr GpioCapability none() { return GpioCapability{}; }

constexpr GpioCapability io(std::int8_t adc1 = -1, std::int8_t adc2 = -1,
                            bool strapping = false, bool touch = false,
                            const char* note = nullptr) {
  GpioCapability cap{};
  cap.exists = true;
  cap.adc1Channel = adc1;
  cap.adc2Channel = adc2;
  cap.strapping = strapping;
  cap.touchCapable = touch;
  cap.note = note;
  return cap;
}

constexpr GpioCapability inputOnly(std::int8_t adc1, const char* note = nullptr) {
  GpioCapability cap{};
  cap.exists = true;
  cap.inputOnly = true;
  cap.adc1Channel = adc1;
  cap.note = note;
  return cap;
}

constexpr GpioCapability reserved(const char* note) {
  GpioCapability cap{};
  cap.exists = true;
  cap.reserved = true;
  cap.note = note;
  return cap;
}

// ---------------------------------------------------------------------------
//  ESP32 / WROOM-32 (classic).  40 GPIO numbers, several of them fictional.
// ---------------------------------------------------------------------------
constexpr GpioCapability kEsp32Gpio[] = {
    /* 0  */ io(-1, 1, /*strapping=*/true, true,
                "Boot strapping pin: must be HIGH at reset"),
    /* 1  */ io(-1, -1, false, false, "UART0 TX — used by the serial console"),
    /* 2  */ io(-1, 2, true, true,
                "Strapping pin; on many boards also the onboard LED"),
    /* 3  */ io(-1, -1, false, false, "UART0 RX — used by the serial console"),
    /* 4  */ io(-1, 0, false, true),
    /* 5  */ io(-1, -1, true, false, "Strapping pin (VSPI CS default)"),
    /* 6  */ reserved("Connected to SPI flash"),
    /* 7  */ reserved("Connected to SPI flash"),
    /* 8  */ reserved("Connected to SPI flash"),
    /* 9  */ reserved("Connected to SPI flash"),
    /* 10 */ reserved("Connected to SPI flash"),
    /* 11 */ reserved("Connected to SPI flash"),
    /* 12 */ io(-1, 5, true, true,
                "Strapping pin (MTDI): must be LOW at reset on 3.3 V flash"),
    /* 13 */ io(-1, 4, false, true),
    /* 14 */ io(-1, 6, false, true),
    /* 15 */ io(-1, 3, true, true, "Strapping pin: silences boot log when LOW"),
    /* 16 */ io(-1, -1, false, false, "Used for PSRAM on WROVER modules"),
    /* 17 */ io(-1, -1, false, false, "Used for PSRAM on WROVER modules"),
    /* 18 */ io(),
    /* 19 */ io(),
    /* 20 */ none(),
    /* 21 */ io(),
    /* 22 */ io(),
    /* 23 */ io(),
    /* 24 */ none(),
    /* 25 */ io(-1, 8),
    /* 26 */ io(-1, 9),
    /* 27 */ io(-1, 7, false, true),
    /* 28 */ none(),
    /* 29 */ none(),
    /* 30 */ none(),
    /* 31 */ none(),
    /* 32 */ io(4, -1, false, true),
    /* 33 */ io(5, -1, false, true),
    /* 34 */ inputOnly(6),
    /* 35 */ inputOnly(7),
    /* 36 */ inputOnly(0, "SENSOR_VP"),
    /* 37 */ inputOnly(1, "Not exposed on most DevKit boards"),
    /* 38 */ inputOnly(2, "Not exposed on most DevKit boards"),
    /* 39 */ inputOnly(3, "SENSOR_VN"),
};

// ---------------------------------------------------------------------------
//  ESP32-S3.  49 GPIO numbers.  26..32 belong to the in-package flash/PSRAM on
//  every module we intend to support; 33..37 are additionally taken when the
//  module carries octal PSRAM, which we cannot detect from a static table, so
//  they are marked with a note rather than hard-reserved.
// ---------------------------------------------------------------------------
constexpr GpioCapability kEsp32s3Gpio[] = {
    /* 0  */ io(-1, -1, true, false, "Boot strapping pin"),
    /* 1  */ io(0, -1, false, true),
    /* 2  */ io(1, -1, false, true),
    /* 3  */ io(2, -1, true, true, "Strapping pin (JTAG source select)"),
    /* 4  */ io(3, -1, false, true),
    /* 5  */ io(4, -1, false, true),
    /* 6  */ io(5, -1, false, true),
    /* 7  */ io(6, -1, false, true),
    /* 8  */ io(7, -1, false, true),
    /* 9  */ io(8, -1, false, true),
    /* 10 */ io(9, -1, false, true),
    /* 11 */ io(-1, 0, false, true),
    /* 12 */ io(-1, 1, false, true),
    /* 13 */ io(-1, 2, false, true),
    /* 14 */ io(-1, 3, false, true),
    /* 15 */ io(-1, 4),
    /* 16 */ io(-1, 5),
    /* 17 */ io(-1, 6),
    /* 18 */ io(-1, 7),
    /* 19 */ io(-1, 8, false, false, "USB D-"),
    /* 20 */ io(-1, 9, false, false, "USB D+"),
    /* 21 */ io(),
    /* 22 */ none(),
    /* 23 */ none(),
    /* 24 */ none(),
    /* 25 */ none(),
    /* 26 */ reserved("SPI flash / PSRAM"),
    /* 27 */ reserved("SPI flash / PSRAM"),
    /* 28 */ reserved("SPI flash / PSRAM"),
    /* 29 */ reserved("SPI flash / PSRAM"),
    /* 30 */ reserved("SPI flash / PSRAM"),
    /* 31 */ reserved("SPI flash / PSRAM"),
    /* 32 */ reserved("SPI flash / PSRAM"),
    /* 33 */ io(-1, -1, false, false, "Occupied when the module has octal PSRAM"),
    /* 34 */ io(-1, -1, false, false, "Occupied when the module has octal PSRAM"),
    /* 35 */ io(-1, -1, false, false, "Occupied when the module has octal PSRAM"),
    /* 36 */ io(-1, -1, false, false, "Occupied when the module has octal PSRAM"),
    /* 37 */ io(-1, -1, false, false, "Occupied when the module has octal PSRAM"),
    /* 38 */ io(),
    /* 39 */ io(),
    /* 40 */ io(),
    /* 41 */ io(),
    /* 42 */ io(),
    /* 43 */ io(-1, -1, false, false, "UART0 TX — serial console"),
    /* 44 */ io(-1, -1, false, false, "UART0 RX — serial console"),
    /* 45 */ io(-1, -1, true, false, "Strapping pin (VDD_SPI voltage)"),
    /* 46 */ io(-1, -1, true, false, "Strapping pin (boot log control)"),
    /* 47 */ io(),
    /* 48 */ io(-1, -1, false, false, "Onboard RGB LED on many boards"),
};

constexpr ChipProfile kEsp32Profile = {
    /*name*/ "esp32",
    /*gpio*/ kEsp32Gpio,
    /*gpioCount*/ static_cast<std::uint8_t>(sizeof(kEsp32Gpio) / sizeof(kEsp32Gpio[0])),
    /*i2cBusCount*/ 2,
    /*spiBusCount*/ 2,   // HSPI + VSPI
    /*uartPortCount*/ 3,
    /*ledcChannelCount*/ 16,
    /*ledcTimerCount*/ 4,
    /*adc2UsableWithWifi*/ false,
};

constexpr ChipProfile kEsp32s3Profile = {
    /*name*/ "esp32s3",
    /*gpio*/ kEsp32s3Gpio,
    /*gpioCount*/ static_cast<std::uint8_t>(sizeof(kEsp32s3Gpio) / sizeof(kEsp32s3Gpio[0])),
    /*i2cBusCount*/ 2,
    /*spiBusCount*/ 2,
    /*uartPortCount*/ 3,
    /*ledcChannelCount*/ 8,
    /*ledcTimerCount*/ 4,
    /*adc2UsableWithWifi*/ false,
};

}  // namespace

const ChipProfile& ChipProfile::esp32() { return kEsp32Profile; }
const ChipProfile& ChipProfile::esp32s3() { return kEsp32s3Profile; }

const ChipProfile& ChipProfile::current() {
#if defined(LC_TARGET_ESP32S3)
  return kEsp32s3Profile;
#else
  // The host test build intentionally reports the classic ESP32 map so that
  // pin-validation tests exercise the same table the DevKit will use.
  return kEsp32Profile;
#endif
}

}  // namespace lc
