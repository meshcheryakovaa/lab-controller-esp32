// =============================================================================
//  Milestone 3 — REST API and WebSocket telemetry, tested without a network.
//      pio test -e native
//
//  The last group is the Milestone 3 acceptance criterion: a sensor is added,
//  configured and removed entirely over HTTP, the configuration file follows,
//  and the values reach the socket.
// =============================================================================
#include <unity.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "api/PathRouter.h"
#include "api/RestApi.h"
#include "api/TelemetryBatcher.h"
#include "app/SystemManager.h"
#include "core/Clock.h"
#include "platform/host/HostBusProvider.h"
#include "platform/host/HostRandom.h"
#include "storage/PosixBackend.h"

using namespace lc;

void setUp() {}
void tearDown() {}

namespace {

std::string makeTempRoot() {
  char pattern[] = "/tmp/lc-api-XXXXXX";
  const char* dir = mkdtemp(pattern);
  return dir != nullptr ? std::string(dir) : std::string("/tmp/lc-api-fallback");
}

class FakeSink final : public IWebSocketSink {
 public:
  std::size_t clients = 1;
  bool busy = false;
  std::vector<std::string> frames;

  std::size_t clientCount() const override { return clients; }
  bool canSend() const override { return !busy; }
  bool broadcast(const char* text, std::size_t length) override {
    frames.emplace_back(text, length);
    return true;
  }
};


/**
 * A network manager with no radio (M16).
 *
 * Everything worth testing about /network is policy, not physics: that a
 * password never comes back, that a second connect attempt is refused, that a
 * hostname is validated.  This fake makes those ordinary host assertions, and
 * records the password purely so a test can prove the API never returns it.
 */
class FakeNetworkManager final : public INetworkManager {
 public:
  NetworkStatus state;
  ScanState scan = ScanState::kIdle;
  std::vector<NetworkCandidate> candidates;
  // What was handed to testCredentials().  Present so a test can assert the
  // secret reached the manager and still never appears in any response.
  std::string lastSsid;
  std::string lastPassword;
  int connectCalls = 0;
  int clearCalls = 0;
  bool busy = false;

  FakeNetworkManager() {
    state.state = NetworkState::kApOnly;
    state.accessPointActive = true;
    state.accessPointSsid.assign("LAB-CONTROLLER-A1B2C3");
    state.accessPointIp.assign("192.168.4.1");
    state.hostname.assign("lab-controller-a1b2c3");
  }

  NetworkStatus status() const override { return state; }

  Status beginScan() override {
    if (busy) return fail(ErrorCode::kResourceBusy, "busy");
    scan = ScanState::kComplete;
    return ok();
  }
  ScanState scanState() const override { return scan; }
  std::size_t scanResults(NetworkCandidate* out,
                          std::size_t capacity) const override {
    const std::size_t count =
        candidates.size() < capacity ? candidates.size() : capacity;
    for (std::size_t i = 0; i < count; ++i) out[i] = candidates[i];
    return count;
  }

  Status testCredentials(const char* ssid, const char* password) override {
    if (busy) {
      return fail(ErrorCode::kResourceBusy, "a connection test is running");
    }
    ++connectCalls;
    lastSsid = ssid != nullptr ? ssid : "";
    lastPassword = password != nullptr ? password : "";
    busy = true;
    state.state = NetworkState::kStationConnecting;
    state.testing = true;
    return ok();
  }

  Status clearCredentials() override {
    ++clearCalls;
    state = NetworkStatus{};
    state.state = NetworkState::kApOnly;
    state.accessPointActive = true;
    state.accessPointIp.assign("192.168.4.1");
    state.accessPointSsid.assign("LAB-CONTROLLER-A1B2C3");
    return ok();
  }

  Status setHostname(const char* hostname) override {
    if (!INetworkManager::hostnameIsValid(hostname)) {
      return fail(ErrorCode::kInvalidArgument, "bad hostname");
    }
    state.hostname.assign(hostname);
    return ok();
  }
};

int g_rebootCalls = 0;
void countReboot(void*) { ++g_rebootCalls; }

// A complete controller plus its API, wired exactly as main.cpp does.
struct ApiRig {
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
  NullSystemMetrics metrics;
  // A board with nothing wired to it: pins and buses exist, no sensor answers.
  // Without this, every GPIO-using module would be refused for the wrong
  // reason and the pin-level validation below would never be reached.
  platform::HostBusProvider buses;
  SystemManager system{makeSystemServices()};
  RestApi api{makeApiServices()};
  FakeNetworkManager network;
  FakeSink sink;
  TelemetryBatcher telemetry{channels, clock};


  SystemManager::Services makeSystemServices() {
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
    services.buses = &buses;
    return services;
  }

  RestApi::Services makeApiServices() {
    RestApi::Services services;
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
    services.system = &system;
    services.metrics = &metrics;
    services.network = &network;
    services.buses = &buses;
    services.reboot = countReboot;
    return services;
  }

  ApiRig() {
    // Before begin(): the boot sequence reads control.json, and an applier that
    // does not yet know about the control layer would silently drop it.
    applier.setSafety(&safety);
    applier.setControl(&control);
    system.begin();
  }

  ~ApiRig() {
    devices.removeAll();
    const std::string command = "rm -rf " + root;
    if (std::system(command.c_str()) != 0) { /* best effort */ }
  }

  // --- request helpers -----------------------------------------------------
  // The session cookie this rig sends with every request.  Empty until a test
  // signs in — which is the point: the policy is exercised by the tests rather
  // than waved through by the harness.
  std::string cookie;

  ApiResponse call(HttpMethod method, const char* path, const char* query = "",
                   const char* body = nullptr) {
    ApiRequest request;
    request.method = method;
    request.path = path;
    request.query = query;
    request.body = body;
    request.bodyLength = (body != nullptr) ? std::strlen(body) : 0;
    request.cookie = cookie.empty() ? nullptr : cookie.c_str();
    ApiResponse response;
    api.handle(request, response);
    // Sign-in and sign-out arrive as a Set-Cookie, exactly as a browser would
    // see them.
    if (!response.setCookie.empty()) {
      const std::string header = response.setCookie.c_str();
      const std::size_t semicolon = header.find(';');
      cookie = header.substr(0, semicolon);
      if (cookie.find("=;") != std::string::npos ||
          cookie.substr(cookie.find('=') + 1).empty()) {
        cookie.clear();
      }
    }
    return response;
  }

  // Sets a password and signs in, for the tests that are not about signing in.
  void signIn(const char* password = "bench-password") {
    post("/api/v1/auth/password", (std::string(R"({"password":")") + password +
                                   R"("})").c_str());
    post("/api/v1/auth/login", (std::string(R"({"password":")") + password +
                                R"("})").c_str());
  }

  ApiResponse get(const char* path, const char* query = "") {
    return call(HttpMethod::kGet, path, query);
  }
  ApiResponse post(const char* path, const char* body, const char* query = "") {
    return call(HttpMethod::kPost, path, query, body);
  }

  // What is actually on "disk" right now.
  JsonDocument storedDevices() {
    JsonDocument document;
    storage.load(ConfigSection::kDevices, document);
    return document;
  }
};

const char* kSimulatorBody = R"({
  "key": "sim_01",
  "module": "sim_signal",
  "name": "Bench simulator",
  "sample_interval_us": 100000,
  "config": { "waveform": "constant", "amplitude": 0, "offset": 42.0,
              "period_s": 10, "noise": 0 },
  "channels": { "value": { "key": "ref_signal", "unit": "degC" } }
})";

}  // namespace

// ===========================================================================
//  Path router and request parsing
// ===========================================================================
static void test_path_is_split_into_segments() {
  PathSegments path("/api/v1/devices/hx711_01/actions/self-test");
  TEST_ASSERT_EQUAL_UINT(6, path.count());
  TEST_ASSERT_TRUE(path.isApiV1());
  TEST_ASSERT_EQUAL_STRING("devices", path.at(2));
  TEST_ASSERT_EQUAL_STRING("hx711_01", path.at(3));
  TEST_ASSERT_EQUAL_STRING("self-test", path.at(5));
  TEST_ASSERT_FALSE(path.truncated());

  // Trailing slashes, doubled slashes and a query string must not change the
  // segment list.
  PathSegments messy("//api//v1/devices/?dry_run=1");
  TEST_ASSERT_EQUAL_UINT(3, messy.count());
  TEST_ASSERT_EQUAL_STRING("devices", messy.at(2));
}

static void test_path_percent_decodes_keys() {
  // A channel key with a space survives the round trip through a URL.
  PathSegments path("/api/v1/channels/sample%20mass");
  TEST_ASSERT_EQUAL_STRING("sample mass", path.at(3));
}

static void test_query_flags_are_parsed() {
  ApiRequest request;
  request.query = "dry_run=1&values";
  TEST_ASSERT_TRUE(request.queryFlag("dry_run"));
  TEST_ASSERT_TRUE(request.queryFlag("values"));   // bare flag means yes
  TEST_ASSERT_FALSE(request.queryFlag("missing"));

  request.query = "dry_run=0";
  TEST_ASSERT_FALSE(request.queryFlag("dry_run"));

  // "dry" must not match "dry_run".
  request.query = "dry_run=1";
  TEST_ASSERT_FALSE(request.queryFlag("dry"));
}

static void test_errors_map_onto_honest_http_statuses() {
  TEST_ASSERT_EQUAL_INT(404, httpStatusFor(ErrorCode::kNotFound));
  TEST_ASSERT_EQUAL_INT(409, httpStatusFor(ErrorCode::kResourceBusy));
  TEST_ASSERT_EQUAL_INT(409, httpStatusFor(ErrorCode::kAlreadyExists));
  // Syntactically valid, semantically wrong -> the form stays filled in.
  TEST_ASSERT_EQUAL_INT(422, httpStatusFor(ErrorCode::kDeviceConfigInvalid));
  TEST_ASSERT_EQUAL_INT(422, httpStatusFor(ErrorCode::kGpioInputOnly));
  TEST_ASSERT_EQUAL_INT(507, httpStatusFor(ErrorCode::kFilesystemFull));
  TEST_ASSERT_EQUAL_INT(504, httpStatusFor(ErrorCode::kDeviceNotResponding));
}

static void test_error_envelope_has_a_stable_shape() {
  ApiResponse response;
  response.setError(fail(ErrorCode::kResourceBusy, "used by I2C0 SDA"), "data_pin",
                    "GPIO21 is already in use");

  TEST_ASSERT_EQUAL_INT(409, response.status);
  TEST_ASSERT_EQUAL_STRING("RESOURCE_BUSY", response.body["error"]["code"]);
  TEST_ASSERT_EQUAL_INT(200, response.body["error"]["numeric"].as<int>());
  TEST_ASSERT_EQUAL_STRING("GPIO21 is already in use",
                           response.body["error"]["message"]);
  TEST_ASSERT_EQUAL_STRING("used by I2C0 SDA", response.body["error"]["detail"]);
  TEST_ASSERT_EQUAL_STRING("data_pin", response.body["error"]["field"]);
}

// ===========================================================================
//  Read-only endpoints
// ===========================================================================
static void test_modules_endpoint_carries_everything_a_form_needs() {
  ApiRig rig;
  const ApiResponse response = rig.get("/api/v1/modules/hx711");
  TEST_ASSERT_EQUAL_INT(200, response.status);
  TEST_ASSERT_EQUAL_STRING("hx711", response.body["id"]);
  TEST_ASSERT_EQUAL_STRING("sensor", response.body["category"]);

  JsonArrayConst params = response.body["params"];
  TEST_ASSERT_EQUAL_UINT(4, params.size());

  // The pin picker needs the type AND the intended use, or it cannot filter.
  bool foundClock = false;
  for (JsonObjectConst param : params) {
    if (std::strcmp(param["key"] | "", "clock_pin") != 0) continue;
    foundClock = true;
    TEST_ASSERT_EQUAL_STRING("gpio", param["type"]);
    TEST_ASSERT_EQUAL_STRING("digital_output", param["pin_use"]);
    TEST_ASSERT_TRUE(param["required"].as<bool>());
  }
  TEST_ASSERT_TRUE(foundClock);

  // Select options travel with the manifest; the UI never hard-codes them.
  for (JsonObjectConst param : params) {
    if (std::strcmp(param["key"] | "", "gain") != 0) continue;
    TEST_ASSERT_EQUAL_UINT(3, param["options"].size());
    TEST_ASSERT_EQUAL_STRING("128", param["options"][0]["value"]);
  }

  JsonArrayConst channels = response.body["channels"];
  TEST_ASSERT_EQUAL_UINT(1, channels.size());
  TEST_ASSERT_EQUAL_STRING("mass", channels[0]["id"]);
  TEST_ASSERT_EQUAL_STRING("mass", channels[0]["quantity"]);
}

static void test_gpio_endpoint_explains_why_a_pin_is_unavailable() {
  ApiRig rig;
  TEST_ASSERT_TRUE(rig.resources.claimPin(21, PinUse::kBusSignal, kInvalidDevice,
                                          "I2C0 SDA").ok());

  const ApiResponse response = rig.get("/api/v1/gpio");
  TEST_ASSERT_EQUAL_INT(200, response.status);
  TEST_ASSERT_EQUAL_STRING("esp32", response.body["chip"]);

  bool sawFlashPin = false, sawInputOnly = false, sawOwned = false;
  for (JsonObjectConst pin : response.body["pins"].as<JsonArrayConst>()) {
    const int number = pin["pin"] | -1;
    if (number == 7) {
      sawFlashPin = true;
      TEST_ASSERT_FALSE(pin["usable"].as<bool>());
      TEST_ASSERT_EQUAL_STRING("Connected to SPI flash", pin["advisory"]);
    }
    if (number == 34) {
      sawInputOnly = true;
      TEST_ASSERT_TRUE(pin["input_only"].as<bool>());
    }
    if (number == 21) {
      sawOwned = true;
      TEST_ASSERT_EQUAL_STRING("I2C0 SDA", pin["owner"]);
    }
  }
  TEST_ASSERT_TRUE(sawFlashPin);
  TEST_ASSERT_TRUE(sawInputOnly);
  TEST_ASSERT_TRUE(sawOwned);
}

static void test_unknown_routes_and_methods_are_refused_cleanly() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(404, rig.get("/api/v1/nonsense").status);
  TEST_ASSERT_EQUAL_INT(404, rig.get("/api/v2/system").status);
  TEST_ASSERT_EQUAL_INT(404, rig.get("/api/v1/modules/no_such_module").status);
  TEST_ASSERT_EQUAL_INT(405,
                        rig.call(HttpMethod::kDelete, "/api/v1/system").status);
}

static void test_system_and_diagnostics_report_real_numbers() {
  ApiRig rig;
  rig.clock.advanceMicros(5000000);

  const ApiResponse system = rig.get("/api/v1/system");
  TEST_ASSERT_EQUAL_INT(200, system.status);
  TEST_ASSERT_EQUAL_STRING("esp32", system.body["chip"]);
  TEST_ASSERT_EQUAL_INT(5000, system.body["uptime_ms"].as<int>());
  // The clock is honest about not being synchronised.
  TEST_ASSERT_FALSE(system.body["time_synchronised"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("NORMAL", system.body["boot_mode"]);

  const ApiResponse diagnostics = rig.get("/api/v1/diagnostics");
  TEST_ASSERT_EQUAL_INT(200, diagnostics.status);
  TEST_ASSERT_TRUE(diagnostics.body["tasks"].is<JsonArrayConst>());
  TEST_ASSERT_TRUE(diagnostics.body["loop"]["passes"].is<unsigned>());
  TEST_ASSERT_EQUAL_INT(2, diagnostics.body["api"]["requests"].as<int>());
}

// Milestone 14: the browser files local recordings under this string, so two
// rigs that share an address must not share an identity.  A blank or missing
// field would make a client fall back to the origin, and in access-point mode
// every controller is 192.168.4.1 — two experiments in one archive, silently.
static void test_system_names_the_controller_independently_of_its_address() {
  ApiRig rig;
  const ApiResponse system = rig.get("/api/v1/system");
  TEST_ASSERT_EQUAL_INT(200, system.status);
  TEST_ASSERT_TRUE(system.body["controller_id"].is<const char*>());
  const char* id = system.body["controller_id"];
  TEST_ASSERT_NOT_NULL(id);
  // Never empty: an empty identity is the failure this field exists to prevent.
  TEST_ASSERT_TRUE(std::strlen(id) > 0);
  // Stable across calls — it is an identity, not a nonce.
  const ApiResponse again = rig.get("/api/v1/system");
  TEST_ASSERT_EQUAL_STRING(id, again.body["controller_id"]);
  // And it is not the address.
  TEST_ASSERT_TRUE(std::strcmp(id, "0.0.0.0") != 0);
  TEST_ASSERT_TRUE(std::strcmp(id, "192.168.4.1") != 0);
}


static void test_reboot_requires_post_and_calls_the_hook() {
  ApiRig rig;
  g_rebootCalls = 0;
  TEST_ASSERT_EQUAL_INT(405, rig.get("/api/v1/system/reboot").status);
  TEST_ASSERT_EQUAL_INT(0, g_rebootCalls);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/system/reboot", "{}").status);
  TEST_ASSERT_EQUAL_INT(1, g_rebootCalls);
}

// ===========================================================================
//  Validation and dry-run
// ===========================================================================
static void test_dry_run_validates_without_creating_anything() {
  ApiRig rig;
  const char* body = R"({"key":"hx_01","module":"hx711",
                         "config":{"data_pin":16,"clock_pin":34,"gain":128}})";

  const ApiResponse response = rig.post("/api/v1/devices", body, "dry_run=1");
  // GPIO34 has no output driver, and the answer says which field is wrong.
  TEST_ASSERT_EQUAL_INT(422, response.status);
  TEST_ASSERT_EQUAL_STRING("GPIO_INPUT_ONLY", response.body["error"]["code"]);
  TEST_ASSERT_EQUAL_STRING("clock_pin", response.body["error"]["field"]);

  TEST_ASSERT_EQUAL_UINT(0, rig.devices.activeCount());
  // devices.json exists from the first boot; what a dry run must not do is put
  // anything IN it.
  JsonDocument afterDryRun = rig.storedDevices();
  TEST_ASSERT_EQUAL_UINT(0, afterDryRun["devices"].size());

  // The same request with a workable pin passes validation and still creates
  // nothing — which is exactly what makes live form validation trustworthy.
  const char* good = R"({"key":"hx_01","module":"hx711",
                         "config":{"data_pin":16,"clock_pin":17,"gain":128}})";
  const ApiResponse valid = rig.post("/api/v1/devices", good, "dry_run=1");
  TEST_ASSERT_EQUAL_INT(200, valid.status);
  TEST_ASSERT_TRUE(valid.body["valid"].as<bool>());
  TEST_ASSERT_EQUAL_UINT(0, rig.devices.activeCount());
  TEST_ASSERT_EQUAL_UINT(0, rig.resources.claimCount());
}

