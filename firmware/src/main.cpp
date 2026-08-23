// =============================================================================
//  main.cpp — Universal ESP32 Laboratory Controller.
//
//  This file does exactly two things: it constructs the object graph and it
//  runs the loop.  There is no application logic here, no device is named here,
//  and no GPIO number appears here.  A rig is described in /config/devices.json
//  and nowhere else (§50, §63).
//
//  If this file ever needs to change in order to support a new sensor, the
//  architecture has been violated.
// =============================================================================
#include <Arduino.h>

#include <cstring>

#include "api/RestApi.h"
#include "api/TelemetryBatcher.h"
#include "api/esp32/Esp32SystemMetrics.h"
#include "api/esp32/PsychicHttpAdapter.h"
#include "app/SystemManager.h"
#include "buses/esp32/Esp32BusProvider.h"
#include "platform/esp32/WifiManager.h"
#include "core/EventBus.h"
#include "core/ModuleRegistry.h"
#include "core/ResourceManager.h"
#include "core/Scheduler.h"
#include "platform/esp32/Esp32Clock.h"
#include "platform/esp32/Esp32Random.h"
#include "platform/esp32/CloudCredentialStore.h"
#include "platform/esp32/CloudUploadTask.h"
#include "platform/esp32/NvsBootCounter.h"
#include "platform/esp32/YandexAccount.h"
#include "platform/esp32/YandexDiskClient.h"
#include "platform/esp32/YandexOAuthClient.h"
#include "storage/CloudUploadQueue.h"
#include "services/ChannelManager.h"
#include "services/DeviceManager.h"
#include "services/ProcessingManager.h"
#include "storage/ConfigApplier.h"
#include "storage/ConfigStorage.h"
#include "storage/LittleFsBackend.h"

