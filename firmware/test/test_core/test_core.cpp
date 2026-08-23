// =============================================================================
//  Unit tests for the platform-independent core.  Run with:
//      pio test -e native
// =============================================================================
#include <unity.h>

#include <cstring>

#include "core/Crc32.h"
#include "core/Format.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "core/ChipProfile.h"
#include "core/Clock.h"
#include "core/EventBus.h"
#include "core/ModuleRegistry.h"
#include "core/ResourceManager.h"
#include "core/Scheduler.h"
#include "core/Sha256.h"

// The footprint budget below needs to see every long-lived object.
#include "api/RestApi.h"
#include "api/TelemetryBatcher.h"
#include "app/SystemManager.h"
#include "services/AuthManager.h"
#include "services/CalibrationManager.h"
#include "services/ChannelManager.h"
#include "services/ControlManager.h"
#include "services/DataLogger.h"
#include "services/DeviceManager.h"
#include "services/ExperimentEngine.h"
#include "services/OutputManager.h"
#include "services/ProcessingManager.h"
#include "services/SafetyManager.h"
#include "storage/ConfigApplier.h"
#include "storage/ConfigStorage.h"
#include "storage/DashboardStore.h"
#include "storage/ExperimentStore.h"
#include "storage/LogStore.h"
#include "storage/RunLog.h"

using namespace lc;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
//  FixedString
// ---------------------------------------------------------------------------
static void test_fixed_string_truncates_and_reports() {
  FixedString<8> s;
  TEST_ASSERT_TRUE(s.assign("1234567"));       // exactly fits (7 + NUL)
  TEST_ASSERT_EQUAL_STRING("1234567", s.c_str());
  TEST_ASSERT_FALSE(s.assign("12345678"));     // one too many
  TEST_ASSERT_EQUAL_STRING("1234567", s.c_str());
  TEST_ASSERT_EQUAL_UINT(7, s.size());
}

// ---------------------------------------------------------------------------
//  EventBus
// ---------------------------------------------------------------------------
namespace {
struct Counter {
  int calls = 0;
  ErrorCode lastCode = ErrorCode::kOk;
};

void countEvent(const Event& event, void* context) {
  Counter* counter = static_cast<Counter*>(context);
  ++counter->calls;
  counter->lastCode = event.code;
}
}  // namespace

static void test_eventbus_delivers_only_subscribed_types() {
  EventBus bus;
  Counter errors;
  Counter everything;

  bus.subscribe(eventMask(EventType::kDeviceError), countEvent, &errors);
  bus.subscribe(kAllEvents, countEvent, &everything);

  Event e;
  e.type = EventType::kDeviceError;
  e.code = ErrorCode::kDeviceNotResponding;
  bus.publish(e);

  e.type = EventType::kConfigChanged;
  e.code = ErrorCode::kOk;
  bus.publish(e);

  TEST_ASSERT_EQUAL_INT(1, errors.calls);
  TEST_ASSERT_EQUAL_INT(2, everything.calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDeviceNotResponding),
                        static_cast<int>(errors.lastCode));
}

static void test_eventbus_post_is_deferred_until_drain() {
  EventBus bus;
  Counter counter;
  bus.subscribe(kAllEvents, countEvent, &counter);

  Event e;
  e.type = EventType::kAlarmTriggered;
  TEST_ASSERT_TRUE(bus.post(e));
  TEST_ASSERT_EQUAL_INT(0, counter.calls);

  TEST_ASSERT_EQUAL_UINT(1, bus.drainPending());
  TEST_ASSERT_EQUAL_INT(1, counter.calls);
}

static void test_eventbus_unsubscribe_stops_delivery() {
  EventBus bus;
  Counter counter;
  const SubscriptionId id = bus.subscribe(kAllEvents, countEvent, &counter);
  TEST_ASSERT_TRUE(bus.unsubscribe(id));
  TEST_ASSERT_FALSE(bus.unsubscribe(id));

  Event e;
  bus.publish(e);
  TEST_ASSERT_EQUAL_INT(0, counter.calls);
}