static void test_a_device_that_failed_at_boot_is_reported_not_swallowed() {
  ApiRig rig;

  // Two devices in the stored configuration; the second one names a waveform
  // this build does not have.  It will never appear in /devices — it has no
  // record, no handle and no channel — so /system is the only place the
  // operator can learn that it was configured at all (§46).
  JsonDocument stored;
  deserializeJson(stored, R"({"schemaVersion":1,"devices":[
    {"key":"good_01","module":"sim_signal",
     "config":{"waveform":"constant","offset":1,"period_s":10}},
    {"key":"bad_01","module":"sim_signal",
     "config":{"waveform":"chainsaw","offset":1,"period_s":10}}]})");
  TEST_ASSERT_TRUE(rig.storage.save(ConfigSection::kDevices, stored).ok());

  rig.system.reloadConfiguration();

  const ApiResponse response = rig.get("/api/v1/system");
  TEST_ASSERT_EQUAL_INT(200, response.status);
  TEST_ASSERT_EQUAL_INT(1, response.body["counts"]["devices"].as<int>());
  TEST_ASSERT_EQUAL_INT(1, response.body["boot"]["devices_started"].as<int>());
  TEST_ASSERT_EQUAL_INT(1, response.body["boot"]["devices_failed"].as<int>());

  // Naming the device and the field is the difference between a banner the
  // operator can act on and one that only says something went wrong.
  JsonObjectConst failure = response.body["boot"]["first_failure"];
  TEST_ASSERT_FALSE(failure.isNull());
  TEST_ASSERT_EQUAL_STRING("bad_01", failure["device"]);
  TEST_ASSERT_TRUE(failure["code"].as<const char*>()[0] != '\0');
}

static void test_dry_run_and_create_agree_about_a_taken_key() {
  ApiRig rig;

  // A device is there for real.
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);

  // Asking the same question twice must produce the same answer.  A dry run
  // that says "valid" about a key the real create rejects with 409 turns the
  // live form check back into a guess — the very thing ?dry_run=1 exists to
  // avoid (ADR-0013).
  const ApiResponse dry = rig.post("/api/v1/devices", kSimulatorBody, "dry_run=1");
  const ApiResponse real = rig.post("/api/v1/devices", kSimulatorBody);

  TEST_ASSERT_EQUAL_INT(409, dry.status);
  TEST_ASSERT_EQUAL_INT(409, real.status);
  TEST_ASSERT_EQUAL_STRING("ALREADY_EXISTS", dry.body["error"]["code"]);
  TEST_ASSERT_EQUAL_STRING("ALREADY_EXISTS", real.body["error"]["code"]);
  // The wizard highlights a field, so the answer has to name one.
  TEST_ASSERT_EQUAL_STRING("key", dry.body["error"]["field"]);
  TEST_ASSERT_EQUAL_STRING("sim_01", dry.body["error"]["detail"]);

  // The rejected dry run changed nothing.
  TEST_ASSERT_EQUAL_UINT(1, rig.devices.activeCount());
}

static void test_bad_json_is_a_400_with_a_reason() {
  ApiRig rig;
  const ApiResponse response = rig.post("/api/v1/devices", "{\"key\": ");
  TEST_ASSERT_EQUAL_INT(400, response.status);
  TEST_ASSERT_EQUAL_STRING("request body is not valid JSON",
                           response.body["error"]["message"]);
}

static void test_a_device_that_fails_to_start_is_rolled_back_out_of_the_file() {
  ApiRig rig;
  // period_s = 0 passes JSON parsing and manifest range checks are satisfied by
  // the default, but the driver rejects it in configure().
  const char* body = R"({"key":"bad_01","module":"sim_signal",
                         "config":{"waveform":"constant","period_s":0}})";

  const ApiResponse response = rig.post("/api/v1/devices", body);
  TEST_ASSERT_TRUE(response.isError());
  TEST_ASSERT_EQUAL_STRING("DEVICE_CONFIG_INVALID", response.body["error"]["code"]);

  // The stored configuration must not keep a device that could not start —
  // otherwise it would fail again at every boot, forever.
  JsonDocument stored = rig.storedDevices();
  TEST_ASSERT_TRUE(
      ConfigApplier::findDevice(stored, "bad_01").isNull());
  TEST_ASSERT_EQUAL_UINT(0, rig.devices.activeCount());
  // Only the two channels the experiment engine publishes its run state on
  // remain; nothing the failed device asked for survived (ADR-0018).
  TEST_ASSERT_EQUAL_UINT(2, rig.channels.activeCount());
}

// ===========================================================================
//  Milestone 3 acceptance criterion
// ===========================================================================
static void test_a_sensor_is_added_configured_and_removed_over_http() {
  ApiRig rig;

  // --- create --------------------------------------------------------------
  const ApiResponse created = rig.post("/api/v1/devices", kSimulatorBody);
  TEST_ASSERT_EQUAL_INT(201, created.status);
  TEST_ASSERT_EQUAL_STRING("sim_01", created.body["key"]);
  TEST_ASSERT_EQUAL_STRING("RUNNING", created.body["state"]);
  TEST_ASSERT_EQUAL_STRING("ref_signal", created.body["channels"][0]["key"]);

  // It exists in the running rig...
  TEST_ASSERT_EQUAL_UINT(1, rig.devices.activeCount());
  const ChannelHandle handle = rig.channels.findByKey("ref_signal");
  TEST_ASSERT_TRUE(handle != kInvalidChannel);

  // ...and on disk, so it survives a reboot.
  JsonDocument stored = rig.storedDevices();
  JsonObjectConst entry =
      ConfigApplier::findDevice(stored, "sim_01");
  TEST_ASSERT_FALSE(entry.isNull());
  TEST_ASSERT_EQUAL_STRING("sim_signal", entry["module"]);
  TEST_ASSERT_EQUAL_INT(42, entry["config"]["offset"].as<int>());

  // --- it produces data ----------------------------------------------------
  for (int i = 0; i < 5; ++i) {
    rig.clock.advanceMicros(100000);
    rig.system.loop();
  }
  const ApiResponse channel = rig.get("/api/v1/channels/ref_signal");
  TEST_ASSERT_EQUAL_INT(200, channel.status);
  TEST_ASSERT_EQUAL_STRING("degC", channel.body["unit"]);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.0f,
                           channel.body["value"]["processed"].as<float>());
  TEST_ASSERT_EQUAL_STRING("GOOD", channel.body["value"]["quality"]);

  // --- reconfigure ---------------------------------------------------------
  const ApiResponse patched = rig.call(
      HttpMethod::kPatch, "/api/v1/devices/sim_01", "",
      R"({"name":"Renamed","config":{"waveform":"constant","offset":7.5,"period_s":10}})");
  TEST_ASSERT_EQUAL_INT(200, patched.status);
  TEST_ASSERT_EQUAL_STRING("Renamed", patched.body["name"]);

  rig.clock.advanceMicros(200000);
  rig.system.loop();
  const ChannelHandle rebuilt = rig.channels.findByKey("ref_signal");
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.5f,
                           rig.channels.value(rebuilt)->processed);

  stored = rig.storedDevices();
  entry = ConfigApplier::findDevice(stored, "sim_01");
  TEST_ASSERT_EQUAL_STRING("Renamed", entry["name"]);

  // --- self-test -----------------------------------------------------------
  const ApiResponse selfTest =
      rig.post("/api/v1/devices/sim_01/actions/self-test", "{}");
  TEST_ASSERT_EQUAL_INT(200, selfTest.status);
  TEST_ASSERT_TRUE(selfTest.body["passed"].as<bool>());

  // --- disable / enable ----------------------------------------------------
  TEST_ASSERT_EQUAL_INT(
      200, rig.post("/api/v1/devices/sim_01/actions/disable", "{}").status);
  const ApiResponse disabled = rig.get("/api/v1/devices/sim_01");
  TEST_ASSERT_EQUAL_STRING("DISABLED", disabled.body["state"]);
  TEST_ASSERT_EQUAL_INT(
      200, rig.post("/api/v1/devices/sim_01/actions/enable", "{}").status);

  // --- delete --------------------------------------------------------------
  const ApiResponse deleted =
      rig.call(HttpMethod::kDelete, "/api/v1/devices/sim_01");
  TEST_ASSERT_EQUAL_INT(200, deleted.status);

  TEST_ASSERT_EQUAL_UINT(0, rig.devices.activeCount());
  TEST_ASSERT_EQUAL_UINT(0, rig.resources.claimCount());
  TEST_ASSERT_EQUAL_UINT(kInvalidChannel, rig.channels.findByKey("ref_signal"));
  // The two system channels the experiment engine owns are not the device's to
  // take with it.
  TEST_ASSERT_EQUAL_UINT(2, rig.channels.activeCount());

  stored = rig.storedDevices();
  TEST_ASSERT_TRUE(
      ConfigApplier::findDevice(stored, "sim_01").isNull());
  TEST_ASSERT_EQUAL_INT(404, rig.get("/api/v1/devices/sim_01").status);
}

static void test_processing_chain_is_applied_and_persisted_over_http() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);

  const char* pipeline = R"({
    "stages": [
      { "type": "calibration",
        "config": { "type": "polynomial", "x_center": 0, "x_scale": 1,
                    "coefficients": [1.0, 2.0] } },
      { "type": "moving_average", "config": { "window": 2 } }
    ]
  })";

  const ApiResponse applied =
      rig.call(HttpMethod::kPut, "/api/v1/processing/ref_signal", "", pipeline);
  TEST_ASSERT_EQUAL_INT(200, applied.status);
  TEST_ASSERT_EQUAL_UINT(2, applied.body["active_stages"].size());
  TEST_ASSERT_EQUAL_STRING("calibration", applied.body["active_stages"][0]);

  // Running: 42 -> calibration 1 + 2*42 = 85
  rig.clock.advanceMicros(100000);
  rig.system.loop();
  const ChannelHandle handle = rig.channels.findByKey("ref_signal");
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 85.0f, rig.channels.value(handle)->calibrated);

  // Persisted, so it comes back after a reboot.
  JsonDocument stored;
  TEST_ASSERT_TRUE(rig.storage.load(ConfigSection::kProcessing, stored).ok());
  TEST_ASSERT_EQUAL_STRING("calibration",
                           stored["pipelines"]["ref_signal"]["stages"][0]["type"]);

  // A pipeline that cannot be built is rejected and NOT written to the file.
  const char* broken = R"({"stages":[{"type":"moving_average",
                                      "config":{"window":9999}}]})";
  const ApiResponse rejected =
      rig.call(HttpMethod::kPut, "/api/v1/processing/ref_signal", "", broken);
  TEST_ASSERT_TRUE(rejected.isError());
  TEST_ASSERT_EQUAL_UINT(2, rig.processing.stageCount(handle));

  JsonDocument unchanged;
  rig.storage.load(ConfigSection::kProcessing, unchanged);
  TEST_ASSERT_EQUAL_UINT(
      2, unchanged["pipelines"]["ref_signal"]["stages"].size());
}

static void test_configuration_export_and_import_round_trip() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);

  const ApiResponse exported = rig.get("/api/v1/config/export");
  TEST_ASSERT_EQUAL_INT(200, exported.status);
  TEST_ASSERT_EQUAL_INT(1, exported.body["schemaVersion"].as<int>());
  TEST_ASSERT_EQUAL_STRING(
      "sim_01", exported.body["sections"]["devices"]["devices"][0]["key"]);

  std::string payload;
  serializeJson(exported.body, payload);

  // Wipe the rig, then restore it from the export alone.
  TEST_ASSERT_EQUAL_INT(
      200, rig.call(HttpMethod::kDelete, "/api/v1/devices/sim_01").status);
  TEST_ASSERT_EQUAL_UINT(0, rig.devices.activeCount());

  const ApiResponse imported =
      rig.post("/api/v1/config/import", payload.c_str());
  TEST_ASSERT_EQUAL_INT(200, imported.status);
  TEST_ASSERT_EQUAL_INT(1, imported.body["devices_started"].as<int>());
  TEST_ASSERT_EQUAL_INT(0, imported.body["devices_failed"].as<int>());
  TEST_ASSERT_EQUAL_UINT(1, rig.devices.activeCount());
  TEST_ASSERT_TRUE(rig.channels.findByKey("ref_signal") != kInvalidChannel);

  // A configuration from newer firmware is refused, not guessed at.
  const ApiResponse future =
      rig.post("/api/v1/config/import", R"({"schemaVersion":999,"sections":{}})");
  TEST_ASSERT_EQUAL_STRING("CONFIG_SCHEMA_TOO_NEW", future.body["error"]["code"]);
}

// ===========================================================================
//  Telemetry
// ===========================================================================
static void test_telemetry_batches_many_updates_into_one_frame() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  const ChannelHandle handle = rig.channels.findByKey("ref_signal");

  TEST_ASSERT_TRUE(
      rig.telemetry.begin(rig.scheduler, rig.events, rig.sink, 5.0f).ok());
  const ChannelHandle subscription[] = {handle};
  TEST_ASSERT_TRUE(rig.telemetry.subscribe(subscription, 1).ok());
  rig.sink.frames.clear();

  // Twenty samples between two flushes must produce exactly one frame.
  for (int i = 0; i < 20; ++i) {
    rig.channels.publishRaw(handle, 40.0f + i, rig.clock.nowMicros());
    rig.clock.advanceMicros(1000);
  }
  rig.telemetry.flush();

  TEST_ASSERT_EQUAL_UINT(1, rig.sink.frames.size());
  TEST_ASSERT_EQUAL_UINT(1, rig.telemetry.framesSent());

  JsonDocument frame;
  TEST_ASSERT_FALSE(deserializeJson(frame, rig.sink.frames[0]));
  TEST_ASSERT_EQUAL_STRING("channels", frame["type"]);
  // Addressed by handle, so renaming a channel mid-experiment breaks nothing.
  char key[6];
  std::snprintf(key, sizeof(key), "%u", static_cast<unsigned>(handle));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 59.0f, frame["data"][key].as<float>());

  // Nothing new since the last flush -> no frame at all.
  rig.telemetry.flush();
  TEST_ASSERT_EQUAL_UINT(1, rig.sink.frames.size());
}

static void test_telemetry_sends_only_subscribed_channels() {
  ApiRig rig;
  ChannelDescriptor a, b;
  a.key.assign("wanted");
  b.key.assign("ignored");
  const ChannelHandle wanted = rig.channels.create(a).value();
  const ChannelHandle ignored = rig.channels.create(b).value();

  TEST_ASSERT_TRUE(
      rig.telemetry.begin(rig.scheduler, rig.events, rig.sink, 5.0f).ok());
  const ChannelHandle subscription[] = {wanted};
  rig.telemetry.subscribe(subscription, 1);
  rig.sink.frames.clear();

  rig.channels.publishRaw(ignored, 1.0f, rig.clock.nowMicros());
  rig.telemetry.flush();
  // An open page that asked for nothing costs the firmware no telemetry at all.
  TEST_ASSERT_EQUAL_UINT(0, rig.sink.frames.size());

  rig.channels.publishRaw(wanted, 2.0f, rig.clock.nowMicros());
  rig.telemetry.flush();
  TEST_ASSERT_EQUAL_UINT(1, rig.sink.frames.size());

  JsonDocument frame;
  deserializeJson(frame, rig.sink.frames[0]);
  TEST_ASSERT_EQUAL_UINT(1, frame["data"].size());
}

static void test_quality_is_sent_only_when_it_changes() {
  ApiRig rig;
  ChannelDescriptor descriptor;
  descriptor.key.assign("t");
  descriptor.minimum = 0.0f;
  descriptor.maximum = 100.0f;
  const ChannelHandle handle = rig.channels.create(descriptor).value();
  // Keyed by the handle the channel actually got, not by "1": the rig creates
  // system channels of its own, and a test that assumes it owns the first slot
  // is a test that breaks the next time the platform grows one.
  char key[8];
  std::snprintf(key, sizeof(key), "%u", static_cast<unsigned>(handle));

  rig.telemetry.begin(rig.scheduler, rig.events, rig.sink, 5.0f);
  const ChannelHandle subscription[] = {handle};
  rig.telemetry.subscribe(subscription, 1);
  rig.sink.frames.clear();

  rig.channels.publishRaw(handle, 20.0f, rig.clock.nowMicros());
  rig.telemetry.flush();
  JsonDocument first;
  deserializeJson(first, rig.sink.frames.back());
  TEST_ASSERT_EQUAL_STRING("GOOD", first["quality"][key]);  // first report

  rig.channels.publishRaw(handle, 21.0f, rig.clock.nowMicros());
  rig.telemetry.flush();
  JsonDocument second;
  deserializeJson(second, rig.sink.frames.back());
  // Unchanged quality costs nothing on the wire.
  TEST_ASSERT_TRUE(second["quality"].isNull());

  rig.channels.publishRaw(handle, 300.0f, rig.clock.nowMicros());
  rig.telemetry.flush();
  JsonDocument third;
  deserializeJson(third, rig.sink.frames.back());
  TEST_ASSERT_EQUAL_STRING("OUT_OF_RANGE", third["quality"][key]);
}

