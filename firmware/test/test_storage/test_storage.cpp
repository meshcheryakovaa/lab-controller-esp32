// =============================================================================
//  Milestone 1 — configuration storage and the boot sequence.
//      pio test -e native
//
//  The last test in this file is the Milestone 1 acceptance criterion:
//  a device exists because it is written in devices.json, and disappears when
//  it is removed from devices.json.  No call to add() anywhere in main.cpp.
// =============================================================================
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app/SystemManager.h"
#include "core/Clock.h"
#include "storage/ConfigApplier.h"
#include "storage/ControlStore.h"
#include "storage/ConfigStorage.h"
#include "platform/host/HostRandom.h"
#include "storage/PosixBackend.h"

using namespace lc;

void setUp() {}
void tearDown() {}

namespace {

std::string makeTempRoot() {
  char pattern[] = "/tmp/lc-test-XXXXXX";
  const char* dir = mkdtemp(pattern);
  return dir != nullptr ? std::string(dir) : std::string("/tmp/lc-test-fallback");
}

// Everything a booted controller consists of, wired the same way main.cpp does.
struct Rig {
  std::string root = makeTempRoot();
  ManualClock clock;
  platform::PosixBackend backend{root};
  MemoryBootCounter bootCounter;
  ModuleRegistry registry;
  ResourceManager resources{ChipProfile::esp32()};
  ChannelManager channels{clock};
  Scheduler scheduler{clock};
  EventBus events;
  DeviceManager devices{clock, registry, resources, channels, scheduler, events};
  ProcessingManager processing{registry, channels};
  OutputManager outputs{clock, channels, scheduler, events};
  SafetyManager safety{clock, channels, outputs, scheduler, events};
  ControlManager control{clock, channels, outputs, scheduler, events};
  ConfigStorage storage{backend, events};
  CalibrationManager calibrations;
  ExperimentEngine experiments{clock,   channels,  outputs, control,
                               devices, scheduler, events};
  RunLog runLog{backend, storage, devices, &calibrations};
  DataLogger logger{clock, channels, scheduler, events};
  LogStore logStore{backend, storage, &calibrations};
  platform::HostRandom random;
  // 1000 iterations, not 20 000: the cost knob is stored with the hash, and a
  // suite that signs in fifty times should not spend a minute stretching keys.
  AuthManager auth{clock, random, backend, events, 1000};
  ConfigApplier applier{devices, processing, channels, &calibrations};
  SystemManager system{makeServices()};

  Rig() { wire(); }

  // Boots a SECOND controller on an existing filesystem — the only way to test
  // what survives a reboot, which is most of what this project promises.
  // `keep` decides which of the two clears the directory up.
  Rig(std::string existingRoot, bool keep)
      : root(std::move(existingRoot)), keepFiles(keep) {
    wire();
  }

  void wire() {
    applier.setSafety(&safety);
    applier.setControl(&control);
  }

  SystemManager::Services makeServices() {
    SystemManager::Services services;
    services.clock = &clock;
    services.registry = &registry;
    services.resources = &resources;
    services.channels = &channels;
    services.scheduler = &scheduler;
    services.events = &events;
    services.devices = &devices;
    services.processing = &processing;
    services.calibrations = &calibrations;
    services.outputs = &outputs;
    services.safety = &safety;
    services.control = &control;
    services.experiments = &experiments;
    services.runLog = &runLog;
    services.logger = &logger;
    services.logStore = &logStore;
    services.auth = &auth;
    services.storage = &storage;
    services.applier = &applier;
    services.bootCounter = &bootCounter;
    return services;
  }

  bool keepFiles = false;

  ~Rig() {
    devices.removeAll();
    if (keepFiles) return;
    std::string command = "rm -rf " + root;
    if (std::system(command.c_str()) != 0) { /* best effort cleanup */ }
  }

