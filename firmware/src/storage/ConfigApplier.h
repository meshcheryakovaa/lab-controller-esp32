// =============================================================================
//  storage/ConfigApplier.h — turns configuration documents into a running rig,
//  and a running rig back into documents.
//
//  This is the piece that makes the Milestone 1 acceptance criterion true:
//  a device appears because it is written in devices.json, not because someone
//  called add() from main.cpp.
//
//  Partial failure is the normal case, not an exception: one sensor with a bad
//  pin must not stop the other eleven from starting.  Every applyX() therefore
//  returns a report instead of a single Status, and records the first error per
//  device so the UI can show exactly what went wrong and where.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "services/ControlManager.h"
#include "services/DeviceManager.h"
#include "services/CalibrationManager.h"
#include "services/ProcessingManager.h"
#include "services/SafetyManager.h"
#include "storage/ConfigStorage.h"

namespace lc {

struct ApplyReport {
  std::size_t applied = 0;
  std::size_t failed = 0;
  // First failure, kept for the boot log and the UI banner.
  KeyString firstFailedKey;
  LabelString firstFailedField;
  Error firstError;

  bool allApplied() const { return failed == 0; }
};

class ConfigApplier {
 public:
  ConfigApplier(DeviceManager& devices, ProcessingManager& processing,
                ChannelManager& channels,
                CalibrationManager* calibrations = nullptr)
      : devices_(devices),
        processing_(processing),
        channels_(channels),
        calibrations_(calibrations) {}

  // document = { "schemaVersion": 1, "devices": [ … ] }
  ApplyReport applyDevices(JsonObjectConst document);

  // document = { "schemaVersion": 1, "calibrations": [ … ] }
  // Fills the CalibrationManager with the active record of each channel and
  // moves the channel's unit, precision and range to match — a channel that
  // reads grams while its descriptor still says "counts" is wrong everywhere
  // downstream, including in the range check.
  ApplyReport applyCalibrations(JsonObjectConst document);

  // document = { "schemaVersion": 1, "pipelines": { "<channelKey>": {…} } }
  // `calibrations` is the calibrations document; the stored `calibration`
  // stage is a placeholder, and its coefficients are resolved from there at
  // apply time so that the numbers live in exactly one file (ADR-0014).
  ApplyReport applyProcessing(JsonObjectConst document,
                              JsonObjectConst calibrations = JsonObjectConst());

  // document = { "schemaVersion": 1, "limits": […], "loops": […], "rules": […] }
  // Limits are installed FIRST and unconditionally, before any loop or rule
  // exists.  A rig whose regulators start one pass before its interlocks is a
  // rig with no interlocks for one pass, and that is the pass in which the
  // heater it just found already at 400 °C gets commanded (§30).
  //
  // Loops always come up in mode OFF whatever the document says; that decision
  // lives in ControlManager::addLoop, not here, so it holds for every caller.
  ApplyReport applyControl(JsonObjectConst document);

  // Optional wiring: a sensor-only build simply never sets these and
  // applyControl() becomes a no-op.
  void setControl(ControlManager* control) { control_ = control; }
  void setSafety(SafetyManager* safety) { safety_ = safety; }

  // --- the document is the source of truth (ADR-0010) ----------------------
  // Live objects are a projection of devices.json, never the other way round.
  // A device record deliberately does NOT keep a copy of its driver
  // configuration: that would duplicate state and the two copies would drift.
  // Creating or editing a device is therefore read-modify-write on the stored
  // document, followed by re-applying it.
  //  These take the DOCUMENT, not a JsonObject, on purpose: `doc.as<JsonObject>()`
  //  on a freshly created document returns a NULL object, and every write
  //  through it is silently discarded.  Taking the document lets these
  //  materialise the object themselves, so the very first device ever added —
  //  when devices.json does not exist yet — cannot be lost.
  static Status upsertDevice(JsonDocument& document, JsonObjectConst entry);
  static bool removeDevice(JsonDocument& document, const char* key);
  static JsonObjectConst findDevice(const JsonDocument& document, const char* key);

  // Read-only projection of what is actually running, for the diagnostics page
  // and for GET /api/v1/devices.  Not a save path.
  void describeDevices(JsonArray out) const;

  // Parses one entry of the "devices" array.  Exposed because the REST layer
  // uses exactly the same parser for POST /api/v1/devices — one code path,
  // so a device created through the API and one loaded from disk cannot drift.
  static Status parseDeviceSpec(JsonObjectConst entry, DeviceSpec& out);
  static void parseGeometry(JsonObjectConst source, Geometry& out);
  static void serializeGeometry(const Geometry& geometry, JsonObject out);

 private:
  DeviceManager& devices_;
  ProcessingManager& processing_;
  ChannelManager& channels_;
  CalibrationManager* calibrations_ = nullptr;
  ControlManager* control_ = nullptr;
  SafetyManager* safety_ = nullptr;
};

}  // namespace lc
