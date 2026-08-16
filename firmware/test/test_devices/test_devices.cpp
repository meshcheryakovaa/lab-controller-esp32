// =============================================================================
//  Milestone 1 — DeviceManager and ProcessingManager.
//      pio test -e native
//
//  These tests use a purpose-built fake driver rather than a real sensor: the
//  point is to exercise validation, resource rollback and lifecycle, and a fake
//  can be made to fail on demand in ways a BMP280 cannot.
// =============================================================================
#include <unity.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/Clock.h"
#include "modules/processing/CalibrationProcessor.h"
#include "modules/processing/MovingAverageProcessor.h"
#include "modules/sensors/SignalSimulator.h"
#include "services/DeviceManager.h"
#include "services/ProcessingManager.h"

using namespace lc;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
//  Test helpers
// ---------------------------------------------------------------------------
namespace {

class MapConfig final : public IConfigView {
 public:
  MapConfig& set(const char* key, const std::string& value) {
    order_.push_back(key);
    scalars_[key] = value;
    return *this;
  }
  MapConfig& setArray(const char* key, std::vector<float> values) {
    arrays_[key] = std::move(values);
    return *this;
  }

  bool has(const char* key) const override { return scalars_.count(key) > 0; }
  std::int32_t getInt(const char* key, std::int32_t fallback) const override {
    auto it = scalars_.find(key);
    return it == scalars_.end() ? fallback
                                : static_cast<std::int32_t>(
                                      std::strtol(it->second.c_str(), nullptr, 0));
  }
  float getFloat(const char* key, float fallback) const override {
    auto it = scalars_.find(key);
    return it == scalars_.end() ? fallback
                                : static_cast<float>(std::atof(it->second.c_str()));
  }
  bool getBool(const char* key, bool fallback) const override {
    auto it = scalars_.find(key);
    return it == scalars_.end() ? fallback : (it->second == "true");
  }
  const char* getString(const char* key, const char* fallback) const override {
    auto it = scalars_.find(key);
    return it == scalars_.end() ? fallback : it->second.c_str();
  }
  std::size_t arraySize(const char* key) const override {
    auto it = arrays_.find(key);
    return it == arrays_.end() ? 0 : it->second.size();
  }
  float getFloatAt(const char* key, std::size_t index, float fallback) const override {
    auto it = arrays_.find(key);
    if (it == arrays_.end() || index >= it->second.size()) return fallback;
    return it->second[index];
  }
  std::size_t keyCount() const override { return order_.size(); }
  const char* keyAt(std::size_t index) const override {
    return index < order_.size() ? order_[index].c_str() : nullptr;
  }

 private:
  std::map<std::string, std::string> scalars_;
  std::map<std::string, std::vector<float>> arrays_;
  std::vector<std::string> order_;
};

// --- fake driver -----------------------------------------------------------
//  Claims two pins in configure() and can be told to fail at either stage.
constexpr ParamOption kModeOptions[] = {{"fast", "Fast"}, {"slow", "Slow"}};

constexpr ParamSpec kFakeParams[] = {
    ParamSpec{"data_pin", "Data pin", ParamType::kGpio, nullptr, nullptr,
              0, 0, 0, nullptr, nullptr, 0, PinUse::kDigitalInput, true, false, nullptr},
    ParamSpec{"clock_pin", "Clock pin", ParamType::kGpio, nullptr, nullptr,
              0, 0, 0, nullptr, nullptr, 0, PinUse::kDigitalOutput, true, false, nullptr},
    ParamSpec{"gain", "Gain", ParamType::kInt, nullptr, nullptr,
              1, 128, 1, "128", nullptr, 0, PinUse::kDigitalInput, false, false, nullptr},
    ParamSpec{"mode", "Mode", ParamType::kSelect, nullptr, nullptr,
              0, 0, 0, "fast", kModeOptions, 2, PinUse::kDigitalInput, false, false, nullptr},
    ParamSpec{"fail_configure", "Fail configure", ParamType::kBool, nullptr, nullptr,
              0, 0, 0, "false", nullptr, 0, PinUse::kDigitalInput, false, true, nullptr},
    ParamSpec{"slow_begin", "Slow begin", ParamType::kBool, nullptr, nullptr,
              0, 0, 0, "false", nullptr, 0, PinUse::kDigitalInput, false, true, nullptr},
};

constexpr ChannelSpec kFakeChannels[] = {
    ChannelSpec{"value", "Value", "V", "voltage", ChannelDirection::kInput,
                0.0f, 10.0f, 3, true},
    ChannelSpec{"count", "Count", "", "count", ChannelDirection::kInput,
                0.0f, 0.0f, 0, false},
};

constexpr ModuleManifest kFakeManifest = {
    "test_sensor", "Test Sensor", ModuleCategory::kSensor, "fake driver",
    BusRequirement::kNone,
    kFakeParams, 6,
    kFakeChannels, 2,
    /*maxInstances*/ 2,
    /*defaultSampleIntervalUs*/ 50000,
    /*minSampleIntervalUs*/ 1000,
    /*schemaVersion*/ 1,
};

class FakeSensor final : public IDevice {
 public:
  static IDevice* create() { return new FakeSensor(); }
  static int liveInstances;