  void writeRaw(const char* path, const std::string& text) {
    TEST_ASSERT_TRUE(backend.writeAtomic(path, text.c_str(), text.size()).ok());
  }
};

const char* kOneSimulator = R"({
  "schemaVersion": 1,
  "devices": [
    {
      "key": "sim_01",
      "module": "sim_signal",
      "name": "Bench simulator",
      "enabled": true,
      "sample_interval_us": 100000,
      "geometry": { "system": "cylindrical", "a": 0, "b": 0, "c": 4,
                    "group": "Simulators", "role": "Reference signal" },
      "config": { "waveform": "constant", "amplitude": 0, "offset": 42.0,
                  "period_s": 10, "noise": 0 },
      "channels": { "value": { "key": "ref_signal", "unit": "degC",
                               "precision": 2 } }
    }
  ]
})";

}  // namespace

// ---------------------------------------------------------------------------
//  ConfigStorage
// ---------------------------------------------------------------------------
static void test_save_and_load_round_trip() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());

  JsonDocument out;
  out["devices"][0]["key"] = "sim_01";
  out["devices"][0]["module"] = "sim_signal";
  TEST_ASSERT_TRUE(rig.storage.save(ConfigSection::kDevices, out).ok());
  TEST_ASSERT_EQUAL_UINT(1, rig.storage.revision());

  JsonDocument back;
  TEST_ASSERT_TRUE(rig.storage.load(ConfigSection::kDevices, back).ok());
  TEST_ASSERT_EQUAL_STRING("sim_01", back["devices"][0]["key"]);
  TEST_ASSERT_EQUAL_INT(ConfigStorage::kSchemaVersion,
                        back["schemaVersion"].as<int>());
}

static void test_missing_section_is_not_an_error_condition() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  JsonDocument document;
  // kVirtual is not one of the sections begin() seeds, so this is a file that
  // genuinely does not exist.  "Never configured" must stay distinguishable
  // from "broken".
  const Status status = rig.storage.load(ConfigSection::kVirtual, document);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kNotFound),
                        static_cast<int>(status.code));
}

static void test_first_boot_writes_empty_sections_once() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());

  // The five documents the boot sequence reads now exist and parse, so nothing
  // downstream has to have an opinion about a missing file — and the log stops
  // describing a healthy board at error level.
  const ConfigSection seeded[] = {
      ConfigSection::kSystem, ConfigSection::kDevices,
      ConfigSection::kProcessing, ConfigSection::kCalibrations,
      ConfigSection::kControl};
  for (ConfigSection section : seeded) {
    JsonDocument document;
    TEST_ASSERT_TRUE(rig.storage.load(section, document).ok());
    TEST_ASSERT_EQUAL_INT(ConfigStorage::kSchemaVersion,
                          document["schemaVersion"].as<int>());
  }

  JsonDocument devices;
  TEST_ASSERT_TRUE(rig.storage.load(ConfigSection::kDevices, devices).ok());
  TEST_ASSERT_EQUAL_UINT(0, devices["devices"].size());

  // And it is a FIRST-boot action: the second call must not touch the
  // filesystem, or a user's configuration would be replaced by an empty one on
  // every restart.
  TEST_ASSERT_EQUAL_UINT(0, rig.storage.ensureDefaults());
}

static void test_seeding_never_overwrites_a_configuration() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  rig.writeRaw("/config/devices.json", kOneSimulator);

  TEST_ASSERT_EQUAL_UINT(0, rig.storage.ensureDefaults());

  JsonDocument devices;
  TEST_ASSERT_TRUE(rig.storage.load(ConfigSection::kDevices, devices).ok());
  TEST_ASSERT_EQUAL_UINT(1, devices["devices"].size());
}

static void test_corrupt_json_is_reported_not_guessed() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  rig.writeRaw("/config/devices.json", "{ \"devices\": [ {\"key\": ");

  JsonDocument document;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kConfigCorrupt),
                        static_cast<int>(
                            rig.storage.load(ConfigSection::kDevices, document).code));
}

static void test_missing_schema_version_is_refused() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  rig.writeRaw("/config/devices.json", "{\"devices\":[]}");

  JsonDocument document;
  const Status status = rig.storage.load(ConfigSection::kDevices, document);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kConfigCorrupt),
                        static_cast<int>(status.code));
  TEST_ASSERT_EQUAL_STRING("missing schemaVersion", status.detail.c_str());
}

