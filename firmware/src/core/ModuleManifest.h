// =============================================================================
//  core/ModuleManifest.h — self-describing modules (§7).
//
//  A manifest is `static constexpr` data living in flash.  It is the ONLY
//  description of a module the rest of the system needs:
//
//    * the REST API serialises it to JSON at /api/v1/modules;
//    * the frontend renders the whole "Add device" form from it — no
//      hand-written settings page per sensor;
//    * DeviceManager validates a submitted configuration against it before any
//      driver code runs;
//    * ResourceManager learns which pins to claim and with which PinUse.
//
//  Consequence: adding a supported sensor means adding a driver + a manifest.
//  It means touching zero frontend files.  That is the acceptance criterion in
//  §63, encoded in a data structure.
// =============================================================================
#pragma once

#include <cstdint>

#include "core/ResourceManager.h"
#include "core/Types.h"

namespace lc {

enum class ModuleCategory : std::uint8_t {
  kSensor = 0,
  kOutput,
  kProcessing,
  kControl,
  kVirtual,
  kSystem,
  kCount
};

const char* toString(ModuleCategory category);

// How a form field is rendered and validated.
enum class ParamType : std::uint8_t {
  kGpio = 0,      // pin picker, filtered by `pinUse` and the chip profile
  kInt,
  kFloat,
  kBool,
  kSelect,        // one of `options`
  kText,
  kI2cAddress,    // hex picker, pre-filled from an I²C scan
  kBusRef,        // "which I²C/SPI/UART bus"
  kChannelRef,    // reference to another channel (compensation, PID input)
};

struct ParamOption {
  const char* value = nullptr;  // stored in the configuration
  const char* label = nullptr;  // shown in the UI
};

struct ParamSpec {
  const char* key = nullptr;    // configuration key, stable forever
  const char* label = nullptr;
  ParamType type = ParamType::kInt;
  const char* unit = nullptr;
  const char* help = nullptr;

  float minimum = 0.0f;
  float maximum = 0.0f;
  float step = 0.0f;
  const char* defaultValue = nullptr;  // textual; parsed according to `type`

  const ParamOption* options = nullptr;
  std::uint8_t optionCount = 0;

  PinUse pinUse = PinUse::kDigitalInput;  // only meaningful for kGpio
  bool required = true;
  bool advanced = false;         // hidden behind "Advanced" in the UI (§60)
  const char* visibleIf = nullptr;  // "mode=continuous" — simple UI condition
};

// One data stream this module type produces (or consumes, for outputs).
struct ChannelSpec {
  const char* id = nullptr;       // "temperature" — unique within the module
  const char* name = nullptr;     // "Temperature"
  const char* unit = nullptr;     // "degC"
  // Physical quantity, used for unit conversion, sensible default widgets and
  // for suggesting compensation sources.  Free-form but conventional:
  // temperature | humidity | pressure | mass | distance | voltage | current |
  // power | ratio | count | raw.
  const char* quantity = "raw";
  ChannelDirection direction = ChannelDirection::kInput;
  float minimum = 0.0f;
  float maximum = 0.0f;
  std::uint8_t precision = 2;
  bool defaultLogged = true;

  // OUTPUTS ONLY.  The value this channel is driven to whenever the system is
  // not certain a command is still wanted: at boot, when a command expires,
  // when the device faults, when the safety layer trips.  There is no
  // "unspecified" — an unspecified safe state becomes zero, and zero is wrong
  // on the first device where it is wrong (ADR-0016).
  float safeValue = 0.0f;
  // When true the configuration may not override `safeValue`.  A heater's safe
  // state of 0 % is not a default — "safe = 40 %" is not a configuration, it is
  // an accident waiting for a power cut.
  bool safeValueFixed = false;
  // Seconds a command stays valid without being renewed.  0 waives the
  // deadline, which a manifest may do only where leaving the output engaged
  // unattended is genuinely harmless.
  float defaultHoldSeconds = 0.0f;
};

// What the module needs from the hardware layer.  Declared rather than
// inferred: DeviceManager checks it during validate(), so `?dry_run=1` cannot
// approve a configuration that the real create would then reject.  (It once
// could: an HX711 validated fine on a build with no GPIO port and then failed
// in configure() with "incomplete device context".)
enum class BusRequirement : std::uint8_t {
  kNone = 0,   // needs nothing (software modules)
  kGpio,       // needs the GPIO port (bit-banged parts, digital inputs)
  kAdc,        // needs the ADC port
  kPwm,        // needs a hardware PWM channel
  kI2c,
  kSpi,
  kUart,
  kOneWire,
};

struct ModuleManifest {
  const char* id = nullptr;    // "hx711" — stable, lowercase, used in configs
  const char* name = nullptr;  // "HX711 Load Cell"
  ModuleCategory category = ModuleCategory::kSensor;
  const char* description = nullptr;

  BusRequirement bus = BusRequirement::kNone;

  const ParamSpec* params = nullptr;
  std::uint8_t paramCount = 0;

  const ChannelSpec* channels = nullptr;
  std::uint8_t channelCount = 0;

  // 0 = unlimited.  Set for singletons such as an on-chip temperature sensor.
  std::uint8_t maxInstances = 0;

  // Acquisition timing bounds enforced by DeviceManager (§16).
  Micros defaultSampleIntervalUs = 100000;  // 10 Hz
  Micros minSampleIntervalUs = 1000;        // 1 kHz ceiling by default

  // Bumped when a manifest changes in a way that invalidates stored configs;
  // ConfigStorage migrations key off it.
  std::uint16_t schemaVersion = 1;

  const ParamSpec* findParam(const char* key) const;
  const ChannelSpec* findChannel(const char* id) const;
};

}  // namespace lc
