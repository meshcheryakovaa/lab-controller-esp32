#include "app/SystemManager.h"

namespace lc {

const char* toString(BootMode mode) {
  switch (mode) {
    case BootMode::kNormal: return "NORMAL";
    case BootMode::kSafe:   return "SAFE";
  }
  return "UNKNOWN";
}

Micros SystemManager::uptimeUs() const {
  const Micros now = s_.clock->nowMicros();
  return (now > startedAtUs_) ? (now - startedAtUs_) : 0;
}

void SystemManager::enterSafeMode(const Error& reason) {
  report_.mode = BootMode::kSafe;
  report_.safeModeReason = reason;
  // Safe mode means the configuration could not be trusted.  Nothing that can
  // act is allowed to act until somebody has looked at it.
  if (s_.outputs != nullptr) s_.outputs->trip("controller started in safe mode");

  Event event;
  event.type = EventType::kSystemMessage;
  event.severity = 4;
  event.code = reason.code;
  event.detail = "booted in safe mode";
  event.timestamp = s_.clock->nowMicros();
  s_.events->publish(event);
}

const BootReport& SystemManager::begin() {
  if (begun_) return report_;
  begun_ = true;
  startedAtUs_ = s_.clock->nowMicros();

  registerBuiltinModules(*s_.registry);
  s_.devices->setBusProvider(s_.buses);

  // The safety layer comes up FIRST, before the retry task, before storage and
  // long before any device exists.  An output whose safe state is not yet being
  // enforced must not be creatable for even one scheduler pass (ADR-0016).
  if (s_.outputs != nullptr) {
    s_.devices->setOutputManager(s_.outputs);
    const Status outputsReady = s_.outputs->begin();
    if (!outputsReady.ok()) {
      enterSafeMode(outputsReady);
      return report_;
    }
  }

  // Limits next, and before the controllers by construction: SafetyManager
  // runs at kSafety and ControlManager at kControl, but the scheduler can only
  // order tasks that exist, so the registration order is part of the guarantee.
  if (s_.safety != nullptr) {
    const Status safetyReady = s_.safety->begin();
    if (!safetyReady.ok()) {
      enterSafeMode(safetyReady);
      return report_;
    }
  }
  // The experiment engine registers before the control layer, so that within a
  // pass the scenario decides and the regulator acts on that decision rather
  // than on the previous one.  It is at kControl, not kSafety: an experiment is
  // not a safety mechanism and must not be given a priority that says it is.
  if (s_.logger != nullptr) {
    if (s_.logStore != nullptr) s_.logger->setSink(s_.logStore);
    const Status loggerReady = s_.logger->begin();
    if (!loggerReady.ok()) {
      enterSafeMode(loggerReady);
      return report_;
    }
  }
  if (s_.experiments != nullptr) {
    if (s_.runLog != nullptr) s_.experiments->setRunSink(s_.runLog);
    if (s_.logger != nullptr) s_.experiments->setLogger(s_.logger);
    const Status experimentsReady = s_.experiments->begin();
    if (!experimentsReady.ok()) {
      enterSafeMode(experimentsReady);
      return report_;
    }
  }
  if (s_.control != nullptr) {
    const Status controlReady = s_.control->begin();
    if (!controlReady.ok()) {
      enterSafeMode(controlReady);
      return report_;
    }
  }

  // The retry task and the output sink must exist before any device is added.
  const Status devicesReady = s_.devices->begin();
  if (!devicesReady.ok()) {
    enterSafeMode(devicesReady);
    return report_;
  }
  const Status processingReady = s_.processing->begin();
  if (!processingReady.ok()) {
    enterSafeMode(processingReady);
    return report_;
  }

  // Repeated failures mean the stored configuration is what is killing us.
  // Coming up without devices is the only way the user can fix it remotely.
  if (s_.bootCounter->consecutiveFailures() >= kMaxConsecutiveFailures) {
    enterSafeMode(fail(ErrorCode::kConfigCorrupt,
                       "too many failed boots; devices not started"));
    return report_;
  }
  s_.bootCounter->markAttempt();

  const Status storageReady = s_.storage->begin();
  report_.storageMounted = storageReady.ok();
  if (!storageReady.ok()) {
    report_.storageError = storageReady;
    // Not fatal: an unreadable filesystem still leaves an API the user can talk
    // to, and a configuration they can re-import.
    enterSafeMode(storageReady);
    return report_;
  }

  // After storage is mounted, before anything can be changed: an instrument
  // that comes up unprotected because its credential file was read too early
  // is an instrument that is unprotected.
  if (s_.auth != nullptr) {
    const Status credentials = s_.auth->begin();
    if (!credentials.ok()) report_.storageError = credentials;
  }
  if (s_.logStore != nullptr) {
    const Status logsReady = s_.logStore->begin();
    if (!logsReady.ok()) report_.storageError = logsReady;
  }
  if (s_.runLog != nullptr) {
    s_.runLog->begin(*s_.scheduler);
    // A run that was in progress when the controller stopped gets its record
    // written now, as ABORTED.  Nothing resumes: the controller cannot know how
    // long it was away or what the rig did meanwhile (ADR-0018).
    const Status recovered = s_.runLog->recoverInterrupted();
    if (!recovered.ok()) report_.storageError = recovered;
  }

  loadAndApply();
  return report_;
}

void SystemManager::configureBuses() {
  if (s_.busConfigurator == nullptr || busesConfigured_) return;
  busesConfigured_ = true;

  JsonDocument document;
  const Status loaded = s_.storage->load(ConfigSection::kSystem, document);
  if (!loaded.ok()) {
    if (loaded.code != ErrorCode::kNotFound) report_.storageError = loaded;
    return;  // no system.json yet: no buses, and that is a valid empty rig
  }

  JsonArrayConst list = document["buses"]["i2c"].as<JsonArrayConst>();
  if (list.isNull()) return;

  for (JsonObjectConst entry : list) {
    const Status status = s_.busConfigurator->configureI2c(
        static_cast<std::uint8_t>(entry["index"] | 0),
        static_cast<std::uint8_t>(entry["sda"] | 0),
        static_cast<std::uint8_t>(entry["scl"] | 0),
        static_cast<std::uint32_t>(entry["frequency"] | 400000));
    if (status.ok()) {
      ++report_.buses.applied;
      continue;
    }
    // A bus that will not start is worth reporting loudly: every sensor on it
    // is about to fail with a much less useful message.
    ++report_.buses.failed;
    if (report_.buses.failed == 1) report_.buses.firstError = status;

    Event event;
    event.type = EventType::kSystemMessage;
    event.severity = 3;
    event.code = status.code;
    event.detail = "an I2C bus could not be started";
    s_.events->publish(event);
  }
}

void SystemManager::loadAndApply() {
  configureBuses();

  {
    JsonDocument document;
    const Status loaded = s_.storage->load(ConfigSection::kDevices, document);
    if (loaded.ok()) {
      report_.devicesConfigPresent = true;
      report_.devices = s_.applier->applyDevices(document.as<JsonObjectConst>());
    } else if (loaded.code != ErrorCode::kNotFound) {
      // Corrupt or from-the-future configuration: report it loudly and come up
      // empty rather than half-applying something we do not understand.
      report_.storageError = loaded;
      Event event;
      event.type = EventType::kSystemMessage;
      event.severity = 4;
      event.code = loaded.code;
      event.detail = "devices.json could not be loaded";
      s_.events->publish(event);
    }
  }

  // Calibrations before pipelines, and both after devices: a calibration needs
  // its channel to exist, and a pipeline needs the calibration to resolve its
  // coefficients from.  The document stays on the stack for both steps so the
  // file is read once.
  {
    JsonDocument calibrations;
    const Status loadedCalibrations =
        s_.storage->load(ConfigSection::kCalibrations, calibrations);
    if (loadedCalibrations.ok()) {
      report_.calibrationsConfigPresent = true;
      report_.calibrations =
          s_.applier->applyCalibrations(calibrations.as<JsonObjectConst>());
    } else if (loadedCalibrations.code != ErrorCode::kNotFound) {
      report_.storageError = loadedCalibrations;
    }

    JsonDocument document;
    const Status loaded = s_.storage->load(ConfigSection::kProcessing, document);
    if (loaded.ok()) {
      report_.processingConfigPresent = true;
      report_.processing = s_.applier->applyProcessing(
          document.as<JsonObjectConst>(), calibrations.as<JsonObjectConst>());
    } else if (loaded.code != ErrorCode::kNotFound) {
      report_.storageError = loaded;
    }
  }

  // Control last: a loop needs its channels, and a limit that installs before
  // the channel it watches exists would trip on the first pass.  Loading it
  // here — after devices, calibrations and pipelines — means "the sensor is
  // missing" and "the sensor reads 400 °C" stay different statements.
  {
    JsonDocument document;
    const Status loaded = s_.storage->load(ConfigSection::kControl, document);
    if (loaded.ok()) {
      report_.controlConfigPresent = true;
      report_.control = s_.applier->applyControl(document.as<JsonObjectConst>());
    } else if (loaded.code != ErrorCode::kNotFound) {
      report_.storageError = loaded;
      // A control file we cannot parse means limits we cannot install.  Coming
      // up with regulators and without interlocks is the one combination this
      // milestone exists to prevent.
      enterSafeMode(fail(ErrorCode::kConfigCorrupt,
                         "control.json could not be loaded; outputs are held"));
    }
  }

  if (report_.devices.failed > 0 || report_.processing.failed > 0) {
    Event event;
    event.type = EventType::kSystemMessage;
    event.severity = 3;
    event.code = report_.devices.firstError.code;
    event.detail = "some devices could not be started";
    event.timestamp = s_.clock->nowMicros();
    s_.events->publish(event);
  }
}

Status SystemManager::reloadConfiguration() {
  // A running scenario is commanding channels that are about to be destroyed.
  // It stops first, and its record says the rig was rebuilt underneath it —
  // which is a different thing from an operator pressing stop, and the dataset
  // has to be able to tell them apart.
  if (s_.experiments != nullptr && s_.experiments->busy()) {
    s_.experiments->stop(StopReason::kReconfigured);
  }
  // A dataset records channels by handle, and the handles are about to be
  // reissued to different measurements.  Closing it here means the file says
  // where it stopped instead of continuing under new management.
  if (s_.logger != nullptr && s_.logger->recording()) {
    s_.logger->stop(LogStopReason::kShutdown);
  }
  // Controllers stop before the devices they command are torn down, and
  // clearAll() releases whatever they were holding.  The other order leaves a
  // loop commanding a channel whose driver is being deleted underneath it.
  if (s_.control != nullptr) s_.control->clearAll();
  if (s_.safety != nullptr) s_.safety->clearAll();
  s_.processing->clearAll();
  s_.devices->removeAll();

  report_.devices = ApplyReport{};
  report_.processing = ApplyReport{};
  report_.calibrations = ApplyReport{};
  report_.control = ApplyReport{};
  report_.controlConfigPresent = false;
  if (s_.calibrations != nullptr) s_.calibrations->clearAll();
  // Buses are NOT torn down or restarted on reload: pins claimed by a bus
  // outlive the devices hanging off it, and re-running Wire.begin() on a live
  // controller is a good way to hang it.  Changing bus pins requires a reboot,
  // and the UI says so.
  report_.devicesConfigPresent = false;
  report_.processingConfigPresent = false;
  report_.calibrationsConfigPresent = false;

  loadAndApply();

  Event event;
  event.type = EventType::kConfigChanged;
  event.detail = "configuration reloaded";
  event.timestamp = s_.clock->nowMicros();
  s_.events->publish(event);

  return report_.devices.failed == 0 ? ok() : report_.devices.firstError;
}

void SystemManager::loop() {
  s_.scheduler->runPass();
  s_.events->drainPending();

  if (!healthyMarked_ && uptimeUs() >= kHealthyUptimeUs) {
    // Survived long enough to be trusted: clear the failure streak so a single
    // bad boot months ago does not eventually force safe mode.
    s_.bootCounter->markSuccess();
    healthyMarked_ = true;
  }
}

}  // namespace lc