static void test_a_file_from_the_future_is_refused_not_reinterpreted() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  rig.writeRaw("/config/devices.json", "{\"schemaVersion\":999,\"devices\":[]}");

  JsonDocument document;
  // This is the OTA-rollback case.  Silently "upgrading" our understanding of a
  // future format is how a user's calibrations get destroyed.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kConfigSchemaTooNew),
                        static_cast<int>(
                            rig.storage.load(ConfigSection::kDevices, document).code));
}

static void test_interrupted_write_leaves_the_old_file_intact() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  rig.writeRaw("/config/devices.json", "{\"schemaVersion\":1,\"devices\":[]}");
  // Simulate a power cut between "write temp" and "rename".
  rig.writeRaw("/config/devices.json.tmp", "{\"schemaVersion\":1,\"dev");

  TEST_ASSERT_TRUE(rig.backend.exists("/config/devices.json.tmp"));
  TEST_ASSERT_TRUE(rig.storage.begin().ok());  // boot again
  TEST_ASSERT_FALSE(rig.backend.exists("/config/devices.json.tmp"));

  JsonDocument document;
  TEST_ASSERT_TRUE(rig.storage.load(ConfigSection::kDevices, document).ok());
}

static void test_full_filesystem_fails_loudly() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  rig.backend.setQuota(16);  // smaller than any real document

  JsonDocument document;
  document["devices"][0]["key"] = "sim_01";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kFilesystemFull),
                        static_cast<int>(
                            rig.storage.save(ConfigSection::kDevices, document).code));
}

// ---------------------------------------------------------------------------
//  Boot behaviour
// ---------------------------------------------------------------------------
static void test_first_boot_without_configuration_is_normal() {
  Rig rig;
  const BootReport& report = rig.system.begin();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BootMode::kNormal),
                        static_cast<int>(report.mode));
  // devices.json exists from the first boot and is empty; "present" and
  // "describes a rig" are different questions, and only the second one matters
  // here.
  TEST_ASSERT_EQUAL_UINT(0, report.devices.applied);
  TEST_ASSERT_EQUAL_UINT(0, report.devices.failed);
  TEST_ASSERT_EQUAL_UINT(0, rig.devices.activeCount());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kOk),
                        static_cast<int>(report.storageError.code));
}

static void test_repeated_boot_failures_lead_to_safe_mode() {
  Rig rig;
  rig.writeRaw("/config/devices.json", kOneSimulator);
  for (int i = 0; i < SystemManager::kMaxConsecutiveFailures; ++i) {
    rig.bootCounter.markAttempt();
  }

  const BootReport& report = rig.system.begin();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BootMode::kSafe),
                        static_cast<int>(report.mode));
  // Safe mode is defined by what it does NOT do: no devices are started, so a
  // configuration that hangs the board can still be fixed over the network.
  TEST_ASSERT_EQUAL_UINT(0, rig.devices.activeCount());
}

static void test_one_bad_device_does_not_stop_the_others() {
  Rig rig;
  rig.writeRaw("/config/devices.json", R"({
    "schemaVersion": 1,
    "devices": [
      { "key": "good_01", "module": "sim_signal",
        "config": { "waveform": "constant", "offset": 1 } },
      { "key": "bad_01", "module": "sim_signal",
        "config": { "waveform": "constant", "period_s": 0 } },
      { "key": "good_02", "module": "sim_signal",
        "config": { "waveform": "constant", "offset": 2 } }
    ]
  })");

  const BootReport& report = rig.system.begin();
  TEST_ASSERT_EQUAL_UINT(2, report.devices.applied);
  TEST_ASSERT_EQUAL_UINT(1, report.devices.failed);
  TEST_ASSERT_EQUAL_STRING("bad_01", report.devices.firstFailedKey.c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDeviceConfigInvalid),
                        static_cast<int>(report.devices.firstError.code));
  TEST_ASSERT_EQUAL_UINT(2, rig.devices.activeCount());
}

