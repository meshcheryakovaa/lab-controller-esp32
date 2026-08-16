#include "storage/ConfigApplier.h"

#include <cstring>

#include "storage/JsonUtils.h"

#include "storage/CalibrationStore.h"
#include "storage/ControlStore.h"
#include "storage/JsonConfigView.h"

namespace lc {
namespace {

// IPipelineSource over  { "calibration_stage": 0, "stages": [ {type, config} ] }
class JsonPipelineSource final : public IPipelineSource {
 public:
  explicit JsonPipelineSource(JsonObjectConst pipeline)
      : stages_(pipeline["stages"].as<JsonArrayConst>()),
        calibrationStage_(pipeline["calibration_stage"].is<int>()
                              ? static_cast<std::int8_t>(
                                    pipeline["calibration_stage"].as<int>())
                              : kAutoCalibrationStage) {}

  std::size_t stageCount() const override {
    return stages_.isNull() ? 0 : stages_.size();
  }

  const char* stageType(std::size_t index) const override {
    return stages_[index]["type"].as<const char*>();
  }

  const IConfigView& stageConfig(std::size_t index) const override {
    // ProcessingManager consumes the view immediately inside configure(), so a
    // single reusable view is enough and costs no allocation.
    view_.bind(stages_[index]["config"].as<JsonObjectConst>());
    return view_;
  }

  std::int8_t calibrationStage() const override { return calibrationStage_; }

