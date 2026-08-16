// =============================================================================
//  storage/DashboardStore.h — dashboards.json (§22–§26, ADR-0015).
//
//  WHAT THE FIRMWARE KNOWS ABOUT A DASHBOARD, AND WHAT IT DELIBERATELY DOES NOT
//
//  It knows the SHAPE: a list of dashboards, each with a stable key, a name, a
//  grid, and a bounded list of widgets with integer positions.  It enforces the
//  bounds, the uniqueness of keys, and the size of the file — that partition is
//  640 KB and the web interface lives in it too.
//
//  It does NOT know what a widget IS.  `type` is an opaque string and `config`
//  an opaque object.  Whether "gauge" exists, and what settings it takes, is a
//  fact about the web interface, not about the rig — and §63 applied to the UI
//  says adding a widget type must be one component plus a registry entry, with
//  no firmware change.  Validating it here would mean maintaining the widget
//  vocabulary in two places, which is how the two drift.
//
//  The one reference the firmware DOES understand is `config.channel`: a widget
//  pointing at a channel that no longer exists is reported (never silently
//  dropped — deleting somebody's dashboard because a wire fell out would be
//  unforgivable), so the browser can show the tile as broken instead of blank.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Error.h"
#include "core/Types.h"
#include "services/ChannelManager.h"

namespace lc {

struct DashboardReport {
  std::size_t widgets = 0;
  std::size_t danglingChannels = 0;
  // First widget whose channel is missing, for a message that names something.
  KeyString firstDanglingChannel;
  KeyString firstDanglingWidget;
};

class DashboardStore {
 public:
  static JsonArray dashboardsArray(JsonDocument& document);
  static JsonArrayConst dashboardsArray(const JsonDocument& document);

  static JsonObjectConst findByKey(const JsonDocument& document, const char* key);
  static JsonObject findByKeyMutable(JsonDocument& document, const char* key);
  static bool removeByKey(JsonDocument& document, const char* key);

  // Structural validation.  `offendingField` is filled so the editor can point
  // at the input rather than at the dashboard.
  static Status validate(JsonObjectConst dashboard, LabelString* offendingField);

  // Replaces (or appends) a dashboard.  Refuses to exceed kMaxDashboards.
  static Status upsert(JsonDocument& document, JsonObjectConst dashboard);

  // Counts widgets and checks every `config.channel` against the live rig.
  static DashboardReport inspect(JsonObjectConst dashboard,
                                 const ChannelManager& channels);

  // A listing without the widgets: the full set of dashboards does not fit in
  // one API response, and the picker only needs names anyway.
  static void summarise(const JsonDocument& document, const ChannelManager& channels,
                        JsonArray out);
};

}  // namespace lc