static void test_processing_section_is_applied_to_named_channels() {
  Rig rig;
  rig.writeRaw("/config/devices.json", kOneSimulator);
  rig.writeRaw("/config/processing.json", R"({
    "schemaVersion": 1,
    "pipelines": {
      "ref_signal": {
        "stages": [
          { "type": "calibration",
            "config": { "type": "polynomial", "x_center": 0, "x_scale": 1,
                        "coefficients": [1.0, 2.0] } },
          { "type": "moving_average", "config": { "window": 2 } }
        ]
      }
    }
  })");

  const BootReport& report = rig.system.begin();
  TEST_ASSERT_EQUAL_UINT(1, report.processing.applied);
  TEST_ASSERT_EQUAL_UINT(0, report.processing.failed);

  const ChannelHandle handle = rig.channels.findByKey("ref_signal");
  TEST_ASSERT_EQUAL_UINT(2, rig.processing.stageCount(handle));
  // calibration_stage was not given, so it is auto-detected.
  rig.channels.publishRaw(handle, 10.0f, 1000);
  TEST_ASSERT_EQUAL_FLOAT(21.0f, rig.channels.value(handle)->calibrated);
}

// ---------------------------------------------------------------------------
//  Milestone 1 acceptance criterion
// ---------------------------------------------------------------------------
static void test_a_device_lives_and_dies_by_the_configuration_file() {
  Rig rig;
  rig.writeRaw("/config/devices.json", kOneSimulator);

  // --- boot ---------------------------------------------------------------
  const BootReport& report = rig.system.begin();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BootMode::kNormal),
                        static_cast<int>(report.mode));
  TEST_ASSERT_EQUAL_UINT(1, report.devices.applied);
  TEST_ASSERT_EQUAL_UINT(0, report.devices.failed);

  const DeviceRecord* record = rig.devices.findByKey("sim_01");
  TEST_ASSERT_NOT_NULL(record);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kRunning),
                        static_cast<int>(record->state));
  TEST_ASSERT_EQUAL_STRING("Bench simulator", record->name.c_str());

  // Channel key, unit and geometry all came from the file.
  const ChannelHandle handle = rig.channels.findByKey("ref_signal");
  TEST_ASSERT_TRUE(handle != kInvalidChannel);
  const ChannelDescriptor* descriptor = rig.channels.descriptor(handle);
  TEST_ASSERT_EQUAL_STRING("degC", descriptor->unit.c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CoordinateSystem::kCylindrical),
                        static_cast<int>(descriptor->geometry.system));
  TEST_ASSERT_EQUAL_FLOAT(4.0f, descriptor->geometry.c);

  // --- it actually runs ----------------------------------------------------
  for (int i = 0; i < 5; ++i) {
    rig.clock.advanceMicros(100000);
    rig.system.loop();
  }
  TEST_ASSERT_EQUAL_FLOAT(42.0f, rig.channels.value(handle)->processed);
  TEST_ASSERT_TRUE(rig.channels.value(handle)->sequence >= 5);

  // --- edit the file, reload ----------------------------------------------
  JsonDocument document;
  TEST_ASSERT_TRUE(rig.storage.load(ConfigSection::kDevices, document).ok());
  TEST_ASSERT_TRUE(
      ConfigApplier::removeDevice(document, "sim_01"));
  TEST_ASSERT_TRUE(rig.storage.save(ConfigSection::kDevices, document).ok());

  TEST_ASSERT_TRUE(rig.system.reloadConfiguration().ok());

  // --- gone, completely ----------------------------------------------------
  TEST_ASSERT_NULL(rig.devices.findByKey("sim_01"));
  TEST_ASSERT_EQUAL_UINT(0, rig.devices.activeCount());
  TEST_ASSERT_EQUAL_UINT(kInvalidChannel, rig.channels.findByKey("ref_signal"));
  // Two channels remain and they are not the device's: the experiment engine
  // publishes its run state AS channels so that dashboards, rules and safety
  // limits can read it without knowing what an experiment is (ADR-0018).  They
  // belong to the system, not to any device, and outlive every reload.
  TEST_ASSERT_EQUAL_UINT(2, rig.channels.activeCount());
  TEST_ASSERT_TRUE(rig.channels.findByKey("experiment_state") != kInvalidChannel);
  TEST_ASSERT_EQUAL_UINT(0, rig.resources.claimCount());
  TEST_ASSERT_EQUAL_UINT(0, rig.processing.totalStages());
}

