#include "storage/LogStore.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "storage/JsonUtils.h"

namespace lc {

Status LogStore::begin() {
  const Status directory = backend_.ensureDirectory(kDirectory);
  if (!directory.ok()) return directory;
  return ok();
}

std::size_t LogStore::writableBytes() const {
  const std::size_t free = backend_.freeBytes();
  return (free > kReserveBytes) ? (free - kReserveBytes) : 0;
}

Status LogStore::loadIndex(JsonDocument& out) const {
  out.clear();
  if (!backend_.exists(kIndexPath)) {
    JsonObject root = out.to<JsonObject>();
    root["schemaVersion"] = LC_CONFIG_SCHEMA_VERSION;
    root["next"] = 1;
    root["logs"].to<JsonArray>();
    return ok();
  }

  const Result<std::size_t> fileSize = backend_.size(kIndexPath);
  if (!fileSize.ok()) return fileSize.error();
  if (fileSize.value() >= kMaxIndexBytes) {
    return fail(ErrorCode::kPayloadTooLarge, kIndexPath);
  }
  const std::size_t capacity = fileSize.value() + 1;
  char* buffer = new (std::nothrow) char[capacity];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const Result<std::size_t> read = backend_.read(kIndexPath, buffer, capacity);
  if (!read.ok()) {
    delete[] buffer;
    return read.error();
  }
  // const char*, so ArduinoJson copies instead of pointing into a buffer that
  // is about to be freed — the trap ConfigStorage documents and RunLog fell
  // into once already.
  const DeserializationError parsed =
      deserializeJson(out, static_cast<const char*>(buffer), read.value());
  delete[] buffer;
  if (parsed) return fail(ErrorCode::kConfigCorrupt, parsed.c_str());
  return ok();
}

Status LogStore::saveIndex(JsonDocument& document) {
  const std::size_t needed = measureJson(document) + 1;
  if (needed >= kMaxIndexBytes) return fail(ErrorCode::kPayloadTooLarge, kIndexPath);
  char* buffer = new (std::nothrow) char[needed];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const std::size_t written = serializeJson(document, buffer, needed);
  const Status saved = backend_.writeAtomic(kIndexPath, buffer, written);
  delete[] buffer;
  return saved;
}

std::size_t LogStore::sessionCount() const {
  JsonDocument index;
  if (!loadIndex(index).ok()) return 0;
  return index["logs"].as<JsonArrayConst>().size();
}

Status LogStore::writeHeader(const LogSpec& spec, const char* const* columns,
                             std::size_t columnCount) {
  // Built in one buffer and written once: a header assembled by six appends is
  // a header that can end up half-written.
  char header[1024];
  std::size_t used = 0;

  used += static_cast<std::size_t>(std::snprintf(
      header + used, sizeof(header) - used,
      "# dataset: %s\n# name: %s\n# experiment: %s\n# operator: %s\n"
      "# sample: %s\n",
      currentId_.c_str(), spec.name.c_str(),
      spec.experiment.empty() ? "(manual)" : spec.experiment.c_str(),
      spec.operatorName.empty() ? "(unknown)" : spec.operatorName.c_str(),
      spec.sample.empty() ? "(unspecified)" : spec.sample.c_str()));

  used += static_cast<std::size_t>(std::snprintf(
      header + used, sizeof(header) - used,
      "# firmware: %s\n# config_fingerprint: %08x\n# config_revision: %u\n"
      "# rate_hz: %.4g\n",
      LC_FIRMWARE_VERSION, static_cast<unsigned>(storage_.fingerprint()),
      static_cast<unsigned>(storage_.revision()), static_cast<double>(spec.rateHz)));

  // Which calibrations produced the units in the column names.  Without this
  // line the numbers are grams only by assertion.
  used += static_cast<std::size_t>(std::snprintf(
      header + used, sizeof(header) - used, "# calibrations:"));
  std::size_t listed = 0;
  if (calibrations_ != nullptr) {
    for (std::size_t i = 0; i < calibrations_->count() && used < sizeof(header) - 96; ++i) {
      const ActiveCalibration& record = calibrations_->at(i);
      used += static_cast<std::size_t>(std::snprintf(
          header + used, sizeof(header) - used, "%s %s=%s",
          listed == 0 ? "" : ",", record.channel.c_str(), record.id.c_str()));
      ++listed;
    }
  }
  if (listed == 0) {
    used += static_cast<std::size_t>(std::snprintf(
        header + used, sizeof(header) - used, " none"));
  }
  used += static_cast<std::size_t>(std::snprintf(
      header + used, sizeof(header) - used, "\n"));

  used += static_cast<std::size_t>(std::snprintf(
      header + used, sizeof(header) - used, "t_ms,epoch_ms"));
  for (std::size_t i = 0; i < columnCount && used < sizeof(header) - 48; ++i) {
    used += static_cast<std::size_t>(std::snprintf(
        header + used, sizeof(header) - used, ",%s", columns[i]));
  }
  used += static_cast<std::size_t>(std::snprintf(
      header + used, sizeof(header) - used, ",quality_mask\n"));

  return backend_.append(currentPath_.c_str(), header, used);
}

Status LogStore::openSession(const LogSpec& spec, const char* const* columns,
                             std::size_t columnCount, KeyString& id) {
  if (open_) return fail(ErrorCode::kResourceBusy, "a dataset is already open");

  JsonDocument index;
  const Status loaded = loadIndex(index);
  if (!loaded.ok()) return loaded;

  JsonArray logs = index["logs"].as<JsonArray>();
  if (logs.size() >= limits::kMaxLogSessions) {
    // Refused, not made room for.  Which dataset is expendable is not a
    // decision firmware gets to make (§33).
    return fail(ErrorCode::kOutOfCapacity,
                "the log index is full; delete a dataset first");
  }

  const std::uint32_t next = index["next"] | 1;
  char buffer[limits::kKeyLength];
  std::snprintf(buffer, sizeof(buffer), "log_%04u", static_cast<unsigned>(next));
  currentId_.assign(buffer);
  id = currentId_;

  char path[64];
  std::snprintf(path, sizeof(path), "%s/%s.csv", kDirectory, currentId_.c_str());
  currentPath_.assign(path);

  const Status header = writeHeader(spec, columns, columnCount);
  if (!header.ok()) {
    backend_.remove(currentPath_.c_str());
    return header;
  }

  // The index entry is written at OPEN, not at close: a controller that loses
  // power mid-session must still leave a trace that the dataset exists, and the
  // file on disk is not self-describing to a page that lists datasets.
  JsonObject entry = logs.add<JsonObject>();
  entry["id"] = jsonCopy(currentId_.c_str());
  entry["name"] = jsonCopy(spec.name.c_str());
  entry["experiment"] = jsonCopy(spec.experiment.c_str());
  entry["operator"] = jsonCopy(spec.operatorName.c_str());
  entry["sample"] = jsonCopy(spec.sample.c_str());
  entry["path"] = jsonCopy(currentPath_.c_str());
  entry["rate_hz"] = spec.rateHz;
  entry["channels"] = spec.channelCount;
  entry["config_revision"] = storage_.revision();
  entry["config_fingerprint"] = storage_.fingerprint();
  entry["firmware"] = LC_FIRMWARE_VERSION;
  entry["state"] = "RECORDING";
  entry["rows"] = 0;
  entry["dropped"] = 0;
  entry["bytes"] = 0;
  entry["truncated"] = false;
  JsonArray columnNames = entry["columns"].to<JsonArray>();
  for (std::size_t i = 0; i < columnCount; ++i) columnNames.add(jsonCopy(columns[i]));

  index["next"] = next + 1;
  const Status saved = saveIndex(index);
  if (!saved.ok()) {
    backend_.remove(currentPath_.c_str());
    return saved;
  }

  open_ = true;
  return ok();
}

Status LogStore::appendRows(const char* text, std::size_t bytes) {
  if (!open_) return fail(ErrorCode::kInvalidState, "no dataset is open");
  return backend_.append(currentPath_.c_str(), text, bytes);
}

void LogStore::closeSession(const LogStatus& status) {
  if (!open_) return;

  // The footer.  A dataset that simply stops is a dataset somebody will assume
  // is complete, so every one of them ends with a line that says how.
  char footer[256];
  const int used = std::snprintf(
      footer, sizeof(footer),
      "# ended: %s\n# rows: %u\n# dropped_rows: %u\n%s",
      toString(status.stopReason), static_cast<unsigned>(status.rows),
      static_cast<unsigned>(status.droppedRows),
      status.truncated
          ? "# TRUNCATED: the medium reached its reserve; this dataset is incomplete\n"
          : "# complete\n");
  if (used > 0) {
    backend_.append(currentPath_.c_str(), footer, static_cast<std::size_t>(used));
  }

  JsonDocument index;
  if (loadIndex(index).ok()) {
    for (JsonObject entry : index["logs"].as<JsonArray>()) {
      if (std::strcmp(entry["id"] | "", currentId_.c_str()) != 0) continue;
      entry["state"] = status.truncated ? "TRUNCATED" : "COMPLETE";
      entry["reason"] = toString(status.stopReason);
      entry["rows"] = status.rows;
      entry["dropped"] = status.droppedRows;
      entry["bytes"] = status.bytesWritten;
      entry["truncated"] = status.truncated;
      entry["started_epoch_ms"] = status.startedEpochMs;
      entry["duration_s"] =
          static_cast<double>(status.rows) /
          (status.rateHz > 0.0f ? static_cast<double>(status.rateHz) : 1.0);
      if (!status.lastError.ok()) {
        JsonObject error = entry["error"].to<JsonObject>();
        error["code"] = status.lastError.symbol();
        error["detail"] = jsonCopy(status.lastError.detail.c_str());
      }
      break;
    }
    saveIndex(index);
  }

  open_ = false;
  currentId_.assign("");
  currentPath_.assign("");
}

bool LogStore::pathFor(const char* id, FixedString<64>& out) const {
  JsonDocument index;
  if (!loadIndex(index).ok()) return false;
  for (JsonObjectConst entry : index["logs"].as<JsonArrayConst>()) {
    if (std::strcmp(entry["id"] | "", id) != 0) continue;
    out.assign(entry["path"] | "");
    return !out.empty();
  }
  return false;
}

Status LogStore::removeSession(const char* id) {
  if (open_ && currentId_.equals(id)) {
    return fail(ErrorCode::kResourceBusy, "this dataset is being recorded");
  }

  JsonDocument index;
  const Status loaded = loadIndex(index);
  if (!loaded.ok()) return loaded;

  JsonArray logs = index["logs"].as<JsonArray>();
  for (std::size_t i = 0; i < logs.size(); ++i) {
    if (std::strcmp(logs[i]["id"] | "", id) != 0) continue;
    const char* path = logs[i]["path"] | "";
    if (path[0] != '\0') backend_.remove(path);
    logs.remove(i);
    return saveIndex(index);
  }
  return fail(ErrorCode::kNotFound, id);
}

}  // namespace lc