static void test_a_busy_socket_drops_the_frame_but_not_the_data() {
  ApiRig rig;
  ChannelDescriptor descriptor;
  descriptor.key.assign("t");
  const ChannelHandle handle = rig.channels.create(descriptor).value();

  rig.telemetry.begin(rig.scheduler, rig.events, rig.sink, 5.0f);
  const ChannelHandle subscription[] = {handle};
  rig.telemetry.subscribe(subscription, 1);
  rig.sink.frames.clear();
  char key[8];
  std::snprintf(key, sizeof(key), "%u", static_cast<unsigned>(handle));

  rig.sink.busy = true;
  rig.channels.publishRaw(handle, 10.0f, rig.clock.nowMicros());
  rig.telemetry.flush();
  TEST_ASSERT_EQUAL_UINT(0, rig.sink.frames.size());
  TEST_ASSERT_EQUAL_UINT(1, rig.telemetry.framesDropped());

  // The channel is still marked dirty, so the next frame carries the newest
  // value — nothing anybody wanted was lost.
  rig.sink.busy = false;
  rig.channels.publishRaw(handle, 11.0f, rig.clock.nowMicros());
  rig.telemetry.flush();
  TEST_ASSERT_EQUAL_UINT(1, rig.sink.frames.size());

  JsonDocument frame;
  deserializeJson(frame, rig.sink.frames[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 11.0f, frame["data"][key].as<float>());
}

static void test_device_errors_reach_the_socket_as_events() {
  ApiRig rig;
  rig.telemetry.begin(rig.scheduler, rig.events, rig.sink, 5.0f);
  rig.sink.frames.clear();

  Event event;
  event.type = EventType::kDeviceError;
  event.source = 7;
  event.code = ErrorCode::kDeviceNotResponding;
  event.detail = "ERROR";
  rig.events.publish(event);

  TEST_ASSERT_EQUAL_UINT(1, rig.sink.frames.size());
  JsonDocument message;
  deserializeJson(message, rig.sink.frames[0]);
  TEST_ASSERT_EQUAL_STRING("device", message["type"]);
  TEST_ASSERT_EQUAL_INT(7, message["handle"].as<int>());
  TEST_ASSERT_EQUAL_STRING("DEVICE_NOT_RESPONDING", message["code"]);
}


// ===========================================================================
//  Calibration (Milestone 5)
// ===========================================================================
namespace {

// A device whose channel starts out in raw ADC counts, exactly like an HX711
// before anybody has put a weight on it.
const char* kCountsSource = R"({
  "key": "balance_01",
  "module": "sim_signal",
  "name": "Sample balance",
  "sample_interval_us": 100000,
  "config": { "waveform": "constant", "amplitude": 0, "offset": 453211,
              "period_s": 10, "noise": 0 },
  "channels": { "value": { "key": "mass_01", "unit": "counts" } }
})";

// Three weights on a load cell.  The steps are 45111 and 45097 counts: real
// enough to leave a residual, which is the point — a fit that comes back with
// R² exactly 1 usually means the points were invented.
const char* kThreeWeights = R"({
  "channel": "mass_01", "kind": "linear", "unit": "g", "precision": 2,
  "note": "three weights, 21 degC",
  "points": [ { "raw": 453211, "reference": 0 },
              { "raw": 498322, "reference": 100 },
              { "raw": 543419, "reference": 200 } ]
})";

}  // namespace

static void test_solve_reports_the_fit_without_storing_it() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kCountsSource).status);

  const ApiResponse solved =
      rig.post("/api/v1/calibrations/solve", kThreeWeights);
  TEST_ASSERT_EQUAL_INT(200, solved.status);
  TEST_ASSERT_FALSE(solved.body["stored"].as<bool>());

  // The summary an operator judges the calibration by.
  TEST_ASSERT_TRUE(solved.body["fit"]["r_squared"].as<double>() > 0.9999);
  TEST_ASSERT_TRUE(solved.body["fit"]["rms_residual"].as<double>() < 0.1);

  // And the per-point residuals, which are what actually say WHERE it is bad.
  JsonArrayConst residuals = solved.body["residuals"].as<JsonArrayConst>();
  TEST_ASSERT_EQUAL_UINT(3, residuals.size());
  for (JsonObjectConst point : residuals) {
    TEST_ASSERT_TRUE(std::fabs(point["residual"].as<double>()) < 0.2);
  }

  // Nothing was written: a preview that saves is not a preview.  The file is
  // there — the first boot creates it empty — so the question is whether it
  // gained a record.
  JsonDocument stored;
  TEST_ASSERT_TRUE(
      rig.storage.load(ConfigSection::kCalibrations, stored).ok());
  TEST_ASSERT_EQUAL_UINT(0, stored["calibrations"].size());
  TEST_ASSERT_EQUAL_UINT(0, rig.calibrations.count());
}

static void test_a_fit_that_cannot_be_made_says_so() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kCountsSource).status);

  // Two points cannot determine a quadratic.
  const ApiResponse thin = rig.post("/api/v1/calibrations/solve", R"({
    "channel": "mass_01", "kind": "poly2",
    "points": [ { "raw": 1, "reference": 1 }, { "raw": 2, "reference": 2 } ] })");
  TEST_ASSERT_EQUAL_STRING("CALIBRATION_INSUFFICIENT_POINTS",
                           thin.body["error"]["code"]);
  TEST_ASSERT_EQUAL_STRING("points", thin.body["error"]["field"]);

  // The same raw value twice: the operator forgot to change the weight.
  const ApiResponse degenerate = rig.post("/api/v1/calibrations/solve", R"({
    "channel": "mass_01", "kind": "linear",
    "points": [ { "raw": 500, "reference": 0 },
                { "raw": 500, "reference": 100 } ] })");
  TEST_ASSERT_EQUAL_STRING("CALIBRATION_SINGULAR",
                           degenerate.body["error"]["code"]);

  // NaN would sail straight through the normal equations and come back as a
  // fit of NaNs that looks successful.
  const ApiResponse notANumber = rig.post("/api/v1/calibrations/solve", R"({
    "channel": "mass_01", "kind": "linear",
    "points": [ { "raw": 500, "reference": 0 },
                { "raw": 600, "reference": null } ] })");
  TEST_ASSERT_TRUE(notANumber.isError());
}

static void test_history_is_kept_and_a_rollback_is_one_call() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kCountsSource).status);

  const ApiResponse first = rig.post("/api/v1/calibrations", kThreeWeights);
  TEST_ASSERT_EQUAL_INT(201, first.status);
  TEST_ASSERT_EQUAL_STRING("mass_01#1", first.body["id"]);

  // Recalibrating APPENDS.  It must never edit version 1: a dataset recorded
  // yesterday was produced by those numbers and has to stay traceable to them.
  const ApiResponse second = rig.post("/api/v1/calibrations", R"({
    "channel": "mass_01", "kind": "linear", "unit": "g",
    "points": [ { "raw": 453211, "reference": 0 },
                { "raw": 498322, "reference": 101 } ] })");
  TEST_ASSERT_EQUAL_INT(201, second.status);
  TEST_ASSERT_EQUAL_STRING("mass_01#2", second.body["id"]);

  const ApiResponse history = rig.get("/api/v1/calibrations", "channel=mass_01");
  JsonArrayConst records = history.body["calibrations"].as<JsonArrayConst>();
  TEST_ASSERT_EQUAL_UINT(2, records.size());
  TEST_ASSERT_FALSE(records[0]["active"].as<bool>());
  TEST_ASSERT_TRUE(records[1]["active"].as<bool>());
  // The original points are still there, not just the coefficients.
  TEST_ASSERT_EQUAL_UINT(3, records[0]["points"].as<JsonArrayConst>().size());

  const ChannelHandle handle = rig.channels.findByKey("mass_01");
  rig.channels.publishRaw(handle, 498322.0f, 1000);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 101.0f, rig.channels.value(handle)->processed);

  // Roll back.  One call, no editing of the pipeline, no arithmetic.
  TEST_ASSERT_EQUAL_INT(
      200, rig.post("/api/v1/calibrations/mass_01%231/activate", "{}").status);
  TEST_ASSERT_EQUAL_STRING("mass_01#1",
                           rig.calibrations.activeFor("mass_01")->id.c_str());

  rig.channels.publishRaw(handle, 498322.0f, 2000);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 100.0f, rig.channels.value(handle)->processed);

  // Deleting the running calibration would silently change every reading.
  const ApiResponse refused =
      rig.call(HttpMethod::kDelete, "/api/v1/calibrations/mass_01%231");
  TEST_ASSERT_EQUAL_INT(409, refused.status);

  TEST_ASSERT_EQUAL_INT(
      200, rig.post("/api/v1/calibrations/mass_01%231/deactivate", "{}").status);
  TEST_ASSERT_EQUAL_INT(
      200, rig.call(HttpMethod::kDelete, "/api/v1/calibrations/mass_01%231").status);

  // Version numbers never restart: an id that has identified a dataset must not
  // come back meaning something else.
  const ApiResponse third = rig.post("/api/v1/calibrations", kThreeWeights);
  TEST_ASSERT_EQUAL_STRING("mass_01#3", third.body["id"]);
}

static void test_an_uncalibrated_channel_reports_raw_as_calibrated() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kCountsSource).status);
  const ChannelHandle handle = rig.channels.findByKey("mass_01");

  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/calibrations", kThreeWeights).status);
  TEST_ASSERT_EQUAL_INT(
      200, rig.post("/api/v1/calibrations/mass_01%231/deactivate", "{}").status);

  // The placeholder stage is still in processing.json, but with nothing active
  // it must pass the value through — calibrated == raw is exactly what "not
  // calibrated" means, and it is visible as such on the Channels page.
  rig.channels.publishRaw(handle, 498322.0f, 1000);
  const ChannelValue* value = rig.channels.value(handle);
  TEST_ASSERT_EQUAL_FLOAT(498322.0f, value->raw);
  TEST_ASSERT_EQUAL_FLOAT(498322.0f, value->calibrated);
  TEST_ASSERT_NULL(rig.calibrations.activeFor("mass_01"));

  // And the unit goes back to what the channel was declared with.  Leaving it
  // at "g" would put raw ADC counts under a unit that makes them look like a
  // real mass — worse than never having calibrated it at all.
  TEST_ASSERT_EQUAL_STRING("counts",
                           rig.channels.descriptor(handle)->unit.c_str());
}

static void test_editing_the_pipeline_does_not_drop_the_calibration() {
  // The stored calibration stage is a placeholder, and PUT /processing sends
  // it straight back.  If the coefficients are not resolved on the way in, the
  // stage is configured as an identity and the channel silently returns to raw
  // counts — while its unit still says grams.
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kCountsSource).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/calibrations", kThreeWeights).status);

  const ApiResponse stored = rig.get("/api/v1/processing/mass_01");
  TEST_ASSERT_EQUAL_STRING("active",
                           stored.body["stages"][0]["config"]["source"]);

  // Round-trip exactly what the editor round-trips: read it, add a stage, save.
  const ApiResponse saved = rig.call(HttpMethod::kPut, "/api/v1/processing/mass_01", "",
      R"({"stages":[{"type":"calibration","config":{"source":"active"}},
                    {"type":"moving_average","config":{"window":2}}]})");
  TEST_ASSERT_EQUAL_INT(200, saved.status);

  const ChannelHandle handle = rig.channels.findByKey("mass_01");
  rig.channels.publishRaw(handle, 498322.0f, 1000);
  rig.channels.publishRaw(handle, 498322.0f, 2000);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 100.0f, rig.channels.value(handle)->processed);

  // And the file still holds the placeholder, not a second copy of the numbers.
  JsonDocument document;
  TEST_ASSERT_TRUE(rig.storage.load(ConfigSection::kProcessing, document).ok());
  TEST_ASSERT_EQUAL_STRING(
      "active", document["pipelines"]["mass_01"]["stages"][0]["config"]["source"]);
}

static void test_calibration_survives_a_reboot() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kCountsSource).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/calibrations", kThreeWeights).status);

  // Everything comes back from the two files and nothing else.
  rig.system.reloadConfiguration();

  const ChannelHandle handle = rig.channels.findByKey("mass_01");
  TEST_ASSERT_TRUE(handle != kInvalidChannel);
  TEST_ASSERT_EQUAL_STRING("g", rig.channels.descriptor(handle)->unit.c_str());
  TEST_ASSERT_EQUAL_STRING("mass_01#1",
                           rig.calibrations.activeFor("mass_01")->id.c_str());

  rig.channels.publishRaw(handle, 543419.0f, 1000);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 200.0f, rig.channels.value(handle)->processed);
}

// ===========================================================================
//  Milestone 5 acceptance criterion
// ===========================================================================
static void test_a_load_cell_is_calibrated_from_three_weights_in_the_browser() {
  ApiRig rig;

  // --- the rig as it comes out of the box ---------------------------------
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kCountsSource).status);
  const ChannelHandle handle = rig.channels.findByKey("mass_01");
  rig.channels.publishRaw(handle, 453211.0f, 1000);

  TEST_ASSERT_EQUAL_STRING("counts", rig.channels.descriptor(handle)->unit.c_str());
  TEST_ASSERT_EQUAL_FLOAT(453211.0f, rig.channels.value(handle)->processed);

  // --- the operator previews the fit before committing to it --------------
  const ApiResponse preview =
      rig.post("/api/v1/calibrations/solve", kThreeWeights);
  TEST_ASSERT_EQUAL_INT(200, preview.status);

  // --- and saves it -------------------------------------------------------
  const ApiResponse saved = rig.post("/api/v1/calibrations", kThreeWeights);
  TEST_ASSERT_EQUAL_INT(201, saved.status);
  TEST_ASSERT_TRUE(saved.body["active"].as<bool>());

  // The channel is now a mass channel: unit, precision and pipeline all moved
  // together.  A channel reading grams while its descriptor says "counts" is
  // wrong on screen, wrong in the log and wrong in the range check.
  TEST_ASSERT_EQUAL_STRING("g", rig.channels.descriptor(handle)->unit.c_str());
  TEST_ASSERT_EQUAL_UINT(2, rig.channels.descriptor(handle)->precision);

  // --- and the channel reads grams ----------------------------------------
  rig.channels.publishRaw(handle, 453211.0f, 2000);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 0.0f, rig.channels.value(handle)->processed);

  rig.channels.publishRaw(handle, 498322.0f, 3000);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 100.0f, rig.channels.value(handle)->processed);

  rig.channels.publishRaw(handle, 543419.0f, 4000);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 200.0f, rig.channels.value(handle)->processed);

  // A weight that was never on the scale, interpolated by the fit.
  rig.channels.publishRaw(handle, 475766.0f, 5000);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 50.0f, rig.channels.value(handle)->processed);

  // Raw is still raw: the reprocessing of a stored dataset with corrected
  // coefficients depends on it (§48).
  TEST_ASSERT_EQUAL_FLOAT(475766.0f, rig.channels.value(handle)->raw);

  // And it all survives the power going off.
  rig.system.reloadConfiguration();
  const ChannelHandle after = rig.channels.findByKey("mass_01");
  rig.channels.publishRaw(after, 498322.0f, 6000);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 100.0f, rig.channels.value(after)->processed);
}


// ===========================================================================
//  Dashboards (Milestone 6)
// ===========================================================================
namespace {

const char* kSmallDashboard = R"({
  "key": "evaporation", "name": "Evaporation",
  "grid": { "columns": 12, "row_height": 40 },
  "widgets": [
    { "id": "w1", "type": "value", "x": 0, "y": 0, "w": 3, "h": 1,
      "config": { "channel": "ref_signal", "precision": 3 } },
    { "id": "w2", "type": "chart", "x": 0, "y": 1, "w": 8, "h": 4,
      "config": { "window_s": 900,
                  "series": [ { "channel": "ref_signal", "axis": "left" } ] } }
  ]
})";

}  // namespace

static void test_a_dashboard_is_stored_and_read_back_whole() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);

  const ApiResponse created = rig.post("/api/v1/dashboards", kSmallDashboard);
  TEST_ASSERT_EQUAL_INT(201, created.status);
  TEST_ASSERT_EQUAL_STRING("evaporation", created.body["key"]);
  TEST_ASSERT_EQUAL_INT(2, created.body["health"]["widgets"].as<int>());
  TEST_ASSERT_EQUAL_INT(0, created.body["health"]["dangling_channels"].as<int>());

  // The list is a summary: eight dashboards of twenty-four widgets do not fit
  // in one response, and the picker needs names, not layouts.
  const ApiResponse list = rig.get("/api/v1/dashboards");
  TEST_ASSERT_EQUAL_UINT(1, list.body["dashboards"].as<JsonArrayConst>().size());
  TEST_ASSERT_EQUAL_STRING("Evaporation", list.body["dashboards"][0]["name"]);
  TEST_ASSERT_EQUAL_INT(2, list.body["dashboards"][0]["widgets"].as<int>());
  TEST_ASSERT_TRUE(list.body["dashboards"][0]["grid"].isNull());
  TEST_ASSERT_EQUAL_INT(12, list.body["limits"]["columns"].as<int>());

  // ...and the individual dashboard comes back exactly as it went in.
  const ApiResponse one = rig.get("/api/v1/dashboards/evaporation");
  TEST_ASSERT_EQUAL_INT(200, one.status);
  TEST_ASSERT_EQUAL_INT(40, one.body["grid"]["row_height"].as<int>());
  TEST_ASSERT_EQUAL_STRING("w2", one.body["widgets"][1]["id"]);
  TEST_ASSERT_EQUAL_INT(900, one.body["widgets"][1]["config"]["window_s"].as<int>());

  TEST_ASSERT_EQUAL_INT(404, rig.get("/api/v1/dashboards/nothing").status);
}

static void test_the_firmware_checks_the_shape_and_not_the_widget() {
  ApiRig rig;

  // A widget type this build has never heard of is stored without complaint:
  // the widget vocabulary belongs to the web interface (ADR-0015).
  const ApiResponse exotic = rig.post("/api/v1/dashboards", R"({
    "key": "d1", "name": "Exotic",
    "widgets": [ { "id": "w1", "type": "sankey_of_the_future",
                   "x": 0, "y": 0, "w": 4, "h": 2, "config": { "whatever": 7 } } ] })");
  TEST_ASSERT_EQUAL_INT(201, exotic.status);

  // The shape, on the other hand, is the firmware's business.
  const ApiResponse offGrid = rig.post("/api/v1/dashboards", R"({
    "key": "d2", "name": "Off grid",
    "widgets": [ { "id": "w1", "type": "value", "x": 10, "y": 0, "w": 4, "h": 1 } ] })");
  TEST_ASSERT_EQUAL_INT(422, offGrid.status);
  TEST_ASSERT_EQUAL_STRING("widgets", offGrid.body["error"]["field"]);

  const ApiResponse duplicated = rig.post("/api/v1/dashboards", R"({
    "key": "d3", "name": "Twins",
    "widgets": [ { "id": "w1", "type": "value", "x": 0, "y": 0, "w": 1, "h": 1 },
                 { "id": "w1", "type": "value", "x": 1, "y": 0, "w": 1, "h": 1 } ] })");
  TEST_ASSERT_EQUAL_STRING("ALREADY_EXISTS", duplicated.body["error"]["code"]);

  const ApiResponse anonymous = rig.post("/api/v1/dashboards", R"({
    "name": "No key", "widgets": [] })");
  TEST_ASSERT_EQUAL_STRING("key", anonymous.body["error"]["field"]);

  // An empty dashboard is a legal dashboard — that is what a new one is.
  TEST_ASSERT_EQUAL_INT(
      201, rig.post("/api/v1/dashboards", R"({"key":"d4","name":"Empty"})").status);
}