 private:
  JsonArrayConst stages_;
  std::int8_t calibrationStage_;
  mutable JsonConfigView view_;
};

const char* coordinateSystemName(CoordinateSystem system) {
  switch (system) {
    case CoordinateSystem::kCartesian:   return "cartesian";
    case CoordinateSystem::kCylindrical: return "cylindrical";
    case CoordinateSystem::kNone:        break;
  }
  return "none";
}

}  // namespace

// ---------------------------------------------------------------------------
//  Geometry
// ---------------------------------------------------------------------------
void ConfigApplier::parseGeometry(JsonObjectConst source, Geometry& out) {
  if (source.isNull()) return;
  const char* system = source["system"] | "none";
  if (std::strcmp(system, "cartesian") == 0) {
    out.system = CoordinateSystem::kCartesian;
  } else if (std::strcmp(system, "cylindrical") == 0) {
    out.system = CoordinateSystem::kCylindrical;
  } else {
    out.system = CoordinateSystem::kNone;
  }
  out.a = source["a"] | 0.0f;
  out.b = source["b"] | 0.0f;
  out.c = source["c"] | 0.0f;
  out.group.assign(source["group"] | "");
  out.role.assign(source["role"] | "");
}

void ConfigApplier::serializeGeometry(const Geometry& geometry, JsonObject out) {
  out["system"] = coordinateSystemName(geometry.system);
  out["a"] = geometry.a;
  out["b"] = geometry.b;
  out["c"] = geometry.c;
  if (!geometry.group.empty()) out["group"] = jsonCopy(geometry.group.c_str());
  if (!geometry.role.empty()) out["role"] = jsonCopy(geometry.role.c_str());
}

// ---------------------------------------------------------------------------
//  Device spec
// ---------------------------------------------------------------------------
Status ConfigApplier::parseDeviceSpec(JsonObjectConst entry, DeviceSpec& out) {
  const char* key = entry["key"] | "";
  if (key[0] == '\0') return fail(ErrorCode::kInvalidArgument, "device key is empty");
  if (!out.key.assign(key)) return fail(ErrorCode::kNameTooLong, "device key");

  out.name.assign(entry["name"] | "");
  out.enabled = entry["enabled"] | true;
  out.sampleIntervalUs =
      static_cast<Micros>(entry["sample_interval_us"] | 0);

  parseGeometry(entry["geometry"].as<JsonObjectConst>(), out.geometry);

  // "channels": { "<specId>": { "key": …, "name": …, "unit": …, … } }
  JsonObjectConst channels = entry["channels"].as<JsonObjectConst>();
  out.overrideCount = 0;
  if (channels.isNull()) return ok();

  for (JsonPairConst pair : channels) {
    if (out.overrideCount >= DeviceSpec::kMaxChannelsPerDevice) break;
    ChannelOverride& custom = out.overrides[out.overrideCount];
    custom = ChannelOverride{};
    custom.specId.assign(pair.key().c_str());

    JsonObjectConst source = pair.value().as<JsonObjectConst>();
    custom.key.assign(source["key"] | "");
    custom.name.assign(source["name"] | "");
    custom.unit.assign(source["unit"] | "");
    if (source["min"].is<float>() && source["max"].is<float>()) {
      custom.minimum = source["min"].as<float>();
      custom.maximum = source["max"].as<float>();
      custom.hasRange = true;
    }
    custom.precision = source["precision"].is<int>()
                           ? static_cast<std::uint8_t>(source["precision"].as<int>())
                           : 0xFF;
    custom.color = source["color"].is<unsigned>()
                       ? source["color"].as<std::uint32_t>()
                       : 0u;
    custom.logged = source["logged"] | true;
    custom.visible = source["visible"] | true;
    if (source["geometry"].is<JsonObjectConst>()) {
      parseGeometry(source["geometry"].as<JsonObjectConst>(), custom.geometry);
      custom.hasGeometry = true;
    }
    ++out.overrideCount;
  }
  return ok();
}

// ---------------------------------------------------------------------------
//  Apply
// ---------------------------------------------------------------------------
ApplyReport ConfigApplier::applyDevices(JsonObjectConst document) {
  ApplyReport report;
  JsonArrayConst list = document["devices"].as<JsonArrayConst>();
  if (list.isNull()) return report;

  for (JsonObjectConst entry : list) {
    DeviceSpec spec;
    Status status = parseDeviceSpec(entry, spec);
    LabelString field;

    if (status.ok()) {
      const char* moduleId = entry["module"] | "";
      JsonConfigView config(entry["config"].as<JsonObjectConst>());
      const Result<DeviceHandle> added =
          devices_.add(moduleId, spec, config, &field);
      status = added.ok() ? ok() : added.error();
    }

    if (status.ok()) {
      ++report.applied;
      continue;
    }
    // One bad device must not stop the rest of the rig from starting.
    ++report.failed;
    if (report.failed == 1) {
      report.firstFailedKey = spec.key;
      report.firstFailedField = field;
      report.firstError = status;
    }
  }
  return report;
}

ApplyReport ConfigApplier::applyCalibrations(JsonObjectConst document) {
  ApplyReport report;
  if (calibrations_ == nullptr) return report;
  JsonArrayConst records = document["calibrations"].as<JsonArrayConst>();
  if (records.isNull()) return report;

  for (JsonObjectConst record : records) {
    if (!(record["active"] | false)) continue;

    const char* key = record["channel"] | "";
    Status status = ok();
    ActiveCalibration active;

    const ChannelHandle handle = channels_.findByKey(key);
    if (handle == kInvalidChannel) {
      // The device that owned this channel is gone or failed to start.  The
      // record stays in the file — deleting somebody's calibration because a
      // wire fell out would be unforgivable — but it is reported, not ignored.
      status = fail(ErrorCode::kChannelNotFound, key);
    } else {
      status = CalibrationStore::toActive(record, active);
      if (status.ok()) status = calibrations_->setActive(active);
      if (status.ok()) {
        status = channels_.setPresentation(
            handle, active.unit.empty() ? nullptr : active.unit.c_str(),
            static_cast<std::uint8_t>(record["precision"] | 0),
            record["min"] | 0.0f, record["max"] | 0.0f);
      }
    }

    if (status.ok()) {
      ++report.applied;
      continue;
    }
    ++report.failed;
    if (report.failed == 1) {
      report.firstFailedKey.assign(key);
      report.firstError = status;
    }
  }
  return report;
}

ApplyReport ConfigApplier::applyProcessing(JsonObjectConst document,
                                           JsonObjectConst calibrations) {
  ApplyReport report;
  JsonObjectConst pipelines = document["pipelines"].as<JsonObjectConst>();
  if (pipelines.isNull()) return report;

  JsonArrayConst records = calibrations.isNull()
                               ? JsonArrayConst()
                               : calibrations["calibrations"].as<JsonArrayConst>();

  for (JsonPairConst pair : pipelines) {
    const ChannelHandle handle = channels_.findByKey(pair.key().c_str());
    Status status = ok();
    if (handle == kInvalidChannel) {
      status = fail(ErrorCode::kChannelNotFound, pair.key().c_str());
    } else {
      // Resolve the calibration placeholder into real coefficients.  The
      // resolved document is a stack temporary and is never written back.
      JsonDocument resolved;
      status = CalibrationStore::resolvePipeline(
          pair.value().as<JsonObjectConst>(),
          CalibrationStore::findActive(records, pair.key().c_str()), resolved);
      if (status.ok()) {
        const JsonPipelineSource source(resolved.as<JsonObjectConst>());
        status = processing_.apply(handle, source);
      }
    }

    if (status.ok()) {
      ++report.applied;
      continue;
    }
    ++report.failed;
    if (report.failed == 1) {
      report.firstFailedKey.assign(pair.key().c_str());
      report.firstError = status;
    }
  }
  return report;
}

// ---------------------------------------------------------------------------
//  Document editing (the document is the source of truth — ADR-0010)
// ---------------------------------------------------------------------------
JsonObjectConst ConfigApplier::findDevice(const JsonDocument& document,
                                          const char* key) {
  JsonArrayConst list = document["devices"].as<JsonArrayConst>();
  if (list.isNull() || key == nullptr) return JsonObjectConst();
  for (JsonObjectConst entry : list) {
    const char* candidate = entry["key"] | "";
    if (std::strcmp(candidate, key) == 0) return entry;
  }
  return JsonObjectConst();
}

Status ConfigApplier::upsertDevice(JsonDocument& document, JsonObjectConst entry) {
  const char* key = entry["key"] | "";
  if (key[0] == '\0') return fail(ErrorCode::kInvalidArgument, "device key is empty");

  JsonArray list = document["devices"].is<JsonArray>()
                       ? document["devices"].as<JsonArray>()
                       : document["devices"].to<JsonArray>();
  if (list.isNull()) return fail(ErrorCode::kInternal, "cannot create device list");

  for (JsonObject existing : list) {
    if (std::strcmp(existing["key"] | "", key) != 0) continue;
    // Replace in place so the device keeps its position in the file; a stable
    // order keeps diffs of an exported configuration readable.
    existing.clear();
    for (JsonPairConst pair : entry) existing[pair.key()] = pair.value();
    return ok();
  }

  JsonObject added = list.add<JsonObject>();
  for (JsonPairConst pair : entry) added[pair.key()] = pair.value();
  return ok();
}

bool ConfigApplier::removeDevice(JsonDocument& document, const char* key) {
  JsonArray list = document["devices"].as<JsonArray>();
  if (list.isNull() || key == nullptr) return false;
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (std::strcmp(list[i]["key"] | "", key) == 0) {
      list.remove(i);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
//  Read-only projection of the live rig (diagnostics / GET /api/v1/devices)
// ---------------------------------------------------------------------------
void ConfigApplier::describeDevices(JsonArray out) const {
  for (std::size_t i = 0; i < DeviceManager::capacity(); ++i) {
    const DeviceRecord& record = devices_.slot(i);
    if (!record.active) continue;

    JsonObject entry = out.add<JsonObject>();
    entry["handle"] = record.handle;
    entry["key"] = jsonCopy(record.key.c_str());
    entry["module"] = record.manifest->id;
    entry["name"] = jsonCopy(record.name.c_str());
    entry["state"] = toString(record.state);
    entry["sample_interval_us"] = record.sampleIntervalUs;
    if (!record.lastError.ok()) {
      JsonObject error = entry["error"].to<JsonObject>();
      error["code"] = record.lastError.symbol();
      error["numeric"] = static_cast<int>(record.lastError.code);
      error["detail"] = jsonCopy(record.lastError.detail.c_str());
    }
    if (record.geometry.isDefined()) {
      serializeGeometry(record.geometry, entry["geometry"].to<JsonObject>());
    }

    JsonArray channels = entry["channels"].to<JsonArray>();
    for (std::uint8_t c = 0; c < record.channelCount; ++c) {
      const ChannelDescriptor* descriptor =
          channels_.descriptor(record.channels[c]);
      if (descriptor == nullptr) continue;
      JsonObject channel = channels.add<JsonObject>();
      channel["handle"] = record.channels[c];
      channel["key"] = jsonCopy(descriptor->key.c_str());
      channel["unit"] = jsonCopy(descriptor->unit.c_str());
      channel["quantity"] = jsonCopy(descriptor->quantity.c_str());
      channel["precision"] = descriptor->precision;
      channel["stages"] = processing_.stageCount(record.channels[c]);
    }
  }
}

ApplyReport ConfigApplier::applyControl(JsonObjectConst document) {
  ApplyReport report;
  if (document.isNull()) return report;

  // --- limits first, and separately ---------------------------------------
  if (safety_ != nullptr) {
    JsonArrayConst limits = document["limits"].as<JsonArrayConst>();
    if (!limits.isNull()) {
      for (JsonObjectConst entry : limits) {
        SafetyLimit limit;
        LabelString field;
        Status status = ControlStore::parseLimit(entry, limit, field);
        if (status.ok()) status = safety_->add(limit);
        if (status.ok()) {
          ++report.applied;
          continue;
        }
        ++report.failed;
        if (report.failed == 1) {
          report.firstFailedKey.assign(entry["id"] | "");
          report.firstFailedField = field;
          report.firstError = status;
        }
      }
    }
  }

  if (control_ == nullptr) return report;

  JsonArrayConst loops = document["loops"].as<JsonArrayConst>();
  if (!loops.isNull()) {
    for (JsonObjectConst entry : loops) {
      ControlLoop loop;
      LabelString field;
      Status status = ControlStore::parseLoop(entry, loop, field);
      if (status.ok()) status = control_->addLoop(loop);
      if (status.ok()) {
        ++report.applied;
        continue;
      }
      ++report.failed;
      if (report.failed == 1) {
        report.firstFailedKey.assign(entry["id"] | "");
        report.firstFailedField = field;
        report.firstError = status;
      }
    }
  }

  JsonArrayConst rules = document["rules"].as<JsonArrayConst>();
  if (!rules.isNull()) {
    for (JsonObjectConst entry : rules) {
      ControlRule rule;
      LabelString field;
      Status status = ControlStore::parseRule(entry, rule, field);
      if (status.ok()) status = control_->addRule(rule);
      if (status.ok()) {
        ++report.applied;
        continue;
      }
      ++report.failed;
      if (report.failed == 1) {
        report.firstFailedKey.assign(entry["id"] | "");
        report.firstFailedField = field;
        report.firstError = status;
      }
    }
  }
  return report;
}

}  // namespace lc
