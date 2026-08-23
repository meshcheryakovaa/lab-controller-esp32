// Declaration-only Arduino core, for the syntax check described in
// tools/esp32_typecheck/README.md.  Nothing here is implemented.
#pragma once

#include <cassert>
#include <climits>
#include <cstdarg>
#include <functional>
#include <cstddef>
#include <cstdint>

#include "WString.h"
#include "pgmspace.h"

#define ARDUINO 200
#define ESP32 1

#define HIGH 0x1
#define LOW 0x0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define INPUT_PULLDOWN 0x09
#define OUTPUT_OPEN_DRAIN 0x13
#define PULLUP 0x04
#define PULLDOWN 0x08
#define LED_BUILTIN 2

using boolean = bool;
using byte = std::uint8_t;

unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void yield();

// esp32-hal-time.h, which the real Arduino.h pulls in.  M17 needs a wall clock
// before it will talk TLS, and this is what starts SNTP.
void configTime(long gmtOffset_sec, int daylightOffset_sec, const char* server1,
                const char* server2 = nullptr, const char* server3 = nullptr);

void pinMode(std::uint8_t pin, std::uint8_t mode);
void digitalWrite(std::uint8_t pin, std::uint8_t value);
int digitalRead(std::uint8_t pin);
std::uint16_t analogRead(std::uint8_t pin);
std::uint32_t analogReadMilliVolts(std::uint8_t pin);
void analogReadResolution(std::uint8_t bits);
void analogSetAttenuation(int attenuation);
void analogSetPinAttenuation(std::uint8_t pin, int attenuation);

typedef enum {
  ADC_0db, ADC_2_5db, ADC_6db, ADC_11db, ADC_ATTENDB_MAX
} adc_attenuation_t;

// LEDC (PWM).  The pin-oriented overloads are the ESP32 Arduino core 2.x
// spelling, which is what platformio.ini pins.
double ledcSetup(std::uint8_t channel, double freq, std::uint8_t resolution);
void ledcAttachPin(std::uint8_t pin, std::uint8_t channel);
void ledcDetachPin(std::uint8_t pin);
void ledcWrite(std::uint8_t channel, std::uint32_t duty);

class Print {
 public:
  virtual ~Print() = default;
  virtual std::size_t write(std::uint8_t) = 0;
  virtual std::size_t write(const std::uint8_t* buffer, std::size_t size);
  virtual void flush();
  std::size_t print(const char*);
  std::size_t print(const String&);
  std::size_t print(int);
  std::size_t println(const char*);
  std::size_t println(const String&);
  std::size_t println();
  std::size_t printf(const char* format, ...);
};

class Printable {
 public:
  virtual ~Printable() = default;
  virtual std::size_t printTo(Print& p) const = 0;
};

class Stream : public Print {
 public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  // VIRTUAL, as in the real Arduino Stream: M17's upload adapter overrides it
  // so HTTPClient pulls the file 4 KiB at a time instead of byte by byte.
  virtual std::size_t readBytes(char* buffer, std::size_t length);
  std::size_t readBytes(std::uint8_t* buffer, std::size_t length);
  virtual void flush();
};

class HardwareSerial : public Stream {
 public:
  void begin(unsigned long baud);
  void flush();
  std::size_t write(std::uint8_t) override;
  int available() override;
  int read() override;
  int peek() override;
};

extern HardwareSerial Serial;

class IPAddress {
 public:
  IPAddress() = default;
  IPAddress(std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t) {}
  String toString() const;
  operator std::uint32_t() const;
  std::uint8_t operator[](int index) const;
};

// The Arduino-ESP32 chip singleton.
class EspClass {
 public:
  std::uint32_t getFreeHeap();
  std::uint32_t getMinFreeHeap();
  std::uint32_t getMaxAllocHeap();
  std::uint32_t getHeapSize();
  std::uint32_t getPsramSize();
  std::uint32_t getFreePsram();
  std::uint32_t getSketchSize();
  std::uint32_t getFreeSketchSpace();
  std::uint32_t getFlashChipSize();
  std::uint32_t getCpuFreqMHz();
  const char* getChipModel();
  std::uint8_t getChipRevision();
  std::uint64_t getEfuseMac();
  void restart();
};

extern EspClass ESP;

std::uint32_t esp_get_free_heap_size();