// ---------------------------------------------------------------------------
//  Scheduler
// ---------------------------------------------------------------------------
namespace {
int g_fastRuns = 0;
int g_slowRuns = 0;
int g_orderMarker = 0;
int g_safetyOrder = 0;
int g_telemetryOrder = 0;

void bumpFast(void*) { ++g_fastRuns; }
void bumpSlow(void*) { ++g_slowRuns; }
void markSafety(void*) { g_safetyOrder = ++g_orderMarker; }
void markTelemetry(void*) { g_telemetryOrder = ++g_orderMarker; }
}  // namespace

static void test_scheduler_respects_periods() {
  ManualClock clock;
  Scheduler scheduler(clock);
  g_fastRuns = 0;
  g_slowRuns = 0;

  scheduler.addPeriodic("fast", 1000, TaskPriority::kAcquisition, bumpFast, nullptr);
  scheduler.addPeriodic("slow", 10000, TaskPriority::kAcquisition, bumpSlow, nullptr);

  // 20 ms of virtual time in 1 ms steps.
  for (int i = 0; i < 20; ++i) {
    clock.advanceMicros(1000);
    scheduler.runPass(0);
  }

  TEST_ASSERT_EQUAL_INT(20, g_fastRuns);
  TEST_ASSERT_EQUAL_INT(2, g_slowRuns);
}

static void test_scheduler_runs_safety_before_telemetry() {
  ManualClock clock;
  Scheduler scheduler(clock);
  g_orderMarker = 0;

  // Registered in the "wrong" order on purpose.
  scheduler.addPeriodic("telemetry", 1000, TaskPriority::kTelemetry, markTelemetry, nullptr);
  scheduler.addPeriodic("safety", 1000, TaskPriority::kSafety, markSafety, nullptr);

  clock.advanceMicros(1000);
  scheduler.runPass(0);

  TEST_ASSERT_EQUAL_INT(1, g_safetyOrder);
  TEST_ASSERT_EQUAL_INT(2, g_telemetryOrder);
}

static void test_scheduler_does_not_burst_after_a_long_stall() {
  ManualClock clock;
  Scheduler scheduler(clock);
  g_fastRuns = 0;
  scheduler.addPeriodic("fast", 1000, TaskPriority::kAcquisition, bumpFast, nullptr);

  // Simulate a one-second blocking event, then a single pass.
  clock.advanceMicros(1000000);
  scheduler.runPass(0);

  // One catch-up execution, not a thousand.
  TEST_ASSERT_EQUAL_INT(1, g_fastRuns);
}

static void test_scheduler_one_shot_fires_once() {
  ManualClock clock;
  Scheduler scheduler(clock);
  g_slowRuns = 0;

  scheduler.addOneShot("once", 5000, TaskPriority::kControl, bumpSlow, nullptr);
  TEST_ASSERT_EQUAL_UINT(1, scheduler.taskCount());

  clock.advanceMicros(6000);
  scheduler.runPass(0);
  clock.advanceMicros(6000);
  scheduler.runPass(0);

  TEST_ASSERT_EQUAL_INT(1, g_slowRuns);
  TEST_ASSERT_EQUAL_UINT(0, scheduler.taskCount());
}

// ---------------------------------------------------------------------------
//  ResourceManager
// ---------------------------------------------------------------------------
static void test_resource_manager_reports_the_conflicting_owner() {
  ResourceManager resources(ChipProfile::esp32());

  TEST_ASSERT_TRUE(resources.claimPin(21, PinUse::kBusSignal, 0, "I2C0 SDA").ok());

  const Status conflict =
      resources.claimPin(21, PinUse::kDigitalInput, 7, "HX711 #1 DOUT");
  TEST_ASSERT_FALSE(conflict.ok());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kResourceBusy),
                        static_cast<int>(conflict.code));
  TEST_ASSERT_EQUAL_STRING("used by I2C0 SDA", conflict.detail.c_str());
}

