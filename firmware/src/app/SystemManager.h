// =============================================================================
//  app/SystemManager.h — the start-up sequence and the main loop.
//
//  `app/` is a thin layer that exists so the dependency rules stay honest:
//  services/ must not know about JSON or the filesystem, and storage/ must not
//  know about the boot order.  SystemManager is allowed to know about both, and
//  main.cpp is allowed to know only about SystemManager.
//
//  Boot sequence:
//      backend.mount ─fail─► SAFE MODE (API only, configuration editable)
//          │
//      bootCounter.markAttempt()          ← persists across a hang/reset
//          │   too many failures ────────► SAFE MODE
//          │
//      storage.begin()                    ← /config, purge interrupted writes
//          │
//      load devices.json ──► applyDevices()   one bad device does not stop the rest
//          │
//      load calibrations.json ──► applyCalibrations()
//      load processing.json ──► applyProcessing()  (resolves the coefficients)
//          │
//      devices.begin(), processing.begin()
//          │
//      (after kHealthyUptime) bootCounter.markSuccess()
// =============================================================================
#pragma once

#include "app/BootCounter.h"
#include "core/Clock.h"
#include "core/EventBus.h"
#include "core/ModuleRegistry.h"
#include "core/ResourceManager.h"
#include "core/Scheduler.h"
#include "services/ChannelManager.h"
#include "services/DeviceManager.h"
#include "services/CalibrationManager.h"
#include "services/ControlManager.h"
#include "services/AuthManager.h"
#include "services/DataLogger.h"
#include "services/ExperimentEngine.h"
#include "services/OutputManager.h"
#include "services/ProcessingManager.h"
#include "services/SafetyManager.h"
#include "storage/ConfigApplier.h"
#include "storage/ConfigStorage.h"
#include "storage/LogStore.h"
#include "storage/RunLog.h"

namespace lc {

enum class BootMode : std::uint8_t { kNormal = 0, kSafe };

const char* toString(BootMode mode);

struct BootReport {
  BootMode mode = BootMode::kNormal;
  bool storageMounted = false;
  bool devicesConfigPresent = false;
  bool processingConfigPresent = false;
  bool calibrationsConfigPresent = false;
  bool controlConfigPresent = false;
  ApplyReport buses;
  ApplyReport devices;
  ApplyReport processing;
  ApplyReport calibrations;
  ApplyReport control;
  Error storageError;
  Error safeModeReason;
};

class SystemManager {
 public:
  // A boot is considered successful once the system has run this long without
  // resetting.  Short enough to be reached in a normal session, long enough to
  // outlive a driver that hangs during initialisation.
  static constexpr Micros kHealthyUptimeUs = 30ULL * 1000000ULL;
  static constexpr std::uint8_t kMaxConsecutiveFailures = 3;

  struct Services {
    const IClock* clock = nullptr;
    ModuleRegistry* registry = nullptr;
    ResourceManager* resources = nullptr;
    ChannelManager* channels = nullptr;
    Scheduler* scheduler = nullptr;
    EventBus* events = nullptr;
    DeviceManager* devices = nullptr;
    ProcessingManager* processing = nullptr;
    // Optional: a build without calibration support simply never populates it,
    // and applyCalibrations() becomes a no-op instead of a crash.
    CalibrationManager* calibrations = nullptr;
    // The safety layer for anything that acts.  Optional only so that a
    // sensor-only build need not carry it; when present it is started BEFORE
    // any device can be created (§30, ADR-0016).
    OutputManager* outputs = nullptr;
    // Also optional, and started in this order: limits before regulators.  The
    // pass in which a loop can command must never be a pass in which the limit
    // that overrules it has not run (§30, ADR-0017).
    SafetyManager* safety = nullptr;
    ControlManager* control = nullptr;
    // Optional, and started BEFORE the control layer: an experiment decides a
    // setpoint and the loop acts on it in the same pass, not the next one.
    ExperimentEngine* experiments = nullptr;
    // Optional.  Without it a run still runs; it just leaves no record, which
    // is why the API says so rather than pretending it saved one.
    RunLog* runLog = nullptr;
    // Optional pair.  Without them a scenario with logging steps is refused
    // rather than run without recording (§33, ADR-0019).
    DataLogger* logger = nullptr;
    LogStore* logStore = nullptr;
    // Optional.  A build without it is an instrument with no password, which is
    // a decision somebody can make on a closed network — not an accident.
    AuthManager* auth = nullptr;
    ConfigStorage* storage = nullptr;
    ConfigApplier* applier = nullptr;
    IBootCounter* bootCounter = nullptr;

    // Optional: a rig made only of software modules needs neither.
    IBusProvider* buses = nullptr;
    IBusConfigurator* busConfigurator = nullptr;
  };

  explicit SystemManager(const Services& services) : s_(services) {}

  // Runs the boot sequence.  Never returns an error: a system that cannot read
  // its configuration must still come up far enough for the user to fix it.
  // The report says what actually happened.
  const BootReport& begin();

  // One pass of the cooperative loop.  Call from loop() / the control task.
  void loop();

  const BootReport& report() const { return report_; }
  BootMode mode() const { return report_.mode; }
  bool healthy() const { return healthyMarked_; }
  Micros uptimeUs() const;

  // Re-reads the configuration from storage and rebuilds the rig.  Used by the
  // REST layer after an import and by the profile switcher.
  Status reloadConfiguration();

 private:
  void enterSafeMode(const Error& reason);
  void configureBuses();
  void loadAndApply();

  Services s_;
  BootReport report_;
  Micros startedAtUs_ = 0;
  bool healthyMarked_ = false;
  bool busesConfigured_ = false;
  bool begun_ = false;
};

}  // namespace lc
