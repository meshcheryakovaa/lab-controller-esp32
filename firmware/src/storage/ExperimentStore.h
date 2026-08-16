// =============================================================================
//  storage/ExperimentStore.h — experiments.json, and everything that knows its
//  shape (§31, §32, ADR-0018).
//
//  ExperimentEngine runs one scenario and knows nothing about files.  This
//  class holds the other half: turning what the editor sent into typed steps,
//  and typed steps back into a document.
//
//  THE VOCABULARY IS CLOSED.  An `op` this parser does not know is an error,
//  not an extension point — that is the whole difference between a scenario
//  that is data and a scenario that is code.  A step with `op: "EVAL"` and a
//  string of C++ in it must be impossible to express, not merely unsupported.
//
//  Document shape:
//    { "schemaVersion": 1,
//      "experiments": [
//        { "key": "evaporation_60c", "name": "Evaporation at 60 °C",
//          "metadata": { "operator": "", "sample": "", "description": "",
//                        "notes": "" },
//          "steps": [
//            { "op": "SET", "target": "bath.setpoint", "value": 60 },
//            { "op": "SET", "target": "bath.mode", "mode": "automatic" },
//            { "op": "WAIT_UNTIL", "channel": "t_bath", "comparison": ">=",
//              "value": 59, "timeout_s": 900, "on_timeout": "abort" },
//            { "op": "MARK_EVENT", "label": "steady state reached" },
//            { "op": "RUN_FOR", "duration_s": 1800 },
//            { "op": "SET", "target": "heater", "value": 0 },
//            { "op": "STOP" } ] } ] }
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Error.h"
#include "services/ExperimentEngine.h"

namespace lc {

class ExperimentStore {
 public:
  // `offendingStep` is 1-based and 0 when the problem is not in a step, so the
  // editor can highlight the row rather than showing one sentence above a
  // scenario of thirty.
  static Status parse(JsonObjectConst entry, Experiment& out,
                      std::size_t& offendingStep, LabelString& offendingField);
  static void serialize(const Experiment& experiment, JsonObject out);

  static JsonObjectConst find(const JsonDocument& document, const char* key);
  static Status upsert(JsonDocument& document, JsonObjectConst entry);
  static bool removeByKey(JsonDocument& document, const char* key);
  static void makeEmpty(JsonDocument& out);

  // Names and step counts only — the picker does not need the steps, and eight
  // full scenarios do not fit in one response.
  static void summarise(const JsonDocument& document, JsonArray out);
};

}  // namespace lc
