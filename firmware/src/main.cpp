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
#include "platform/esp32/NvsBootCounter.h"
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
  services.reboot = requestReboot;
  return services;
}

RestApi g_api(makeApiServices());
platform::PsychicHttpAdapter g_http(g_api, g_telemetry, g_storage,
                                    g_metrics.controllerId());

// Until the web UI exists (Milestone 4), the serial console is the only place
// events can be observed.  It is a subscriber like any other, not a special case.
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

  const lc::Status http = g_http.begin();
  if (!http.ok()) {
    Serial.printf("http: %s (%s)\n", http.symbol(), http.detail.c_str());
  }

  const lc::Status telemetry =
      g_telemetry.begin(g_scheduler, g_events, g_http,
                        lc::TelemetryBatcher::kDefaultRateHz);
  if (!telemetry.ok()) {
    Serial.printf("telemetry: %s\n", telemetry.symbol());
  }
}

void loop() {
  g_system.loop();
  g_wifi.tick(millis());

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