  FakeSensor() { ++liveInstances; }
  ~FakeSensor() override { --liveInstances; }

  Status configure(const DeviceContext& context) override {
    ctx_ = context;
    const std::uint8_t dataPin =
        static_cast<std::uint8_t>(context.config->getInt("data_pin", 0));
    const std::uint8_t clockPin =
        static_cast<std::uint8_t>(context.config->getInt("clock_pin", 0));
    failConfigure_ = context.config->getBool("fail_configure", false);
    slowBegin_ = context.config->getBool("slow_begin", false);

    Status status = context.resources->claimPin(dataPin, PinUse::kDigitalInput,
                                                context.self, "TEST DOUT");
    if (!status.ok()) return status;

    if (failConfigure_) {
      // Deliberately bail out AFTER one successful claim: DeviceManager must
      // still release it.  This is the scenario that leaves pins stuck forever
      // in naive implementations.
      return fail(ErrorCode::kDeviceConfigInvalid, "asked to fail");
    }

    status = context.resources->claimPin(clockPin, PinUse::kDigitalOutput,
                                         context.self, "TEST SCK");
    if (!status.ok()) return status;

    state_ = DeviceState::kConfigured;
    return ok();
  }

  Status begin() override {
    if (slowBegin_ && beginCalls_++ < 3) return fail(ErrorCode::kTimeout);
    state_ = DeviceState::kRunning;
    return ok();
  }

  void poll(Micros now) override {
    ++polls_;
    if (failAfterPolls_ > 0 && polls_ >= failAfterPolls_) {
      lastError_ = fail(ErrorCode::kDeviceNotResponding, "cable fell off");
      state_ = DeviceState::kError;
      return;
    }
    ctx_.channels->publishRaw(ctx_.channelHandles[0], 1.5f, now);
    ctx_.channels->publishRaw(ctx_.channelHandles[1],
                              static_cast<float>(polls_), now);
  }

  void failAfter(int polls) { failAfterPolls_ = polls; }

  void end() override { state_ = DeviceState::kDisabled; }
  DeviceState state() const override { return state_; }
  const Error& lastError() const override { return lastError_; }

  int polls() const { return polls_; }

 private:
  DeviceContext ctx_{};
  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
  bool failConfigure_ = false;
  bool slowBegin_ = false;
  int beginCalls_ = 0;
  int polls_ = 0;
  int failAfterPolls_ = 0;
};

int FakeSensor::liveInstances = 0;

// --- fixture ---------------------------------------------------------------
struct Rig {
  ManualClock clock;
  ModuleRegistry registry;
  ResourceManager resources{ChipProfile::esp32()};
  ChannelManager channels{clock};
  Scheduler scheduler{clock};
  EventBus events;
  DeviceManager devices{clock, registry, resources, channels, scheduler, events};
  ProcessingManager processing{registry, channels};