static void test_a_widget_pointing_at_a_deleted_channel_is_named_not_dropped() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/dashboards", kSmallDashboard).status);

  // The sensor is unplugged and removed.  The dashboard must survive: deleting
  // somebody's layout because a wire fell out would be unforgivable.
  TEST_ASSERT_EQUAL_INT(
      200, rig.call(HttpMethod::kDelete, "/api/v1/devices/sim_01").status);

  const ApiResponse one = rig.get("/api/v1/dashboards/evaporation");
  TEST_ASSERT_EQUAL_INT(200, one.status);
  TEST_ASSERT_EQUAL_UINT(2, one.body["widgets"].as<JsonArrayConst>().size());

  // Both the tile and the chart series count, and the answer NAMES one.
  TEST_ASSERT_EQUAL_INT(2, one.body["health"]["dangling_channels"].as<int>());
  TEST_ASSERT_EQUAL_STRING("w1", one.body["health"]["first_dangling"]["widget"]);
  TEST_ASSERT_EQUAL_STRING("ref_signal", one.body["health"]["first_dangling"]["channel"]);

  // And the picker says so before the dashboard is even opened.
  const ApiResponse list = rig.get("/api/v1/dashboards");
  TEST_ASSERT_EQUAL_INT(2, list.body["dashboards"][0]["dangling_channels"].as<int>());
}

static void test_dashboards_are_bounded_because_the_partition_is() {
  ApiRig rig;
  char body[128];
  for (int i = 0; i < 8; ++i) {
    std::snprintf(body, sizeof(body), R"({"key":"d%d","name":"D%d"})", i, i);
    TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/dashboards", body).status);
  }
  const ApiResponse ninth =
      rig.post("/api/v1/dashboards", R"({"key":"d8","name":"Too many"})");
  TEST_ASSERT_EQUAL_STRING("OUT_OF_CAPACITY", ninth.body["error"]["code"]);

  // Replacing an existing one is not "another one".
  const ApiResponse put = rig.call(HttpMethod::kPut, "/api/v1/dashboards/d0", "",
                                   R"({"key":"d0","name":"Renamed"})");
  TEST_ASSERT_EQUAL_INT(200, put.status);
  // The response is held in a local: `rig.get(...).body["name"]` hands out a
  // pointer into a temporary JsonDocument.
  const ApiResponse renamed = rig.get("/api/v1/dashboards/d0");
  TEST_ASSERT_EQUAL_STRING("Renamed", renamed.body["name"]);
}

// ===========================================================================
//  Milestone 6 acceptance criterion
// ===========================================================================
static void test_a_dashboard_is_built_saved_and_survives_a_reboot() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);

  // --- built in the browser ------------------------------------------------
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/dashboards", kSmallDashboard).status);

  // --- and dragged about ---------------------------------------------------
  // The editor saves the whole layout; this is what one debounced write looks
  // like after a widget was moved and resized.
  const ApiResponse moved = rig.call(HttpMethod::kPut,
      "/api/v1/dashboards/evaporation", "", R"({
    "key": "evaporation", "name": "Evaporation",
    "grid": { "columns": 12, "row_height": 40 },
    "widgets": [
      { "id": "w1", "type": "value", "x": 9, "y": 0, "w": 3, "h": 2,
        "config": { "channel": "ref_signal", "precision": 3 } },
      { "id": "w2", "type": "chart", "x": 0, "y": 0, "w": 9, "h": 4,
        "config": { "window_s": 900,
                    "series": [ { "channel": "ref_signal", "axis": "left" } ] } } ] })");
  TEST_ASSERT_EQUAL_INT(200, moved.status);

  // --- the power goes off --------------------------------------------------
  rig.system.reloadConfiguration();

  const ApiResponse after = rig.get("/api/v1/dashboards/evaporation");
  TEST_ASSERT_EQUAL_INT(200, after.status);
  TEST_ASSERT_EQUAL_INT(9, after.body["widgets"][0]["x"].as<int>());
  TEST_ASSERT_EQUAL_INT(2, after.body["widgets"][0]["h"].as<int>());
  TEST_ASSERT_EQUAL_INT(9, after.body["widgets"][1]["w"].as<int>());
  TEST_ASSERT_EQUAL_INT(0, after.body["health"]["dangling_channels"].as<int>());

  // A dashboard is configuration, so it travels with the rig.
  const ApiResponse exported = rig.get("/api/v1/config/export");
  TEST_ASSERT_EQUAL_STRING(
      "evaporation",
      exported.body["sections"]["dashboards"]["dashboards"][0]["key"]);
}


// ===========================================================================
//  Outputs and the safety layer (Milestone 7)
// ===========================================================================
namespace {

// A relay on a pin, with a fast contact-protection interval so the test does
// not have to wait a second.
const char* kRelayBody = R"({
  "key": "pump_01", "module": "relay", "name": "Circulation pump",
  "config": { "pin": 18, "safe_level": "low", "invert": false,
              "min_switch_s": 0.2, "hold_s": 5 },
  "channels": { "state": { "key": "pump" } }
})";

// A heater with a hard power limit and a short deadline.
const char* kHeaterBody = R"({
  "key": "heat_01", "module": "heater", "name": "Bath heater",
  "config": { "pin": 19, "frequency": 10, "max_duty": 60, "hold_s": 30 },
  "channels": { "power": { "key": "heat" } }
})";

}  // namespace

static void test_an_output_comes_up_in_its_safe_state_and_not_before() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kHeaterBody).status);

  const ChannelHandle handle = rig.channels.findByKey("heat");
  TEST_ASSERT_TRUE(handle != kInvalidChannel);

  // Registered with the safety layer by DeviceManager, not by the driver: a
  // driver cannot forget to do something that was never its job (ADR-0016).
  const OutputRecord* record = rig.outputs.find(handle);
  TEST_ASSERT_NOT_NULL(record);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, record->safeValue);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OutputHoldState::kSafe),
                        static_cast<int>(record->state));

  // A heater's safe state is fixed at zero and cannot be configured away.  The
  // manifest simply does not offer the parameter, so the attempt is refused by
  // the ordinary unknown-key check and names the field.
  const ApiResponse tryToArm = rig.post("/api/v1/devices", R"({
    "key": "heat_02", "module": "heater", "name": "Second heater",
    "config": { "pin": 21, "frequency": 10, "max_duty": 100, "hold_s": 30,
                "safe_value": 40 },
    "channels": { "power": { "key": "heat2" } } })");
  TEST_ASSERT_EQUAL_INT(422, tryToArm.status);
  TEST_ASSERT_EQUAL_STRING("safe_value", tryToArm.body["error"]["field"]);

  // A fan, on the other hand, may legitimately have a non-zero safe state: on
  // a hot enclosure the safe thing for the fan to do is keep running.
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", R"({
    "key": "fan_01", "module": "fan", "name": "Enclosure fan",
    "config": { "pin": 21, "frequency": 25000, "min_duty": 20,
                "kickstart_ms": 0, "safe_value": 30, "hold_s": 0 },
    "channels": { "speed": { "key": "fan" } } })").status);
  TEST_ASSERT_EQUAL_FLOAT(
      30.0f, rig.outputs.find(rig.channels.findByKey("fan"))->safeValue);
  // ...and it is already running at it, before anything commanded anything.
  TEST_ASSERT_EQUAL_FLOAT(
      30.0f, rig.channels.value(rig.channels.findByKey("fan"))->processed);
}

static void test_a_command_expires_and_the_output_lets_go() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kHeaterBody).status);
  const ChannelHandle handle = rig.channels.findByKey("heat");

  const ApiResponse written = rig.post("/api/v1/channels/heat/write",
                                       R"({"value": 50})");
  TEST_ASSERT_EQUAL_INT(200, written.status);
  TEST_ASSERT_EQUAL_STRING("COMMANDED", written.body["output"]["state"]);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 30.0f,
                           written.body["output"]["expires_in_s"].as<float>());
  TEST_ASSERT_EQUAL_FLOAT(50.0f, rig.channels.value(handle)->processed);

  // 29 seconds later it is still on: the deadline is a deadline, not a nag.
  rig.clock.advanceMicros(29000000);
  rig.outputs.tick(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OutputHoldState::kCommanded),
                        static_cast<int>(rig.outputs.find(handle)->state));

  // Renewing is what a controller — or a browser that is still open — does.
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/outputs/heat/renew", "{}").status);
  rig.clock.advanceMicros(29000000);
  rig.outputs.tick(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OutputHoldState::kCommanded),
                        static_cast<int>(rig.outputs.find(handle)->state));

  // Nobody renews it.  This is the browser being closed, the tablet leaving
  // Wi-Fi range, the operator going to lunch.
  rig.clock.advanceMicros(31000000);
  rig.outputs.tick(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OutputHoldState::kExpired),
                        static_cast<int>(rig.outputs.find(handle)->state));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rig.channels.value(handle)->processed);
  TEST_ASSERT_EQUAL_UINT(1, rig.outputs.expiries());

  // An expired command cannot be renewed back into life: switching the heater
  // on again is a decision, and somebody has to make it.
  TEST_ASSERT_TRUE(rig.post("/api/v1/outputs/heat/renew", "{}").isError());
}

static void test_a_heater_cannot_be_driven_past_its_power_limit() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kHeaterBody).status);
  const ChannelHandle handle = rig.channels.findByKey("heat");

  // Limited, not refused: refusing would leave the heater whereever it was,
  // which is the opposite of what a power limit is for.
  const ApiResponse limited =
      rig.post("/api/v1/channels/heat/write", R"({"value": 100})");
  TEST_ASSERT_EQUAL_INT(200, limited.status);
  TEST_ASSERT_EQUAL_FLOAT(60.0f, rig.channels.value(handle)->processed);
  // The record reports what was ASKED and what was DONE, separately.  A limit
  // that shows up as "you asked for 100 and got 100" is not a limit anybody
  // can see is working.
  TEST_ASSERT_EQUAL_FLOAT(100.0f, limited.body["output"]["commanded"].as<float>());
  TEST_ASSERT_EQUAL_FLOAT(60.0f, limited.body["output"]["applied"].as<float>());

  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/heat/write",
                                      R"({"value": 20})").status);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, rig.channels.value(handle)->processed);

  // Not a number never reaches the actuator.
  TEST_ASSERT_TRUE(rig.post("/api/v1/channels/heat/write",
                            R"({"value": 1e40})").isError() ||
                   rig.channels.value(handle)->processed <= 60.0f);
}

static void test_a_relay_refuses_to_be_switched_faster_than_it_can_survive() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kRelayBody).status);

  // The interval runs from the drive to the safe state at boot, so even the
  // first command is subject to it — which is correct, and worth stating.
  TEST_ASSERT_TRUE(
      rig.post("/api/v1/channels/pump/write", R"({"value": 1})").isError());

  rig.clock.advanceMicros(300000);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/pump/write",
                                      R"({"value": 1})").status);

  // Switching OFF is switching to the safe level and is always allowed —
  // see test_contact_protection_can_never_block_a_release_to_safe.
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/pump/write",
                                      R"({"value": 0})").status);

  // Switching back ON immediately is what wears the contacts out: 10^5
  // operations at this rate is a day and a half, and the failure mode is a
  // contact welded closed with the load on.
  const ApiResponse tooSoon =
      rig.post("/api/v1/channels/pump/write", R"({"value": 1})");
  TEST_ASSERT_TRUE(tooSoon.isError());
  TEST_ASSERT_EQUAL_STRING("RESOURCE_BUSY", tooSoon.body["error"]["code"]);

  // Refused OUT LOUD: the relay is still open, and the caller was told so.  A
  // command that appeared to work and did not is worse than one rejected.
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("pump"))->processed);

  rig.clock.advanceMicros(300000);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/pump/write",
                                      R"({"value": 1})").status);
}

static void test_contact_protection_can_never_block_a_release_to_safe() {
  // §49: Safety outranks Reliability.  Contact life is Reliability; letting go
  // of an output is Safety, and the second must not be refusable by the first.
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kRelayBody).status);
  rig.clock.advanceMicros(300000);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/pump/write",
                                      R"({"value": 1})").status);

  // No waiting: the pump was switched a microsecond ago.
  rig.outputs.trip("test");
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("pump"))->processed);

  // Leaving the safe level is still rate-limited, so chattering stays bounded.
  rig.outputs.clearTrip();
  TEST_ASSERT_TRUE(
      rig.post("/api/v1/channels/pump/write", R"({"value": 1})").isError());
}

static void test_a_fan_is_never_commanded_to_a_speed_it_cannot_turn_at() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", R"({
    "key": "fan_01", "module": "fan", "name": "Enclosure fan",
    "config": { "pin": 21, "frequency": 25000, "min_duty": 20,
                "kickstart_ms": 400, "safe_value": 0, "hold_s": 0 },
    "channels": { "speed": { "key": "fan" } } })").status);
  const ChannelHandle handle = rig.channels.findByKey("fan");

  // Starting from rest: full power briefly, to break stiction.  The channel
  // reports 100 because that is what the fan is actually being given.
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/fan/write",
                                      R"({"value": 35})").status);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, rig.channels.value(handle)->processed);

  rig.clock.advanceMicros(500000);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/fan/write",
                                      R"({"value": 35})").status);
  TEST_ASSERT_EQUAL_FLOAT(35.0f, rig.channels.value(handle)->processed);

  // 5 % is below the stall point: a fan given 5 % draws current, moves no air,
  // and looks exactly like a fan that is running.  It is raised, and the
  // channel says so rather than reporting the 5 that was asked for.
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/fan/write",
                                      R"({"value": 5})").status);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, rig.channels.value(handle)->processed);

  // Zero is zero: "off" is always reachable.
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/fan/write",
                                      R"({"value": 0})").status);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rig.channels.value(handle)->processed);
}

static void test_a_device_that_stops_running_lets_go_of_its_output() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kHeaterBody).status);
  const ChannelHandle handle = rig.channels.findByKey("heat");
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/heat/write",
                                      R"({"value": 40})").status);

  // Disabling the device is the same path a driver fault takes.
  TEST_ASSERT_EQUAL_INT(
      200, rig.post("/api/v1/devices/heat_01/actions/disable", "{}").status);

  const OutputRecord* record = rig.outputs.find(handle);
  TEST_ASSERT_NOT_NULL(record);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OutputHoldState::kDeviceFault),
                        static_cast<int>(record->state));

  // And it must not be commandable while its device is not running: a value
  // accepted by an API and applied to nothing is the worst of both.
  TEST_ASSERT_TRUE(
      rig.post("/api/v1/channels/heat/write", R"({"value": 30})").isError());
}

static void test_the_master_stop_drops_everything_and_stays_dropped() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kHeaterBody).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kRelayBody).status);
  rig.clock.advanceMicros(300000);  // past the relay's contact interval
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/heat/write",
                                      R"({"value": 50})").status);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/pump/write",
                                      R"({"value": 1})").status);

  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/outputs/trip", "{}").status);

  TEST_ASSERT_TRUE(rig.outputs.tripped());
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("pump"))->processed);

  // Nothing can be commanded while tripped, and the refusal names the reason.
  const ApiResponse refused =
      rig.post("/api/v1/channels/heat/write", R"({"value": 20})");
  TEST_ASSERT_EQUAL_STRING("SAFETY_INTERLOCK", refused.body["error"]["code"]);

  // Clearing the trip permits commands again; it does NOT resume them.  The
  // operator decides what comes back on, one output at a time.
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/outputs/clear", "{}").status);
  TEST_ASSERT_FALSE(rig.outputs.tripped());
  rig.clock.advanceMicros(300000);
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OutputHoldState::kSafe),
                        static_cast<int>(
                            rig.outputs.find(rig.channels.findByKey("heat"))->state));
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/heat/write",
                                      R"({"value": 20})").status);
}

static void test_a_channel_that_is_not_a_registered_output_cannot_be_commanded() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  // ref_signal is an input.  Writing to it is not "unsupported", it is refused.
  TEST_ASSERT_TRUE(
      rig.post("/api/v1/channels/ref_signal/write", R"({"value": 1})").isError());
}

// ===========================================================================
//  Milestone 7 acceptance criterion
// ===========================================================================
static void test_a_heater_is_added_switched_on_and_never_left_on() {
  ApiRig rig;

  // --- added in the browser, with a power limit ---------------------------
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kHeaterBody).status);
  const ChannelHandle handle = rig.channels.findByKey("heat");

  // It exists, and it is OFF.  Nothing had to be done to make that true.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rig.channels.value(handle)->processed);

  // --- switched on --------------------------------------------------------
  const ApiResponse on =
      rig.post("/api/v1/channels/heat/write", R"({"value": 45})");
  TEST_ASSERT_EQUAL_INT(200, on.status);
  TEST_ASSERT_EQUAL_FLOAT(45.0f, rig.channels.value(handle)->processed);

  // The actuator is a channel like any other: it charts, it logs, it has a
  // quality flag.  An output nobody can see is an output nobody notices stuck.
  const ApiResponse listed = rig.get("/api/v1/channels", "values=1");
  bool found = false;
  for (JsonObjectConst channel : listed.body["channels"].as<JsonArrayConst>()) {
    if (std::strcmp(channel["key"] | "", "heat") != 0) continue;
    found = true;
    TEST_ASSERT_EQUAL_STRING("output", channel["direction"]);
    TEST_ASSERT_EQUAL_STRING("COMMANDED", channel["output"]["state"]);
    TEST_ASSERT_EQUAL_FLOAT(45.0f, channel["value"]["processed"].as<float>());
  }
  TEST_ASSERT_TRUE(found);

  // --- and the browser goes away ------------------------------------------
  rig.clock.advanceMicros(31000000);
  rig.outputs.tick(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rig.channels.value(handle)->processed);
  const ApiResponse expired = rig.get("/api/v1/channels/heat");
  TEST_ASSERT_EQUAL_STRING("EXPIRED", expired.body["output"]["state"]);

  // --- and the power goes off ---------------------------------------------
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/heat/write",
                                      R"({"value": 55})").status);
  rig.system.reloadConfiguration();

  const ChannelHandle after = rig.channels.findByKey("heat");
  TEST_ASSERT_TRUE(after != kInvalidChannel);
  // The configuration came back.  The heater did not.  There is no option to
  // restore the last commanded value, and that is the entire point (ADR-0016).
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rig.channels.value(after)->processed);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OutputHoldState::kSafe),
                        static_cast<int>(rig.outputs.find(after)->state));
}

