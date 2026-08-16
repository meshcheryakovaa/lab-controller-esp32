#include "storage/ExperimentStore.h"

#include <cmath>
#include <cstring>

#include "storage/JsonUtils.h"

namespace lc {
namespace {

Micros secondsToMicros(double seconds) {
  if (!(seconds > 0.0)) return 0;
  return static_cast<Micros>(seconds * 1e6);
}

double microsToSeconds(Micros micros) {
  return static_cast<double>(micros) / 1e6;
}

Status parseStep(JsonObjectConst entry, ExperimentStep& out,
                 LabelString& field) {
  if (entry.isNull()) return fail(ErrorCode::kInvalidArgument, "not an object");

  if (!parseStepOp(entry["op"] | "", out.op)) {
    field.assign("op");
    // Naming the whole vocabulary in the error is deliberate: the editor is
    // generated from it, and an operator hand-editing an exported file should
    // not have to guess.
    return fail(ErrorCode::kInvalidArgument,
                "op: SET | WAIT | WAIT_UNTIL | RUN_FOR | MARK_EVENT | "
                "ENABLE | DISABLE | STOP");
  }

  switch (out.op) {
    case StepOp::kSet:
    case StepOp::kEnable:
    case StepOp::kDisable: {
      if (!out.target.assign(entry["target"] | "") || out.target.empty()) {
        field.assign("target");
        return fail(ErrorCode::kInvalidArgument, "a target is required");
      }
      out.value = entry["value"] | 0.0f;
      if (!std::isfinite(out.value)) {
        field.assign("value");
        return fail(ErrorCode::kInvalidArgument, "value is not a number");
      }
      if (!out.mode.assign(entry["mode"] | "")) {
        field.assign("mode");
        return fail(ErrorCode::kInvalidArgument, "mode is too long");
      }
      break;
    }

    case StepOp::kWaitUntil: {
      if (!out.channel.assign(entry["channel"] | "") || out.channel.empty()) {
        field.assign("channel");
        return fail(ErrorCode::kInvalidArgument, "a channel is required");
      }
      // "op2" is what the format specification called it; "comparison" is what
      // the editor sends.  Both are accepted so an exported scenario from the
      // spec can be imported unchanged.
      const char* comparison = entry["comparison"] | (entry["op2"] | ">=");
      if (!parseComparison(comparison, out.comparison)) {
        field.assign("comparison");
        return fail(ErrorCode::kInvalidArgument, "comparison: >= | <= | > | <");
      }
      out.threshold = entry["value"] | 0.0f;
      if (!std::isfinite(out.threshold)) {
        field.assign("value");
        return fail(ErrorCode::kInvalidArgument, "value is not a number");
      }
      const double timeout = entry["timeout_s"] | 0.0;
      if (!(timeout > 0.0)) {
        field.assign("timeout_s");
        // The rule that does not bend.  See ExperimentEngine.h.
        return fail(ErrorCode::kRuleInvalid,
                    "every wait needs a timeout; a scenario that can wait "
                    "forever will");
      }
      if (timeout > 86400.0) {
        field.assign("timeout_s");
        return fail(ErrorCode::kInvalidArgument, "timeout must be under a day");
      }
      out.timeoutUs = secondsToMicros(timeout);
      if (!parseOnTimeout(entry["on_timeout"] | "abort", out.onTimeout)) {
        field.assign("on_timeout");
        return fail(ErrorCode::kInvalidArgument, "on_timeout: abort | continue");
      }
      break;
    }

    case StepOp::kWait:
    case StepOp::kRunFor: {
      const double duration = entry["duration_s"] | 0.0;
      if (!(duration > 0.0) || duration > 86400.0) {
        field.assign("duration_s");
        return fail(ErrorCode::kInvalidArgument, "duration must be 0…86400 s");
      }
      out.durationUs = secondsToMicros(duration);
      break;
    }

    case StepOp::kMarkEvent:
      if (!out.label.assign(entry["label"] | "") || out.label.empty()) {
        field.assign("label");
        return fail(ErrorCode::kInvalidArgument, "an event needs a label");
      }
      break;

    case StepOp::kStop:
      break;

    case StepOp::kStartLogging:
    case StepOp::kStopLogging:
      // WHAT is recorded lives in the scenario's `logging` block, not in the
      // step: sixteen channel keys per step would be sixteen per step of every
      // scenario.  The step says when.
      break;
  }
  return ok();
}

void serializeStep(const ExperimentStep& step, JsonObject out) {
  out["op"] = toString(step.op);
  switch (step.op) {
    case StepOp::kSet:
    case StepOp::kEnable:
    case StepOp::kDisable:
      out["target"] = jsonCopy(step.target.c_str());
      if (!step.mode.empty()) out["mode"] = jsonCopy(step.mode.c_str());
      if (step.op == StepOp::kSet && step.mode.empty()) out["value"] = step.value;
      break;
    case StepOp::kWaitUntil:
      out["channel"] = jsonCopy(step.channel.c_str());
      out["comparison"] = toString(step.comparison);
      out["value"] = step.threshold;
      out["timeout_s"] = microsToSeconds(step.timeoutUs);
      out["on_timeout"] = (step.onTimeout == OnTimeout::kAbort) ? "abort" : "continue";
      break;
    case StepOp::kWait:
    case StepOp::kRunFor:
      out["duration_s"] = microsToSeconds(step.durationUs);
      break;
    case StepOp::kMarkEvent:
      out["label"] = jsonCopy(step.label.c_str());
      break;
    case StepOp::kStop:
    case StepOp::kStartLogging:
    case StepOp::kStopLogging:
      break;
  }
}

}  // namespace

Status ExperimentStore::parse(JsonObjectConst entry, Experiment& out,
                              std::size_t& offendingStep,
                              LabelString& offendingField) {
  offendingStep = 0;
  if (entry.isNull()) return fail(ErrorCode::kInvalidArgument, "empty body");

  if (!out.key.assign(entry["key"] | "") || out.key.empty()) {
    offendingField.assign("key");
    return fail(ErrorCode::kInvalidArgument, "an experiment needs a key");
  }
  if (!out.name.assign(entry["name"] | out.key.c_str())) {
    offendingField.assign("name");
    return fail(ErrorCode::kNameTooLong, "name is too long");
  }

  JsonObjectConst metadata = entry["metadata"].as<JsonObjectConst>();
  out.metadata.operatorName.assign(metadata["operator"] | "");
  out.metadata.sample.assign(metadata["sample"] | "");
  out.metadata.description.assign(metadata["description"] | "");
  out.metadata.notes.assign(metadata["notes"] | "");

  // What this scenario records, if anything.  Validated here rather than at
  // start: a channel list with twenty entries is a mistake worth catching in
  // the editor, not at three in the morning.
  JsonObjectConst logging = entry["logging"].as<JsonObjectConst>();
  out.logging = ExperimentLogging{};
  if (!logging.isNull()) {
    out.logging.rateHz = logging["rate_hz"] | 1.0f;
    out.logging.includeRaw = logging["raw"] | true;
    if (!(out.logging.rateHz > 0.0f) || out.logging.rateHz > DataLogger::kMaxRateHz) {
      offendingField.assign("rate_hz");
      return fail(ErrorCode::kInvalidArgument, "rate must be 0…50 Hz");
    }
    JsonArrayConst channels = logging["channels"].as<JsonArrayConst>();
    if (channels.size() > limits::kMaxLoggedChannels) {
      offendingField.assign("channels");
      return fail(ErrorCode::kOutOfCapacity, "at most 16 channels in one dataset");
    }
    for (JsonVariantConst channel : channels) {
      if (!out.logging.channels[out.logging.channelCount].assign(channel | "")) {
        offendingField.assign("channels");
        return fail(ErrorCode::kInvalidArgument, "a channel key is too long");
      }
      ++out.logging.channelCount;
    }
  }

  JsonArrayConst steps = entry["steps"].as<JsonArrayConst>();
  if (steps.isNull() || steps.size() == 0) {
    offendingField.assign("steps");
    return fail(ErrorCode::kInvalidArgument, "an experiment with no steps");
  }
  if (steps.size() > limits::kMaxExperimentSteps) {
    offendingField.assign("steps");
    return fail(ErrorCode::kOutOfCapacity, "too many steps");
  }

  out.stepCount = 0;
  for (JsonObjectConst step : steps) {
    ExperimentStep parsed;
    LabelString field;
    const Status status = parseStep(step, parsed, field);
    if (!status.ok()) {
      offendingStep = out.stepCount + 1;
      offendingField = field;
      return status;
    }
    out.steps[out.stepCount++] = parsed;
  }
  return ok();
}

void ExperimentStore::serialize(const Experiment& experiment, JsonObject out) {
  out["key"] = jsonCopy(experiment.key.c_str());
  out["name"] = jsonCopy(experiment.name.c_str());
  JsonObject metadata = out["metadata"].to<JsonObject>();
  metadata["operator"] = jsonCopy(experiment.metadata.operatorName.c_str());
  metadata["sample"] = jsonCopy(experiment.metadata.sample.c_str());
  metadata["description"] = jsonCopy(experiment.metadata.description.c_str());
  metadata["notes"] = jsonCopy(experiment.metadata.notes.c_str());

  if (experiment.logging.channelCount > 0) {
    JsonObject logging = out["logging"].to<JsonObject>();
    logging["rate_hz"] = experiment.logging.rateHz;
    logging["raw"] = experiment.logging.includeRaw;
    JsonArray channels = logging["channels"].to<JsonArray>();
    for (std::size_t i = 0; i < experiment.logging.channelCount; ++i) {
      channels.add(jsonCopy(experiment.logging.channels[i].c_str()));
    }
  }

  JsonArray steps = out["steps"].to<JsonArray>();
  for (std::size_t i = 0; i < experiment.stepCount; ++i) {
    serializeStep(experiment.steps[i], steps.add<JsonObject>());
  }
}

JsonObjectConst ExperimentStore::find(const JsonDocument& document,
                                      const char* key) {
  if (key == nullptr) return JsonObjectConst();
  for (JsonObjectConst entry : document["experiments"].as<JsonArrayConst>()) {
    if (std::strcmp(entry["key"] | "", key) == 0) return entry;
  }
  return JsonObjectConst();
}

void ExperimentStore::makeEmpty(JsonDocument& out) {
  out.clear();
  JsonObject root = out.to<JsonObject>();
  root["schemaVersion"] = LC_CONFIG_SCHEMA_VERSION;
  root["experiments"].to<JsonArray>();
}

Status ExperimentStore::upsert(JsonDocument& document, JsonObjectConst entry) {
  const char* key = entry["key"] | "";
  if (key[0] == '\0') return fail(ErrorCode::kInvalidArgument, "key is required");

  // Taking the DOCUMENT rather than a JsonObject, for the same reason
  // ConfigApplier::upsertDevice does: `doc.as<JsonObject>()` on a fresh
  // document is null, and every write through it is silently discarded — so
  // the very first experiment ever created would vanish.
  if (document["experiments"].isNull()) {
    if (document.isNull() || !document.is<JsonObject>()) {
      document.to<JsonObject>();
    }
    document["schemaVersion"] = LC_CONFIG_SCHEMA_VERSION;
    document["experiments"].to<JsonArray>();
  }

  JsonArray list = document["experiments"].as<JsonArray>();
  for (JsonObject existing : list) {
    if (std::strcmp(existing["key"] | "", key) != 0) continue;
    existing.clear();
    for (JsonPairConst pair : entry) existing[pair.key()] = pair.value();
    return ok();
  }
  if (list.size() >= limits::kMaxDashboards * 2) {
    return fail(ErrorCode::kOutOfCapacity, "too many experiments");
  }
  list.add(entry);
  return ok();
}

bool ExperimentStore::removeByKey(JsonDocument& document, const char* key) {
  JsonArray list = document["experiments"].as<JsonArray>();
  if (list.isNull() || key == nullptr) return false;
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (std::strcmp(list[i]["key"] | "", key) != 0) continue;
    list.remove(i);
    return true;
  }
  return false;
}

void ExperimentStore::summarise(const JsonDocument& document, JsonArray out) {
  for (JsonObjectConst entry : document["experiments"].as<JsonArrayConst>()) {
    JsonObject summary = out.add<JsonObject>();
    summary["key"] = jsonCopy(entry["key"] | "");
    summary["name"] = jsonCopy(entry["name"] | "");
    summary["steps"] = entry["steps"].as<JsonArrayConst>().size();
    summary["description"] =
        jsonCopy(entry["metadata"]["description"] | "");
  }
}

}  // namespace lc