  Rig() {
    registerBuiltinModules(registry);
    registry.add(ModuleDescriptor{&kFakeManifest, &FakeSensor::create, nullptr,
                                  nullptr});
    devices.begin();
    processing.begin();
  }
};

MapConfig goodFakeConfig() {
  MapConfig config;
  config.set("data_pin", "16").set("clock_pin", "17").set("gain", "128")
        .set("mode", "fast");
  return config;
}

DeviceSpec spec(const char* key) {
  DeviceSpec s;
  s.key.assign(key);
  return s;
}

// A pipeline description backed by plain structs — no JSON needed.
struct StageDescription {
  std::string type;
  MapConfig config;
};

class VectorPipeline final : public IPipelineSource {
 public:
  std::vector<StageDescription> stages;
  std::int8_t calibration = IPipelineSource::kAutoCalibrationStage;

  std::size_t stageCount() const override { return stages.size(); }
  const char* stageType(std::size_t i) const override { return stages[i].type.c_str(); }
  const IConfigView& stageConfig(std::size_t i) const override { return stages[i].config; }
  std::int8_t calibrationStage() const override { return calibration; }
};

}  // namespace

// ---------------------------------------------------------------------------
//  Validation
// ---------------------------------------------------------------------------
static void test_validate_rejects_unknown_module() {
  Rig rig;
  MapConfig config;
  const Status status = rig.devices.validate("no_such_module", config);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDriverNotRegistered),
                        static_cast<int>(status.code));
}

static void test_validate_reports_the_offending_field() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  config.set("gain", "500");  // manifest says 1..128

  LabelString field;
  const Status status = rig.devices.validate("test_sensor", config, &field);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDeviceConfigInvalid),
                        static_cast<int>(status.code));
  TEST_ASSERT_EQUAL_STRING("gain", field.c_str());
}

static void test_validate_rejects_value_outside_the_select_options() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  config.set("mode", "turbo");

  LabelString field;
  const Status status = rig.devices.validate("test_sensor", config, &field);
  TEST_ASSERT_FALSE(status.ok());
  TEST_ASSERT_EQUAL_STRING("mode", field.c_str());
}

static void test_validate_rejects_a_typo_in_a_key() {
  Rig rig;

  // A missing required parameter is reported as such, naming the parameter the
  // driver expected rather than the typo.
  MapConfig missing;
  missing.set("data_pin", "16").set("clock_pln", "17");
  LabelString field;
  Status status = rig.devices.validate("test_sensor", missing, &field);
  TEST_ASSERT_FALSE(status.ok());
  TEST_ASSERT_EQUAL_STRING("clock_pin", field.c_str());

  // With every required parameter present, the stray key is still refused —
  // otherwise "gian: 64" would be silently ignored and the device would quietly
  // run at the default gain.
  MapConfig stray = goodFakeConfig();
  stray.set("gian", "64");
  status = rig.devices.validate("test_sensor", stray, &field);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDeviceConfigInvalid),
                        static_cast<int>(status.code));
  TEST_ASSERT_EQUAL_STRING("gian", field.c_str());
}

static void test_validate_rejects_an_impossible_pin() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  config.set("clock_pin", "34");  // input-only on the classic ESP32

  LabelString field;
  const Status status = rig.devices.validate("test_sensor", config, &field);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kGpioInputOnly),
                        static_cast<int>(status.code));
  TEST_ASSERT_EQUAL_STRING("clock_pin", field.c_str());
}

static void test_validate_rejects_the_same_pin_twice_in_one_device() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  config.set("clock_pin", "16");  // same as data_pin

  LabelString field;
  const Status status = rig.devices.validate("test_sensor", config, &field);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kResourceBusy),
                        static_cast<int>(status.code));
  TEST_ASSERT_EQUAL_STRING("clock_pin", field.c_str());
}