namespace {

using namespace lc;

// ---------------------------------------------------------------------------
//  Composition root.  Namespace-scope objects with known lifetimes: no `new`
//  for infrastructure, no singleton beyond the module registry, and therefore
//  no static-destruction-order surprises.
// ---------------------------------------------------------------------------
platform::Esp32Clock g_clock;
platform::LittleFsBackend g_backend;
platform::NvsBootCounter g_bootCounter;

EventBus g_events;
Scheduler g_scheduler(g_clock);
ResourceManager g_resources(ChipProfile::current());
ChannelManager g_channels(g_clock);
platform::Esp32BusProvider g_buses(g_resources);

DeviceManager g_devices(g_clock, ModuleRegistry::instance(), g_resources,
                        g_channels, g_scheduler, g_events);
ProcessingManager g_processing(ModuleRegistry::instance(), g_channels);
OutputManager g_outputs(g_clock, g_channels, g_scheduler, g_events);
SafetyManager g_safety(g_clock, g_channels, g_outputs, g_scheduler, g_events);
ControlManager g_control(g_clock, g_channels, g_outputs, g_scheduler, g_events);
ConfigStorage g_storage(g_backend, g_events);
CalibrationManager g_calibrations;
ExperimentEngine g_experiments(g_clock, g_channels, g_outputs, g_control,
                               g_devices, g_scheduler, g_events);
RunLog g_runLog(g_backend, g_storage, g_devices, &g_calibrations);
DataLogger g_logger(g_clock, g_channels, g_scheduler, g_events);
LogStore g_logStore(g_backend, g_storage, &g_calibrations);
platform::Esp32Random g_random;
AuthManager g_auth(g_clock, g_random, g_backend, g_events);
ConfigApplier g_applier(g_devices, g_processing, g_channels, &g_calibrations);

// --- M17: the cloud uploader ------------------------------------------------
// Constructed here like everything else, and like everything else main.cpp
// knows only how the pieces fit — no OAuth logic and no Yandex Disk paths.
CloudUploadQueue g_cloudQueue(g_backend);
CloudManager g_cloud(g_clock, g_cloudQueue, g_events);
platform::CloudCredentialStore g_cloudStore;
platform::YandexOAuthClient g_oauth(g_clock, g_cloudStore);
platform::YandexDiskClient g_disk(g_clock, g_oauth);
platform::YandexAccount g_cloudAccount(g_cloudStore, g_oauth, g_disk, g_cloud);
platform::CloudUploadTask g_cloudTask(g_cloud, g_oauth);

platform::Esp32SystemMetrics g_metrics;
platform::WifiManager g_wifi(g_events);
TelemetryBatcher g_telemetry(g_channels, g_clock);

void requestReboot(void*) {
  // Give the HTTP response time to leave the socket; rebooting inside the
  // handler would look to the browser like a dropped connection.
  g_scheduler.addOneShot("system.reboot", 500000, TaskPriority::kBackground,
                         [](void*) { ESP.restart(); }, nullptr);
}

SystemManager::Services makeServices() {
  SystemManager::Services services;
  services.clock = &g_clock;
  services.registry = &ModuleRegistry::instance();
  services.resources = &g_resources;
  services.channels = &g_channels;
  services.scheduler = &g_scheduler;
  services.events = &g_events;
  services.devices = &g_devices;
  services.processing = &g_processing;
  services.calibrations = &g_calibrations;
  services.outputs = &g_outputs;
  services.safety = &g_safety;
  services.control = &g_control;
  services.experiments = &g_experiments;
  services.runLog = &g_runLog;
  services.logger = &g_logger;
  services.logStore = &g_logStore;
  services.auth = &g_auth;
  services.storage = &g_storage;
  services.applier = &g_applier;
  services.bootCounter = &g_bootCounter;
  services.buses = &g_buses;
  services.busConfigurator = &g_buses;
  return services;
}

SystemManager g_system(makeServices());

RestApi::Services makeApiServices() {
  RestApi::Services services;
  services.clock = &g_clock;
  services.registry = &ModuleRegistry::instance();
  services.resources = &g_resources;
  services.channels = &g_channels;
  services.scheduler = &g_scheduler;
  services.events = &g_events;
  services.devices = &g_devices;
  services.processing = &g_processing;
  services.calibrations = &g_calibrations;
  services.outputs = &g_outputs;
  services.safety = &g_safety;
  services.control = &g_control;
  services.experiments = &g_experiments;
  services.runLog = &g_runLog;
  services.logger = &g_logger;
  services.logStore = &g_logStore;
  services.auth = &g_auth;
  services.storage = &g_storage;
  services.applier = &g_applier;
  services.system = &g_system;
  services.buses = &g_buses;
  services.metrics = &g_metrics;
  services.network = &g_wifi;
  services.cloud = &g_cloud;
  services.cloudAccount = &g_cloudAccount;
  services.reboot = requestReboot;
  return services;
}

RestApi g_api(makeApiServices());
platform::PsychicHttpAdapter g_http(g_api, g_telemetry, g_storage,
                                    g_metrics.controllerId());

// How often loop() re-attempts a web server that has not started yet.
constexpr std::uint32_t kHttpRetryMs = 500;
std::uint32_t g_lastHttpAttemptMs = 0;

// Starting the web server, reported once.  Kept out of setup() because loop()
// retries it: a controller that came up with no web interface is a controller
// somebody has to walk over to and power-cycle, which is the outcome this whole
// fix exists to prevent.
bool g_httpFailureReported = false;

bool startHttp() {
  const lc::Status http = g_http.begin();
  if (http.ok()) {
    Serial.println("http: listening on port 80");
    return true;
  }
  // Reported once.  The retry below runs twice a second, and a console that
  // repeats the same line forever is a console nobody reads the useful lines in.
  if (!g_httpFailureReported) {
    g_httpFailureReported = true;
    Serial.printf("http: %s (%s) — will keep retrying\n", http.symbol(),
                  http.detail.c_str());
  }
  return false;
}

// Until the web UI exists (Milestone 4), the serial console is the only place
// events can be observed.  It is a subscriber like any other, not a special case.
/**
 * Hands the segment that has just closed to the cloud queue (M17).
 *
 * Reads the metadata back out of LogStore rather than trusting the event to
 * carry it: the event exists to say WHEN, and the store is the only thing that
 * knows the file is finished, how big it is and what its checksum is.
 */
void queueClosedSegment(std::uint32_t sequence) {
  const LogStatus& status = g_logger.status();
  if (status.id.empty()) return;

  LogStore::SegmentInfo segment;
  if (!g_logStore.segmentInfo(status.id.c_str(), sequence, segment)) return;
  // A part a power cut interrupted has no trustworthy checksum, so it is left
  // for the operator to look at rather than sent as if it were complete.
  if (!segment.state.equals("READY")) return;

  const char* fileName = std::strrchr(segment.path.c_str(), '/');
  fileName = (fileName != nullptr) ? fileName + 1 : segment.path.c_str();

  char crc[12];
  std::snprintf(crc, sizeof(crc), "%08x",
                static_cast<unsigned>(segment.payloadCrc32));
  char segmentId[16];
  std::snprintf(segmentId, sizeof(segmentId), "p%06u",
                static_cast<unsigned>(segment.sequence));

  const Result<std::uint32_t> queued = g_cloud.enqueueSegment(
      status.id.c_str(), segmentId, segment.path.c_str(), fileName,
      segment.bytes, crc);
  if (!queued.ok()) {
    Serial.printf("cloud: %s (%s)\n", queued.error().symbol(),
                  queued.error().detail.c_str());
  }
}

void logEvent(const Event& event, void*) {
  Serial.printf("[%s] src=%u code=%s %s\n", toString(event.type),
                static_cast<unsigned>(event.source), errorSymbol(event.code),
                event.detail != nullptr ? event.detail : "");
}

void reportBoot(const BootReport& report) {
  Serial.printf("boot mode: %s\n", toString(report.mode));
  if (report.mode == BootMode::kSafe) {
    Serial.printf("  reason: %s (%s)\n", report.safeModeReason.symbol(),
                  report.safeModeReason.detail.c_str());
    return;
  }
  Serial.printf("  I2C buses:  %u started, %u failed\n",
                static_cast<unsigned>(report.buses.applied),
                static_cast<unsigned>(report.buses.failed));
  Serial.printf("  devices:    %u started, %u failed%s\n",
                static_cast<unsigned>(report.devices.applied),
                static_cast<unsigned>(report.devices.failed),
                report.devicesConfigPresent ? "" : " (no devices.json yet)");
  Serial.printf("  processing: %u applied, %u failed\n",
                static_cast<unsigned>(report.processing.applied),
                static_cast<unsigned>(report.processing.failed));
  if (report.devices.failed > 0) {
    Serial.printf("  first failure: device '%s' field '%s': %s (%s)\n",
                  report.devices.firstFailedKey.c_str(),
                  report.devices.firstFailedField.c_str(),
                  report.devices.firstError.symbol(),
                  report.devices.firstError.detail.c_str());
  }
  Serial.printf("  channels:   %u active\n",
                static_cast<unsigned>(g_channels.activeCount()));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);  // the only delay() in the firmware: letting USB-CDC enumerate