// ===========================================================================
//  Control: loops, rules and limits (Milestone 8)
// ===========================================================================
namespace {

// A heater whose command expires quickly, so a test can prove that a running
// loop keeps its own command alive rather than relying on a generous deadline.
const char* kShortHoldHeater = R"({
  "key": "heat_01", "module": "heater", "name": "Bath heater",
  "config": { "pin": 19, "frequency": 10, "max_duty": 60, "hold_s": 2 },
  "channels": { "power": { "key": "heat" } }
})";

// One scheduler pass, in the order the scheduler actually runs these:
// acquisition bookkeeping, then everything at kSafety, then kControl.  Written
// out rather than calling system.loop() so a test can decide exactly which
// layer is allowed to run — that is how "the layers are independent" is
// checked rather than asserted.
void pass(ApiRig& rig) {
  const Micros now = rig.clock.nowMicros();
  rig.channels.tick(now);
  rig.outputs.tick(now);
  rig.safety.tick(now);
  // The scenario decides, then the regulator acts on that decision — the same
  // order the scheduler runs them in (ADR-0018).
  rig.experiments.tick(now);
  rig.control.tick(now);
}

// The sensor reports.  Publishing directly is the point: the loop must read the
// channel, not the device, and a test that drove the simulator instead would be
// testing the simulator.
void feed(ApiRig& rig, float value) {
  rig.channels.publishRaw(rig.channels.findByKey("ref_signal"), value,
                          rig.clock.nowMicros());
}

// Advances time in small steps, feeding and running a pass at each one.
void run(ApiRig& rig, Micros totalUs, Micros stepUs, float value) {
  for (Micros elapsed = 0; elapsed < totalUs; elapsed += stepUs) {
    rig.clock.advanceMicros(stepUs);
    feed(rig, value);
    pass(rig);
  }
}

const char* kBathLoop = R"({
  "loops": [ { "id": "bath", "input": "ref_signal", "output": "heat",
               "setpoint": 50, "kp": 2, "ki": 0.5, "kd": 0,
               "min": 0, "max": 100, "period_s": 5, "input_grace_s": 2 } ]
})";

}  // namespace

static void test_a_running_loop_keeps_its_own_command_alive() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kShortHoldHeater).status);

  const ApiResponse configured =
      rig.call(HttpMethod::kPut, "/api/v1/control", "", kBathLoop);
  TEST_ASSERT_EQUAL_INT(200, configured.status);
  // Loaded, and OFF.  A loop never comes up commanding, whatever the file says.
  TEST_ASSERT_EQUAL_STRING("off", configured.body["loops"][0]["mode"]);
  TEST_ASSERT_TRUE(configured.body["loops"][0]["input_present"].as<bool>());
  TEST_ASSERT_TRUE(configured.body["loops"][0]["output_present"].as<bool>());

  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/control/loops/bath/mode",
                                      R"({"mode":"automatic"})").status);

  const ChannelHandle heat = rig.channels.findByKey("heat");
  run(rig, 1000000, 250000, 30.0f);
  TEST_ASSERT_TRUE(rig.channels.value(heat)->processed > 0.0f);

  // The loop recomputes every 5 s; the heater lets go after 2.  Between the
  // two, somebody has to say "still here" — and the somebody is the loop.
  // Without that renewal the heater would flicker at its own safety timer.
  run(rig, 4000000, 250000, 30.0f);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OutputHoldState::kCommanded),
                        static_cast<int>(rig.outputs.find(heat)->state));
  TEST_ASSERT_TRUE(rig.channels.value(heat)->processed > 0.0f);

  const ApiResponse listed = rig.get("/api/v1/control");
  TEST_ASSERT_EQUAL_STRING("RUNNING", listed.body["loops"][0]["state"]);
  TEST_ASSERT_EQUAL_STRING("degC", listed.body["loops"][0]["unit"]);
}

static void test_a_loop_that_loses_its_sensor_lets_go_of_the_heater() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kShortHoldHeater).status);
  TEST_ASSERT_EQUAL_INT(200,
      rig.call(HttpMethod::kPut, "/api/v1/control", "", kBathLoop).status);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/control/loops/bath/mode",
                                      R"({"mode":"automatic"})").status);

  const ChannelHandle heat = rig.channels.findByKey("heat");
  run(rig, 1000000, 250000, 30.0f);
  TEST_ASSERT_TRUE(rig.channels.value(heat)->processed > 0.0f);

  // The thermocouple falls off.  Nothing else changes: the loop still runs, the
  // heater still works, and the last measurement it saw said "cold".
  for (Micros elapsed = 0; elapsed < 3000000; elapsed += 250000) {
    rig.clock.advanceMicros(250000);
    pass(rig);
  }

  const ApiResponse listed = rig.get("/api/v1/control");
  TEST_ASSERT_EQUAL_STRING("NO_INPUT", listed.body["loops"][0]["state"]);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rig.channels.value(heat)->processed);
  // Released, not merely expired: a loop that has lost its sensor must not keep
  // heating for the length of a hold time it did not choose.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OutputHoldState::kSafe),
                        static_cast<int>(rig.outputs.find(heat)->state));
  TEST_ASSERT_EQUAL_STRING("SAFETY_INTERLOCK",
                           listed.body["loops"][0]["fault"]["code"]);
}

static void test_an_integrator_that_saturated_can_still_come_back() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kShortHoldHeater).status);
  TEST_ASSERT_EQUAL_INT(200,
      rig.call(HttpMethod::kPut, "/api/v1/control", "", kBathLoop).status);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/control/loops/bath/mode",
                                      R"({"mode":"automatic"})").status);

  // Twenty degrees below setpoint for two minutes: the classic wind-up case.
  run(rig, 120000000, 250000, 30.0f);
  const ApiResponse wound = rig.get("/api/v1/control");
  // Full power, and it stayed there — that part is correct behaviour.
  TEST_ASSERT_TRUE(wound.body["loops"][0]["output_value"].as<float>() >= 60.0f);
  // The integral, however, did not spend two minutes accumulating.  Conditional
  // integration stops adding as soon as adding would push the command further
  // past a limit it is already against, so the number stays inside the output
  // span instead of reaching the thousands.
  TEST_ASSERT_TRUE(wound.body["loops"][0]["integral"].as<float>() <= 100.0f);

  // The bath reaches temperature.  A wound-up integrator would keep the heater
  // at full power for as long as it spent saturated — minutes of overshoot.
  run(rig, 15000000, 250000, 60.0f);
  const ApiResponse recovered = rig.get("/api/v1/control");
  TEST_ASSERT_TRUE(recovered.body["loops"][0]["output_value"].as<float>() < 60.0f);
}

static void test_two_entries_with_one_id_are_refused() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kShortHoldHeater).status);

  // Installing this would leave one interlock running and one in the file that
  // nothing enforces — the worst possible combination, because the document
  // says the rig is protected twice.
  const ApiResponse refused = rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"limits":[{"id":"overtemp","channel":"ref_signal","high":300},
                    {"id":"overtemp","channel":"ref_signal","high":80}]})");
  TEST_ASSERT_TRUE(refused.isError());
  TEST_ASSERT_EQUAL_STRING("id", refused.body["error"]["field"]);
  TEST_ASSERT_EQUAL_INT(0, rig.get("/api/v1/control").body["limits"].size());
}

static void test_a_rule_needs_hysteresis_and_holds_its_relay() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kRelayBody).status);

  // A threshold with no band chatters on noise sitting exactly on it, and on a
  // relay that is a contact-life problem.  Refused at the door, with the field.
  const ApiResponse chattering = rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"rules":[{"id":"fan","input":"ref_signal","output":"pump",
                    "on_above":40,"off_below":40}]})");
  TEST_ASSERT_TRUE(chattering.isError());
  TEST_ASSERT_EQUAL_STRING("off_below", chattering.body["error"]["field"]);

  TEST_ASSERT_EQUAL_INT(200, rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"rules":[{"id":"fan","input":"ref_signal","output":"pump",
                    "on_above":40,"off_below":35,"min_hold_s":0}]})").status);

  const ChannelHandle pump = rig.channels.findByKey("pump");
  run(rig, 1000000, 250000, 45.0f);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, rig.channels.value(pump)->processed);

  // Past the relay's 5 s hold time.  A rule that engaged once and then stopped
  // saying so would have let the pump drop out from under itself.
  run(rig, 8000000, 250000, 45.0f);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, rig.channels.value(pump)->processed);

  // In the band: still on.  That is what the band is for.
  run(rig, 1000000, 250000, 37.0f);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, rig.channels.value(pump)->processed);

  run(rig, 1000000, 250000, 30.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rig.channels.value(pump)->processed);
  const ApiResponse listed = rig.get("/api/v1/control");
  TEST_ASSERT_FALSE(listed.body["rules"][0]["engaged"].as<bool>());
  TEST_ASSERT_EQUAL_INT(1, listed.body["rules"][0]["activations"].as<int>());
}

static void test_an_interlock_trips_on_a_sensor_that_stopped_reporting() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kHeaterBody).status);
  TEST_ASSERT_EQUAL_INT(200, rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"limits":[{"id":"overtemp","channel":"ref_signal","condition":"above",
                     "high":300,"action":"trip_all","for_s":0,
                     "message":"bath over temperature"}]})").status);

  const ChannelHandle heat = rig.channels.findByKey("heat");
  run(rig, 1000000, 250000, 25.0f);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/heat/write",
                                      R"({"value": 50})").status);
  TEST_ASSERT_EQUAL_FLOAT(50.0f, rig.channels.value(heat)->processed);

  // The sensor stops reporting.  Not "reads high" — stops.  An interlock that
  // can be switched off by unplugging a thermocouple is not an interlock.
  for (Micros elapsed = 0; elapsed < 1000000; elapsed += 250000) {
    rig.clock.advanceMicros(250000);
    pass(rig);
  }
  TEST_ASSERT_TRUE(rig.outputs.tripped());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rig.channels.value(heat)->processed);

  const ApiResponse tripped = rig.get("/api/v1/control");
  TEST_ASSERT_TRUE(tripped.body["limits"][0]["latched"].as<bool>());
  TEST_ASSERT_EQUAL_INT(1, tripped.body["limits"][0]["trips"].as<int>());
  TEST_ASSERT_EQUAL_INT(1, tripped.body["latched"].as<int>());

  // The master-stop button cannot clear an interlock.  If it could, "clear"
  // would be the way to switch a safety limit off (§30).
  const ApiResponse refused = rig.post("/api/v1/outputs/clear", "{}");
  TEST_ASSERT_TRUE(refused.isError());
  TEST_ASSERT_EQUAL_STRING("SAFETY_INTERLOCK", refused.body["error"]["code"]);

  // Resetting the limit while its cause is still there re-trips it at once.
  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/control/limits/overtemp/reset", "{}").status);
  rig.clock.advanceMicros(250000);
  pass(rig);
  TEST_ASSERT_TRUE(rig.outputs.tripped());
  TEST_ASSERT_TRUE(rig.get("/api/v1/control").body["limits"][0]["latched"].as<bool>());

  // The sensor comes back.  Now the reset holds, and the trip can be cleared.
  run(rig, 500000, 250000, 25.0f);
  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/control/limits/overtemp/reset", "{}").status);
  rig.clock.advanceMicros(250000);
  feed(rig, 25.0f);
  pass(rig);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/outputs/clear", "{}").status);
  TEST_ASSERT_FALSE(rig.outputs.tripped());
}

// ===========================================================================
//  Milestone 8 acceptance criterion
// ===========================================================================
//  A PID regulates a bath.  Then, one at a time, each of the three layers is
//  taken away, and each of the other two is shown to hold on its own.
static void test_three_layers_and_none_of_them_trusts_the_others() {
  ApiRig rig;
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kShortHoldHeater).status);
  TEST_ASSERT_EQUAL_INT(200, rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"limits":[{"id":"overtemp","channel":"ref_signal","condition":"above",
                     "high":80,"action":"trip_all","for_s":0}],
          "loops":[{"id":"bath","input":"ref_signal","output":"heat",
                    "setpoint":50,"kp":2,"ki":0.5,"kd":0,
                    "min":0,"max":100,"period_s":1,"input_grace_s":2}]})").status);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/control/loops/bath/mode",
                                      R"({"mode":"automatic"})").status);

  const ChannelHandle heat = rig.channels.findByKey("heat");

  // --- layer 1 works: the loop regulates ----------------------------------
  run(rig, 2000000, 250000, 30.0f);
  TEST_ASSERT_TRUE(rig.channels.value(heat)->processed > 0.0f);

  // --- layer 1 fails: the loop is wrong about the world -------------------
  // The bath is at 400 °C and the loop, whose sensor now lies to it in the
  // other direction, is still asking for heat.  The limit watches the CHANNEL,
  // not the loop, so the loop's opinion is irrelevant.
  run(rig, 1000000, 250000, 400.0f);
  TEST_ASSERT_TRUE(rig.outputs.tripped());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rig.channels.value(heat)->processed);

  const ApiResponse blocked = rig.get("/api/v1/control");
  // The loop has not been switched off — it is still in automatic, still
  // asking, and still being refused.  That distinction is what "independent"
  // means, and it is visible rather than inferred.
  TEST_ASSERT_EQUAL_STRING("automatic", blocked.body["loops"][0]["mode"]);
  TEST_ASSERT_EQUAL_STRING("BLOCKED", blocked.body["loops"][0]["state"]);

  // --- layers 1 and 2 both fail: the deadline is still there ---------------
  // A fresh rig, no limits at all, and a loop that hangs after commanding —
  // the case no amount of care inside the controller can cover.
  ApiRig second;
  TEST_ASSERT_EQUAL_INT(201, second.post("/api/v1/devices", kSimulatorBody).status);
  TEST_ASSERT_EQUAL_INT(201,
                        second.post("/api/v1/devices", kShortHoldHeater).status);
  TEST_ASSERT_EQUAL_INT(200,
      second.call(HttpMethod::kPut, "/api/v1/control", "", kBathLoop).status);
  TEST_ASSERT_EQUAL_INT(200, second.post("/api/v1/control/loops/bath/mode",
                                         R"({"mode":"automatic"})").status);
  const ChannelHandle heat2 = second.channels.findByKey("heat");
  run(second, 1000000, 250000, 30.0f);
  TEST_ASSERT_TRUE(second.channels.value(heat2)->processed > 0.0f);

  // ControlManager::tick is never called again: this is a controller that hung
  // with the heater on.  Only the output deadline is left.
  for (Micros elapsed = 0; elapsed < 3000000; elapsed += 250000) {
    second.clock.advanceMicros(250000);
    second.outputs.tick(second.clock.nowMicros());
  }
  TEST_ASSERT_EQUAL_FLOAT(0.0f, second.channels.value(heat2)->processed);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(OutputHoldState::kExpired),
                        static_cast<int>(second.outputs.find(heat2)->state));
}

// ===========================================================================
//  Experiments (Milestone 9)
// ===========================================================================
namespace {

// The scenario from the specification, shortened so a test can watch it run:
// reach a temperature, mark the instant, hold, switch off, stop.
const char* kEvaporation = R"({
  "key": "evaporation",
  "name": "Evaporation run",
  "metadata": { "operator": "", "sample": "", "description": "spec scenario" },
  "steps": [
    { "op": "SET", "target": "bath.setpoint", "value": 60 },
    { "op": "SET", "target": "bath.mode", "mode": "automatic" },
    { "op": "WAIT_UNTIL", "channel": "ref_signal", "comparison": ">=",
      "value": 59, "timeout_s": 30, "on_timeout": "abort" },
    { "op": "MARK_EVENT", "label": "steady state reached" },
    { "op": "RUN_FOR", "duration_s": 5 },
    { "op": "SET", "target": "heat", "value": 0 },
    { "op": "STOP" } ]
})";

const char* kBathLoopForRuns = R"({
  "loops": [ { "id": "bath", "input": "ref_signal", "output": "heat",
               "setpoint": 0, "kp": 4, "ki": 0.2, "kd": 0,
               "min": 0, "max": 100, "period_s": 1, "input_grace_s": 5 } ]
})";

// A rig with a simulator, a heater and one loop — the smallest thing a
// scenario can be run against.
void prepareRig(ApiRig& rig) {
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kShortHoldHeater).status);
  TEST_ASSERT_EQUAL_INT(200,
      rig.call(HttpMethod::kPut, "/api/v1/control", "", kBathLoopForRuns).status);
}

}  // namespace

static void test_a_wait_without_a_deadline_cannot_be_saved() {
  ApiRig rig;
  prepareRig(rig);

  // The one rule this milestone refuses to soften.  It is refused where it is
  // written, with the step named, rather than discovered at three in the
  // morning by a rig that is still waiting.
  const ApiResponse refused = rig.post("/api/v1/experiments", R"({
    "key": "forever", "name": "Wait forever",
    "steps": [ { "op": "WAIT_UNTIL", "channel": "ref_signal",
                 "comparison": ">=", "value": 100 } ] })");
  TEST_ASSERT_TRUE(refused.isError());
  TEST_ASSERT_EQUAL_STRING("timeout_s", refused.body["error"]["field"]);
  TEST_ASSERT_EQUAL_INT(1, refused.body["step"].as<int>());

  // And a step the vocabulary does not contain is not an extension point.
  const ApiResponse notALanguage = rig.post("/api/v1/experiments", R"({
    "key": "clever", "name": "Clever",
    "steps": [ { "op": "EVAL", "code": "digitalWrite pin 2 HIGH" } ] })");
  TEST_ASSERT_TRUE(notALanguage.isError());
  TEST_ASSERT_EQUAL_STRING("op", notALanguage.body["error"]["field"]);
}

static void test_a_scenario_that_logs_nothing_cannot_say_it_logs() {
  ApiRig rig;
  prepareRig(rig);
  // START_LOGGING with no channel list would open a dataset with no columns:
  // a file that exists and answers nothing.  Saved (a scenario may be written
  // before the rig is finished) but explicitly not runnable.
  const ApiResponse saved = rig.post("/api/v1/experiments", R"({
    "key": "logged", "name": "Logged run",
    "steps": [ { "op": "START_LOGGING" },
               { "op": "RUN_FOR", "duration_s": 10 } ] })");
  TEST_ASSERT_EQUAL_INT(201, saved.status);
  TEST_ASSERT_FALSE(saved.body["runnable"].as<bool>());

  const ApiResponse refused =
      rig.post("/api/v1/experiments/logged/actions/start", R"({"operator":"AM"})");
  TEST_ASSERT_TRUE(refused.isError());
  TEST_ASSERT_EQUAL_STRING("INVALID_ARGUMENT", refused.body["error"]["code"]);
}