static void test_upsert_edits_the_document_in_place() {
  Rig rig;
  rig.writeRaw("/config/devices.json", kOneSimulator);

  JsonDocument document;
  TEST_ASSERT_TRUE(rig.storage.load(ConfigSection::kDevices, document).ok());

  JsonDocument edit;
  edit["key"] = "sim_01";
  edit["module"] = "sim_signal";
  edit["config"]["waveform"] = "sine";
  edit["config"]["offset"] = 7.5;
  TEST_ASSERT_TRUE(ConfigApplier::upsertDevice(document, edit.as<JsonObjectConst>()).ok());

  // Replaced, not duplicated.
  TEST_ASSERT_EQUAL_UINT(1, document["devices"].size());
  TEST_ASSERT_EQUAL_STRING("sine", document["devices"][0]["config"]["waveform"]);

  JsonDocument added;
  added["key"] = "sim_02";
  added["module"] = "sim_signal";
  TEST_ASSERT_TRUE(ConfigApplier::upsertDevice(document, added.as<JsonObjectConst>()).ok());
  TEST_ASSERT_EQUAL_UINT(2, document["devices"].size());

  TEST_ASSERT_TRUE(rig.storage.save(ConfigSection::kDevices, document).ok());
  const BootReport& report = rig.system.begin();
  TEST_ASSERT_EQUAL_UINT(2, report.devices.applied);
}

// ---------------------------------------------------------------------------
//  control.json (Milestone 8)
// ---------------------------------------------------------------------------
static void test_a_loop_comes_back_from_the_file_with_its_numbers_but_not_its_authority() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  rig.writeRaw("/config/devices.json", kOneSimulator);
  rig.writeRaw("/config/control.json", R"({
    "schemaVersion": 1,
    "limits": [ { "id": "overtemp", "channel": "ref_signal",
                  "condition": "above", "high": 300, "action": "trip_all",
                  "for_s": 0.5 } ],
    "loops":  [ { "id": "bath", "input": "ref_signal", "output": "heat",
                  "setpoint": 180, "kp": 2, "ki": 0.05, "kd": 10,
                  "min": 0, "max": 100, "period_s": 2, "input_grace_s": 5,
                  "mode": "automatic" } ],
    "rules":  [ { "id": "fan", "input": "ref_signal", "output": "fan",
                  "on_above": 40, "off_below": 35, "min_hold_s": 10 } ]
  })");

  const BootReport& report = rig.system.begin();
  TEST_ASSERT_TRUE(report.controlConfigPresent);
  TEST_ASSERT_EQUAL_UINT(3, report.control.applied);
  TEST_ASSERT_EQUAL_UINT(0, report.control.failed);

  const ControlLoop* loop = rig.control.findLoop("bath");
  TEST_ASSERT_NOT_NULL(loop);
  TEST_ASSERT_EQUAL_FLOAT(180.0f, loop->setpoint);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, loop->kd);
  TEST_ASSERT_EQUAL_UINT(2000000ULL, loop->periodUs);
  // The setpoint is a number and survives.  The authority to act on it is not
  // a number, and does not: whatever the file says, the loop comes up OFF.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LoopMode::kOff),
                        static_cast<int>(loop->mode));

  // The limit came back too, and with its debounce in microseconds.
  const SafetyLimit* limit = rig.safety.find("overtemp");
  TEST_ASSERT_NOT_NULL(limit);
  TEST_ASSERT_EQUAL_UINT(500000ULL, limit->forUs);
  TEST_ASSERT_TRUE(limit->requireFreshInput);
  TEST_ASSERT_FALSE(limit->latched);
  TEST_ASSERT_EQUAL_UINT(1, rig.control.ruleCount());
}

