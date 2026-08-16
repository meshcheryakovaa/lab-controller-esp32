#include "storage/CalibrationStore.h"

#include <cmath>
#include <cstring>

#include "storage/JsonUtils.h"

namespace lc {
namespace {

void setField(LabelString* field, const char* name) {
  if (field != nullptr) field->assign(name);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Parsing
// ---------------------------------------------------------------------------
Status CalibrationStore::parseDraft(JsonObjectConst body, CalibrationDraft& out,
                                    LabelString* offendingField) {
  if (body.isNull()) return fail(ErrorCode::kInvalidArgument, "empty body");

  const char* channel = body["channel"] | "";
  if (channel[0] == '\0') {
    setField(offendingField, "channel");
    return fail(ErrorCode::kInvalidArgument, "channel is required");
  }
  if (!out.channel.assign(channel)) {
    setField(offendingField, "channel");
    return fail(ErrorCode::kInvalidArgument, "channel key is too long");
  }

  if (!parseCalibrationKind(body["kind"] | "linear", out.kind)) {
    setField(offendingField, "kind");
    return fail(ErrorCode::kInvalidArgument,
                "kind: offset | linear | poly2 | poly3 | table");
  }

  JsonArrayConst points = body["points"].as<JsonArrayConst>();
  if (points.isNull() || points.size() == 0) {
    setField(offendingField, "points");
    return fail(ErrorCode::kCalibrationInsufficientPoints, "no points given");
  }
  if (points.size() > kMaxCalibrationPoints) {
    setField(offendingField, "points");
    return fail(ErrorCode::kInvalidArgument, "at most 16 reference points");
  }

  out.pointCount = 0;
  for (JsonObjectConst point : points) {
    if (point.isNull()) {
      setField(offendingField, "points");
      return fail(ErrorCode::kInvalidArgument, "a point is not an object");
    }
    if (point["raw"].isNull() || point["reference"].isNull()) {
      setField(offendingField, "points");
      return fail(ErrorCode::kInvalidArgument,
                  "every point needs raw and reference");
    }
    const float raw = point["raw"].as<float>();
    const float reference = point["reference"].as<float>();
    // NaN in a reference point poisons the whole normal-equations system and
    // comes back as a fit of NaNs that looks like a successful calibration.
    if (!std::isfinite(raw) || !std::isfinite(reference)) {
      setField(offendingField, "points");
      return fail(ErrorCode::kInvalidArgument, "a point is not a finite number");
    }
    out.points[out.pointCount].raw = raw;
    out.points[out.pointCount].reference = reference;
    ++out.pointCount;
  }

  const std::size_t required =
      (out.kind == CalibrationKind::kTable) ? 2u
                                            : polynomialOrderFor(out.kind) + 1u;
  if (out.pointCount < required) {
    setField(offendingField, "points");
    return fail(ErrorCode::kCalibrationInsufficientPoints,
                "this fit needs more reference points");
  }

  if (!body["unit"].isNull()) {
    if (!out.unit.assign(body["unit"] | "")) {
      setField(offendingField, "unit");
      return fail(ErrorCode::kInvalidArgument, "unit is too long");
    }
  }
  if (!body["precision"].isNull()) {
    const int precision = body["precision"].as<int>();
    if (precision < 0 || precision > 6) {
      setField(offendingField, "precision");
      return fail(ErrorCode::kInvalidArgument, "precision must be 0..6");
    }
    out.precision = static_cast<std::uint8_t>(precision);
  }
  out.minimum = body["min"] | 0.0f;
  out.maximum = body["max"] | 0.0f;
  if (!body["note"].isNull() && !out.note.assign(body["note"] | "")) {
    setField(offendingField, "note");
    return fail(ErrorCode::kInvalidArgument, "note is too long");
  }
  return ok();
}

Result<PolynomialFit> CalibrationStore::solve(const CalibrationDraft& draft) {
  if (draft.kind == CalibrationKind::kTable) {
    // A table interpolates through its points, so it does not have a fit and
    // it does not have residuals.  Saying R² = 1 here would be technically
    // true and completely misleading: it would sit next to a polynomial's
    // honest 0.9993 and look better.
    PolynomialFit identity;
    identity.order = 1;
    identity.coefficients[0] = 0.0;
    identity.coefficients[1] = 1.0;
    identity.xCenter = 0.0;
    identity.xScale = 1.0;
    identity.rmsResidual = 0.0;
    identity.maxResidual = 0.0;
    identity.rSquared = 0.0;

    for (std::uint8_t i = 1; i < draft.pointCount; ++i) {
      if (!(draft.points[i].raw > draft.points[i - 1].raw)) {
        return fail(ErrorCode::kCalibrationSingular,
                    "table points must be sorted by increasing raw value");
      }
    }
    return identity;
  }

  if (draft.kind == CalibrationKind::kOffset) {
    return CalibrationSolver::fitOffset(draft.points, draft.pointCount);
  }
  return CalibrationSolver::fitPolynomial(draft.points, draft.pointCount,
                                          polynomialOrderFor(draft.kind));
}

// ---------------------------------------------------------------------------
//  Serialisation
// ---------------------------------------------------------------------------
void CalibrationStore::serializeFit(const PolynomialFit& fit, JsonObject out) {
  JsonArray coefficients = out["coefficients"].to<JsonArray>();
  for (std::size_t i = 0; i <= fit.order; ++i) coefficients.add(fit.coefficients[i]);
  out["order"] = fit.order;
  out["x_center"] = fit.xCenter;
  out["x_scale"] = fit.xScale;
  out["rms_residual"] = fit.rmsResidual;
  out["max_residual"] = fit.maxResidual;
  out["r_squared"] = fit.rSquared;
}

void CalibrationStore::serializeResiduals(const CalibrationDraft& draft,
                                          const PolynomialFit& fit,
                                          JsonArray out) {
  const bool isTable = (draft.kind == CalibrationKind::kTable);
  for (std::uint8_t i = 0; i < draft.pointCount; ++i) {
    JsonObject entry = out.add<JsonObject>();
    entry["raw"] = draft.points[i].raw;
    entry["reference"] = draft.points[i].reference;
    const double predicted =
        isTable ? draft.points[i].reference : fit.evaluate(draft.points[i].raw);
    entry["predicted"] = predicted;
    entry["residual"] = draft.points[i].reference - predicted;
  }
}

// ---------------------------------------------------------------------------
//  Document access
// ---------------------------------------------------------------------------
JsonArray CalibrationStore::calibrationsArray(JsonDocument& document) {
  JsonArray existing = document["calibrations"].as<JsonArray>();
  if (!existing.isNull()) return existing;
  document["schemaVersion"] = 1;
  return document["calibrations"].to<JsonArray>();
}

JsonArrayConst CalibrationStore::calibrationsArray(const JsonDocument& document) {
  return document["calibrations"].as<JsonArrayConst>();
}

JsonObjectConst CalibrationStore::findById(const JsonDocument& document,
                                           const char* id) {
  if (id == nullptr) return JsonObjectConst();
  JsonArrayConst records = calibrationsArray(document);
  if (records.isNull()) return JsonObjectConst();
  for (JsonObjectConst record : records) {
    const char* stored = record["id"] | "";
    if (std::strcmp(stored, id) == 0) return record;
  }
  return JsonObjectConst();
}

JsonObject CalibrationStore::findByIdMutable(JsonDocument& document,
                                             const char* id) {
  JsonArray records = document["calibrations"].as<JsonArray>();
  if (records.isNull() || id == nullptr) return JsonObject();
  for (JsonObject record : records) {
    if (std::strcmp(record["id"] | "", id) == 0) return record;
  }
  return JsonObject();
}

JsonObjectConst CalibrationStore::findActive(JsonArrayConst records,
                                             const char* channelKey) {
  if (records.isNull() || channelKey == nullptr) return JsonObjectConst();
  for (JsonObjectConst record : records) {
    if (!(record["active"] | false)) continue;
    if (std::strcmp(record["channel"] | "", channelKey) != 0) continue;
    return record;
  }
  return JsonObjectConst();
}

std::uint16_t CalibrationStore::nextVersion(const JsonDocument& document,
                                            const char* channelKey) {
  std::uint16_t highest = 0;
  JsonArrayConst records = calibrationsArray(document);
  if (records.isNull() || channelKey == nullptr) return 1;
  for (JsonObjectConst record : records) {
    if (std::strcmp(record["channel"] | "", channelKey) != 0) continue;
    const std::uint16_t version = record["version"] | 0;
    if (version > highest) highest = version;
  }
  // Versions never restart, even after every record of a channel was deleted:
  // an id that has ever identified a dataset must not come back meaning
  // something else.
  return static_cast<std::uint16_t>(highest + 1);
}

void CalibrationStore::deactivateChannel(JsonDocument& document,
                                         const char* channelKey) {
  JsonArray records = document["calibrations"].as<JsonArray>();
  if (records.isNull() || channelKey == nullptr) return;
  for (JsonObject record : records) {
    if (std::strcmp(record["channel"] | "", channelKey) != 0) continue;
    record["active"] = false;
  }
}

bool CalibrationStore::removeById(JsonDocument& document, const char* id) {
  JsonArray records = document["calibrations"].as<JsonArray>();
  if (records.isNull() || id == nullptr) return false;
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (std::strcmp(records[i]["id"] | "", id) != 0) continue;
    records.remove(i);
    return true;
  }
  return false;
}

std::size_t CalibrationStore::removeChannel(JsonDocument& document,
                                            const char* channelKey) {
  JsonArray records = document["calibrations"].as<JsonArray>();
  if (records.isNull() || channelKey == nullptr) return 0;
  std::size_t removed = 0;
  for (std::size_t i = records.size(); i-- > 0;) {
    if (std::strcmp(records[i]["channel"] | "", channelKey) != 0) continue;
    records.remove(i);
    ++removed;
  }
  return removed;
}

Status CalibrationStore::append(JsonDocument& document,
                                const CalibrationDraft& draft,
                                const PolynomialFit& fit, const char* id,
                                std::uint16_t version, EpochMs createdEpochMs,
                                bool active, JsonObject* written) {
  JsonArray records = calibrationsArray(document);
  if (records.isNull()) return fail(ErrorCode::kStorageFailure, "calibrations");

  JsonObject record = records.add<JsonObject>();
  if (record.isNull()) return fail(ErrorCode::kOutOfCapacity, "calibrations");

  record["id"] = jsonCopy(id);
  record["channel"] = jsonCopy(draft.channel.c_str());
  record["version"] = version;
  record["kind"] = toString(draft.kind);
  if (!draft.unit.empty()) record["unit"] = jsonCopy(draft.unit.c_str());
  if (draft.precision > 0) record["precision"] = draft.precision;
  if (draft.maximum > draft.minimum) {
    record["min"] = draft.minimum;
    record["max"] = draft.maximum;
  }
  if (!draft.note.empty()) record["note"] = jsonCopy(draft.note.c_str());
  record["created_epoch_ms"] = createdEpochMs;
  record["active"] = active;

  JsonArray points = record["points"].to<JsonArray>();
  for (std::uint8_t i = 0; i < draft.pointCount; ++i) {
    JsonObject point = points.add<JsonObject>();
    point["raw"] = draft.points[i].raw;
    point["reference"] = draft.points[i].reference;
  }
  serializeFit(fit, record["fit"].to<JsonObject>());

  if (written != nullptr) *written = record;
  return ok();
}

// ---------------------------------------------------------------------------
//  Turning a record into a running stage
// ---------------------------------------------------------------------------
Status CalibrationStore::stageConfig(JsonObjectConst record, JsonObject out) {
  if (record.isNull()) return fail(ErrorCode::kNotFound, "calibration record");

  CalibrationKind kind = CalibrationKind::kLinear;
  if (!parseCalibrationKind(record["kind"] | "linear", kind)) {
    return fail(ErrorCode::kConfigCorrupt, "unknown calibration kind");
  }

  // The stage carries the id so that GET /processing shows which calibration a
  // channel is actually running, not merely that it is calibrated.
  out["calibration_id"] = jsonCopy(record["id"] | "");

  if (kind == CalibrationKind::kTable) {
    out["type"] = "table";
    JsonArray x = out["table_x"].to<JsonArray>();
    JsonArray y = out["table_y"].to<JsonArray>();
    JsonArrayConst points = record["points"].as<JsonArrayConst>();
    if (points.isNull() || points.size() < 2) {
      return fail(ErrorCode::kCalibrationInsufficientPoints, "table needs 2 points");
    }
    for (JsonObjectConst point : points) {
      x.add(point["raw"].as<float>());
      y.add(point["reference"].as<float>());
    }
    return ok();
  }

  JsonObjectConst fit = record["fit"].as<JsonObjectConst>();
  JsonArrayConst coefficients = fit["coefficients"].as<JsonArrayConst>();
  if (coefficients.isNull() || coefficients.size() == 0) {
    return fail(ErrorCode::kConfigCorrupt, "calibration has no coefficients");
  }
  out["type"] = "polynomial";
  JsonArray copied = out["coefficients"].to<JsonArray>();
  for (JsonVariantConst value : coefficients) copied.add(value.as<double>());
  // Coefficients are meaningless without the transform they were fitted in —
  // see the header of CalibrationSolver.h.  Carrying one without the other is
  // how a calibration silently becomes wrong by orders of magnitude.
  out["x_center"] = fit["x_center"] | 0.0;
  out["x_scale"] = fit["x_scale"] | 1.0;
  return ok();
}

Status CalibrationStore::toActive(JsonObjectConst record,
                                  ActiveCalibration& out) {
  if (record.isNull()) return fail(ErrorCode::kNotFound, "calibration record");
  if (!out.channel.assign(record["channel"] | "")) {
    return fail(ErrorCode::kConfigCorrupt, "channel key too long");
  }
  if (!out.id.assign(record["id"] | "")) {
    return fail(ErrorCode::kConfigCorrupt, "calibration id too long");
  }
  if (!parseCalibrationKind(record["kind"] | "linear", out.kind)) {
    return fail(ErrorCode::kConfigCorrupt, "unknown calibration kind");
  }
  out.version = record["version"] | 0;
  out.unit.assign(record["unit"] | "");
  JsonArrayConst points = record["points"].as<JsonArrayConst>();
  out.pointCount = points.isNull() ? 0
                                   : static_cast<std::uint8_t>(points.size());

  JsonObjectConst fit = record["fit"].as<JsonObjectConst>();
  out.rmsResidual = fit["rms_residual"] | 0.0f;
  out.maxResidual = fit["max_residual"] | 0.0f;
  out.rSquared = fit["r_squared"] | 0.0f;
  out.createdEpochMs = record["created_epoch_ms"] | 0ULL;
  return ok();
}

Result<bool> CalibrationStore::installStage(JsonObject pipeline) {
  if (pipeline.isNull()) return fail(ErrorCode::kInvalidArgument, "no pipeline");

  JsonArray stages = pipeline["stages"].as<JsonArray>();
  if (stages.isNull()) stages = pipeline["stages"].to<JsonArray>();

  for (JsonObject stage : stages) {
    if (std::strcmp(stage["type"] | "", "calibration") != 0) continue;
    stage["config"].to<JsonObject>()["source"] = "active";
    return false;  // it was already there
  }

  if (stages.size() + 1 > limits::kMaxProcessorsPerChannel) {
    return fail(ErrorCode::kProcessorChainTooLong,
                "no room for a calibration stage");
  }

  // No calibration stage yet: put one at the FRONT.  Calibration converts the
  // sensor's own units into physical ones, and every filter after it then
  // works in those units — a moving average of ADC counts followed by a
  // calibration is not the same number, and the difference is not obvious.
  JsonDocument scratch;
  JsonObject inserted = scratch.to<JsonObject>();
  inserted["type"] = "calibration";
  inserted["config"].to<JsonObject>()["source"] = "active";

  stages.add(JsonObject());
  for (std::size_t i = stages.size(); i-- > 1;) stages[i] = stages[i - 1];
  stages[0] = inserted;
  return true;
}

Status CalibrationStore::resolvePipeline(JsonObjectConst stored,
                                         JsonObjectConst record,
                                         JsonDocument& out) {
  JsonObject target = out.to<JsonObject>();
  if (!stored.isNull()) {
    if (!target.set(stored)) {
      return fail(ErrorCode::kOutOfCapacity, "pipeline is too large to resolve");
    }
  }

  JsonArray stages = target["stages"].as<JsonArray>();
  if (stages.isNull()) return ok();

  for (JsonObject stage : stages) {
    if (std::strcmp(stage["type"] | "", "calibration") != 0) continue;
    JsonObject config = stage["config"].as<JsonObject>();

    // Only a PLACEHOLDER is resolved.  A stage that carries its own
    // coefficients — written by hand, or imported from another board — is left
    // exactly as it is: silently overwriting somebody's explicit numbers with
    // "whatever is active" would be the worst kind of helpfulness.
    if (std::strcmp(config["source"] | "", "active") != 0) continue;

    config.clear();
    if (record.isNull()) {
      // No active calibration: pass through, so `calibrated == raw`, which is
      // precisely what "not calibrated yet" means and is visible as such.
      config["type"] = "identity";
      continue;
    }
    const Status filled = stageConfig(record, config);
    if (!filled.ok()) return filled;
  }
  return ok();
}

bool CalibrationStore::removeStage(JsonObject pipeline) {
  if (pipeline.isNull()) return false;
  JsonArray stages = pipeline["stages"].as<JsonArray>();
  if (stages.isNull()) return false;
  for (std::size_t i = 0; i < stages.size(); ++i) {
    if (std::strcmp(stages[i]["type"] | "", "calibration") != 0) continue;
    stages.remove(i);
    return true;
  }
  return false;
}

}  // namespace lc
