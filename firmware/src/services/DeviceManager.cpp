#include "services/DeviceManager.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lc {
namespace {

// Default series colours, handed out round-robin so a fresh dashboard is
// readable before the user picks anything.
constexpr std::uint32_t kPalette[] = {0x4C9AFF, 0xFF8A3D, 0x3FB950,
                                      0xD29922, 0xA371F7, 0x2DD4BF};

bool isBlank(const char* text) { return text == nullptr || text[0] == '\0'; }

void setField(LabelString* field, const char* key) {
  if (field != nullptr) field->assign(key);
}

}  // namespace

const ChannelOverride* DeviceSpec::findOverride(const char* specId) const {
  if (specId == nullptr) return nullptr;
  for (std::uint8_t i = 0; i < overrideCount; ++i) {
    if (overrides[i].specId.equals(specId)) return &overrides[i];
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
//  Construction
// ---------------------------------------------------------------------------
DeviceManager::DeviceManager(const IClock& clock, ModuleRegistry& registry,
                             ResourceManager& resources, ChannelManager& channels,
                             Scheduler& scheduler, EventBus& events)
    : clock_(clock),
      registry_(registry),
      resources_(resources),
      channels_(channels),
      scheduler_(scheduler),
      events_(events) {}

DeviceManager::~DeviceManager() { removeAll(); }

Status DeviceManager::begin() {
  if (started_) return ok();

  // One shared retry task drives every device still in INITIALIZING.  A task
  // per device would exhaust the scheduler table for no benefit: initialisation
  // is rare and short.
  const Result<TaskId> task = scheduler_.addPeriodic(
      "devices.init", 10000, TaskPriority::kControl, retryTrampoline, this);
  if (!task.ok()) return task.error();
  retryTask_ = task.value();

  channels_.setOutputSink(outputSinkTrampoline, this);
  started_ = true;
  return ok();
}

// ---------------------------------------------------------------------------
//  Validation
// ---------------------------------------------------------------------------
Status DeviceManager::validateParam(const ParamSpec& param,
                                    const IConfigView& config,
                                    DeviceHandle ignoreOwner,
                                    std::uint8_t* pinsSeen,
                                    std::uint8_t& pinsSeenCount,
                                    LabelString* field) const {
  const bool present = config.has(param.key);
  if (!present) {
    // A required parameter without a default is genuinely missing.  A required
    // parameter WITH a default is fine: the default is the value.
    if (param.required && param.defaultValue == nullptr) {
      setField(field, param.key);
      return fail(ErrorCode::kDeviceConfigInvalid, "required value is missing");
    }
    if (param.type != ParamType::kGpio) return ok();
    if (param.defaultValue == nullptr) return ok();
  }

  switch (param.type) {
    case ParamType::kInt:
    case ParamType::kFloat: {
      const float value = config.getFloat(param.key, 0.0f);
      if (std::isnan(value) || std::isinf(value)) {
        setField(field, param.key);
        return fail(ErrorCode::kDeviceConfigInvalid, "value is not finite");
      }
      if (param.minimum < param.maximum &&
          (value < param.minimum || value > param.maximum)) {
        setField(field, param.key);
        return fail(ErrorCode::kDeviceConfigInvalid, "value out of range");
      }
      return ok();
    }

    case ParamType::kSelect: {
      const char* value = config.getString(param.key, param.defaultValue);
      for (std::uint8_t i = 0; i < param.optionCount; ++i) {
        if (param.options[i].value != nullptr &&
            std::strcmp(param.options[i].value, value) == 0) {
          return ok();
        }
      }
      setField(field, param.key);
      return fail(ErrorCode::kDeviceConfigInvalid, "value is not one of the options");
    }

    case ParamType::kGpio: {
      const std::int32_t pin = config.getInt(
          param.key, param.defaultValue != nullptr ? std::atoi(param.defaultValue) : -1);
      if (pin < 0) {
        setField(field, param.key);
        return fail(ErrorCode::kDeviceConfigInvalid, "no pin selected");
      }
      if (pin > 0xFF) {
        setField(field, param.key);
        return fail(ErrorCode::kGpioInvalid, "pin number out of range");
      }
      const std::uint8_t number = static_cast<std::uint8_t>(pin);

      const Status capability = resources_.checkPinCapability(number, param.pinUse);
      if (!capability.ok()) {
        setField(field, param.key);
        return capability;
      }

      // Same pin twice inside one device's own configuration.  Nothing else
      // catches this: ResourceManager only sees the first claim.
      for (std::uint8_t i = 0; i < pinsSeenCount; ++i) {
        if (pinsSeen[i] == number) {
          setField(field, param.key);
          return fail(ErrorCode::kResourceBusy, "pin used twice in this device");
        }
      }
      if (pinsSeenCount < DeviceSpec::kMaxChannelsPerDevice * 2) {
        pinsSeen[pinsSeenCount++] = number;
      }

      const ResourceClaim* claim = resources_.find(gpioResource(number));
      if (claim != nullptr && claim->owner != ignoreOwner) {
        char detail[limits::kDetailLength];
        std::snprintf(detail, sizeof(detail), "used by %s", claim->label.c_str());
        setField(field, param.key);
        return fail(ErrorCode::kResourceBusy, detail);
      }
      return ok();
    }

    case ParamType::kI2cAddress: {
      const std::int32_t address = config.getInt(param.key, -1);
      if (address < 0x08 || address > 0x77) {
        setField(field, param.key);
        return fail(ErrorCode::kDeviceConfigInvalid,
                    "I2C address must be 0x08..0x77");
      }
      const std::int32_t bus = config.getInt("bus", 0);
      const ResourceClaim* claim = resources_.find(i2cAddressResource(
          static_cast<std::uint8_t>(bus), static_cast<std::uint8_t>(address)));
      if (claim != nullptr && claim->owner != ignoreOwner) {
        char detail[limits::kDetailLength];
        std::snprintf(detail, sizeof(detail), "used by %s", claim->label.c_str());
        setField(field, param.key);
        return fail(ErrorCode::kI2cAddressBusy, detail);
      }
      return ok();
    }

    case ParamType::kBusRef: {
      const std::int32_t bus = config.getInt(param.key, 0);
      if (bus < 0 || bus >= resources_.chip().i2cBusCount) {
        setField(field, param.key);
        return fail(ErrorCode::kBusNotConfigured, "no such bus on this chip");
      }
      return ok();
    }

    case ParamType::kChannelRef: {
      const char* key = config.getString(param.key, nullptr);
      if (isBlank(key) || channels_.findByKey(key) == kInvalidChannel) {
        setField(field, param.key);
        return fail(ErrorCode::kChannelNotFound, "referenced channel not found");
      }
      return ok();
    }

    case ParamType::kBool:
    case ParamType::kText:
      return ok();
  }
  return ok();
}

Status DeviceManager::validate(const char* moduleId, const IConfigView& config,
                               LabelString* offendingField,
                               DeviceHandle ignoreOwner) const {
  if (offendingField != nullptr) offendingField->assign("");

  const ModuleDescriptor* descriptor = registry_.findById(moduleId);
  if (descriptor == nullptr) {
    return fail(ErrorCode::kDriverNotRegistered, moduleId);
  }
  if (descriptor->createDevice == nullptr) {
    return fail(ErrorCode::kNotSupported, "module is not a device");
  }
  const ModuleManifest& manifest = *descriptor->manifest;

  if (manifest.maxInstances > 0) {
    std::size_t existing = 0;
    for (std::size_t i = 0; i < limits::kMaxDevices; ++i) {
      if (records_[i].active && records_[i].manifest == &manifest &&
          records_[i].handle != ignoreOwner) {
        ++existing;
      }
    }
    if (existing >= manifest.maxInstances) {
      return fail(ErrorCode::kOutOfCapacity, "instance limit reached");
    }
  }

  // A module that needs hardware cannot start before that hardware exists.
  // Checking the DECLARED requirement here is what keeps `?dry_run=1` honest:
  // whatever validation approves, the create must be able to do.
  if (manifest.bus != BusRequirement::kNone) {
    if (buses_ == nullptr) {
      return fail(ErrorCode::kBusNotConfigured,
                  "this build has no hardware bus layer");
    }
    switch (manifest.bus) {
      case BusRequirement::kGpio:
        if (buses_->gpio() == nullptr) {
          return fail(ErrorCode::kNotSupported, "no GPIO port available");
        }
        break;
      case BusRequirement::kAdc:
        if (buses_->adc() == nullptr) {
          return fail(ErrorCode::kNotSupported, "no ADC port available");
        }
        break;
      case BusRequirement::kPwm:
        // Checked here as well as in configure(): a PWM output that validates
        // on a build with no LEDC and then fails to create is the same
        // dry-run/create disagreement that ADR-0013 was written about.
        if (buses_->pwm() == nullptr) {
          return fail(ErrorCode::kNotSupported, "no PWM peripheral available");
        }
        if (buses_->pwm()->channelsInUse() >= buses_->pwm()->channelCount()) {
          return fail(ErrorCode::kPwmChannelExhausted,
                      "every hardware PWM channel is already in use");
        }
        break;
      case BusRequirement::kI2c: {
        const std::int32_t index = config.getInt("bus", 0);
        if (index < 0 || buses_->i2c(static_cast<std::uint8_t>(index)) == nullptr) {
          setField(offendingField, "bus");
          return fail(ErrorCode::kBusNotConfigured, "I2C bus is not configured");
        }
        break;
      }
      default:
        return fail(ErrorCode::kNotSupported, "bus type is not implemented yet");
    }
  }

  std::uint8_t pinsSeen[DeviceSpec::kMaxChannelsPerDevice * 2] = {0};
  std::uint8_t pinsSeenCount = 0;

  for (std::uint8_t i = 0; i < manifest.paramCount; ++i) {
    const Status status = validateParam(manifest.params[i], config, ignoreOwner,
                                        pinsSeen, pinsSeenCount, offendingField);
    if (!status.ok()) return status;
  }

  // Unknown keys.  A typo like "clock_pln" would otherwise be silently ignored
  // and the driver would quietly run on its default pin.
  const std::size_t keyCount = config.keyCount();
  for (std::size_t i = 0; i < keyCount; ++i) {
    const char* key = config.keyAt(i);
    if (key == nullptr) continue;
    if (manifest.findParam(key) != nullptr) continue;
    setField(offendingField, key);
    return fail(ErrorCode::kDeviceConfigInvalid, "unknown configuration key");
  }

  return ok();
}

// ---------------------------------------------------------------------------
//  Records
// ---------------------------------------------------------------------------
DeviceRecord* DeviceManager::mutableFind(DeviceHandle handle) {
  if (handle == kInvalidDevice || handle > limits::kMaxDevices) return nullptr;
  DeviceRecord& record = records_[handle - 1];
  return record.active ? &record : nullptr;
}

const DeviceRecord* DeviceManager::find(DeviceHandle handle) const {
  return const_cast<DeviceManager*>(this)->mutableFind(handle);
}

const DeviceRecord* DeviceManager::findByKey(const char* key) const {
  if (key == nullptr) return nullptr;
  for (std::size_t i = 0; i < limits::kMaxDevices; ++i) {
    if (records_[i].active && records_[i].key.equals(key)) return &records_[i];
  }
  return nullptr;
}

DeviceHandle DeviceManager::handleForChannel(ChannelHandle channel) const {
  const ChannelDescriptor* descriptor = channels_.descriptor(channel);
  return (descriptor != nullptr) ? descriptor->source : kInvalidDevice;
}

DeviceRecord* DeviceManager::allocate() {
  for (std::size_t i = 0; i < limits::kMaxDevices; ++i) {
    if (records_[i].active) continue;
    records_[i] = DeviceRecord{};
    records_[i].active = true;
    records_[i].owner = this;
    records_[i].clock = &clock_;
    records_[i].handle = static_cast<DeviceHandle>(i + 1);
    ++activeCount_;
    return &records_[i];
  }
  return nullptr;
}

std::size_t DeviceManager::countInState(DeviceState state) const {
  std::size_t total = 0;
  for (std::size_t i = 0; i < limits::kMaxDevices; ++i) {
    if (records_[i].active && records_[i].state == state) ++total;
  }
  return total;
}

// ---------------------------------------------------------------------------
//  Channel creation
// ---------------------------------------------------------------------------
Status DeviceManager::resolveSafeValues(DeviceRecord& record,
                                        const IConfigView& config) {
  const ModuleManifest& manifest = *record.manifest;
  for (std::uint8_t i = 0; i < record.channelCount; ++i) {
    if (i >= manifest.channelCount) break;
    const ChannelSpec& spec = manifest.channels[i];
    // `safeValueFixed` is what makes a heater's zero a fact about the module
    // rather than a line of driver code that a future refactor can drop.
    if (spec.direction != ChannelDirection::kOutput || spec.safeValueFixed) {
      record.channelSafeValues[i] = spec.safeValue;
      continue;
    }
    // Two spellings of the same idea: a numeric `safe_value` for anything
    // continuous, and a `safe_level` of low/high for anything that is simply
    // on or off.  Both resolve HERE, so the driver and the safety layer read
    // one number that was decided in one place.
    if (config.has("safe_level")) {
      record.channelSafeValues[i] =
          (std::strcmp(config.getString("safe_level", "low"), "high") == 0)
              ? 1.0f : 0.0f;
    } else {
      record.channelSafeValues[i] = config.getFloat("safe_value", spec.safeValue);
    }
  }
  return ok();
}

Status DeviceManager::registerOutputs(DeviceRecord& record,
                                      const IConfigView& config) {
  if (outputs_ == nullptr) return ok();
  const ModuleManifest& manifest = *record.manifest;

  for (std::uint8_t i = 0; i < record.channelCount; ++i) {
    if (i >= manifest.channelCount) break;
    const ChannelSpec& spec = manifest.channels[i];
    if (spec.direction != ChannelDirection::kOutput) continue;

    // The already-resolved number, never a second read of the configuration.
    const float safeValue = record.channelSafeValues[i];
    const float holdSeconds = config.getFloat("hold_s", spec.defaultHoldSeconds);
    if (holdSeconds < 0.0f) {
      return fail(ErrorCode::kDeviceConfigInvalid, "hold_s must not be negative");
    }

    const Status registered = outputs_->registerOutput(
        record.channels[i], record.handle, safeValue,
        static_cast<Micros>(holdSeconds * 1000000.0f));
    if (!registered.ok()) return registered;
  }
  return ok();
}

Status DeviceManager::createChannels(DeviceRecord& record,
                                     const DeviceSpec& spec) {
  const ModuleManifest& manifest = *record.manifest;
  const std::uint8_t count =
      (manifest.channelCount < DeviceSpec::kMaxChannelsPerDevice)
          ? manifest.channelCount
          : static_cast<std::uint8_t>(DeviceSpec::kMaxChannelsPerDevice);

  for (std::uint8_t i = 0; i < count; ++i) {
    const ChannelSpec& channelSpec = manifest.channels[i];
    const ChannelOverride* custom = spec.findOverride(channelSpec.id);

    ChannelDescriptor descriptor;

    if (custom != nullptr && !custom->key.empty()) {
      descriptor.key = custom->key;
    } else {
      // "<deviceKey>.<specId>" — unique by construction, because device keys
      // are unique and spec ids are unique within a manifest.  Length is
      // checked BEFORE formatting: a silently truncated key would collide with
      // another channel and be impossible to reference from a formula.
      const std::size_t needed = record.key.size() + 1 +
                                 std::strlen(channelSpec.id) + 1;
      if (needed > limits::kKeyLength) {
        return fail(ErrorCode::kNameTooLong,
                    "device key plus channel id exceeds the key length");
      }
      // Built by hand rather than with snprintf: the length is already known
      // to fit, and a formatting call here only invites a truncation warning
      // that would have to be suppressed.
      char generated[limits::kKeyLength];
      char* out = generated;
      std::memcpy(out, record.key.c_str(), record.key.size());
      out += record.key.size();
      *out++ = '.';
      const std::size_t idLength = std::strlen(channelSpec.id);
      std::memcpy(out, channelSpec.id, idLength);
      out[idLength] = '\0';
      descriptor.key.assign(generated);
    }

    // Naming rule, in order of preference:
    //   1. what the user typed for this channel;
    //   2. the device name, when the device has exactly one channel — a tile
    //      reading "Surface temperature" is useful, one reading "Simulated
    //      value" three times over is not;
    //   3. "<device> <channel>" for multi-channel devices ("Chamber air
    //      Temperature", "Chamber air Relative humidity");
    //   4. the manifest's channel name.
    if (custom != nullptr && !custom->name.empty()) {
      descriptor.name = custom->name;
    } else if (!spec.name.empty() && count == 1) {
      descriptor.name = spec.name;
    } else if (!spec.name.empty()) {
      // Built by hand: assign() reports truncation, and a composed name is
      // allowed to be truncated (unlike a key, which must stay unique).
      char composed[limits::kNameLength];
      std::size_t written = 0;
      for (const char* c = spec.name.c_str();
           *c != '\0' && written + 1 < sizeof(composed); ++c) {
        composed[written++] = *c;
      }
      if (written + 1 < sizeof(composed)) composed[written++] = ' ';
      for (const char* c = channelSpec.name;
           c != nullptr && *c != '\0' && written + 1 < sizeof(composed); ++c) {
        composed[written++] = *c;
      }
      composed[written] = '\0';
      descriptor.name.assign(composed);
    } else {
      descriptor.name.assign(channelSpec.name);
    }
    descriptor.unit.assign((custom != nullptr && !custom->unit.empty())
                               ? custom->unit.c_str()
                               : channelSpec.unit);
    descriptor.quantity.assign(channelSpec.quantity);
    descriptor.source = record.handle;
    descriptor.direction = channelSpec.direction;

    if (custom != nullptr && custom->hasRange) {
      descriptor.minimum = custom->minimum;
      descriptor.maximum = custom->maximum;
    } else {
      descriptor.minimum = channelSpec.minimum;
      descriptor.maximum = channelSpec.maximum;
    }

    descriptor.precision = (custom != nullptr && custom->precision != 0xFF)
                               ? custom->precision
                               : channelSpec.precision;
    descriptor.color = (custom != nullptr && custom->color != 0)
                           ? custom->color
                           : kPalette[(record.handle + i) % (sizeof(kPalette) /
                                                             sizeof(kPalette[0]))];
    descriptor.logged = (custom != nullptr) ? custom->logged
                                              : channelSpec.defaultLogged;
    descriptor.visible = (custom == nullptr) || custom->visible;

    // Staleness detection needs to know what "on time" means for this channel.
    descriptor.expectedIntervalUs =
        (channelSpec.direction == ChannelDirection::kInput)
            ? record.sampleIntervalUs
            : 0;

    descriptor.geometry = (custom != nullptr && custom->hasGeometry)
                              ? custom->geometry
                              : spec.geometry;

    const Result<ChannelHandle> created = channels_.create(descriptor);
    if (!created.ok()) return created.error();
    record.channels[record.channelCount++] = created.value();
  }
  return ok();
}

// ---------------------------------------------------------------------------
//  State transitions
// ---------------------------------------------------------------------------
void DeviceManager::setState(DeviceRecord& record, DeviceState state,
                             const Error& error) {
  if (record.state == state && error.code == record.lastError.code) return;
  record.state = state;
  record.lastError = error;

  Event event;
  event.type = (state == DeviceState::kError) ? EventType::kDeviceError
                                              : EventType::kDeviceStateChanged;
  event.source = record.handle;
  event.integer = static_cast<std::int32_t>(state);
  event.code = error.code;
  event.detail = toString(state);
  event.timestamp = clock_.nowMicros();
  event.severity = (state == DeviceState::kError) ? 3 : 1;
  events_.publish(event);

  if (outputs_ != nullptr) {
    // An actuator carried by a device that is no longer running is an actuator
    // nobody is watching.  The safety layer hears about it here, on the single
    // path every state change goes through.
    outputs_->onDeviceStateChanged(
        record.handle,
        state == DeviceState::kRunning || state == DeviceState::kWarning);
  }

  if (state == DeviceState::kError) {
    // A dead sensor must not leave stale numbers on the dashboard looking fresh.
    channels_.setSourceQuality(record.handle, ChannelQuality::kFaulted);
    if (record.pollTask != kInvalidTask) {
      scheduler_.setEnabled(record.pollTask, false);
    }
  } else if ((state == DeviceState::kRunning || state == DeviceState::kWarning) &&
             record.pollTask != kInvalidTask) {
    scheduler_.setEnabled(record.pollTask, true);
  }
}

void DeviceManager::tryBegin(DeviceRecord& record) {
  const Status status = record.instance->begin();
  if (status.ok()) {
    record.initAttempts = 0;
    setState(record, DeviceState::kRunning, ok());
    return;
  }
  if (status.code == ErrorCode::kTimeout) {
    if (++record.initAttempts >= kMaxInitAttempts) {
      setState(record, DeviceState::kError,
               fail(ErrorCode::kDeviceInitFailed, "initialisation timed out"));
      return;
    }
    setState(record, DeviceState::kInitializing, ok());
    return;
  }
  setState(record, DeviceState::kError, status);
}

// ---------------------------------------------------------------------------
//  add / remove
// ---------------------------------------------------------------------------
Result<DeviceHandle> DeviceManager::add(const char* moduleId,
                                        const DeviceSpec& spec,
                                        const IConfigView& config,
                                        LabelString* offendingField) {
  if (spec.key.empty()) {
    setField(offendingField, "key");
    return fail(ErrorCode::kInvalidArgument, "device key is empty");
  }
  if (findByKey(spec.key.c_str()) != nullptr) {
    setField(offendingField, "key");
    return fail(ErrorCode::kAlreadyExists, spec.key.c_str());
  }

  const Status valid = validate(moduleId, config, offendingField);
  if (!valid.ok()) return valid;

  const ModuleDescriptor* descriptor = registry_.findById(moduleId);
  const ModuleManifest& manifest = *descriptor->manifest;

  DeviceRecord* record = allocate();
  if (record == nullptr) {
    return fail(ErrorCode::kOutOfCapacity, "device table full");
  }

  record->key = spec.key;
  record->name.assign(spec.name.empty() ? manifest.name : spec.name.c_str());
  record->manifest = &manifest;
  record->geometry = spec.geometry;

  Micros interval = (spec.sampleIntervalUs > 0) ? spec.sampleIntervalUs
                                                : manifest.defaultSampleIntervalUs;
  if (manifest.minSampleIntervalUs > 0 && interval < manifest.minSampleIntervalUs) {
    interval = manifest.minSampleIntervalUs;
  }
  record->sampleIntervalUs = interval;

  const Status channelsCreated = createChannels(*record, spec);
  if (!channelsCreated.ok()) {
    teardown(*record);
    return channelsCreated;
  }

  // Before the driver instance even exists.  An output whose safe state has
  // not been registered must not be reachable for a single scheduler pass.
  resolveSafeValues(*record, config);
  const Status outputsRegistered = registerOutputs(*record, config);
  if (!outputsRegistered.ok()) {
    teardown(*record);
    return outputsRegistered;
  }

  record->instance = descriptor->createDevice();
  if (record->instance == nullptr) {
    teardown(*record);
    return fail(ErrorCode::kInternal, "device factory returned null");
  }

  DeviceContext context;
  context.self = record->handle;
  context.manifest = &manifest;
  context.config = &config;
  context.clock = &clock_;
  context.resources = &resources_;
  context.channels = &channels_;
  context.events = &events_;
  context.buses = buses_;
  context.channelHandles = record->channels;
  context.channelSafeValues = record->channelSafeValues;
  context.channelCount = record->channelCount;

  const Status configured = record->instance->configure(context);
  if (!configured.ok()) {
    // Complete unwind, including every claim the driver managed to take.
    teardown(*record);
    return configured;
  }

  record->state = DeviceState::kConfigured;

  if (!spec.enabled) {
    setState(*record, DeviceState::kDisabled, ok());
    return record->handle;
  }

  const Result<TaskId> task =
      scheduler_.addPeriodic(record->key.c_str(), record->sampleIntervalUs,
                             TaskPriority::kAcquisition, pollTrampoline, record);
  if (!task.ok()) {
    teardown(*record);
    return task.error();
  }
  record->pollTask = task.value();
  scheduler_.setEnabled(record->pollTask, false);  // enabled once RUNNING

  tryBegin(*record);
  return record->handle;
}

void DeviceManager::teardown(DeviceRecord& record) {
  record.tearingDown = true;

  // FIRST, and before the driver is destroyed: drive the outputs this device
  // carries to their safe values while they can still be written, then forget
  // them.  Doing it after delete would leave the actuator wherever it was.
  if (outputs_ != nullptr && record.handle != kInvalidDevice) {
    for (std::uint8_t i = 0; i < record.channelCount; ++i) {
      const OutputRecord* output = outputs_->find(record.channels[i]);
      if (output == nullptr) continue;
      if (record.instance != nullptr &&
          (record.state == DeviceState::kRunning ||
           record.state == DeviceState::kWarning)) {
        outputs_->release(record.channels[i], OutputHoldState::kSafe);
      }
    }
    outputs_->forgetDevice(record.handle);
  }

  if (record.pollTask != kInvalidTask) {
    scheduler_.remove(record.pollTask);
    record.pollTask = kInvalidTask;
  }
  if (record.instance != nullptr) {
    record.instance->end();
    delete record.instance;
    record.instance = nullptr;
  }
  // Order matters: release hardware before channels, so a listener reacting to
  // channel removal never sees a device that still owns pins.
  resources_.releaseAllOwnedBy(record.handle);
  channels_.removeAllFrom(record.handle);

  record = DeviceRecord{};
  if (activeCount_ > 0) --activeCount_;
}

Status DeviceManager::remove(DeviceHandle handle) {
  DeviceRecord* record = mutableFind(handle);
  if (record == nullptr) return fail(ErrorCode::kNotFound, "device");

  Event event;
  event.type = EventType::kDeviceDisconnected;
  event.source = handle;
  event.timestamp = clock_.nowMicros();
  events_.publish(event);

  teardown(*record);
  return ok();
}

void DeviceManager::removeAll() {
  for (std::size_t i = 0; i < limits::kMaxDevices; ++i) {
    if (records_[i].active) teardown(records_[i]);
  }
}

Status DeviceManager::setEnabled(DeviceHandle handle, bool enabled) {
  DeviceRecord* record = mutableFind(handle);
  if (record == nullptr) return fail(ErrorCode::kNotFound, "device");

  if (!enabled) {
    if (record->pollTask != kInvalidTask) {
      scheduler_.setEnabled(record->pollTask, false);
    }
    record->instance->end();
    setState(*record, DeviceState::kDisabled, ok());
    return ok();
  }

  if (record->state != DeviceState::kDisabled &&
      record->state != DeviceState::kError) {
    return ok();  // already on
  }
  record->initAttempts = 0;
  if (record->pollTask == kInvalidTask) {
    const Result<TaskId> task = scheduler_.addPeriodic(
        record->key.c_str(), record->sampleIntervalUs, TaskPriority::kAcquisition,
        pollTrampoline, record);
    if (!task.ok()) return task.error();
    record->pollTask = task.value();
    scheduler_.setEnabled(record->pollTask, false);
  }
  tryBegin(*record);
  return ok();
}

Status DeviceManager::reconfigure(DeviceHandle handle, const char* moduleId,
                                  const DeviceSpec& spec,
                                  const IConfigView& config,
                                  LabelString* offendingField) {
  DeviceRecord* record = mutableFind(handle);
  if (record == nullptr) return fail(ErrorCode::kNotFound, "device");

  // Validate against the new configuration while ignoring the claims this
  // device already holds — otherwise "keep GPIO16, change the gain" would fail
  // with a conflict against itself.
  const Status valid = validate(moduleId, config, offendingField, handle);
  if (!valid.ok()) return valid;

  teardown(*record);
  const Result<DeviceHandle> rebuilt = add(moduleId, spec, config, offendingField);
  return rebuilt.ok() ? ok() : rebuilt.error();
}

// ---------------------------------------------------------------------------
//  Scheduler / sink trampolines
// ---------------------------------------------------------------------------
void DeviceManager::syncDriverState(DeviceRecord& record) {
  if (record.instance == nullptr) return;
  const DeviceState reported = record.instance->state();
  if (reported == record.state) return;
  // Only runtime transitions are interesting here; anything else is driven by
  // add()/setEnabled()/remove() and already went through setState().
  if (reported == DeviceState::kRunning || reported == DeviceState::kWarning ||
      reported == DeviceState::kError) {
    setState(record, reported, record.instance->lastError());
  }
}

void DeviceManager::pollTrampoline(void* context) {
  DeviceRecord* record = static_cast<DeviceRecord*>(context);
  if (record == nullptr || !record->active || record->tearingDown) return;
  if (record->instance == nullptr) return;
  if (record->state != DeviceState::kRunning &&
      record->state != DeviceState::kWarning) {
    return;
  }
  record->instance->poll(record->clock->nowMicros());
  if (record->owner != nullptr) record->owner->syncDriverState(*record);
}

void DeviceManager::retryTrampoline(void* context) {
  DeviceManager* self = static_cast<DeviceManager*>(context);
  for (std::size_t i = 0; i < limits::kMaxDevices; ++i) {
    DeviceRecord& record = self->records_[i];
    if (!record.active || record.state != DeviceState::kInitializing) continue;
    if (record.instance == nullptr) continue;
    self->tryBegin(record);
  }
}

Status DeviceManager::outputSinkTrampoline(ChannelHandle handle, float value,
                                           float* applied, void* context) {
  DeviceManager* self = static_cast<DeviceManager*>(context);
  const ChannelDescriptor* descriptor = self->channels_.descriptor(handle);
  if (descriptor == nullptr) return fail(ErrorCode::kChannelNotFound);

  DeviceRecord* record = self->mutableFind(descriptor->source);
  if (record == nullptr || record->instance == nullptr) {
    return fail(ErrorCode::kNotFound, "owning device");
  }
  if (record->state != DeviceState::kRunning &&
      record->state != DeviceState::kWarning) {
    return fail(ErrorCode::kInvalidState, "device is not running");
  }

  IOutputDevice* output = record->instance->asOutput();
  if (output == nullptr) {
    return fail(ErrorCode::kChannelTypeMismatch, "device is not an output");
  }
  return output->write(handle, value, applied);
}

}  // namespace lc
