#include "storage/RunLog.h"

#include <new>

#include "storage/JsonUtils.h"

namespace lc {

Status RunLog::begin(Scheduler& scheduler) {
  const Status directory = backend_.ensureDirectory("/data");
  if (!directory.ok()) return directory;
  if (task_ != kInvalidTask) return ok();

  // kBackground and one second apart: this task exists to keep a flash write
  // out of the control pass, so putting it anywhere near the front would defeat
  // the point.  A run that ended a second ago is not waiting on anything.
  const Result<TaskId> added = scheduler.addPeriodic(
      "runlog.flush", 1000000, TaskPriority::kBackground, flushTrampoline, this);
  if (!added.ok()) return added.error();
  task_ = added.value();
  return ok();
}

void RunLog::flushTrampoline(void* context) {
  RunLog* self = static_cast<RunLog*>(context);
  if (self->pending_) self->flushPending();
}

void RunLog::onRunStarted(const RunRecord& record) {
  // A small file, written on the operator's request path rather than in a
  // scheduler pass.  It exists so that a controller which never gets to write
  // the closing record still leaves proof that a run was open.
  JsonDocument document;
  JsonObject root = document.to<JsonObject>();
  root["schemaVersion"] = LC_CONFIG_SCHEMA_VERSION;
  serialize(record, root["run"].to<JsonObject>());
  root["run"]["state"] = toString(ExperimentState::kRunning);

  const std::size_t needed = measureJson(document) + 1;
  if (needed >= kMaxBytes) return;
  char* buffer = new (std::nothrow) char[needed];
  if (buffer == nullptr) return;
  const std::size_t written = serializeJson(document, buffer, needed);
  const Status saved = backend_.writeAtomic(kMarkerPath, buffer, written);
  delete[] buffer;
  if (!saved.ok()) lastError_ = saved;
}

Status RunLog::recoverInterrupted() {
  if (!backend_.exists(kMarkerPath)) return ok();

  const Result<std::size_t> fileSize = backend_.size(kMarkerPath);
  if (!fileSize.ok() || fileSize.value() >= kMaxBytes) {
    backend_.remove(kMarkerPath);
    return fail(ErrorCode::kConfigCorrupt, kMarkerPath);
  }
  const std::size_t capacity = fileSize.value() + 1;
  char* buffer = new (std::nothrow) char[capacity];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const Result<std::size_t> read = backend_.read(kMarkerPath, buffer, capacity);
  JsonDocument marker;
  DeserializationError parsed;
  if (read.ok()) {
    parsed = deserializeJson(marker, static_cast<const char*>(buffer), read.value());
  }
  delete[] buffer;
  if (!read.ok() || parsed) {
    backend_.remove(kMarkerPath);
    return fail(ErrorCode::kConfigCorrupt, kMarkerPath);
  }

  JsonDocument record;
  JsonObject out = record.to<JsonObject>();
  for (JsonPairConst pair : marker["run"].as<JsonObjectConst>()) {
    out[pair.key()] = pair.value();
  }
  // The three fields that make this unmistakable.  An interrupted run is not a
  // finished one and never becomes one, however tempting the last known values
  // are: the controller does not know what the rig did while it was away.
  out["state"] = toString(ExperimentState::kAborted);
  out["reason"] = toString(StopReason::kRestarted);
  JsonObject error = out["error"].to<JsonObject>();
  error["code"] = errorSymbol(ErrorCode::kExperimentAborted);
  error["numeric"] = static_cast<int>(ErrorCode::kExperimentAborted);
  error["detail"] = "the controller restarted while this run was in progress";

  const Status appended = append(out);
  backend_.remove(kMarkerPath);
  return appended;
}

Status RunLog::append(JsonObjectConst record) {
  JsonDocument existing;
  const Status loaded = load(existing);
  if (!loaded.ok()) lastError_ = loaded;

  JsonDocument fresh;
  JsonObject root = fresh.to<JsonObject>();
  root["schemaVersion"] = LC_CONFIG_SCHEMA_VERSION;
  JsonArray runs = root["runs"].to<JsonArray>();
  runs.add(record);
  if (loaded.ok()) {
    for (JsonObjectConst previous : existing["runs"].as<JsonArrayConst>()) {
      if (runs.size() >= limits::kMaxRunRecords) break;
      runs.add(previous);
    }
  }

  const std::size_t needed = measureJson(fresh) + 1;
  if (needed >= kMaxBytes) return fail(ErrorCode::kPayloadTooLarge, kPath);
  char* buffer = new (std::nothrow) char[needed];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const std::size_t written = serializeJson(fresh, buffer, needed);
  const Status saved = backend_.writeAtomic(kPath, buffer, written);
  delete[] buffer;
  if (saved.ok()) ++written_;
  return saved;
}

void RunLog::onRunFinished(const RunRecord& record) {
  // Copy and return.  Everything this call is allowed to do happens in RAM: the
  // caller is a scheduler task, and the rig has already been released.
  pendingRecord_ = record;
  pending_ = true;
}

Status RunLog::load(JsonDocument& out) const {
  out.clear();
  if (!backend_.exists(kPath)) {
    JsonObject root = out.to<JsonObject>();
    root["schemaVersion"] = LC_CONFIG_SCHEMA_VERSION;
    root["runs"].to<JsonArray>();
    return ok();
  }

  const Result<std::size_t> fileSize = backend_.size(kPath);
  if (!fileSize.ok()) return fileSize.error();
  if (fileSize.value() >= kMaxBytes) return fail(ErrorCode::kPayloadTooLarge, kPath);

  const std::size_t capacity = fileSize.value() + 1;
  char* buffer = new (std::nothrow) char[capacity];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const Result<std::size_t> read = backend_.read(kPath, buffer, capacity);
  if (!read.ok()) {
    delete[] buffer;
    return read.error();
  }
  // static_cast to const char* is not cosmetic: given a mutable buffer
  // ArduinoJson parses zero-copy and keeps pointers INTO it, and this one is
  // freed two lines down (same trap as ConfigStorage::load).
  const DeserializationError parsed =
      deserializeJson(out, static_cast<const char*>(buffer), read.value());
  delete[] buffer;
  if (parsed) {
    // A corrupt run log is reported, not repaired in place: the records it
    // still holds are somebody's history, and guessing at them is worse than
    // saying the file is broken.
    return fail(ErrorCode::kConfigCorrupt, parsed.c_str());
  }
  return ok();
}

void RunLog::serialize(const RunRecord& record, JsonObject out) const {
  out["experiment"] = jsonCopy(record.experimentKey.c_str());
  out["name"] = jsonCopy(record.name.c_str());
  out["operator"] = jsonCopy(record.metadata.operatorName.c_str());
  out["sample"] = jsonCopy(record.metadata.sample.c_str());
  out["description"] = jsonCopy(record.metadata.description.c_str());
  out["notes"] = jsonCopy(record.metadata.notes.c_str());

  out["started_epoch_ms"] = record.startedEpochMs;
  out["ended_epoch_ms"] = record.endedEpochMs;
  out["duration_s"] =
      static_cast<double>(record.endedUs - record.startedUs) / 1e6;

  // The three fields that keep an aborted run from ever reading as a finished
  // one.  They are written together and never separately.
  out["state"] = toString(record.finalState);
  out["reason"] = toString(record.reason);
  out["step_reached"] = record.stepReached;
  out["steps"] = record.stepCount;
  if (!record.detail.ok()) {
    JsonObject error = out["error"].to<JsonObject>();
    error["code"] = record.detail.symbol();
    error["numeric"] = static_cast<int>(record.detail.code);
    error["detail"] = jsonCopy(record.detail.detail.c_str());
  }

  // Reproducibility: what the rig WAS, not what it is when somebody reads this.
  out["config_revision"] = storage_.revision();
  // What the configuration WAS, not how many times it had been saved: the
  // counter restarts at every boot, the fingerprint does not (§48).
  out["config_fingerprint"] = storage_.fingerprint();
  out["firmware"] = LC_FIRMWARE_VERSION;

  JsonArray devices = out["devices"].to<JsonArray>();
  for (std::size_t i = 0; i < DeviceManager::capacity(); ++i) {
    const DeviceRecord& device = devices_.slot(i);
    if (device.handle == kInvalidDevice) continue;
    devices.add(jsonCopy(device.key.c_str()));
  }

  JsonArray calibrations = out["calibrations"].to<JsonArray>();
  if (calibrations_ != nullptr) {
    for (std::size_t i = 0; i < calibrations_->count(); ++i) {
      calibrations.add(jsonCopy(calibrations_->at(i).id.c_str()));
    }
  }

  JsonArray events = out["events"].to<JsonArray>();
  for (std::size_t i = 0; i < record.eventCount; ++i) {
    JsonObject event = events.add<JsonObject>();
    event["at_s"] = static_cast<double>(record.events[i].atUs) / 1e6;
    event["step"] = record.events[i].step;
    event["label"] = jsonCopy(record.events[i].label.c_str());
  }
  // Said out loud rather than silently dropped.
  out["events_dropped"] = record.eventsDropped;
}

Status RunLog::flushPending() {
  if (!pending_) return ok();

  JsonDocument document;
  JsonObject record = document.to<JsonObject>();
  serialize(pendingRecord_, record);

  const Status appended = append(record);
  if (!appended.ok()) {
    lastError_ = appended;
    // Stays pending: the next flush tries again.  A full filesystem is a reason
    // to complain, not a reason to forget what the rig did.
    return appended;
  }

  // The run is closed, so the "a run is open" marker must go — otherwise the
  // next boot would resurrect it as an interrupted run.
  backend_.remove(kMarkerPath);
  pending_ = false;
  return ok();
}

}  // namespace lc