static void test_resource_manager_rejects_impossible_pins() {
  ResourceManager resources(ChipProfile::esp32());

  // GPIO34 is input-only on the classic ESP32.
  const Status output = resources.claimPin(34, PinUse::kDigitalOutput, 1, "Heater");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kGpioInputOnly),
                        static_cast<int>(output.code));

  // GPIO7 is wired to the SPI flash.
  const Status flash = resources.claimPin(7, PinUse::kDigitalInput, 1, "Button");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kGpioReserved),
                        static_cast<int>(flash.code));

  // GPIO4 is an ADC2 pin — unusable while Wi-Fi is on.
  const Status adc2 = resources.claimPin(4, PinUse::kAnalogInput, 1, "Analog");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kAdcChannelInvalid),
                        static_cast<int>(adc2.code));

  // GPIO35 is an ADC1 pin — fine.
  TEST_ASSERT_TRUE(resources.claimPin(35, PinUse::kAnalogInput, 1, "Analog").ok());
}

static void test_resource_manager_releases_everything_a_device_owned() {
  ResourceManager resources(ChipProfile::esp32());
  const DeviceHandle device = 5;

  TEST_ASSERT_TRUE(resources.claimPin(18, PinUse::kDigitalInput, device, "DOUT").ok());
  TEST_ASSERT_TRUE(resources.claimPin(19, PinUse::kDigitalOutput, device, "SCK").ok());
  TEST_ASSERT_TRUE(resources.claim(i2cAddressResource(0, 0x76), device, "BMP280").ok());
  TEST_ASSERT_EQUAL_UINT(3, resources.claimCount());

  TEST_ASSERT_EQUAL_UINT(3, resources.releaseAllOwnedBy(device));
  TEST_ASSERT_EQUAL_UINT(0, resources.claimCount());
  TEST_ASSERT_TRUE(resources.isFree(gpioResource(18)));
}

static void test_resource_manager_detects_i2c_address_collisions() {
  ResourceManager resources(ChipProfile::esp32());
  TEST_ASSERT_TRUE(resources.claim(i2cAddressResource(0, 0x38), 1, "AHT20 #1").ok());

  // Same address, same bus -> conflict.
  const Status same = resources.claim(i2cAddressResource(0, 0x38), 2, "AHT20 #2");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kI2cAddressBusy),
                        static_cast<int>(same.code));

  // Same address on a different bus is perfectly legal.
  TEST_ASSERT_TRUE(resources.claim(i2cAddressResource(1, 0x38), 2, "AHT20 #2").ok());
}

// ---------------------------------------------------------------------------
//  ModuleRegistry
// ---------------------------------------------------------------------------
static void test_module_registry_rejects_duplicates() {
  ModuleRegistry registry;
  registerBuiltinModules(registry);

  const std::size_t before = registry.size();
  TEST_ASSERT_TRUE(before >= 3);
  TEST_ASSERT_NOT_NULL(registry.findById("sim_signal"));
  TEST_ASSERT_NOT_NULL(registry.manifestById("calibration"));
  TEST_ASSERT_NULL(registry.findById("does_not_exist"));

  registerBuiltinModules(registry);  // second pass must be a no-op
  TEST_ASSERT_EQUAL_UINT(before, registry.size());

  // The Milestone 2 catalogue.  Asserting the exact set is deliberate: a module
  // silently disappearing from the build is the failure this guards against.
  const char* expectedSensors[] = {"sim_signal", "hx711", "aht20",
                                   "bmp280", "analog_in", "digital_in"};
  for (const char* id : expectedSensors) {
    TEST_ASSERT_NOT_NULL(registry.manifestById(id));
  }
  TEST_ASSERT_EQUAL_UINT(sizeof(expectedSensors) / sizeof(expectedSensors[0]),
                         registry.countByCategory(ModuleCategory::kSensor));
  // Same for the processing library: the pipeline editor is generated from
  // these manifests, so a stage vanishing from the build silently removes it
  // from the UI as well.
  const char* expectedProcessors[] = {"calibration", "moving_average", "median",
                                      "low_pass",    "deadband",       "derivative",
                                      "integral",    "statistics",     "clamp"};
  for (const char* id : expectedProcessors) {
    TEST_ASSERT_NOT_NULL(registry.manifestById(id));
  }
  TEST_ASSERT_EQUAL_UINT(sizeof(expectedProcessors) / sizeof(expectedProcessors[0]),
                         registry.countByCategory(ModuleCategory::kProcessing));

  // And the outputs.  Every one of these declares a safe state in its manifest;
  // one silently dropping out of the build would take its safety contract with
  // it (ADR-0016).
  const char* expectedOutputs[] = {"digital_out", "relay", "pwm_out",
                                   "heater", "fan"};
  for (const char* id : expectedOutputs) {
    const ModuleManifest* manifest = registry.manifestById(id);
    TEST_ASSERT_NOT_NULL(manifest);
    TEST_ASSERT_EQUAL_UINT(1, manifest->channelCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ChannelDirection::kOutput),
                          static_cast<int>(manifest->channels[0].direction));
  }
  TEST_ASSERT_EQUAL_UINT(sizeof(expectedOutputs) / sizeof(expectedOutputs[0]),
                         registry.countByCategory(ModuleCategory::kOutput));
}

