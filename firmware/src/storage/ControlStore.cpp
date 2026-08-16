#include "storage/ControlStore.h"

#include <cmath>

#include "storage/JsonUtils.h"

namespace lc {
namespace {

void setField(LabelString& field, const char* name) { field.assign(name); }

// Seconds in the file, microseconds in the code.  The file is written by
// humans and read by an operator; the code counts scheduler ticks.  Converting
// in exactly one place is what keeps a "5" from meaning five microseconds
// somewhere downstream.
Micros secondsToMicros(double seconds) {
  if (!(seconds > 0.0)) return 0;
  return static_cast<Micros>(seconds * 1e6);
}

double microsToSeconds(Micros micros) {
  return static_cast<double>(micros) / 1e6;
}

Status assignKey(KeyString& target, const char* text, const char* fieldName,
                 LabelString& field) {
  if (text == nullptr || text[0] == '\0') {
    setField(field, fieldName);
    return fail(ErrorCode::kInvalidArgument, "this field is required");
  }
  if (!target.assign(text)) {
    setField(field, fieldName);
    return fail(ErrorCode::kInvalidArgument, "this key is too long");
  }
  return ok();
}

bool finite(float value) { return std::isfinite(value); }

}  // namespace

// ---------------------------------------------------------------------------
//  Loops
// ---------------------------------------------------------------------------
Status ControlStore::parseLoop(JsonObjectConst entry, ControlLoop& out,
                               LabelString& offendingField) {
  if (entry.isNull()) return fail(ErrorCode::kInvalidArgument, "not an object");

  Status status = assignKey(out.id, entry["id"] | "", "id", offendingField);
  if (!status.ok()) return status;
  status = assignKey(out.inputKey, entry["input"] | "", "input", offendingField);
  if (!status.ok()) return status;
  status = assignKey(out.outputKey, entry["output"] | "", "output", offendingField);
  if (!status.ok()) return status;

  out.setpoint = entry["setpoint"] | 0.0f;
  out.kp = entry["kp"] | 1.0f;
  out.ki = entry["ki"] | 0.0f;
  out.kd = entry["kd"] | 0.0f;
  out.outputMin = entry["min"] | 0.0f;
  out.outputMax = entry["max"] | 100.0f;
  out.manualValue = entry["manual"] | 0.0f;
  out.invert = entry["invert"] | false;

  if (!finite(out.setpoint)) {
    setField(offendingField, "setpoint");
    return fail(ErrorCode::kRuleInvalid, "setpoint is not a number");
  }
  if (!finite(out.kp) || !finite(out.ki) || !finite(out.kd)) {
    setField(offendingField, "kp");
    return fail(ErrorCode::kRuleInvalid, "gains must be finite");
  }
  if (!finite(out.outputMin) || !finite(out.outputMax) ||
      !(out.outputMax > out.outputMin)) {
    setField(offendingField, "max");
    return fail(ErrorCode::kRuleInvalid, "max must be greater than min");
  }

  const double period = entry["period_s"] | 1.0;
  if (!(period > 0.0)) {
    setField(offendingField, "period_s");
    return fail(ErrorCode::kRuleInvalid, "period must be greater than zero");
  }
  out.periodUs = secondsToMicros(period);

  // A grace period of zero would release the output on the first sample that
  // arrives a millisecond late.  A grace period of an hour is not a grace
  // period.  Both ends are bounded here rather than in the UI, because the file
  // can be edited by hand and imported.
  const double grace = entry["input_grace_s"] | 5.0;
  if (!(grace >= 0.0) || grace > 300.0) {
    setField(offendingField, "input_grace_s");
    return fail(ErrorCode::kRuleInvalid, "input grace must be 0..300 s");
  }
  out.inputGraceUs = secondsToMicros(grace);

  // `mode` is read for nothing: see the header.  A loop always loads OFF.
  out.mode = LoopMode::kOff;
  return ok();
}

void ControlStore::serializeLoop(const ControlLoop& loop, JsonObject out) {
  out["id"] = jsonCopy(loop.id.c_str());
  out["input"] = jsonCopy(loop.inputKey.c_str());
  out["output"] = jsonCopy(loop.outputKey.c_str());
  out["setpoint"] = loop.setpoint;
  out["kp"] = loop.kp;
  out["ki"] = loop.ki;
  out["kd"] = loop.kd;
  out["min"] = loop.outputMin;
  out["max"] = loop.outputMax;
  out["manual"] = loop.manualValue;
  out["invert"] = loop.invert;
  out["period_s"] = microsToSeconds(loop.periodUs);
  out["input_grace_s"] = microsToSeconds(loop.inputGraceUs);
}

// ---------------------------------------------------------------------------
//  Rules
// ---------------------------------------------------------------------------
Status ControlStore::parseRule(JsonObjectConst entry, ControlRule& out,
                               LabelString& offendingField) {
  if (entry.isNull()) return fail(ErrorCode::kInvalidArgument, "not an object");

  Status status = assignKey(out.id, entry["id"] | "", "id", offendingField);
  if (!status.ok()) return status;
  status = assignKey(out.inputKey, entry["input"] | "", "input", offendingField);
  if (!status.ok()) return status;
  status = assignKey(out.outputKey, entry["output"] | "", "output", offendingField);
  if (!status.ok()) return status;

  out.onAbove = entry["on_above"] | 0.0f;
  out.offBelow = entry["off_below"] | 0.0f;
  out.onValue = entry["on_value"] | 1.0f;
  out.offValue = entry["off_value"] | 0.0f;
  out.enabled = entry["enabled"] | true;

  if (!finite(out.onAbove) || !finite(out.offBelow)) {
    setField(offendingField, "on_above");
    return fail(ErrorCode::kRuleInvalid, "thresholds must be numbers");
  }
  if (!(out.onAbove > out.offBelow)) {
    setField(offendingField, "off_below");
    return fail(ErrorCode::kRuleInvalid,
                "on_above must be greater than off_below (hysteresis)");
  }
  if (!finite(out.onValue) || !finite(out.offValue)) {
    setField(offendingField, "on_value");
    return fail(ErrorCode::kRuleInvalid, "values must be numbers");
  }

  const double hold = entry["min_hold_s"] | 0.0;
  if (!(hold >= 0.0)) {
    setField(offendingField, "min_hold_s");
    return fail(ErrorCode::kRuleInvalid, "minimum hold must not be negative");
  }
  out.minHoldUs = secondsToMicros(hold);

  if (!entry["note"].isNull() && !out.note.assign(entry["note"] | "")) {
    setField(offendingField, "note");
    return fail(ErrorCode::kInvalidArgument, "note is too long");
  }
  return ok();
}

void ControlStore::serializeRule(const ControlRule& rule, JsonObject out) {
  out["id"] = jsonCopy(rule.id.c_str());
  out["input"] = jsonCopy(rule.inputKey.c_str());
  out["output"] = jsonCopy(rule.outputKey.c_str());
  out["on_above"] = rule.onAbove;
  out["off_below"] = rule.offBelow;
  out["on_value"] = rule.onValue;
  out["off_value"] = rule.offValue;
  out["min_hold_s"] = microsToSeconds(rule.minHoldUs);
  out["enabled"] = rule.enabled;
  out["note"] = jsonCopy(rule.note.c_str());
}

// ---------------------------------------------------------------------------
//  Limits
// ---------------------------------------------------------------------------
Status ControlStore::parseLimit(JsonObjectConst entry, SafetyLimit& out,
                                LabelString& offendingField) {
  if (entry.isNull()) return fail(ErrorCode::kInvalidArgument, "not an object");

  Status status = assignKey(out.id, entry["id"] | "", "id", offendingField);
  if (!status.ok()) return status;
  status = assignKey(out.channelKey, entry["channel"] | "", "channel",
                     offendingField);
  if (!status.ok()) return status;

  if (!parseSafetyCondition(entry["condition"] | "above", out.condition)) {
    setField(offendingField, "condition");
    return fail(ErrorCode::kRuleInvalid, "condition: above | below | outside");
  }
  if (!parseSafetyAction(entry["action"] | "trip_all", out.action)) {
    setField(offendingField, "action");
    return fail(ErrorCode::kRuleInvalid,
                "action: trip_all | release_output | alarm_only");
  }
  if (out.action == SafetyAction::kReleaseOutput) {
    status = assignKey(out.targetKey, entry["target"] | "", "target",
                       offendingField);
    if (!status.ok()) return status;
  } else if (!entry["target"].isNull()) {
    out.targetKey.assign(entry["target"] | "");
  }

  out.low = entry["low"] | 0.0f;
  out.high = entry["high"] | 0.0f;
  if (!finite(out.low) || !finite(out.high)) {
    setField(offendingField, "high");
    return fail(ErrorCode::kRuleInvalid, "limits must be numbers");
  }
  if (out.condition == SafetyCondition::kOutside && !(out.high > out.low)) {
    setField(offendingField, "high");
    return fail(ErrorCode::kRuleInvalid, "outside needs high above low");
  }

  const double debounce = entry["for_s"] | 0.0;
  // Bounded on purpose.  A debounce of a minute on an over-temperature limit is
  // not a filter, it is a minute of over-temperature.
  if (!(debounce >= 0.0) || debounce > 60.0) {
    setField(offendingField, "for_s");
    return fail(ErrorCode::kRuleInvalid, "debounce must be 0..60 s");
  }
  out.forUs = secondsToMicros(debounce);

  out.requireFreshInput = entry["require_fresh_input"] | true;
  out.enabled = entry["enabled"] | true;
  if (!entry["message"].isNull() && !out.message.assign(entry["message"] | "")) {
    setField(offendingField, "message");
    return fail(ErrorCode::kInvalidArgument, "message is too long");
  }
  return ok();
}

void ControlStore::serializeLimit(const SafetyLimit& limit, JsonObject out) {
  out["id"] = jsonCopy(limit.id.c_str());
  out["channel"] = jsonCopy(limit.channelKey.c_str());
  out["target"] = jsonCopy(limit.targetKey.c_str());
  out["condition"] = toString(limit.condition);
  out["action"] = toString(limit.action);
  out["low"] = limit.low;
  out["high"] = limit.high;
  out["for_s"] = microsToSeconds(limit.forUs);
  out["require_fresh_input"] = limit.requireFreshInput;
  out["enabled"] = limit.enabled;
  out["message"] = jsonCopy(limit.message.c_str());
}

// ---------------------------------------------------------------------------
//  Whole document
// ---------------------------------------------------------------------------
void ControlStore::makeEmpty(JsonDocument& out) {
  out.clear();
  JsonObject root = out.to<JsonObject>();
  root["schemaVersion"] = LC_CONFIG_SCHEMA_VERSION;
  root["limits"].to<JsonArray>();
  root["loops"].to<JsonArray>();
  root["rules"].to<JsonArray>();
}

void ControlStore::serializeAll(const ControlManager& control,
                                const SafetyManager* safety, JsonDocument& out) {
  makeEmpty(out);
  JsonObject root = out.as<JsonObject>();

  JsonArray limits = root["limits"].as<JsonArray>();
  if (safety != nullptr) {
    for (std::size_t i = 0; i < safety->count(); ++i) {
      serializeLimit(safety->at(i), limits.add<JsonObject>());
    }
  }

  JsonArray loops = root["loops"].as<JsonArray>();
  for (std::size_t i = 0; i < control.loopCount(); ++i) {
    serializeLoop(control.loopAt(i), loops.add<JsonObject>());
  }

  JsonArray rules = root["rules"].as<JsonArray>();
  for (std::size_t i = 0; i < control.ruleCount(); ++i) {
    serializeRule(control.ruleAt(i), rules.add<JsonObject>());
  }
}

}  // namespace lc