static void test_a_run_needs_somebody_to_own_it() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", kEvaporation).status);

  // §48: a dataset nobody can attribute is a dataset nobody can ask about.
  const ApiResponse anonymous =
      rig.post("/api/v1/experiments/evaporation/actions/start", "{}");
  TEST_ASSERT_TRUE(anonymous.isError());
  TEST_ASSERT_EQUAL_STRING("operator", anonymous.body["error"]["field"]);

  // And a rig that is stopped does not quietly run a scenario that commands
  // nothing: the refusal names the reason.
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/outputs/trip", "{}").status);
  const ApiResponse whileStopped =
      rig.post("/api/v1/experiments/evaporation/actions/start",
               R"({"operator":"AM","sample":"salt"})");
  TEST_ASSERT_TRUE(whileStopped.isError());
  TEST_ASSERT_EQUAL_STRING("SAFETY_INTERLOCK", whileStopped.body["error"]["code"]);
}

static void test_a_scenario_runs_to_the_end_and_the_record_says_so() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", kEvaporation).status);

  const ApiResponse started =
      rig.post("/api/v1/experiments/evaporation/actions/start",
               R"({"operator":"A. Mescheryakov","sample":"NaCl 5 %"})");
  TEST_ASSERT_EQUAL_INT(202, started.status);
  TEST_ASSERT_EQUAL_STRING("RUNNING", started.body["state"]);

  // Cold at first: the loop starts heating and the scenario waits.
  run(rig, 2000000, 250000, 30.0f);
  const ApiResponse waiting = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("WAIT_UNTIL", waiting.body["current"]["op"]);
  TEST_ASSERT_TRUE(rig.channels.value(rig.channels.findByKey("heat"))->processed > 0.0f);

  // The bath reaches temperature: the wait is satisfied, the event is marked,
  // the hold runs out, the heater is switched off and the scenario stops.
  run(rig, 8000000, 250000, 60.0f);

  const ApiResponse done = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("FINISHED", done.body["state"]);
  TEST_ASSERT_EQUAL_STRING("scenario", done.body["reason"]);
  TEST_ASSERT_EQUAL_STRING("steady state reached", done.body["events"][0]["label"]);
  // What the experiment turned on, the experiment turned off.
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);
  const ApiResponse afterRun = rig.get("/api/v1/control");
  TEST_ASSERT_EQUAL_STRING("off", afterRun.body["loops"][0]["mode"]);

  // And the record exists, with everything needed to ask about it a year later.
  TEST_ASSERT_TRUE(rig.runLog.flushPending().ok());
  const ApiResponse runs = rig.get("/api/v1/experiments/runs");
  TEST_ASSERT_EQUAL_STRING("FINISHED", runs.body["runs"][0]["state"]);
  TEST_ASSERT_EQUAL_STRING("A. Mescheryakov", runs.body["runs"][0]["operator"]);
  TEST_ASSERT_EQUAL_STRING("NaCl 5 %", runs.body["runs"][0]["sample"]);
  TEST_ASSERT_EQUAL_INT(7, runs.body["runs"][0]["steps"].as<int>());
  TEST_ASSERT_TRUE(runs.body["runs"][0]["devices"].size() >= 2);
}

static void test_a_wait_that_never_ends_aborts_and_says_where() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", R"({
    "key": "impossible", "name": "Impossible",
    "steps": [ { "op": "SET", "target": "heat", "value": 40 },
               { "op": "WAIT_UNTIL", "channel": "ref_signal", "comparison": ">=",
                 "value": 500, "timeout_s": 5, "on_timeout": "abort" },
               { "op": "RUN_FOR", "duration_s": 60 } ] })").status);
  TEST_ASSERT_EQUAL_INT(202,
      rig.post("/api/v1/experiments/impossible/actions/start",
               R"({"operator":"AM"})").status);

  run(rig, 2000000, 250000, 30.0f);
  TEST_ASSERT_EQUAL_FLOAT(
      40.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);

  run(rig, 5000000, 250000, 30.0f);
  const ApiResponse aborted = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("ABORTED", aborted.body["state"]);
  TEST_ASSERT_EQUAL_STRING("timeout", aborted.body["reason"]);
  // The step it reached, not just "it failed": the difference between "the
  // heater never got there" and "the hold was interrupted".
  TEST_ASSERT_EQUAL_INT(2, aborted.body["step_reached"].as<int>());
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);

  TEST_ASSERT_TRUE(rig.runLog.flushPending().ok());
  const ApiResponse runs = rig.get("/api/v1/experiments/runs");
  TEST_ASSERT_EQUAL_STRING("ABORTED", runs.body["runs"][0]["state"]);
  TEST_ASSERT_EQUAL_STRING("timeout", runs.body["runs"][0]["reason"]);
  TEST_ASSERT_EQUAL_STRING("TIMEOUT", runs.body["runs"][0]["error"]["code"]);
}

static void test_an_interlock_aborts_the_run_and_takes_the_heater_with_it() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(200, rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"limits":[{"id":"overtemp","channel":"ref_signal","condition":"above",
                     "high":80,"action":"trip_all","for_s":0}],
          "loops":[{"id":"bath","input":"ref_signal","output":"heat",
                    "setpoint":0,"kp":4,"ki":0.2,"kd":0,"min":0,"max":100,
                    "period_s":1,"input_grace_s":5}]})").status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", R"({
    "key": "long_hold", "name": "Long hold",
    "steps": [ { "op": "SET", "target": "heat", "value": 50 },
               { "op": "RUN_FOR", "duration_s": 600 } ] })").status);
  TEST_ASSERT_EQUAL_INT(202,
      rig.post("/api/v1/experiments/long_hold/actions/start",
               R"({"operator":"AM"})").status);

  run(rig, 1000000, 250000, 30.0f);
  const ApiResponse running = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("RUNNING", running.body["state"]);

  // The bath runs away.  The interlock is watching the channel, not the
  // scenario, and the scenario is not consulted about whether it minds.
  run(rig, 1000000, 250000, 400.0f);

  const ApiResponse aborted = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("ABORTED", aborted.body["state"]);
  TEST_ASSERT_EQUAL_STRING("safety", aborted.body["reason"]);
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);
  // Aborted, not paused: a run that continued after an interlock fired would be
  // a record of the interlock rather than of the sample.
  TEST_ASSERT_TRUE(rig.outputs.tripped());
}

static void test_a_paused_run_holds_and_does_not_spend_its_hold_time() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", R"({
    "key": "hold", "name": "Hold",
    "steps": [ { "op": "SET", "target": "heat", "value": 30 },
               { "op": "RUN_FOR", "duration_s": 10 },
               { "op": "STOP" } ] })").status);
  TEST_ASSERT_EQUAL_INT(202, rig.post("/api/v1/experiments/hold/actions/start",
                                      R"({"operator":"AM"})").status);
  run(rig, 1000000, 250000, 30.0f);
  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/experiments/hold/actions/pause", "{}").status);

  // Ten seconds of coffee break.  The heater stays where the scenario put it —
  // paused means "wait for me", and the operator who wants it off has a button
  // that says stop.
  run(rig, 10000000, 250000, 30.0f);
  const ApiResponse held = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("PAUSED", held.body["state"]);
  TEST_ASSERT_EQUAL_FLOAT(
      30.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);

  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/experiments/hold/actions/resume", "{}").status);
  // The hold had ~9 s left when it was paused, and the pause did not spend it.
  run(rig, 5000000, 250000, 30.0f);
  const ApiResponse resumed = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("RUNNING", resumed.body["state"]);
  run(rig, 6000000, 250000, 30.0f);
  const ApiResponse done = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("FINISHED", done.body["state"]);
}

static void test_stopping_by_hand_is_not_the_same_as_finishing() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", R"({
    "key": "long", "name": "Long",
    "steps": [ { "op": "SET", "target": "bath.mode", "mode": "automatic" },
               { "op": "RUN_FOR", "duration_s": 3600 } ] })").status);
  TEST_ASSERT_EQUAL_INT(202, rig.post("/api/v1/experiments/long/actions/start",
                                      R"({"operator":"AM"})").status);
  run(rig, 2000000, 250000, 30.0f);
  const ApiResponse driving = rig.get("/api/v1/control");
  TEST_ASSERT_EQUAL_STRING("automatic", driving.body["loops"][0]["mode"]);

  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/experiments/long/actions/stop", "{}").status);

  const ApiResponse stopped = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("ABORTED", stopped.body["state"]);
  TEST_ASSERT_EQUAL_STRING("operator", stopped.body["reason"]);
  // The loop the scenario started is off, and the heater with it.  A regulator
  // left running by a scenario that has ended is the failure this project
  // keeps closing.
  const ApiResponse released = rig.get("/api/v1/control");
  TEST_ASSERT_EQUAL_STRING("off", released.body["loops"][0]["mode"]);
  run(rig, 500000, 250000, 30.0f);
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);
}

static void test_a_scenario_that_names_something_missing_will_not_start() {
  ApiRig rig;
  prepareRig(rig);
  // Saving is allowed: a scenario may legitimately be written before the rig it
  // drives is finished.
  const ApiResponse saved = rig.post("/api/v1/experiments", R"({
    "key": "future", "name": "For a rig not yet wired",
    "steps": [ { "op": "SET", "target": "chiller", "value": 1 } ] })");
  TEST_ASSERT_EQUAL_INT(201, saved.status);
  TEST_ASSERT_FALSE(saved.body["runnable"].as<bool>());
  TEST_ASSERT_EQUAL_INT(1, saved.body["blocking_step"].as<int>());

  // Starting it is not.
  const ApiResponse refused =
      rig.post("/api/v1/experiments/future/actions/start", R"({"operator":"AM"})");
  TEST_ASSERT_TRUE(refused.isError());
  TEST_ASSERT_EQUAL_STRING("CHANNEL_NOT_FOUND", refused.body["error"]["code"]);
}

// ===========================================================================
//  Milestone 9 acceptance criterion
// ===========================================================================
//  The scenario from the specification, driven entirely through the API, and
//  interrupted at an arbitrary moment: what matters is that the interruption
//  leaves the rig safe and leaves a record that cannot be mistaken for a run
//  that finished.
static void test_a_run_interrupted_is_a_run_that_says_it_was_interrupted() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", kEvaporation).status);
  TEST_ASSERT_EQUAL_INT(202,
      rig.post("/api/v1/experiments/evaporation/actions/start",
               R"({"operator":"AM","sample":"NaCl 5 %"})").status);

  run(rig, 3000000, 250000, 61.0f);   // through the wait, into the hold
  const ApiResponse midway = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("RUNNING", midway.body["state"]);
  TEST_ASSERT_EQUAL_STRING("RUN_FOR", midway.body["current"]["op"]);

  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/experiments/evaporation/actions/stop", "{}").status);

  // 1. the rig is safe
  run(rig, 500000, 250000, 61.0f);
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);
  const ApiResponse quiet = rig.get("/api/v1/control");
  TEST_ASSERT_EQUAL_STRING("off", quiet.body["loops"][0]["mode"]);

  // 2. the record exists and is unmistakable
  TEST_ASSERT_TRUE(rig.runLog.flushPending().ok());
  const ApiResponse runs = rig.get("/api/v1/experiments/runs");
  JsonObjectConst record = runs.body["runs"][0].as<JsonObjectConst>();
  TEST_ASSERT_EQUAL_STRING("ABORTED", record["state"]);
  TEST_ASSERT_EQUAL_STRING("operator", record["reason"]);
  TEST_ASSERT_EQUAL_INT(5, record["step_reached"].as<int>());
  TEST_ASSERT_EQUAL_INT(7, record["steps"].as<int>());
  // 3. and it still carries what it was measuring and with what
  TEST_ASSERT_EQUAL_STRING("NaCl 5 %", record["sample"]);
  TEST_ASSERT_EQUAL_STRING("steady state reached", record["events"][0]["label"]);
  TEST_ASSERT_TRUE(record["config_revision"].as<int>() > 0);
}

// ===========================================================================
//  Logging (Milestone 10)
// ===========================================================================
namespace {

// Reads a file straight off the rig's filesystem, so a test can assert what is
// actually IN the dataset rather than what the API says about it.
std::string readFile(ApiRig& rig, const char* path) {
  const Result<std::size_t> size = rig.backend.size(path);
  if (!size.ok()) return {};
  std::string buffer(size.value() + 1, '\0');
  const Result<std::size_t> read = rig.backend.read(path, buffer.data(), buffer.size());
  if (!read.ok()) return {};
  buffer.resize(read.value());
  return buffer;
}

// Runs the logger the way the scheduler does: sample at kTelemetry, flush at
// kBackground, and never both in the same breath.
void logPass(ApiRig& rig, Micros totalUs, Micros stepUs, float value) {
  for (Micros elapsed = 0; elapsed < totalUs; elapsed += stepUs) {
    rig.clock.advanceMicros(stepUs);
    feed(rig, value);
    pass(rig);
    rig.logger.sampleTick(rig.clock.nowMicros());
    rig.logger.flushTick();
  }
}

const char* kLoggedScenario = R"({
  "key": "recorded", "name": "Recorded run",
  "logging": { "channels": ["ref_signal"], "rate_hz": 10, "raw": true },
  "steps": [ { "op": "START_LOGGING" },
             { "op": "SET", "target": "heat", "value": 30 },
             { "op": "RUN_FOR", "duration_s": 3 },
             { "op": "STOP_LOGGING" },
             { "op": "STOP" } ]
})";

}  // namespace

// ===========================================================================
//  Milestone 15 — the offload protocol over REST.
//
//  The route that matters is the ACK: it is the only thing in the system that
//  deletes measurements, and it does so on the say-so of a browser.  These
//  tests are about what the firmware demands before it believes that.
// ===========================================================================
static void test_a_continuous_log_needs_a_collector_to_start() {
  ApiRig rig;
  prepareRig(rig);
  // Refused before a single row exists: without an owner nothing could ever
  // acknowledge, so the queue would fill the filesystem and stop the log.
  const ApiResponse refused = rig.post(
      "/api/v1/logs/start",
      R"({"name":"x","rate_hz":1,"channels":["ref_signal"],
          "storage_mode":"continuous_offload"})");
  TEST_ASSERT_EQUAL_INT(400, refused.status);

  const ApiResponse bogus = rig.post(
      "/api/v1/logs/start",
      R"({"name":"x","rate_hz":1,"channels":["ref_signal"],
          "storage_mode":"sideways","collector_id":"browser-01"})");
  TEST_ASSERT_EQUAL_INT(400, bogus.status);
}

static void test_the_segment_routes_refuse_what_they_cannot_prove() {
  ApiRig rig;
  rig.signIn();

  // An unknown session is a 404 on every route, not an empty success.
  TEST_ASSERT_EQUAL_INT(404, rig.get("/api/v1/logs/log_9999/segments").status);
  TEST_ASSERT_EQUAL_INT(
      404, rig.get("/api/v1/logs/log_9999/segments/1/export.csv").status);
  const ApiResponse ack = rig.post(
      "/api/v1/logs/log_9999/segments/1/ack",
      R"({"collector_id":"browser-01","bytes":10,"payload_crc32":"00000000"})");
  TEST_ASSERT_EQUAL_INT(404, ack.status);
}

static void test_a_single_mode_log_reports_no_offload_queue() {
  ApiRig rig;
  prepareRig(rig);
  const ApiResponse started = rig.post(
      "/api/v1/logs/start",
      R"({"name":"ordinary","rate_hz":1,"channels":["ref_signal"]})");
  TEST_ASSERT_EQUAL_INT(200, started.status);
  rig.post("/api/v1/logs/stop", "{}");

  // The old client asked for nothing new and got the old behaviour.
  const ApiResponse listed = rig.get("/api/v1/logs");
  TEST_ASSERT_EQUAL_INT(200, listed.status);
  JsonArrayConst logs = listed.body["logs"].as<JsonArrayConst>();
  TEST_ASSERT_TRUE(logs.size() >= 1);
  TEST_ASSERT_EQUAL_STRING("single", logs[0]["mode"] | "single");

  // By the id it actually got: hard-coding log_0001 tests the numbering of the
  // suite, not the route.
  char route[64];
  std::snprintf(route, sizeof(route), "/api/v1/logs/%s/segments",
                logs[0]["id"] | "");
  const ApiResponse segments = rig.get(route);
  TEST_ASSERT_EQUAL_INT(200, segments.status);
  TEST_ASSERT_EQUAL_UINT(0, segments.body["pending"].as<JsonArrayConst>().size());
  TEST_ASSERT_EQUAL_STRING("single", segments.body["mode"] | "");
}

static void test_a_dataset_says_what_it_was_measured_with() {
  ApiRig rig;
  prepareRig(rig);

  const ApiResponse started = rig.post("/api/v1/logs/start", R"({
    "name": "smoke", "operator": "AM", "sample": "NaCl 5 %",
    "rate_hz": 10, "channels": ["ref_signal"] })");
  TEST_ASSERT_EQUAL_INT(200, started.status);
  TEST_ASSERT_TRUE(started.body["recording"].as<bool>());

  logPass(rig, 2000000, 50000, 42.0f);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/logs/stop", "{}").status);

  const ApiResponse listed = rig.get("/api/v1/logs");
  JsonObjectConst entry = listed.body["logs"][0].as<JsonObjectConst>();
  TEST_ASSERT_EQUAL_STRING("log_0001", entry["id"]);
  TEST_ASSERT_EQUAL_STRING("COMPLETE", entry["state"]);
  TEST_ASSERT_TRUE(entry["rows"].as<int>() >= 15);
  TEST_ASSERT_FALSE(entry["truncated"].as<bool>());

  const std::string csv = readFile(rig, entry["path"] | "");
  TEST_ASSERT_TRUE(csv.find("# operator: AM") != std::string::npos);
  TEST_ASSERT_TRUE(csv.find("# sample: NaCl 5 %") != std::string::npos);
  // The fingerprint, not just the revision counter: a rig that rebooted this
  // morning would otherwise write "config_revision: 0" and mean nothing by it.
  TEST_ASSERT_TRUE(csv.find("# config_fingerprint: ") != std::string::npos);
  // Raw beside processed, both timestamps, and the quality mask (§33, §48).
  TEST_ASSERT_TRUE(
      csv.find("t_ms,epoch_ms,ref_signal.raw,ref_signal.degC,quality_mask") !=
      std::string::npos);
  TEST_ASSERT_TRUE(csv.find("# complete") != std::string::npos);
}