// ---------------------------------------------------------------------------
//  SHA-256, HMAC and PBKDF2 (Milestone 11)
// ---------------------------------------------------------------------------
namespace {

std::string hexOf(const std::uint8_t* data, std::size_t bytes) {
  char text[160];
  toHex(data, bytes, text, sizeof(text));
  return std::string(text);
}

}  // namespace

static void test_sha256_matches_the_published_vectors() {
  std::uint8_t digest[Sha256::kDigestBytes];

  // The password path is the one piece of this firmware where "it looks right"
  // is worth nothing.  These are the published vectors; if they pass on the
  // host with this code, they pass on the board with this code.
  // The hex is held in a NAMED string: the assertion macro copies the pointer
  // before comparing, so a temporary would be freed underneath it.
  Sha256::hash(reinterpret_cast<const std::uint8_t*>("abc"), 3, digest);
  const std::string abc = hexOf(digest, sizeof(digest));
  TEST_ASSERT_EQUAL_STRING(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      abc.c_str());

  const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  Sha256::hash(reinterpret_cast<const std::uint8_t*>(two), std::strlen(two), digest);
  const std::string twoBlocks = hexOf(digest, sizeof(digest));
  TEST_ASSERT_EQUAL_STRING(
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
      twoBlocks.c_str());

  // RFC 4231, case 1.
  std::uint8_t key[20];
  std::memset(key, 0x0b, sizeof(key));
  hmacSha256(key, sizeof(key), reinterpret_cast<const std::uint8_t*>("Hi There"), 8,
             digest);
  const std::string hmac = hexOf(digest, sizeof(digest));
  TEST_ASSERT_EQUAL_STRING(
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
      hmac.c_str());

  // RFC 6070 case 3, with SHA-256 as the PRF.
  std::uint8_t derived[32];
  pbkdf2Sha256("password", reinterpret_cast<const std::uint8_t*>("salt"), 4, 4096,
               derived, sizeof(derived));
  const std::string pbkdf2 = hexOf(derived, sizeof(derived));
  TEST_ASSERT_EQUAL_STRING(
      "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a",
      pbkdf2.c_str());
}