static void test_instance_limit_is_enforced() {
  Rig rig;
  MapConfig a = goodFakeConfig();
  MapConfig b;
  b.set("data_pin", "18").set("clock_pin", "19");
  MapConfig c;
  c.set("data_pin", "22").set("clock_pin", "23");

  TEST_ASSERT_TRUE(rig.devices.add("test_sensor", spec("s1"), a).ok());
  TEST_ASSERT_TRUE(rig.devices.add("test_sensor", spec("s2"), b).ok());

  const auto third = rig.devices.add("test_sensor", spec("s3"), c);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kOutOfCapacity),
                        static_cast<int>(third.code()));
}

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
static void test_add_creates_channels_and_starts_polling() {
  Rig rig;
  MapConfig config = goodFakeConfig();

  const auto added = rig.devices.add("test_sensor", spec("balance_01"), config);
  TEST_ASSERT_TRUE(added.ok());

  const DeviceRecord* record = rig.devices.find(added.value());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kRunning),
                        static_cast<int>(record->state));
  TEST_ASSERT_EQUAL_UINT(2, record->channelCount);

  // Channel keys are generated as "<deviceKey>.<specId>".
  TEST_ASSERT_EQUAL_UINT(record->channels[0],
                         rig.channels.findByKey("balance_01.value"));
  TEST_ASSERT_EQUAL_STRING("V",
                           rig.channels.descriptor(record->channels[0])->unit.c_str());

  // Two pins claimed, and the acquisition task is registered at the manifest's
  // default rate (50 ms).
  TEST_ASSERT_EQUAL_UINT(2, rig.resources.claimCount());

  rig.clock.advanceMicros(50000);
  rig.scheduler.runPass(0);
  TEST_ASSERT_EQUAL_FLOAT(1.5f, rig.channels.value(record->channels[0])->processed);
}

static void test_failed_configure_leaves_nothing_behind() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  config.set("fail_configure", "true");

  const auto added = rig.devices.add("test_sensor", spec("broken"), config);
  TEST_ASSERT_FALSE(added.ok());

  // The driver claimed data_pin before failing.  Nothing may survive.
  TEST_ASSERT_EQUAL_UINT(0, rig.resources.claimCount());
  TEST_ASSERT_EQUAL_UINT(0, rig.channels.activeCount());
  TEST_ASSERT_EQUAL_UINT(0, rig.devices.activeCount());
  TEST_ASSERT_EQUAL_INT(0, FakeSensor::liveInstances);
  TEST_ASSERT_NULL(rig.devices.findByKey("broken"));

  // ...and the pin is immediately reusable.
  MapConfig retry = goodFakeConfig();
  TEST_ASSERT_TRUE(rig.devices.add("test_sensor", spec("fixed"), retry).ok());
}

static void test_slow_device_initialises_over_several_ticks() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  config.set("slow_begin", "true");

  const auto added = rig.devices.add("test_sensor", spec("slow_01"), config);
  TEST_ASSERT_TRUE(added.ok());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kInitializing),
                        static_cast<int>(rig.devices.find(added.value())->state));

  // The shared retry task runs at 100 Hz; three more attempts are needed.
  for (int i = 0; i < 4; ++i) {
    rig.clock.advanceMicros(10000);
    rig.scheduler.runPass(0);
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kRunning),
                        static_cast<int>(rig.devices.find(added.value())->state));
}

static void test_remove_releases_pins_channels_and_the_poll_task() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  const auto added = rig.devices.add("test_sensor", spec("balance_01"), config);
  const std::size_t tasksWithDevice = rig.scheduler.taskCount();

  TEST_ASSERT_TRUE(rig.devices.remove(added.value()).ok());

  TEST_ASSERT_EQUAL_UINT(0, rig.resources.claimCount());
  TEST_ASSERT_EQUAL_UINT(0, rig.channels.activeCount());
  TEST_ASSERT_EQUAL_UINT(0, rig.devices.activeCount());
  TEST_ASSERT_EQUAL_INT(0, FakeSensor::liveInstances);
  TEST_ASSERT_EQUAL_UINT(tasksWithDevice - 1, rig.scheduler.taskCount());
  TEST_ASSERT_EQUAL_UINT(kInvalidChannel, rig.channels.findByKey("balance_01.value"));
}