static void test_a_control_file_that_cannot_be_read_holds_everything_down() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  rig.writeRaw("/config/devices.json", kOneSimulator);
  rig.writeRaw("/config/control.json", "{ \"limits\": [ { \"id\": ");

  const BootReport& report = rig.system.begin();
  // Devices still came up — a broken control file is not a reason to lose the
  // instrument.  But the interlocks in that file did not, and a rig with
  // regulators and without interlocks is exactly what must not run.
  TEST_ASSERT_EQUAL_UINT(1, report.devices.applied);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BootMode::kSafe),
                        static_cast<int>(report.mode));
  TEST_ASSERT_TRUE(rig.outputs.tripped());
}

static void test_a_limit_that_will_not_parse_does_not_take_the_others_with_it() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  rig.writeRaw("/config/devices.json", kOneSimulator);
  rig.writeRaw("/config/control.json", R"({
    "schemaVersion": 1,
    "limits": [ { "id": "good", "channel": "ref_signal", "condition": "above",
                  "high": 300, "action": "trip_all" },
                { "id": "bad", "channel": "ref_signal", "condition": "outside",
                  "low": 100, "high": 10 } ]
  })");

  const BootReport& report = rig.system.begin();
  TEST_ASSERT_EQUAL_UINT(1, report.control.applied);
  TEST_ASSERT_EQUAL_UINT(1, report.control.failed);
  TEST_ASSERT_EQUAL_STRING("bad", report.control.firstFailedKey.c_str());
  TEST_ASSERT_EQUAL_STRING("high", report.control.firstFailedField.c_str());
  TEST_ASSERT_NOT_NULL(rig.safety.find("good"));
}

static void test_control_serialisation_round_trips() {
  ControlLoop loop;
  loop.id.assign("bath");
  loop.inputKey.assign("temp");
  loop.outputKey.assign("heat");
  loop.setpoint = 42.5f;
  loop.ki = 0.25f;
  loop.periodUs = 1500000;
  loop.inputGraceUs = 3000000;
  loop.invert = true;

  JsonDocument document;
  ControlStore::serializeLoop(loop, document.to<JsonObject>());

  ControlLoop back;
  LabelString field;
  TEST_ASSERT_TRUE(
      ControlStore::parseLoop(document.as<JsonObjectConst>(), back, field).ok());
  TEST_ASSERT_EQUAL_STRING("bath", back.id.c_str());
  TEST_ASSERT_EQUAL_FLOAT(42.5f, back.setpoint);
  TEST_ASSERT_EQUAL_FLOAT(0.25f, back.ki);
  TEST_ASSERT_EQUAL_UINT(1500000ULL, back.periodUs);
  TEST_ASSERT_EQUAL_UINT(3000000ULL, back.inputGraceUs);
  TEST_ASSERT_TRUE(back.invert);

  // Out-of-band values are refused with the field named, not clamped silently.
  JsonDocument absurd;
  absurd.set(document.as<JsonObjectConst>());
  absurd["input_grace_s"] = 3600;
  ControlLoop rejected;
  TEST_ASSERT_FALSE(
      ControlStore::parseLoop(absurd.as<JsonObjectConst>(), rejected, field).ok());
  TEST_ASSERT_EQUAL_STRING("input_grace_s", field.c_str());
}