static void test_the_same_password_with_a_different_salt_is_a_different_hash() {
  std::uint8_t saltA[16];
  std::uint8_t saltB[16];
  std::memset(saltA, 0x01, sizeof(saltA));
  std::memset(saltB, 0x02, sizeof(saltB));

  std::uint8_t a[32];
  std::uint8_t b[32];
  pbkdf2Sha256("correct horse battery", saltA, sizeof(saltA), 1000, a, sizeof(a));
  pbkdf2Sha256("correct horse battery", saltB, sizeof(saltB), 1000, b, sizeof(b));
  // Two instruments with the same password must not have the same stored hash:
  // otherwise one leaked file is every instrument.
  TEST_ASSERT_FALSE(equalsConstantTime(a, b, sizeof(a)));

  std::uint8_t again[32];
  pbkdf2Sha256("correct horse battery", saltA, sizeof(saltA), 1000, again, sizeof(again));
  TEST_ASSERT_TRUE(equalsConstantTime(a, again, sizeof(a)));

  // Hex survives the round trip, because that is how it is stored.
  char text[80];
  toHex(a, sizeof(a), text, sizeof(text));
  std::uint8_t decoded[32];
  TEST_ASSERT_EQUAL_UINT(32, fromHex(text, decoded, sizeof(decoded)));
  TEST_ASSERT_TRUE(equalsConstantTime(a, decoded, sizeof(a)));
  TEST_ASSERT_EQUAL_UINT(0, fromHex("not hex!", decoded, sizeof(decoded)));
}

// ---------------------------------------------------------------------------
//  The static RAM budget
//
//  Milestone 12.  Raising a capacity in core/Types.h used to be free until the
//  ESP32 linker said "region `dram0_0_seg' overflowed by 10600 bytes" — a
//  message that arrives minutes later, on a toolchain not everyone has, and
//  names no object.  This test charges the bill on the host in four seconds.
//
//  The budget is expressed in HOST bytes, where a pointer is eight and the
//  ESP32's is four, so this figure is an upper bound on what the target pays.
//  64 KB here corresponds to comfortably under the ~176 KB of DRAM an ESP32
//  DevKit has for .data + .bss — the rest belongs to the heap, the Wi-Fi stack
//  and the HTTP task, and a firmware that leaves them nothing boots and then
//  dies the first time somebody opens a page.
//
//  If this fails: run tools/ram_report.cpp, which prints what each object costs
//  and which limit it scales with, and decide what the instrument gives up.
//  Raising the budget is a decision, not a fix.
// ---------------------------------------------------------------------------
static void test_the_static_footprint_stays_within_budget() {
  constexpr std::size_t kBudgetBytes = 64 * 1024;

  const std::size_t total =
      sizeof(ChannelManager) + sizeof(DeviceManager) + sizeof(ExperimentEngine) +
      sizeof(CalibrationManager) + sizeof(ResourceManager) +
      sizeof(ControlManager) + sizeof(DataLogger) + sizeof(SafetyManager) +
      sizeof(Scheduler) + sizeof(ProcessingManager) + sizeof(OutputManager) +
      sizeof(TelemetryBatcher) + sizeof(ModuleRegistry) + sizeof(EventBus) +
      sizeof(RunLog) + sizeof(LogStore) + sizeof(DashboardStore) +
      sizeof(ExperimentStore) + sizeof(ConfigStorage) + sizeof(ConfigApplier) +
      sizeof(AuthManager) + sizeof(SystemManager) + sizeof(RestApi);

  if (total > kBudgetBytes) {
    char message[160];
    std::snprintf(message, sizeof(message),
                  "static footprint is %zu bytes, budget is %zu",
                  total, kBudgetBytes);
    TEST_FAIL_MESSAGE(message);
  }
  TEST_ASSERT_TRUE(total > 0);
}


// Milestone 15: this checksum is what authorises the controller to delete a
// CSV, so the browser's implementation and this one must agree exactly.  The
// vectors below are the same ones frontend/src/lib/log-offload/log-offload.test
// asserts — two implementations that agree only by assertion are how a
// corrupted transfer ends with a deleted original.
static void test_crc32_matches_the_vectors_the_browser_uses() {
  TEST_ASSERT_EQUAL_HEX32(0x00000000u, crc32("", 0));
  TEST_ASSERT_EQUAL_HEX32(0xe8b7be43u, crc32("a", 1));
  TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, crc32("123456789", 9));
  const char* fox = "The quick brown fox jumps over the lazy dog";
  TEST_ASSERT_EQUAL_HEX32(0x414fa339u, crc32(fox, std::strlen(fox)));

  // Fed in pieces, it must give the same answer as one pass: a segment is
  // checksummed as it is written, 4 KiB at a time.
  Crc32 running;
  running.update("1234", 4);
  running.update("56789", 5);
  TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, running.value());

  // And reading the running total does not end it.
  Crc32 partial;
  partial.update("123", 3);
  const std::uint32_t peek = partial.value();
  partial.update("456789", 6);
  TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, partial.value());
  TEST_ASSERT_TRUE(peek != partial.value());
}