static void test_a_session_that_cannot_fit_is_refused_before_it_starts() {
  ApiRig rig;
  prepareRig(rig);
  // A card with almost nothing on it: 80 KB total, of which 64 KB is the
  // reserve the log may never touch.
  rig.backend.setQuota(80 * 1024);

  const ApiResponse refused = rig.post("/api/v1/logs/start", R"({
    "name": "eight hours", "operator": "AM", "rate_hz": 10,
    "expected_s": 28800, "channels": ["ref_signal"] })");
  TEST_ASSERT_TRUE(refused.isError());
  TEST_ASSERT_EQUAL_STRING("FILESYSTEM_FULL", refused.body["error"]["code"]);
  // Both numbers, at the start, instead of eight hours later.
  const char* detail = refused.body["error"]["detail"];
  TEST_ASSERT_TRUE(std::strstr(detail, "KB free") != nullptr);

  // The same session without the impossible duration is allowed: the check is
  // arithmetic about THIS run, not a blanket refusal.
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/logs/start", R"({
    "name": "short", "operator": "AM", "rate_hz": 10,
    "expected_s": 20, "channels": ["ref_signal"] })").status);
}

static void test_a_full_medium_stops_the_log_and_never_the_rig() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", kLoggedScenario).status);
  TEST_ASSERT_EQUAL_INT(202,
      rig.post("/api/v1/experiments/recorded/actions/start",
               R"({"operator":"AM","sample":"NaCl"})").status);

  logPass(rig, 500000, 50000, 42.0f);
  TEST_ASSERT_TRUE(rig.logger.recording());
  TEST_ASSERT_EQUAL_FLOAT(
      30.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);

  // The card fills up mid-run.  This is the moment the whole milestone is about.
  const std::size_t used = rig.backend.totalBytes() - rig.backend.freeBytes();
  rig.backend.setQuota(used + LogStore::kReserveBytes);
  logPass(rig, 1500000, 50000, 42.0f);

  const ApiResponse logs = rig.get("/api/v1/logs");
  TEST_ASSERT_FALSE(logs.body["recording"]["recording"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("medium_full", logs.body["recording"]["last_stop"]);
  TEST_ASSERT_TRUE(logs.body["recording"]["last_truncated"].as<bool>());

  // The run is still going: a full card is a Reproducibility problem, not a
  // Safety one, and killing an eight-hour sample over storage would trade a
  // recoverable loss for an unrecoverable one (§49).
  const ApiResponse run = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("RUNNING", run.body["state"]);
  TEST_ASSERT_EQUAL_FLOAT(
      30.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);
  // ...and it says, in the run's own record of events, that it stopped being a
  // complete dataset.
  bool noticed = false;
  for (JsonObjectConst event : run.body["events"].as<JsonArrayConst>()) {
    if (std::strstr(event["label"] | "", "truncated") != nullptr) noticed = true;
  }
  TEST_ASSERT_TRUE(noticed);

  // The dataset says so in both of its other places.
  JsonObjectConst entry = logs.body["logs"][0].as<JsonObjectConst>();
  TEST_ASSERT_EQUAL_STRING("TRUNCATED", entry["state"]);
  TEST_ASSERT_TRUE(entry["truncated"].as<bool>());
  const std::string csv = readFile(rig, entry["path"] | "");
  TEST_ASSERT_TRUE(csv.find("# TRUNCATED") != std::string::npos);

  // And the reserve is still there: the instrument can still save its
  // configuration and serve its own interface.  Samples never cross it; the
  // footer and the index entry — the metadata that makes the dataset legible —
  // are what the few kilobytes of slack below are for.
  TEST_ASSERT_TRUE(rig.backend.freeBytes() >= LogStore::kReserveBytes - 8192);
}

static void test_rows_the_medium_could_not_take_are_counted() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/logs/start", R"({
    "name": "no flush", "operator": "AM", "rate_hz": 50,
    "channels": ["ref_signal"] })").status);

  // Sampling without ever flushing: the writer is stalled and the buffer fills.
  for (Micros elapsed = 0; elapsed < 30000000; elapsed += 20000) {
    rig.clock.advanceMicros(20000);
    rig.logger.sampleTick(rig.clock.nowMicros());
  }
  TEST_ASSERT_TRUE(rig.logger.status().droppedRows > 0);

  rig.post("/api/v1/logs/stop", "{}");
  const ApiResponse logs = rig.get("/api/v1/logs");
  // Reported, not hidden: a log that silently drops samples is a log nobody can
  // reason from.
  TEST_ASSERT_TRUE(logs.body["logs"][0]["dropped"].as<int>() > 0);
  const std::string csv = readFile(rig, logs.body["logs"][0]["path"] | "");
  TEST_ASSERT_TRUE(csv.find("# dropped_rows: ") != std::string::npos);
}

static void test_the_export_is_a_stream_and_not_a_document() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/logs/start", R"({
    "name": "export", "operator": "AM", "rate_hz": 10,
    "channels": ["ref_signal"] })").status);
  logPass(rig, 1000000, 50000, 42.0f);
  rig.post("/api/v1/logs/stop", "{}");

  const ApiResponse exported = rig.get("/api/v1/logs/log_0001/export.csv");
  // The REST layer describes the file; the transport sends it.  A dataset is
  // megabytes and the JSON path is bounded at twelve kilobytes on purpose —
  // the fix for "it does not fit" is a second path, not a bigger buffer.
  TEST_ASSERT_TRUE(exported.isStream());
  TEST_ASSERT_EQUAL_STRING("text/csv", exported.stream.contentType.c_str());
  TEST_ASSERT_EQUAL_STRING("log_0001.csv", exported.stream.filename.c_str());
  TEST_ASSERT_TRUE(rig.backend.exists(exported.stream.path.c_str()));

  TEST_ASSERT_TRUE(rig.get("/api/v1/logs/log_9999/export.csv").isError());
}

static void test_a_dataset_is_only_ever_deleted_by_name() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/logs/start", R"({
    "name": "keep", "operator": "AM", "rate_hz": 10,
    "channels": ["ref_signal"] })").status);
  logPass(rig, 500000, 50000, 42.0f);

  // Not while it is being written.
  TEST_ASSERT_TRUE(
      rig.call(HttpMethod::kDelete, "/api/v1/logs/log_0001").isError());

  rig.post("/api/v1/logs/stop", "{}");
  const ApiResponse deleted = rig.call(HttpMethod::kDelete, "/api/v1/logs/log_0001");
  TEST_ASSERT_EQUAL_INT(200, deleted.status);
  TEST_ASSERT_EQUAL_UINT(0, rig.get("/api/v1/logs").body["logs"].size());
}

// ===========================================================================
//  Milestone 10 acceptance criterion
// ===========================================================================
//  A scenario records its own run: the dataset exists, carries the header that
//  makes it re-analysable, is downloadable as it stands, and the run record
//  points at it.
static void test_an_experiment_records_itself_and_the_file_can_be_taken_away() {
  ApiRig rig;
  prepareRig(rig);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", kLoggedScenario).status);
  TEST_ASSERT_EQUAL_INT(202,
      rig.post("/api/v1/experiments/recorded/actions/start",
               R"({"operator":"A. Mescheryakov","sample":"NaCl 5 %"})").status);

  logPass(rig, 4000000, 50000, 42.0f);

  const ApiResponse run = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("FINISHED", run.body["state"]);

  const ApiResponse logs = rig.get("/api/v1/logs");
  JsonObjectConst entry = logs.body["logs"][0].as<JsonObjectConst>();
  TEST_ASSERT_EQUAL_STRING("COMPLETE", entry["state"]);
  TEST_ASSERT_EQUAL_STRING("recorded", entry["experiment"]);
  TEST_ASSERT_EQUAL_STRING("A. Mescheryakov", entry["operator"]);
  TEST_ASSERT_TRUE(entry["rows"].as<int>() >= 25);

  // The scenario stopped the dataset itself; nothing is left recording a rig
  // that is no longer doing anything.
  TEST_ASSERT_FALSE(rig.logger.recording());

  // It can be taken away, and what leaves is the file — not a JSON document
  // pretending to be one.
  const ApiResponse exported = rig.get("/api/v1/logs/log_0001/export.csv");
  TEST_ASSERT_TRUE(exported.isStream());
  const std::string csv = readFile(rig, exported.stream.path.c_str());
  TEST_ASSERT_TRUE(csv.find("# experiment: recorded") != std::string::npos);
  TEST_ASSERT_TRUE(csv.find("# complete") != std::string::npos);
  // Header + column line + footer + at least the rows it claims.
  std::size_t lines = 0;
  for (char c : csv) if (c == '\n') ++lines;
  TEST_ASSERT_TRUE(lines >= static_cast<std::size_t>(entry["rows"].as<int>()));
}

// ===========================================================================
//  Authentication and dangerous actions (Milestone 11)
// ===========================================================================
static void test_an_instrument_without_a_password_is_open_and_says_so() {
  ApiRig rig;
  // Out of the box: usable, and honest about it.  A device that ships with a
  // default password is a device with no password; one that refuses to work
  // until somebody sets one cannot be used at two in the morning.
  const ApiResponse system = rig.get("/api/v1/system");
  TEST_ASSERT_FALSE(system.body["auth"]["configured"].as<bool>());
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kSimulatorBody).status);

  const ApiResponse tooShort =
      rig.post("/api/v1/auth/password", R"({"password":"short"})");
  TEST_ASSERT_TRUE(tooShort.isError());

  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/auth/password", R"({"password":"bench-password"})").status);
  TEST_ASSERT_TRUE(rig.get("/api/v1/system").body["auth"]["configured"].as<bool>());

  // Setting the password ended every session, including the one that set it.
  const ApiResponse refused = rig.post("/api/v1/devices", kShortHoldHeater);
  TEST_ASSERT_EQUAL_INT(401, refused.status);
  TEST_ASSERT_EQUAL_STRING("UNAUTHORIZED", refused.body["error"]["code"]);

  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/auth/login", R"({"password":"bench-password"})").status);
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/devices", kShortHoldHeater).status);
}

static void test_the_emergency_stop_never_asks_who_you_are() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/heat/write",
                                      R"({"value": 40})").status);
  rig.post("/api/v1/auth/logout", "{}");

  // Signed out.  Ordinary writes are refused...
  TEST_ASSERT_EQUAL_INT(401,
      rig.post("/api/v1/channels/heat/write", R"({"value": 10})").status);

  // ...and the stop button still works, because a person reaching for it is not
  // going to sign in first, and nothing they can do here makes the rig less
  // safe than it already is (§49, ADR-0020).
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/outputs/trip", "{}").status);
  TEST_ASSERT_TRUE(rig.outputs.tripped());
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, rig.channels.value(rig.channels.findByKey("heat"))->processed);

  // CLEARING the stop is an arming action, and arming is what the password is
  // for.  This is the asymmetry the whole policy rests on.
  TEST_ASSERT_EQUAL_INT(401, rig.post("/api/v1/outputs/clear", "{}").status);
  TEST_ASSERT_TRUE(rig.outputs.tripped());

  rig.post("/api/v1/auth/login", R"({"password":"bench-password"})");
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/outputs/clear", "{}").status);
}

static void test_stopping_a_run_does_not_need_a_session_either() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();
  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", kEvaporation).status);
  TEST_ASSERT_EQUAL_INT(202,
      rig.post("/api/v1/experiments/evaporation/actions/start",
               R"({"operator":"AM"})").status);
  rig.post("/api/v1/auth/logout", "{}");

  TEST_ASSERT_EQUAL_INT(401,
      rig.post("/api/v1/experiments/evaporation/actions/pause", "{}").status);
  // Stopping is always allowed.  Pausing is not: a paused run is still holding
  // the rig, so "pause" is not a way to make anything safe.
  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/experiments/evaporation/actions/stop", "{}").status);
  const ApiResponse aborted = rig.get("/api/v1/experiments/state");
  TEST_ASSERT_EQUAL_STRING("ABORTED", aborted.body["state"]);
}

static void test_removing_an_interlock_takes_more_than_a_session() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();
  TEST_ASSERT_EQUAL_INT(200, rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"limits":[{"id":"overtemp","channel":"ref_signal","condition":"above",
                     "high":300,"action":"trip_all"}]})").status);

  // Adding a loop is a write: the session is enough.
  TEST_ASSERT_EQUAL_INT(200, rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"limits":[{"id":"overtemp","channel":"ref_signal","condition":"above",
                     "high":300,"action":"trip_all"}],
          "loops":[{"id":"bath","input":"ref_signal","output":"heat",
                    "setpoint":50,"kp":1,"min":0,"max":100,"period_s":1}]})").status);

  // Taking the interlock away is not.  The password, in this request, whatever
  // the session says — because this is the shape of action that removes a
  // protection (ADR-0020).
  const ApiResponse refused = rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"limits":[],"loops":[]})");
  TEST_ASSERT_EQUAL_INT(403, refused.status);
  TEST_ASSERT_EQUAL_STRING("password", refused.body["error"]["field"]);
  TEST_ASSERT_EQUAL_UINT(1, rig.safety.count());

  // Disabling it counts as removing it, too.
  TEST_ASSERT_EQUAL_INT(403, rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"limits":[{"id":"overtemp","channel":"ref_signal","condition":"above",
                     "high":300,"action":"trip_all","enabled":false}]})").status);

  const ApiResponse confirmed = rig.call(HttpMethod::kPut, "/api/v1/control", "",
      R"({"limits":[],"loops":[],"password":"bench-password"})");
  TEST_ASSERT_EQUAL_INT(200, confirmed.status);
  TEST_ASSERT_EQUAL_UINT(0, rig.safety.count());
}

static void test_guessing_the_password_stops_being_free() {
  ApiRig rig;
  rig.signIn();
  rig.post("/api/v1/auth/logout", "{}");

  for (int attempt = 0; attempt < 5; ++attempt) {
    TEST_ASSERT_EQUAL_INT(401,
        rig.post("/api/v1/auth/login", R"({"password":"guess"})").status);
  }
  // Five wrong answers and the door stops opening for a minute — otherwise a
  // short password on an open network is a five-second problem.  The lock is
  // global, and that is affordable precisely because the emergency stop does
  // not go through here.
  const ApiResponse locked =
      rig.post("/api/v1/auth/login", R"({"password":"bench-password"})");
  TEST_ASSERT_EQUAL_INT(401, locked.status);
  TEST_ASSERT_TRUE(rig.get("/api/v1/auth").body["locked"].as<bool>());
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/outputs/trip", "{}").status);

  rig.clock.advanceMicros(61000000);
  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/auth/login", R"({"password":"bench-password"})").status);
}

static void test_a_full_session_table_never_locks_the_owner_out() {
  ApiRig rig;
  rig.signIn();
  // Four browsers, then a fifth.  The instrument does not refuse the person
  // holding the password because some tabs were left open somewhere: the
  // oldest session goes, and the answer says so.
  for (int i = 0; i < 4; ++i) {
    rig.cookie.clear();
    const ApiResponse again =
        rig.post("/api/v1/auth/login", R"({"password":"bench-password"})");
    TEST_ASSERT_EQUAL_INT(200, again.status);
  }
  rig.cookie.clear();
  const ApiResponse fifth =
      rig.post("/api/v1/auth/login", R"({"password":"bench-password"})");
  TEST_ASSERT_EQUAL_INT(200, fifth.status);
  TEST_ASSERT_TRUE(fifth.body["evicted_a_session"].as<bool>());
  TEST_ASSERT_EQUAL_INT(200, rig.get("/api/v1/auth").status);
}

static void test_a_session_is_not_forever() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/channels/heat/write",
                                      R"({"value": 10})").status);

  // Twelve hours later the tab left open on the shared screen is no longer a
  // key to the instrument.
  rig.clock.advanceMicros(13ULL * 3600ULL * 1000000ULL);
  TEST_ASSERT_EQUAL_INT(401, rig.post("/api/v1/channels/heat/write",
                                      R"({"value": 20})").status);
}

static void test_the_password_never_leaves_in_an_export() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn("bench-password");

  const ApiResponse exported = rig.get("/api/v1/config/export");
  std::string text;
  serializeJson(exported.body, text);
  // The credential is not a ConfigSection, so the export loop cannot reach it
  // even by accident — this test is what keeps that structural.
  TEST_ASSERT_TRUE(text.find("bench-password") == std::string::npos);
  TEST_ASSERT_TRUE(text.find("\"hash\"") == std::string::npos);
  TEST_ASSERT_TRUE(text.find("\"salt\"") == std::string::npos);
  TEST_ASSERT_TRUE(rig.backend.exists(AuthManager::kPath));
}

static void test_an_import_is_confirmed_and_keeps_what_it_replaced() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();

  const ApiResponse exported = rig.get("/api/v1/config/export");
  std::string document;
  serializeJson(exported.body, document);

  // A session is not enough: an import replaces every section, interlocks
  // included, on a rig that may be running.
  TEST_ASSERT_EQUAL_INT(403,
      rig.post("/api/v1/config/import", document.c_str()).status);

  std::string confirmed = document;
  confirmed.insert(1, R"("password":"bench-password",)");
  const ApiResponse imported = rig.post("/api/v1/config/import", confirmed.c_str());
  TEST_ASSERT_EQUAL_INT(200, imported.status);
  TEST_ASSERT_TRUE(imported.body["backup_saved"].as<bool>());

  // And what was there before is still readable: one file, replaced by each
  // import, which is an undo for the most destructive thing this API can do.
  const ApiResponse backup = rig.get("/api/v1/config/backup");
  TEST_ASSERT_EQUAL_INT(200, backup.status);
  TEST_ASSERT_TRUE(!backup.body["sections"]["devices"].isNull());
}