static void test_duplicate_device_key_is_rejected() {
  Rig rig;
  MapConfig first = goodFakeConfig();
  MapConfig second;
  second.set("data_pin", "18").set("clock_pin", "19");

  TEST_ASSERT_TRUE(rig.devices.add("test_sensor", spec("dup"), first).ok());
  const auto again = rig.devices.add("test_sensor", spec("dup"), second);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kAlreadyExists),
                        static_cast<int>(again.code()));
}

static void test_a_second_device_cannot_steal_a_claimed_pin() {
  Rig rig;
  MapConfig first = goodFakeConfig();
  TEST_ASSERT_TRUE(rig.devices.add("test_sensor", spec("s1"), first).ok());

  MapConfig clash;
  clash.set("data_pin", "18").set("clock_pin", "17");  // 17 belongs to s1
  LabelString field;
  const auto second = rig.devices.add("test_sensor", spec("s2"), clash, &field);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kResourceBusy),
                        static_cast<int>(second.code()));
  TEST_ASSERT_EQUAL_STRING("clock_pin", field.c_str());
  TEST_ASSERT_EQUAL_STRING("used by TEST SCK", second.error().detail.c_str());
}

static void test_reconfigure_may_keep_its_own_pins() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  const auto added = rig.devices.add("test_sensor", spec("s1"), config);

  // Same pins, different gain: must not conflict with itself.
  MapConfig changed = goodFakeConfig();
  changed.set("gain", "64");
  LabelString field;
  TEST_ASSERT_TRUE(
      rig.devices.reconfigure(added.value(), "test_sensor", spec("s1"), changed,
                              &field).ok());
  TEST_ASSERT_EQUAL_UINT(2, rig.resources.claimCount());
  TEST_ASSERT_EQUAL_UINT(2, rig.channels.activeCount());
  TEST_ASSERT_EQUAL_INT(1, FakeSensor::liveInstances);
}

static void test_disabled_device_stops_polling_and_can_come_back() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  const auto added = rig.devices.add("test_sensor", spec("s1"), config);
  const ChannelHandle counter = rig.devices.find(added.value())->channels[1];

  rig.clock.advanceMicros(50000);
  rig.scheduler.runPass(0);
  const std::uint32_t before = rig.channels.value(counter)->sequence;

  TEST_ASSERT_TRUE(rig.devices.setEnabled(added.value(), false).ok());
  for (int i = 0; i < 5; ++i) {
    rig.clock.advanceMicros(50000);
    rig.scheduler.runPass(0);
  }
  TEST_ASSERT_EQUAL_UINT(before, rig.channels.value(counter)->sequence);

  TEST_ASSERT_TRUE(rig.devices.setEnabled(added.value(), true).ok());
  rig.clock.advanceMicros(50000);
  rig.scheduler.runPass(0);
  TEST_ASSERT_TRUE(rig.channels.value(counter)->sequence > before);
}

static void test_a_driver_that_fails_mid_poll_is_noticed() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  const auto added = rig.devices.add("test_sensor", spec("s1"), config);
  const DeviceRecord* record = rig.devices.find(added.value());
  const ChannelHandle handle = record->channels[0];

  rig.clock.advanceMicros(50000);
  rig.scheduler.runPass(0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kRunning),
                        static_cast<int>(record->state));

  // The cable falls off on the third poll.  Before this was handled, the driver
  // knew and nobody else did: the record still said RUNNING, the API reported a
  // healthy device, and the channel kept its last value looking fresh.
  static_cast<FakeSensor*>(record->instance)->failAfter(3);
  for (int i = 0; i < 3; ++i) {
    rig.clock.advanceMicros(50000);
    rig.scheduler.runPass(0);
  }

  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kError),
                        static_cast<int>(record->state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDeviceNotResponding),
                        static_cast<int>(record->lastError.code));
  TEST_ASSERT_EQUAL_STRING("cable fell off", record->lastError.detail.c_str());
  // ...and the channel is flagged, not left showing a stale reading as good.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ChannelQuality::kFaulted),
                        static_cast<int>(rig.channels.value(handle)->quality));

  // A faulted device stops being polled; it comes back only on an explicit
  // command, so a flapping sensor cannot spam the log.
  const std::uint32_t before = rig.channels.value(handle)->sequence;
  for (int i = 0; i < 5; ++i) {
    rig.clock.advanceMicros(50000);
    rig.scheduler.runPass(0);
  }
  TEST_ASSERT_EQUAL_UINT(before, rig.channels.value(handle)->sequence);
}