// ---------------------------------------------------------------------------
//  0.15.1-m15 — bounded formatting.
//
//  `used += snprintf(buffer + used, sizeof(buffer) - used, ...)` is the line
//  that rebooted the controller.  snprintf reports what it WANTED to write, so
//  one truncation puts `used` past the end and the next call is handed a
//  wrapped-around capacity of about four billion bytes.
// ---------------------------------------------------------------------------
static void test_append_format_never_walks_past_the_end() {
  char buffer[16];
  std::size_t used = 0;

  TEST_ASSERT_TRUE(appendFormat(buffer, sizeof(buffer), used, "12345"));
  TEST_ASSERT_EQUAL_UINT(5, used);

  // Does not fit: reported, and `used` stops AT the terminator rather than
  // sailing past it.  This is the assertion the old code could not make.
  TEST_ASSERT_FALSE(appendFormat(buffer, sizeof(buffer), used,
                                 "abcdefghijklmnopqrstuvwxyz"));
  TEST_ASSERT_TRUE(used < sizeof(buffer));
  TEST_ASSERT_EQUAL_UINT(sizeof(buffer) - 1, used);
  // What survived is a valid string of exactly `used` characters, so a caller
  // that writes `used` bytes to a file writes only bytes it owns.
  TEST_ASSERT_EQUAL_UINT(used, std::strlen(buffer));

  // A further append on a full buffer is refused rather than compounding.
  TEST_ASSERT_FALSE(appendFormat(buffer, sizeof(buffer), used, "!"));
  TEST_ASSERT_EQUAL_UINT(sizeof(buffer) - 1, used);
  TEST_ASSERT_EQUAL_UINT(used, std::strlen(buffer));

  // Exactly filling the buffer is success, not truncation.
  char exact[6];
  std::size_t fits = 0;
  TEST_ASSERT_TRUE(appendFormat(exact, sizeof(exact), fits, "%s", "abcde"));
  TEST_ASSERT_EQUAL_UINT(5, fits);
  TEST_ASSERT_EQUAL_STRING("abcde", exact);
}

namespace {

/** Keeps the rows the logger produces so a test can read them. */
class CapturingSink final : public ILogSink {
 public:
  Status openSession(const LogSpec&, const char* const*, std::size_t,
                     KeyString& id) override {
    id.assign("log_0001");
    return ok();
  }
  Status appendRows(const LogBatch& batch) override {
    text.append(batch.text, batch.bytes);
    return ok();
  }
  void closeSession(const LogStatus&) override {}
  std::size_t writableBytes() const override { return 8u * 1024u * 1024u; }

  std::string text;
};

}  // namespace

