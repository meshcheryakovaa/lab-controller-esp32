// =============================================================================
//  storage/ControlStore.h — control.json, and everything that knows its shape
//  (§28, §29, §30, ADR-0017).
//
//  ControlManager and SafetyManager hold loops, rules and limits and know
//  nothing about files.  This class holds the other half: parsing what the
//  browser sent, and writing back what is running.
//
//  ONE FILE, THREE LISTS, AND THE ORDER MATTERS.
//  Limits are applied BEFORE loops and rules, always — including on a reload
//  in the middle of an experiment.  A rig that comes up with its regulators
//  running and its interlocks still being parsed has, for those milliseconds,
//  no interlocks.
//
//  WHAT IS DELIBERATELY NOT IN THIS FILE: the mode of a loop.  `mode` is
//  written out for the operator's benefit and ignored on load; every loop comes
//  up OFF (see ControlManager::addLoop).  A setpoint is a number and persists;
//  the authority to act on it is not a number and does not.
//
//  Document shape:
//    { "schemaVersion": 1,
//      "limits": [ { "id": "over_temp", "channel": "temp_01",
//                    "condition": "above", "action": "trip_all",
//                    "low": 0, "high": 300, "for_s": 1.0,
//                    "require_fresh_input": true, "enabled": true,
//                    "message": "furnace over temperature" } ],
//      "loops":  [ { "id": "furnace", "input": "temp_01", "output": "heater_01",
//                    "setpoint": 180, "kp": 2, "ki": 0.05, "kd": 10,
//                    "min": 0, "max": 100, "invert": false,
//                    "period_s": 1.0, "input_grace_s": 5.0,
//                    "manual": 0 } ],
//      "rules":  [ { "id": "fan", "input": "temp_01", "output": "fan_01",
//                    "on_above": 40, "off_below": 35,
//                    "on_value": 1, "off_value": 0, "min_hold_s": 10,
//                    "enabled": true, "note": "keep the case cool" } ] }
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Error.h"
#include "services/ControlManager.h"
#include "services/SafetyManager.h"

namespace lc {

class ControlStore {
 public:
  // --- parsing (the browser's side, and the file's) ------------------------
  // `offendingField` is filled on failure so the editor can highlight the input
  // instead of showing a sentence next to a form of twelve fields.
  static Status parseLoop(JsonObjectConst entry, ControlLoop& out,
                          LabelString& offendingField);
  static Status parseRule(JsonObjectConst entry, ControlRule& out,
                          LabelString& offendingField);
  static Status parseLimit(JsonObjectConst entry, SafetyLimit& out,
                           LabelString& offendingField);

  // --- writing back --------------------------------------------------------
  // Configuration only: no live state, so a document that round-trips through
  // the file is byte-identical to the one that went in.
  static void serializeLoop(const ControlLoop& loop, JsonObject out);
  static void serializeRule(const ControlRule& rule, JsonObject out);
  static void serializeLimit(const SafetyLimit& limit, JsonObject out);

  // Rebuilds control.json from what is actually running.  Used after a live
  // edit (a setpoint change) so the file and the rig cannot drift.
  static void serializeAll(const ControlManager& control,
                           const SafetyManager* safety, JsonDocument& out);

  // Empty document with the right shape, for the first ever write.
  static void makeEmpty(JsonDocument& out);
};

}  // namespace lc