static void test_a_module_that_needs_hardware_is_refused_without_it() {
  Rig rig;  // no bus provider installed
  MapConfig config;
  config.set("data_pin", "16").set("clock_pin", "17").set("gain", "128")
        .set("rate_hz", "10");

  // dry-run and create must agree.  They once did not: validate() approved an
  // HX711 on a build with no GPIO port and configure() then failed.
  const Status validated = rig.devices.validate("hx711", config);
  TEST_ASSERT_FALSE(validated.ok());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kBusNotConfigured),
                        static_cast<int>(validated.code));

  DeviceSpec s;
  s.key.assign("hx_01");
  const auto added = rig.devices.add("hx711", s, config);
  TEST_ASSERT_FALSE(added.ok());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(validated.code),
                        static_cast<int>(added.code()));
  TEST_ASSERT_EQUAL_UINT(0, rig.channels.activeCount());
}

// ---------------------------------------------------------------------------
//  ProcessingManager
// ---------------------------------------------------------------------------
static void test_pipeline_is_built_from_a_description() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  const auto added = rig.devices.add("test_sensor", spec("s1"), config);
  const ChannelHandle handle = rig.devices.find(added.value())->channels[0];

  VectorPipeline pipeline;
  pipeline.stages.push_back({"calibration", MapConfig()});
  pipeline.stages[0].config.set("type", "polynomial")
      .set("x_center", "0").set("x_scale", "1")
      .setArray("coefficients", {0.0f, 2.0f});  // y = 2x
  pipeline.stages.push_back({"moving_average", MapConfig()});
  pipeline.stages[1].config.set("window", "2");

  TEST_ASSERT_TRUE(rig.processing.apply(handle, pipeline).ok());
  TEST_ASSERT_EQUAL_UINT(2, rig.processing.stageCount(handle));
  TEST_ASSERT_EQUAL_STRING("calibration", rig.processing.stageType(handle, 0));

  rig.channels.publishRaw(handle, 1.0f, 1000);
  TEST_ASSERT_EQUAL_FLOAT(2.0f, rig.channels.value(handle)->calibrated);
  TEST_ASSERT_EQUAL_FLOAT(2.0f, rig.channels.value(handle)->processed);

  rig.channels.publishRaw(handle, 3.0f, 2000);
  TEST_ASSERT_EQUAL_FLOAT(6.0f, rig.channels.value(handle)->calibrated);
  TEST_ASSERT_EQUAL_FLOAT(4.0f, rig.channels.value(handle)->processed);
}

static void test_a_bad_stage_leaves_the_previous_pipeline_running() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  const auto added = rig.devices.add("test_sensor", spec("s1"), config);
  const ChannelHandle handle = rig.devices.find(added.value())->channels[0];

  VectorPipeline good;
  good.stages.push_back({"moving_average", MapConfig()});
  good.stages[0].config.set("window", "2");
  TEST_ASSERT_TRUE(rig.processing.apply(handle, good).ok());

  VectorPipeline broken;
  broken.stages.push_back({"moving_average", MapConfig()});
  broken.stages[0].config.set("window", "2");
  broken.stages.push_back({"moving_average", MapConfig()});
  broken.stages[1].config.set("window", "9999");  // out of range

  TEST_ASSERT_FALSE(rig.processing.apply(handle, broken).ok());
  TEST_ASSERT_EQUAL_UINT(1, rig.processing.stageCount(handle));

  // And the surviving pipeline still works.
  rig.channels.publishRaw(handle, 10.0f, 1000);
  rig.channels.publishRaw(handle, 20.0f, 2000);
  TEST_ASSERT_EQUAL_FLOAT(15.0f, rig.channels.value(handle)->processed);
}

