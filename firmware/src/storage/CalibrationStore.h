// =============================================================================
//  storage/CalibrationStore.h — calibrations.json, and everything that knows
//  its shape (§12, §35, ADR-0014).
//
//  CalibrationManager holds the ACTIVE calibration of each channel and knows
//  nothing about files.  This class holds the other half: the version history
//  as a document, the parsing of what the browser sent, and the translation of
//  a stored record into the configuration of a `calibration` pipeline stage.
//
//  Records are append-only.  Recalibrating writes a new version; it never edits
//  an old one, so a dataset recorded last Tuesday can always be traced to the
//  exact numbers that produced it, and rolling back is activating a version
//  that is still there.
//
//  Document shape:
//    { "schemaVersion": 1,
//      "calibrations": [
//        { "id": "mass_01#2", "channel": "mass_01", "version": 2,
//          "kind": "linear", "unit": "g", "precision": 2,
//          "min": 0, "max": 5000,
//          "note": "three weights, 21 degC",
//          "created_epoch_ms": 1786700000000,
//          "active": true,
//          "points": [ { "raw": 453211, "reference": 0 }, ... ],
//          "fit": { "coefficients": [...], "order": 1,
//                   "x_center": ..., "x_scale": ...,
//                   "rms_residual": ..., "max_residual": ..., "r_squared": ... }
//        } ] }
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Error.h"
#include "core/Types.h"
#include "services/CalibrationManager.h"
#include "services/CalibrationSolver.h"

namespace lc {

// What the browser sent, after parsing and before fitting.
struct CalibrationDraft {
  KeyString channel;
  CalibrationKind kind = CalibrationKind::kLinear;
  CalibrationPoint points[kMaxCalibrationPoints];
  std::uint8_t pointCount = 0;

  UnitString unit;             // empty => leave the channel's unit alone
  std::uint8_t precision = 0;  // 0     => leave the channel's precision alone
  float minimum = 0.0f;        // minimum >= maximum => no declared range
  float maximum = 0.0f;
  LabelString note;
};

class CalibrationStore {
 public:
  // --- the browser's side --------------------------------------------------
  // `offendingField` is filled on failure so the editor can highlight the
  // input rather than showing a sentence next to a form of eight fields.
  static Status parseDraft(JsonObjectConst body, CalibrationDraft& out,
                           LabelString* offendingField);

  // Fits the draft.  A table "fit" is the identity polynomial with the points
  // carried through; the residuals of a table are zero by construction and the
  // UI is told so rather than being shown a fake R² of 1.
  static Result<PolynomialFit> solve(const CalibrationDraft& draft);

  static void serializeFit(const PolynomialFit& fit, JsonObject out);
  // Per-point predicted value and residual — the only honest way to show an
  // operator WHERE a fit is bad, as opposed to how bad it is on average.
  static void serializeResiduals(const CalibrationDraft& draft,
                                 const PolynomialFit& fit, JsonArray out);

  // --- the document's side -------------------------------------------------
  static JsonArray calibrationsArray(JsonDocument& document);
  static JsonArrayConst calibrationsArray(const JsonDocument& document);

  static JsonObjectConst findById(const JsonDocument& document, const char* id);
  // Mutable counterpart, for flipping `active` in place.
  static JsonObject findByIdMutable(JsonDocument& document, const char* id);
  // The active record of a channel, straight out of a loaded array.  Used on
  // the boot path, where the document is on the stack and there is no manager
  // to ask yet.
  static JsonObjectConst findActive(JsonArrayConst records, const char* channelKey);
  static std::uint16_t nextVersion(const JsonDocument& document,
                                   const char* channelKey);
  static void deactivateChannel(JsonDocument& document, const char* channelKey);
  static bool removeById(JsonDocument& document, const char* id);
  static std::size_t removeChannel(JsonDocument& document, const char* channelKey);

  static Status append(JsonDocument& document, const CalibrationDraft& draft,
                       const PolynomialFit& fit, const char* id,
                       std::uint16_t version, EpochMs createdEpochMs,
                       bool active, JsonObject* written);

  // --- the running system's side -------------------------------------------
  // Turns a stored record into the config of a `calibration` stage.
  static Status stageConfig(JsonObjectConst record, JsonObject out);

  // Fills the in-RAM half from a stored record.
  static Status toActive(JsonObjectConst record, ActiveCalibration& out);

  // Ensures the STORED pipeline of a channel has a calibration stage.  What is
  // stored is a placeholder — {"type":"calibration","config":{"source":"active"}}
  // — and never the coefficients themselves.  Writing the numbers into
  // processing.json as well as calibrations.json would be two copies of the
  // same fact, and rolling back to an earlier version would then have to edit
  // both files in step (ADR-0010, ADR-0014).
  // Returns true if the pipeline object was changed.
  static Result<bool> installStage(JsonObject pipeline);

  // Removes the calibration stage from a pipeline. Returns true if one was there.
  static bool removeStage(JsonObject pipeline);

  // Produces the RUNNABLE pipeline: a copy of the stored one in which the
  // calibration placeholder carries the coefficients of `record`.  A null
  // record means the channel has no active calibration, and the stage becomes
  // an identity — so `calibrated == raw`, which is exactly what "uncalibrated"
  // means and is visible as such on the Channels page.
  static Status resolvePipeline(JsonObjectConst stored, JsonObjectConst record,
                                JsonDocument& out);
};

}  // namespace lc
