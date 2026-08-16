// =============================================================================
//  storage/RunLog.h — what actually happened, and in what configuration (§48).
//
//  A run record is not configuration and it is not the dataset.  It is the
//  answer to the question somebody asks a year later: "measured with what, in
//  what state, and did it actually finish?"  So it lives in /data, it is NOT
//  part of the configuration export (a record of what THIS rig did is
//  meaningless on another one), and it is written for every run — finished,
//  stopped or aborted.
//
//  AN ABORTED RUN MUST NOT LOOK LIKE A FINISHED ONE.
//  The record carries the final state, the reason, the step it reached, and the
//  error that ended it.  A half-run that reads as complete is worse than no
//  record at all, because it is the one that gets published.
//
//  WRITING HAPPENS LATER, ON PURPOSE.
//  `onRunFinished()` is called from the scheduler's control pass.  A LittleFS
//  write there would block every task behind it, including the safety pass —
//  in a cooperative scheduler a long write is a long stall for everyone.  So
//  the record is copied into RAM and flushed by a kBackground task.  The rig is
//  already safe by then: the engine releases everything it touched before it
//  hands the record over.
//
//  Document shape (/data/runs.json), newest first, bounded:
//    { "schemaVersion": 1,
//      "runs": [ { "experiment": "evaporation_60c", "name": "…",
//                  "operator": "…", "sample": "…", "notes": "…",
//                  "started_epoch_ms": …, "ended_epoch_ms": …, "duration_s": …,
//                  "state": "ABORTED", "reason": "timeout",
//                  "error": { "code": "TIMEOUT", "detail": "…" },
//                  "step_reached": 3, "steps": 8,
//                  "config_revision": 12, "firmware": "1.0.0",
//                  "devices": ["balance_01", "heat_01"],
//                  "calibrations": ["mass_01#2"],
//                  "events": [ { "at_s": 12.4, "step": 3, "label": "…" } ],
//                  "events_dropped": 0 } ] }
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Scheduler.h"
#include "services/CalibrationManager.h"
#include "services/DeviceManager.h"
#include "services/ExperimentEngine.h"
#include "storage/ConfigStorage.h"
#include "storage/IStorageBackend.h"

namespace lc {

class RunLog final : public IRunSink {
 public:
  static constexpr const char* kPath = "/data/runs.json";
  static constexpr const char* kMarkerPath = "/data/current_run.json";
  static constexpr std::size_t kMaxBytes = 8 * 1024;

  RunLog(IStorageBackend& backend, ConfigStorage& storage, DeviceManager& devices,
         CalibrationManager* calibrations = nullptr)
      : backend_(backend), storage_(storage), devices_(devices),
        calibrations_(calibrations) {}

  // Creates /data and registers the background flush.
  Status begin(Scheduler& scheduler);

  void onRunStarted(const RunRecord& record) override;
  void onRunFinished(const RunRecord& record) override;

  // Called once at boot.  If a run was in progress when the controller stopped,
  // there is a marker and no record: the run is written as ABORTED with the
  // reason "restarted".  An experiment never resumes — the controller cannot
  // know how long it was away or what the rig did meanwhile (ADR-0018).
  Status recoverInterrupted();

  // Writes a pending record, if any.  Exposed so a test can force it without a
  // scheduler and so shutdown paths can flush deliberately.
  Status flushPending();
  bool pending() const { return pending_; }

  // Reads the stored records.  Returns an empty document rather than an error
  // when nothing has ever run — "no runs yet" is not a failure.
  Status load(JsonDocument& out) const;

 private:
  static void flushTrampoline(void* context);
  void serialize(const RunRecord& record, JsonObject out) const;
  Status append(JsonObjectConst record);

  IStorageBackend& backend_;
  ConfigStorage& storage_;
  DeviceManager& devices_;
  CalibrationManager* calibrations_ = nullptr;

  RunRecord pendingRecord_;
  bool pending_ = false;
  TaskId task_ = kInvalidTask;
  std::uint32_t written_ = 0;
  Error lastError_;
};

}  // namespace lc