// ===========================================================================
//  Milestone 11 acceptance criterion
// ===========================================================================
//  The instrument can be locked without ever locking away the stop button, and
//  replacing its firmware is refused while it is doing something.
static void test_a_locked_instrument_still_stops_and_will_not_be_reflashed_mid_run() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();

  TEST_ASSERT_EQUAL_INT(201, rig.post("/api/v1/experiments", kEvaporation).status);
  TEST_ASSERT_EQUAL_INT(202,
      rig.post("/api/v1/experiments/evaporation/actions/start",
               R"({"operator":"AM"})").status);
  run(rig, 1000000, 250000, 30.0f);

  // Signed in, password confirmed — and still refused, because the rig is
  // running.  Authorisation is not the only question an OTA has to answer.
  const ApiResponse busy = rig.post("/api/v1/firmware/ota",
                                    R"({"password":"bench-password"})");
  TEST_ASSERT_EQUAL_INT(409, busy.status);
  TEST_ASSERT_EQUAL_STRING("RESOURCE_BUSY", busy.body["error"]["code"]);

  // The stop works without signing in at all.
  rig.post("/api/v1/auth/logout", "{}");
  TEST_ASSERT_EQUAL_INT(200,
      rig.post("/api/v1/experiments/evaporation/actions/stop", "{}").status);
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/outputs/trip", "{}").status);

  // With the rig idle, the OTA still needs the person and not just the browser.
  rig.post("/api/v1/auth/login", R"({"password":"bench-password"})");
  const ApiResponse unconfirmed = rig.post("/api/v1/firmware/ota", "{}");
  TEST_ASSERT_EQUAL_INT(403, unconfirmed.status);

  const ApiResponse confirmed = rig.post("/api/v1/firmware/ota",
                                         R"({"password":"bench-password"})");
  // The policy passed; the host has no partition to write, and says so rather
  // than pretending it flashed something.
  TEST_ASSERT_EQUAL_STRING("NOT_SUPPORTED", confirmed.body["error"]["code"]);
}


// ===========================================================================
//  Milestone 16 — the house network over REST.
//
//  ADR-0022 in test form: changing the network must never make the instrument
//  unreachable, and the password must never come back out.
// ===========================================================================

static void test_the_wifi_password_never_appears_in_any_response() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();

  rig.network.state.configured = true;
  rig.network.state.ssid.assign("HomeWiFi");
  rig.network.state.state = NetworkState::kStationConnected;
  rig.network.state.stationConnected = true;
  rig.network.state.stationIp.assign("192.168.1.74");
  rig.network.state.rssi = -57;

  const ApiResponse status = rig.get("/api/v1/network");
  TEST_ASSERT_EQUAL_INT(200, status.status);
  TEST_ASSERT_EQUAL_STRING("STA_CONNECTED", status.body["state"]);
  TEST_ASSERT_EQUAL_STRING("HomeWiFi", status.body["ssid"]);
  TEST_ASSERT_TRUE(status.body["password_set"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("192.168.1.74", status.body["station"]["ip"]);
  // The address that works AND the friendly name — never only the name, which
  // plenty of machines cannot resolve.
  TEST_ASSERT_EQUAL_STRING("lab-controller-a1b2c3.local", status.body["mdns"]);

  // Send a real password through, then look for it everywhere it could surface.
  const ApiResponse accepted = rig.post("/api/v1/network/connect",
      R"({"ssid":"HomeWiFi","password":"super-secret-8"})");
  TEST_ASSERT_EQUAL_INT(202, accepted.status);
  TEST_ASSERT_EQUAL_STRING("super-secret-8", rig.network.lastPassword.c_str());

  for (const char* route : {"/api/v1/network", "/api/v1/diagnostics",
                            "/api/v1/config/export"}) {
    std::string text;
    serializeJson(rig.get(route).body, text);
    TEST_ASSERT_TRUE(text.find("super-secret-8") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("\"password\"") == std::string::npos);
  }
}

static void test_connect_answers_immediately_and_refuses_a_second_attempt() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();

  // 202, not 200: accepted, not finished.  A handler that waited for the join
  // would stall the very poll the page uses to watch for the result.
  const ApiResponse first = rig.post("/api/v1/network/connect",
      R"({"ssid":"HomeWiFi","password":"good-password"})");
  TEST_ASSERT_EQUAL_INT(202, first.status);
  TEST_ASSERT_TRUE(first.body["accepted"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("STA_CONNECTING", first.body["state"]);
  TEST_ASSERT_EQUAL_INT(1, rig.network.connectCalls);

  // A second attempt while one is in flight is a conflict, not a bad request:
  // trampling the first one's pending credentials is how a working network
  // gets lost.
  const ApiResponse second = rig.post("/api/v1/network/connect",
      R"({"ssid":"Other","password":"another-one"})");
  TEST_ASSERT_EQUAL_INT(409, second.status);
  TEST_ASSERT_EQUAL_INT(1, rig.network.connectCalls);
}

static void test_bad_network_requests_are_refused_before_the_radio() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();

  const ApiResponse empty =
      rig.post("/api/v1/network/connect", R"({"ssid":""})");
  TEST_ASSERT_TRUE(empty.isError());
  TEST_ASSERT_EQUAL_STRING("ssid", empty.body["error"]["field"]);

  std::string longSsid(40, 'x');
  const ApiResponse tooLong = rig.post("/api/v1/network/connect",
      (std::string(R"({"ssid":")") + longSsid + R"("})").c_str());
  TEST_ASSERT_EQUAL_INT(413, tooLong.status);

  // WPA2 will not take a shorter key, so refusing here turns a fifteen-second
  // timeout into an immediate, specific answer.
  const ApiResponse shortPassword = rig.post("/api/v1/network/connect",
      R"({"ssid":"HomeWiFi","password":"short"})");
  TEST_ASSERT_TRUE(shortPassword.isError());
  TEST_ASSERT_EQUAL_STRING("password", shortPassword.body["error"]["field"]);

  // An open network legitimately has no password.
  const ApiResponse open =
      rig.post("/api/v1/network/connect", R"({"ssid":"OpenNet"})");
  TEST_ASSERT_EQUAL_INT(202, open.status);

  TEST_ASSERT_EQUAL_INT(1, rig.network.connectCalls);
}

static void test_changing_the_network_needs_the_right_to_change_settings() {
  ApiRig rig;
  prepareRig(rig);

  // A password exists and nobody is signed in.  Setting one ends every session,
  // including the one that set it, so this leaves the rig genuinely signed out.
  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/auth/password",
      R"({"password":"bench-password"})").status);

  // Reading is fine; rearranging the instrument's connectivity is not — it is
  // the one setting that can put the device out of reach.
  TEST_ASSERT_EQUAL_INT(200, rig.get("/api/v1/network").status);

  TEST_ASSERT_EQUAL_INT(401, rig.post("/api/v1/network/connect",
      R"({"ssid":"HomeWiFi","password":"good-password"})").status);
  TEST_ASSERT_EQUAL_INT(401,
      rig.call(HttpMethod::kDelete, "/api/v1/network/config", "", "").status);
  TEST_ASSERT_EQUAL_INT(401, rig.call(HttpMethod::kPut,
      "/api/v1/network/hostname", "", R"({"hostname":"lab-reactor"})").status);

  TEST_ASSERT_EQUAL_INT(0, rig.network.connectCalls);
  TEST_ASSERT_EQUAL_INT(0, rig.network.clearCalls);
}

static void test_a_hostname_is_validated_the_same_way_everywhere() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();

  const char* refused[] = {"", "-leading", "trailing-", "Upper", "has space",
                           "has.dot", "has_underscore",
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
  for (const char* name : refused) {
    char body[96];
    std::snprintf(body, sizeof(body), R"({"hostname":"%s"})", name);
    const ApiResponse response =
        rig.call(HttpMethod::kPut, "/api/v1/network/hostname", "", body);
    TEST_ASSERT_TRUE(response.isError());
    TEST_ASSERT_EQUAL_STRING("hostname", response.body["error"]["field"]);
    // And the predicate the API used is the one the firmware uses, so the two
    // cannot drift into disagreeing about what mDNS will accept.
    TEST_ASSERT_FALSE(INetworkManager::hostnameIsValid(name));
  }

  const ApiResponse accepted = rig.call(HttpMethod::kPut,
      "/api/v1/network/hostname", "", R"({"hostname":"lab-reactor-2"})");
  TEST_ASSERT_EQUAL_INT(200, accepted.status);
  TEST_ASSERT_EQUAL_STRING("lab-reactor-2", accepted.body["hostname"]);
  TEST_ASSERT_TRUE(INetworkManager::hostnameIsValid("lab-reactor-2"));
}

static void test_forgetting_the_network_says_where_to_find_the_device() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();
  rig.network.state.configured = true;
  rig.network.state.ssid.assign("HomeWiFi");

  const ApiResponse cleared =
      rig.call(HttpMethod::kDelete, "/api/v1/network/config", "", "");
  TEST_ASSERT_EQUAL_INT(200, cleared.status);
  TEST_ASSERT_TRUE(cleared.body["cleared"].as<bool>());
  TEST_ASSERT_EQUAL_INT(1, rig.network.clearCalls);
  // The answer carries the address the operator has to move to.  Telling them
  // the old one is gone without saying where the new one is would be the same
  // as making the instrument unreachable.
  TEST_ASSERT_EQUAL_STRING("AP_ONLY", cleared.body["state"]);
  TEST_ASSERT_EQUAL_STRING("192.168.4.1", cleared.body["ip"]);
  TEST_ASSERT_EQUAL_STRING("LAB-CONTROLLER-A1B2C3", cleared.body["ssid"]);
}

static void test_a_scan_reports_what_it_found_and_does_not_restart_itself() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();

  NetworkCandidate strong;
  strong.ssid.assign("HomeWiFi");
  strong.rssi = -48;
  strong.channel = 6;
  strong.secured = true;
  NetworkCandidate weak;
  weak.ssid.assign("Guest");
  weak.rssi = -71;
  weak.channel = 11;
  weak.secured = true;
  rig.network.candidates = {strong, weak};

  TEST_ASSERT_EQUAL_INT(200, rig.post("/api/v1/network/scan", "").status);

  const ApiResponse results = rig.get("/api/v1/network/scan");
  TEST_ASSERT_EQUAL_INT(200, results.status);
  TEST_ASSERT_EQUAL_STRING("COMPLETE", results.body["state"]);
  TEST_ASSERT_EQUAL_UINT(2, results.body["networks"].size());
  TEST_ASSERT_EQUAL_STRING("HomeWiFi", results.body["networks"][0]["ssid"]);
  TEST_ASSERT_EQUAL_INT(-48, results.body["networks"][0]["rssi"].as<int>());
  TEST_ASSERT_TRUE(results.body["networks"][0]["secured"].as<bool>());

  // Scanning while a connection is being proved would take the radio away from
  // the attempt the operator is waiting on.
  rig.network.busy = true;
  TEST_ASSERT_EQUAL_INT(409, rig.post("/api/v1/network/scan", "").status);
}

static void test_a_build_without_a_radio_says_so_rather_than_pretending() {
  ApiRig rig;
  prepareRig(rig);
  rig.signIn();
  // A host build, or a board with no Wi-Fi.  Reporting a network nobody can
  // configure would be worse than admitting there is none.
  RestApi::Services withoutRadio = rig.makeApiServices();
  withoutRadio.network = nullptr;
  RestApi bare(withoutRadio);

  ApiRequest incoming;
  incoming.method = HttpMethod::kGet;
  incoming.path = "/api/v1/network";
  ApiResponse outgoing;
  bare.handle(incoming, outgoing);
  TEST_ASSERT_EQUAL_INT(501, outgoing.status);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_path_is_split_into_segments);
  RUN_TEST(test_path_percent_decodes_keys);
  RUN_TEST(test_query_flags_are_parsed);
  RUN_TEST(test_errors_map_onto_honest_http_statuses);
  RUN_TEST(test_error_envelope_has_a_stable_shape);
  RUN_TEST(test_modules_endpoint_carries_everything_a_form_needs);
  RUN_TEST(test_gpio_endpoint_explains_why_a_pin_is_unavailable);
  RUN_TEST(test_unknown_routes_and_methods_are_refused_cleanly);
  RUN_TEST(test_system_and_diagnostics_report_real_numbers);
  RUN_TEST(test_system_names_the_controller_independently_of_its_address);
  RUN_TEST(test_a_continuous_log_needs_a_collector_to_start);
  RUN_TEST(test_the_segment_routes_refuse_what_they_cannot_prove);
  RUN_TEST(test_a_single_mode_log_reports_no_offload_queue);
  RUN_TEST(test_reboot_requires_post_and_calls_the_hook);
  RUN_TEST(test_dry_run_validates_without_creating_anything);
  RUN_TEST(test_dry_run_and_create_agree_about_a_taken_key);
  RUN_TEST(test_a_device_that_failed_at_boot_is_reported_not_swallowed);
  RUN_TEST(test_bad_json_is_a_400_with_a_reason);
  RUN_TEST(test_a_device_that_fails_to_start_is_rolled_back_out_of_the_file);
  RUN_TEST(test_a_sensor_is_added_configured_and_removed_over_http);
  RUN_TEST(test_processing_chain_is_applied_and_persisted_over_http);
  RUN_TEST(test_configuration_export_and_import_round_trip);
  RUN_TEST(test_telemetry_batches_many_updates_into_one_frame);
  RUN_TEST(test_telemetry_sends_only_subscribed_channels);
  RUN_TEST(test_quality_is_sent_only_when_it_changes);
  RUN_TEST(test_a_busy_socket_drops_the_frame_but_not_the_data);
  RUN_TEST(test_device_errors_reach_the_socket_as_events);
  RUN_TEST(test_solve_reports_the_fit_without_storing_it);
  RUN_TEST(test_a_fit_that_cannot_be_made_says_so);
  RUN_TEST(test_history_is_kept_and_a_rollback_is_one_call);
  RUN_TEST(test_an_uncalibrated_channel_reports_raw_as_calibrated);
  RUN_TEST(test_editing_the_pipeline_does_not_drop_the_calibration);
  RUN_TEST(test_calibration_survives_a_reboot);
  RUN_TEST(test_a_load_cell_is_calibrated_from_three_weights_in_the_browser);
  RUN_TEST(test_a_dashboard_is_stored_and_read_back_whole);
  RUN_TEST(test_the_firmware_checks_the_shape_and_not_the_widget);
  RUN_TEST(test_a_widget_pointing_at_a_deleted_channel_is_named_not_dropped);
  RUN_TEST(test_dashboards_are_bounded_because_the_partition_is);
  RUN_TEST(test_a_dashboard_is_built_saved_and_survives_a_reboot);
  RUN_TEST(test_an_output_comes_up_in_its_safe_state_and_not_before);
  RUN_TEST(test_a_command_expires_and_the_output_lets_go);
  RUN_TEST(test_a_heater_cannot_be_driven_past_its_power_limit);
  RUN_TEST(test_a_relay_refuses_to_be_switched_faster_than_it_can_survive);
  RUN_TEST(test_contact_protection_can_never_block_a_release_to_safe);
  RUN_TEST(test_a_fan_is_never_commanded_to_a_speed_it_cannot_turn_at);
  RUN_TEST(test_a_device_that_stops_running_lets_go_of_its_output);
  RUN_TEST(test_the_master_stop_drops_everything_and_stays_dropped);
  RUN_TEST(test_a_channel_that_is_not_a_registered_output_cannot_be_commanded);
  RUN_TEST(test_a_heater_is_added_switched_on_and_never_left_on);
  RUN_TEST(test_a_running_loop_keeps_its_own_command_alive);
  RUN_TEST(test_a_loop_that_loses_its_sensor_lets_go_of_the_heater);
  RUN_TEST(test_an_integrator_that_saturated_can_still_come_back);
  RUN_TEST(test_two_entries_with_one_id_are_refused);
  RUN_TEST(test_a_rule_needs_hysteresis_and_holds_its_relay);
  RUN_TEST(test_an_interlock_trips_on_a_sensor_that_stopped_reporting);
  RUN_TEST(test_three_layers_and_none_of_them_trusts_the_others);
  RUN_TEST(test_a_wait_without_a_deadline_cannot_be_saved);
  RUN_TEST(test_a_scenario_that_logs_nothing_cannot_say_it_logs);
  RUN_TEST(test_a_run_needs_somebody_to_own_it);
  RUN_TEST(test_a_scenario_runs_to_the_end_and_the_record_says_so);
  RUN_TEST(test_a_wait_that_never_ends_aborts_and_says_where);
  RUN_TEST(test_an_interlock_aborts_the_run_and_takes_the_heater_with_it);
  RUN_TEST(test_a_paused_run_holds_and_does_not_spend_its_hold_time);
  RUN_TEST(test_stopping_by_hand_is_not_the_same_as_finishing);
  RUN_TEST(test_a_scenario_that_names_something_missing_will_not_start);
  RUN_TEST(test_a_run_interrupted_is_a_run_that_says_it_was_interrupted);
  RUN_TEST(test_a_dataset_says_what_it_was_measured_with);
  RUN_TEST(test_a_session_that_cannot_fit_is_refused_before_it_starts);
  RUN_TEST(test_a_full_medium_stops_the_log_and_never_the_rig);
  RUN_TEST(test_rows_the_medium_could_not_take_are_counted);
  RUN_TEST(test_the_export_is_a_stream_and_not_a_document);
  RUN_TEST(test_a_dataset_is_only_ever_deleted_by_name);
  RUN_TEST(test_an_experiment_records_itself_and_the_file_can_be_taken_away);
  RUN_TEST(test_an_instrument_without_a_password_is_open_and_says_so);
  RUN_TEST(test_the_emergency_stop_never_asks_who_you_are);
  RUN_TEST(test_stopping_a_run_does_not_need_a_session_either);
  RUN_TEST(test_removing_an_interlock_takes_more_than_a_session);
  RUN_TEST(test_guessing_the_password_stops_being_free);
  RUN_TEST(test_a_full_session_table_never_locks_the_owner_out);
  RUN_TEST(test_a_session_is_not_forever);
  RUN_TEST(test_the_password_never_leaves_in_an_export);
  RUN_TEST(test_an_import_is_confirmed_and_keeps_what_it_replaced);
  RUN_TEST(test_a_locked_instrument_still_stops_and_will_not_be_reflashed_mid_run);
  RUN_TEST(test_the_wifi_password_never_appears_in_any_response);
  RUN_TEST(test_connect_answers_immediately_and_refuses_a_second_attempt);
  RUN_TEST(test_bad_network_requests_are_refused_before_the_radio);
  RUN_TEST(test_changing_the_network_needs_the_right_to_change_settings);
  RUN_TEST(test_a_hostname_is_validated_the_same_way_everywhere);
  RUN_TEST(test_forgetting_the_network_says_where_to_find_the_device);
  RUN_TEST(test_a_scan_reports_what_it_found_and_does_not_restart_itself);
  RUN_TEST(test_a_build_without_a_radio_says_so_rather_than_pretending);
  return UNITY_END();
}