  Serial.printf("\nUniversal ESP32 Laboratory Controller %s (chip: %s)\n",
                LC_FIRMWARE_VERSION, lc::ChipProfile::current().name);

  g_events.subscribe(lc::kAllEvents, logEvent, nullptr);

  // NVS first, and here rather than in a constructor: namespace-scope objects
  // are built before Arduino initialises the NVS partition, so a counter that
  // opened itself would read zero on every boot and the safe-mode escape hatch
  // would never arm.  If this fails the instrument still runs — it simply loses
  // its memory of having crashed, and says so.
  if (!g_bootCounter.begin()) {
    Serial.println("boot counter: NVS unavailable; safe mode will not arm");
  }

  // The applier needs to know about the control layer before the boot sequence
  // reads control.json.  Wired here rather than in the constructor because the
  // control managers must be constructed after the OutputManager they command
  // through, and a reference in a constructor argument would fix that order in
  // two places instead of one.
  g_applier.setSafety(&g_safety);
  g_applier.setControl(&g_control);

  const lc::Status mounted = g_backend.mount(/*formatOnFail=*/true);
  if (!mounted.ok()) {
    Serial.printf("filesystem: %s (%s)\n", mounted.symbol(),
                  mounted.detail.c_str());
  }

  reportBoot(g_system.begin());