// ---------------------------------------------------------------------------
//  The row that smashed the stack.
//
//  A full rig, a saturated sensor and a configuration asking for more decimals
//  than a float has: ",%.*f" of 3.4e38 is over forty characters on its own, and
//  sixteen channels of it went straight past `char row[512]` — first past the
//  loop's own guard, then into the return address of the task that ran it.
// ---------------------------------------------------------------------------
static void test_a_wide_row_of_saturated_values_stays_inside_its_buffer() {
  ManualClock clock;
  ChannelManager channels(clock);
  Scheduler scheduler(clock);
  EventBus events;
  DataLogger logger(clock, channels, scheduler, events);
  CapturingSink sink;
  logger.setSink(&sink);

  LogSpec spec;
  spec.name.assign("saturated");
  spec.rateHz = 1.0f;
  spec.includeRaw = true;
  spec.storageMode = LogStorageMode::kContinuousOffload;
  spec.collectorId.assign("browser-01");

  for (std::size_t i = 0; i < limits::kMaxLoggedChannels; ++i) {
    ChannelDescriptor descriptor;
    char key[limits::kKeyLength];
    std::snprintf(key, sizeof(key), "channel_%02u", static_cast<unsigned>(i));
    descriptor.key.assign(key);
    descriptor.unit.assign("units");
    // Far more decimals than a float carries.  It arrives from a configuration
    // file, so the logger has to survive it rather than trust it.
    descriptor.precision = 200;
    descriptor.minimum = -1e38f;
    descriptor.maximum = 1e38f;
    const Result<ChannelHandle> handle = channels.create(descriptor);
    TEST_ASSERT_TRUE(handle.ok());
    spec.channels[i] = handle.value();
    // The widest number a float can be, in both columns.
    channels.publishRaw(handle.value(), 3.4028235e38f, 1000);
  }
  spec.channelCount = limits::kMaxLoggedChannels;

  TEST_ASSERT_TRUE(logger.start(spec).ok());
  for (int i = 0; i < 8; ++i) {
    clock.advanceMicros(1000000);
    logger.sampleTick(clock.nowMicros());
    // Flushed every tick, the way the scheduler does it.  These rows are about
    // 1.1 KB each, so three of them fill the 4 KiB staging buffer — leaving them
    // to pile up would measure the buffer, not the formatting.
    logger.flushTick();
  }

  // Every row arrived — none was dropped as unformattable, which is what would
  // happen if kRowBytes were still a guess rather than derived from the limits.
  TEST_ASSERT_EQUAL_UINT(0, logger.status().droppedRows);
  TEST_ASSERT_EQUAL_UINT(8, logger.status().rows);

  // And every row is WELL FORMED: the right number of columns, ending in the
  // quality mask.  A row cut short by a full buffer would still look like data.
  std::size_t rows = 0;
  std::size_t start = 0;
  while (start < sink.text.size()) {
    const std::size_t end = sink.text.find('\n', start);
    TEST_ASSERT_TRUE(end != std::string::npos);
    const std::string row = sink.text.substr(start, end - start);
    std::size_t commas = 0;
    for (const char c : row) if (c == ',') ++commas;
    // t_ms | epoch_ms | global_row | 16 channels x (raw, value) | quality_mask
    TEST_ASSERT_EQUAL_UINT(2 + 1 + limits::kMaxLoggedChannels * 2, commas);
    TEST_ASSERT_TRUE(row.size() < DataLogger::kRowBytes);
    ++rows;
    start = end + 1;
  }
  TEST_ASSERT_EQUAL_UINT(8, rows);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_append_format_never_walks_past_the_end);
  RUN_TEST(test_a_wide_row_of_saturated_values_stays_inside_its_buffer);
  RUN_TEST(test_the_static_footprint_stays_within_budget);
  RUN_TEST(test_sha256_matches_the_published_vectors);
  RUN_TEST(test_the_same_password_with_a_different_salt_is_a_different_hash);
  RUN_TEST(test_fixed_string_truncates_and_reports);
  RUN_TEST(test_eventbus_delivers_only_subscribed_types);
  RUN_TEST(test_eventbus_post_is_deferred_until_drain);
  RUN_TEST(test_eventbus_unsubscribe_stops_delivery);
  RUN_TEST(test_scheduler_respects_periods);
  RUN_TEST(test_scheduler_runs_safety_before_telemetry);
  RUN_TEST(test_scheduler_does_not_burst_after_a_long_stall);
  RUN_TEST(test_scheduler_one_shot_fires_once);
  RUN_TEST(test_resource_manager_reports_the_conflicting_owner);
  RUN_TEST(test_resource_manager_rejects_impossible_pins);
  RUN_TEST(test_resource_manager_releases_everything_a_device_owned);
  RUN_TEST(test_resource_manager_detects_i2c_address_collisions);
  RUN_TEST(test_module_registry_rejects_duplicates);
  RUN_TEST(test_crc32_matches_the_vectors_the_browser_uses);
  return UNITY_END();
}