static void test_unknown_processor_type_is_reported() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  const auto added = rig.devices.add("test_sensor", spec("s1"), config);
  const ChannelHandle handle = rig.devices.find(added.value())->channels[0];

  VectorPipeline pipeline;
  pipeline.stages.push_back({"kalman_magic", MapConfig()});
  const Status status = rig.processing.apply(handle, pipeline);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDriverNotRegistered),
                        static_cast<int>(status.code));
}

static void test_removing_a_device_destroys_its_pipelines() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  const auto added = rig.devices.add("test_sensor", spec("s1"), config);
  const ChannelHandle handle = rig.devices.find(added.value())->channels[0];

  VectorPipeline pipeline;
  pipeline.stages.push_back({"moving_average", MapConfig()});
  pipeline.stages[0].config.set("window", "4");
  TEST_ASSERT_TRUE(rig.processing.apply(handle, pipeline).ok());
  TEST_ASSERT_EQUAL_UINT(1, rig.processing.totalStages());

  // Without the lifecycle hook this would leave ChannelManager holding pointers
  // to deleted processors.
  TEST_ASSERT_TRUE(rig.devices.remove(added.value()).ok());
  TEST_ASSERT_EQUAL_UINT(0, rig.processing.totalStages());
}

static void test_pipeline_longer_than_the_limit_is_rejected() {
  Rig rig;
  MapConfig config = goodFakeConfig();
  const auto added = rig.devices.add("test_sensor", spec("s1"), config);
  const ChannelHandle handle = rig.devices.find(added.value())->channels[0];

  VectorPipeline pipeline;
  for (std::size_t i = 0; i <= limits::kMaxProcessorsPerChannel; ++i) {
    pipeline.stages.push_back({"moving_average", MapConfig()});
    pipeline.stages.back().config.set("window", "2");
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kProcessorChainTooLong),
                        static_cast<int>(rig.processing.apply(handle, pipeline).code));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_validate_rejects_unknown_module);
  RUN_TEST(test_validate_reports_the_offending_field);
  RUN_TEST(test_validate_rejects_value_outside_the_select_options);
  RUN_TEST(test_validate_rejects_a_typo_in_a_key);
  RUN_TEST(test_validate_rejects_an_impossible_pin);
  RUN_TEST(test_validate_rejects_the_same_pin_twice_in_one_device);
  RUN_TEST(test_instance_limit_is_enforced);
  RUN_TEST(test_add_creates_channels_and_starts_polling);
  RUN_TEST(test_failed_configure_leaves_nothing_behind);
  RUN_TEST(test_slow_device_initialises_over_several_ticks);
  RUN_TEST(test_remove_releases_pins_channels_and_the_poll_task);
  RUN_TEST(test_duplicate_device_key_is_rejected);
  RUN_TEST(test_a_second_device_cannot_steal_a_claimed_pin);
  RUN_TEST(test_reconfigure_may_keep_its_own_pins);
  RUN_TEST(test_disabled_device_stops_polling_and_can_come_back);
  RUN_TEST(test_a_driver_that_fails_mid_poll_is_noticed);
  RUN_TEST(test_a_module_that_needs_hardware_is_refused_without_it);
  RUN_TEST(test_pipeline_is_built_from_a_description);
  RUN_TEST(test_a_bad_stage_leaves_the_previous_pipeline_running);
  RUN_TEST(test_unknown_processor_type_is_reported);
  RUN_TEST(test_removing_a_device_destroys_its_pipelines);
  RUN_TEST(test_pipeline_longer_than_the_limit_is_rejected);
  return UNITY_END();
}