  // Network last: everything the API talks about already exists by now, so the
  // first request cannot catch the system half-built.
  const lc::Status network = g_wifi.begin();
  if (!network.ok()) {
    Serial.printf("network: %s (%s)\n", network.symbol(), network.detail.c_str());
  }
  Serial.printf("  network:    %s at %s\n", g_metrics.networkMode(),
                g_metrics.ipAddress());

  // The web server is started only once esp_netif is actually up and holds an
  // address.  softAP() returning success means the request was accepted, not
  // that the interface exists yet, and PsychicHttp refuses to bind to an
  // interface that is not there — so starting it straight after g_wifi.begin()
  // is a race.  A cold boot usually won it; a software reset, where everything
  // is already warm and setup() gets here sooner, usually lost it, and the
  // controller came back up with no web interface at all (0.15.1-m15).
  if (!network.ok()) {
    Serial.println("http: skipped because the network did not start");
  } else if (!g_wifi.waitUntilReady()) {
    Serial.println("http: the network interface did not come up in time;"
                   " retrying in the background");
  } else {
    Serial.println("  network:    interface ready");
    startHttp();
  }

  // --- M17: the cloud uploader ---------------------------------------------
  // After storage and network, and on its OWN task: TLS and a 100 KiB upload
  // must not run on the web server's stack (that shape produced "Stack canary
  // watchpoint triggered" in 0.15.2) nor on the cooperative scheduler, where a
  // fifteen-second network wait would sit in front of the safety pass.
  g_cloudStore.begin();
  g_oauth.reload();
  g_cloud.setProvider(&g_disk);
  g_cloud.setNetwork(&g_wifi);
  g_cloud.setStorage(&g_backend);
  g_cloud.setControllerId(g_metrics.controllerId());
  {
    lc::FixedString<160> storedRoot;
    if (g_cloudStore.rootPath(storedRoot)) g_cloud.setRoot(storedRoot.c_str());
    g_cloud.setEnabled(g_cloudStore.enabled());
  }
  const lc::Status cloud = g_cloud.begin();
  if (!cloud.ok()) {
    // A queue that will not parse stops the uploader and leaves every CSV
    // exactly where it is.  Reported loudly; nothing is deleted on a guess.
    Serial.printf("cloud: %s (%s)\n", cloud.symbol(), cloud.detail.c_str());
  }
  const lc::Status cloudTask = g_cloudTask.begin();
  if (!cloudTask.ok()) {
    Serial.printf("cloud: %s (%s)\n", cloudTask.symbol(),
                  cloudTask.detail.c_str());
  }

  // A closed segment is handed to the queue as it happens.  Subscribed rather
  // than polled so the upload starts within a second of the rotation, and
  // filtered to ONE event type so nothing else can accidentally queue a file.
  g_events.subscribe(lc::eventMask(lc::EventType::kLogSegmentReady),
                     [](const lc::Event& event, void*) {
                       if (!g_cloud.enabled()) return;
                       queueClosedSegment(event.source);
                     }, nullptr);

  const lc::Status telemetry =
      g_telemetry.begin(g_scheduler, g_events, g_http,
                        lc::TelemetryBatcher::kDefaultRateHz);
  if (!telemetry.ok()) {
    Serial.printf("telemetry: %s\n", telemetry.symbol());
  }
}

void loop() {
  g_system.loop();
  const std::uint32_t nowMs = millis();
  g_wifi.tick(nowMs);

  // If the interface was not ready in setup(), keep trying.  begin() registers
  // its routes once and retries only the start, so this costs nothing but a
  // check twice a second and cannot leak handlers.  Without it, an interface
  // that took longer than the start-up wait would leave the instrument
  // unreachable until somebody power-cycled it.
  if (!g_http.running() && nowMs - g_lastHttpAttemptMs >= kHttpRetryMs) {
    g_lastHttpAttemptMs = nowMs;
    if (g_wifi.interfaceReady()) startHttp();
  }

  // Yield to the Wi-Fi/IDF tasks instead of spinning.  Sleeping until just
  // before the next deadline keeps jitter low without burning the core.
  const lc::Micros idle = g_scheduler.microsUntilNextDue();
  if (idle > 2000) {
    delayMicroseconds(1000);
  } else if (idle > 200) {
    delayMicroseconds(static_cast<unsigned int>(idle - 100));
  } else {
    yield();
  }
}