// ---------------------------------------------------------------------------
//  Experiments and run records (Milestone 9)
// ---------------------------------------------------------------------------
static void test_a_run_interrupted_by_a_reboot_comes_back_as_aborted() {
  const std::string root = makeTempRoot();
  {
    // Same directory for both boots: this test is about what survives one.
    Rig rig(root, /*keep=*/true);
    TEST_ASSERT_TRUE(rig.storage.begin().ok());
    rig.writeRaw("/config/devices.json", kOneSimulator);
    rig.system.begin();

    RunRecord record;
    record.experimentKey.assign("evaporation");
    record.name.assign("Evaporation run");
    record.metadata.operatorName.assign("AM");
    record.metadata.sample.assign("NaCl 5 %");
    record.stepCount = 7;
    record.startedEpochMs = 1786700000000ULL;
    // Exactly what ExperimentEngine::start() does, and then the power goes.
    rig.runLog.onRunStarted(record);
    TEST_ASSERT_TRUE(rig.backend.exists(RunLog::kMarkerPath));
  }

  // Second boot, same filesystem: nothing resumes, and the run that was open
  // is written as ABORTED rather than left as a gap in the history.
  {
    Rig rig(root, /*keep=*/false);
    TEST_ASSERT_TRUE(rig.storage.begin().ok());
    rig.system.begin();

    TEST_ASSERT_FALSE(rig.backend.exists(RunLog::kMarkerPath));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExperimentState::kIdle),
                          static_cast<int>(rig.experiments.state()));

    JsonDocument runs;
    TEST_ASSERT_TRUE(rig.runLog.load(runs).ok());
    JsonObjectConst record = runs["runs"][0].as<JsonObjectConst>();
    TEST_ASSERT_EQUAL_STRING("evaporation", record["experiment"]);
    TEST_ASSERT_EQUAL_STRING("ABORTED", record["state"]);
    TEST_ASSERT_EQUAL_STRING("restarted", record["reason"]);
    // The metadata survives with it: an interrupted run is still a run
    // somebody has to be able to ask about (§48).
    TEST_ASSERT_EQUAL_STRING("NaCl 5 %", record["sample"]);
  }
}

static void test_the_run_log_keeps_the_newest_records_and_drops_the_oldest() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.storage.begin().ok());
  TEST_ASSERT_TRUE(rig.runLog.begin(rig.scheduler).ok());

  for (std::size_t i = 0; i < limits::kMaxRunRecords + 3; ++i) {
    RunRecord record;
    record.experimentKey.assign("run");
    record.name.assign("Run");
    record.metadata.operatorName.assign("AM");
    record.stepCount = 1;
    record.stepReached = 1;
    record.finalState = ExperimentState::kFinished;
    record.reason = StopReason::kScenario;
    record.startedEpochMs = 1000 + i;
    rig.runLog.onRunFinished(record);
    TEST_ASSERT_TRUE(rig.runLog.flushPending().ok());
  }

  JsonDocument runs;
  TEST_ASSERT_TRUE(rig.runLog.load(runs).ok());
  // Bounded on purpose: the flash this file lives on is shared with the web
  // interface, and a history that grows without limit eventually costs the
  // instrument its own front end.
  TEST_ASSERT_EQUAL_UINT(limits::kMaxRunRecords, runs["runs"].size());
  // Newest first.
  TEST_ASSERT_EQUAL_UINT(1000 + limits::kMaxRunRecords + 2,
                         runs["runs"][0]["started_epoch_ms"].as<std::uint64_t>());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_save_and_load_round_trip);
  RUN_TEST(test_missing_section_is_not_an_error_condition);
  RUN_TEST(test_first_boot_writes_empty_sections_once);
  RUN_TEST(test_seeding_never_overwrites_a_configuration);
  RUN_TEST(test_corrupt_json_is_reported_not_guessed);
  RUN_TEST(test_missing_schema_version_is_refused);
  RUN_TEST(test_a_file_from_the_future_is_refused_not_reinterpreted);
  RUN_TEST(test_interrupted_write_leaves_the_old_file_intact);
  RUN_TEST(test_full_filesystem_fails_loudly);
  RUN_TEST(test_first_boot_without_configuration_is_normal);
  RUN_TEST(test_repeated_boot_failures_lead_to_safe_mode);
  RUN_TEST(test_one_bad_device_does_not_stop_the_others);
  RUN_TEST(test_processing_section_is_applied_to_named_channels);
  RUN_TEST(test_a_device_lives_and_dies_by_the_configuration_file);
  RUN_TEST(test_upsert_edits_the_document_in_place);
  RUN_TEST(test_a_loop_comes_back_from_the_file_with_its_numbers_but_not_its_authority);
  RUN_TEST(test_a_control_file_that_cannot_be_read_holds_everything_down);
  RUN_TEST(test_a_limit_that_will_not_parse_does_not_take_the_others_with_it);
  RUN_TEST(test_control_serialisation_round_trips);
  RUN_TEST(test_a_run_interrupted_by_a_reboot_comes_back_as_aborted);
  RUN_TEST(test_the_run_log_keeps_the_newest_records_and_drops_the_oldest);
  return UNITY_END();
}
