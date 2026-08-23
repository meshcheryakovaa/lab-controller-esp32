#include "api/Serializers.h"

#include <cstdio>

#include "storage/JsonUtils.h"

namespace lc {

const char* toString(ParamType type) {
  switch (type) {
    case ParamType::kGpio:       return "gpio";
    case ParamType::kInt:        return "int";
    case ParamType::kFloat:      return "float";
    case ParamType::kBool:       return "bool";
    case ParamType::kSelect:     return "select";
    case ParamType::kText:       return "text";
    case ParamType::kI2cAddress: return "i2c_address";
    case ParamType::kBusRef:     return "bus_ref";
    case ParamType::kChannelRef: return "channel_ref";
  }
  return "text";
}

const char* toString(PinUse use) {
  switch (use) {
    case PinUse::kDigitalInput:  return "digital_input";
    case PinUse::kDigitalOutput: return "digital_output";
    case PinUse::kAnalogInput:   return "analog_input";
    case PinUse::kPwmOutput:     return "pwm_output";
    case PinUse::kBusSignal:     return "bus_signal";
  }
  return "digital_input";
}

const char* toString(BusRequirement bus) {
  switch (bus) {
    case BusRequirement::kNone:    return "none";
    case BusRequirement::kGpio:    return "gpio";
    case BusRequirement::kAdc:     return "adc";
    case BusRequirement::kPwm:     return "pwm";
    case BusRequirement::kI2c:     return "i2c";
    case BusRequirement::kSpi:     return "spi";
    case BusRequirement::kUart:    return "uart";
    case BusRequirement::kOneWire: return "onewire";
  }
  return "none";
}

const char* toString(ChannelDirection direction) {
  return direction == ChannelDirection::kOutput ? "output" : "input";
}

const char* toString(CoordinateSystem system) {
  switch (system) {
    case CoordinateSystem::kCartesian:   return "cartesian";
    case CoordinateSystem::kCylindrical: return "cylindrical";
    case CoordinateSystem::kNone:        break;
  }
  return "none";
}

void serializeGeometry(const Geometry& geometry, JsonObject out) {
  out["system"] = toString(geometry.system);
  out["a"] = geometry.a;
  out["b"] = geometry.b;
  out["c"] = geometry.c;
  if (!geometry.group.empty()) out["group"] = jsonCopy(geometry.group.c_str());
  if (!geometry.role.empty()) out["role"] = jsonCopy(geometry.role.c_str());
}

// ---------------------------------------------------------------------------
//  Manifests
// ---------------------------------------------------------------------------
void serializeManifest(const ModuleManifest& manifest, JsonObject out) {
  out["id"] = manifest.id;
  out["name"] = manifest.name;
  out["category"] = toString(manifest.category);
  if (manifest.description != nullptr) out["description"] = manifest.description;
  out["bus"] = toString(manifest.bus);
  out["max_instances"] = manifest.maxInstances;
  out["default_sample_interval_us"] = manifest.defaultSampleIntervalUs;
  out["min_sample_interval_us"] = manifest.minSampleIntervalUs;
  out["schema_version"] = manifest.schemaVersion;

  JsonArray params = out["params"].to<JsonArray>();
  for (std::uint8_t i = 0; i < manifest.paramCount; ++i) {
    const ParamSpec& spec = manifest.params[i];
    JsonObject param = params.add<JsonObject>();
    param["key"] = spec.key;
    param["label"] = spec.label;
    param["type"] = toString(spec.type);
    param["required"] = spec.required;
    param["advanced"] = spec.advanced;
    if (spec.unit != nullptr) param["unit"] = spec.unit;
    if (spec.help != nullptr) param["help"] = spec.help;
    if (spec.visibleIf != nullptr) param["visible_if"] = spec.visibleIf;
    // Omitting min == max keeps the frontend from applying a nonsense range.
    if (spec.minimum < spec.maximum) {
      param["min"] = spec.minimum;
      param["max"] = spec.maximum;
    }
    if (spec.step != 0.0f) param["step"] = spec.step;
    if (spec.defaultValue != nullptr) param["default"] = spec.defaultValue;
    if (spec.type == ParamType::kGpio) param["pin_use"] = toString(spec.pinUse);

    if (spec.optionCount > 0 && spec.options != nullptr) {
      JsonArray options = param["options"].to<JsonArray>();
      for (std::uint8_t o = 0; o < spec.optionCount; ++o) {
        JsonObject option = options.add<JsonObject>();
        option["value"] = spec.options[o].value;
        option["label"] = spec.options[o].label;
      }
    }
  }

  JsonArray channels = out["channels"].to<JsonArray>();
  for (std::uint8_t i = 0; i < manifest.channelCount; ++i) {
    const ChannelSpec& spec = manifest.channels[i];
    JsonObject channel = channels.add<JsonObject>();
    channel["id"] = spec.id;
    channel["name"] = spec.name;
    channel["unit"] = spec.unit;
    channel["quantity"] = spec.quantity;
    channel["direction"] = toString(spec.direction);
    channel["precision"] = spec.precision;
    channel["default_logged"] = spec.defaultLogged;
    if (spec.minimum < spec.maximum) {
      channel["min"] = spec.minimum;
      channel["max"] = spec.maximum;
    }
  }
}

// ---------------------------------------------------------------------------
//  Hardware
// ---------------------------------------------------------------------------
void serializeGpioMap(const ResourceManager& resources, JsonObject out) {
  const ChipProfile& chip = resources.chip();
  out["chip"] = chip.name;
  out["i2c_buses"] = chip.i2cBusCount;
  out["pwm_channels"] = chip.ledcChannelCount;

  JsonArray pins = out["pins"].to<JsonArray>();
  for (std::uint8_t number = 0; number < chip.gpioCount; ++number) {
    const GpioCapability* capability = chip.pin(number);
    if (capability == nullptr || !capability->exists) continue;

    JsonObject pin = pins.add<JsonObject>();
    pin["pin"] = number;
    pin["usable"] = !capability->reserved;
    if (capability->inputOnly) pin["input_only"] = true;
    if (capability->strapping) pin["strapping"] = true;
    if (capability->adc1Channel >= 0) pin["adc1"] = capability->adc1Channel;
    if (capability->adc2Channel >= 0) pin["adc2"] = capability->adc2Channel;
    if (capability->reserved) pin["reason"] = errorSymbol(ErrorCode::kGpioReserved);

    const char* advisory = resources.pinAdvisory(number);
    if (advisory != nullptr) pin["advisory"] = advisory;

    // The owner is what turns "unavailable" into "used by I2C0 SDA".
    const ResourceClaim* claim = resources.find(gpioResource(number));
    if (claim != nullptr) {
      pin["owner"] = jsonCopy(claim->label.c_str());
      pin["owner_device"] = claim->owner;
      pin["use"] = toString(claim->use);
    }
  }
}

void serializeResourceClaims(const ResourceManager& resources, JsonArray out) {
  for (std::size_t i = 0; i < resources.claimCount(); ++i) {
    const ResourceClaim& claim = resources.claimAt(i);
    JsonObject entry = out.add<JsonObject>();
    entry["kind"] = toString(claim.id.kind);
    entry["index"] = claim.id.index;
    if (claim.id.sub != 0) entry["sub"] = claim.id.sub;
    entry["owner"] = claim.owner;
    entry["label"] = jsonCopy(claim.label.c_str());
  }
}

// ---------------------------------------------------------------------------
//  Channels
// ---------------------------------------------------------------------------
void serializeChannel(ChannelHandle handle, const ChannelDescriptor& descriptor,
                      const ChannelValue& value, JsonObject out,
                      bool includeValue) {
  out["handle"] = handle;
  out["key"] = jsonCopy(descriptor.key.c_str());
  out["name"] = jsonCopy(descriptor.name.c_str());
  out["unit"] = jsonCopy(descriptor.unit.c_str());
  out["quantity"] = jsonCopy(descriptor.quantity.c_str());
  out["source"] = descriptor.source;
  out["direction"] = toString(descriptor.direction);
  out["precision"] = descriptor.precision;
  out["logged"] = descriptor.logged;
  out["visible"] = descriptor.visible;
  out["expected_interval_us"] = descriptor.expectedIntervalUs;
  if (descriptor.minimum < descriptor.maximum) {
    out["min"] = descriptor.minimum;
    out["max"] = descriptor.maximum;
  }

  char colour[8];
  std::snprintf(colour, sizeof(colour), "#%06X",
                static_cast<unsigned>(descriptor.color & 0xFFFFFF));
  out["color"] = jsonCopy(colour);

  if (descriptor.geometry.isDefined()) {
    serializeGeometry(descriptor.geometry, out["geometry"].to<JsonObject>());
  }

  if (!includeValue) return;

  JsonObject reading = out["value"].to<JsonObject>();
  // All three stages, because a channel that reads wrong is diagnosed by
  // comparing them (§48).
  reading["raw"] = value.raw;
  reading["calibrated"] = value.calibrated;
  reading["processed"] = value.processed;
  reading["quality"] = toString(value.quality);
  reading["sequence"] = value.sequence;
  reading["t"] = value.timestampUs / 1000ULL;
  reading["epoch"] = value.epochMs;
}

// ---------------------------------------------------------------------------
//  Scheduler
// ---------------------------------------------------------------------------
void serializeSchedulerStats(const Scheduler& scheduler, JsonArray out) {
  Scheduler::TaskInfo info;
  for (std::size_t i = 0; scheduler.taskAt(i, info); ++i) {
    JsonObject task = out.add<JsonObject>();
    task["name"] = jsonCopy(info.name);
    task["priority"] = static_cast<int>(info.priority);
    task["period_us"] = info.periodUs;
    task["enabled"] = info.enabled;
    task["runs"] = info.stats.runs;
    task["overruns"] = info.stats.overruns;
    task["misses"] = info.stats.misses;
    task["last_us"] = info.stats.lastDurationUs;
    task["max_us"] = info.stats.maxDurationUs;
    task["max_lateness_us"] = info.stats.maxLatenessUs;
  }
}


void serializeNetwork(const INetworkManager& network, JsonObject out) {
  const NetworkStatus status = network.status();

  out["state"] = toString(status.state);
  out["configured"] = status.configured;
  out["ssid"] = jsonCopy(status.ssid.c_str());
  // What the operator needs to know is whether a password is REMEMBERED, which
  // this answers without the password existing anywhere in the response.
  out["password_set"] = status.configured;
  out["hostname"] = jsonCopy(status.hostname.c_str());
  // The address that actually works today.  ".local" is offered as well as the
  // IP, never instead of it: plenty of machines cannot resolve mDNS, and an
  // instrument that only tells you a name you cannot reach is unreachable.
  if (!status.hostname.empty()) {
    char local[kHostnameLength + 8];
    std::snprintf(local, sizeof(local), "%s.local", status.hostname.c_str());
    out["mdns"] = jsonCopy(local);
  }
  // True while credentials are being proved, so the page knows to keep polling
  // rather than concluding the attempt failed.
  out["pending"] = status.testing;

  JsonObject station = out["station"].to<JsonObject>();
  station["connected"] = status.stationConnected;
  station["ip"] = jsonCopy(status.stationIp.c_str());
  station["rssi"] = status.rssi;

  JsonObject ap = out["access_point"].to<JsonObject>();
  ap["active"] = status.accessPointActive;
  ap["ssid"] = jsonCopy(status.accessPointSsid.c_str());
  ap["ip"] = jsonCopy(status.accessPointIp.c_str());

  out["reconnects"] = status.reconnects;
  out["disconnects"] = status.disconnects;
  out["last_disconnect_reason"] =
      jsonCopy(status.lastDisconnectReason.c_str());

  if (status.lastError.ok()) {
    out["last_error"] = nullptr;
  } else {
    JsonObject error = out["last_error"].to<JsonObject>();
    error["code"] = status.lastError.symbol();
    error["detail"] = jsonCopy(status.lastError.detail.c_str());
  }
}

}  // namespace lc
