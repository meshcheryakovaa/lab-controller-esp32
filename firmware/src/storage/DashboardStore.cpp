#include "storage/DashboardStore.h"

#include <cstring>

#include "storage/JsonUtils.h"

namespace lc {
namespace {

void setField(LabelString* field, const char* name) {
  if (field != nullptr) field->assign(name);
}

}  // namespace

JsonArray DashboardStore::dashboardsArray(JsonDocument& document) {
  JsonArray existing = document["dashboards"].as<JsonArray>();
  if (!existing.isNull()) return existing;
  document["schemaVersion"] = 1;
  return document["dashboards"].to<JsonArray>();
}

JsonArrayConst DashboardStore::dashboardsArray(const JsonDocument& document) {
  return document["dashboards"].as<JsonArrayConst>();
}

JsonObjectConst DashboardStore::findByKey(const JsonDocument& document,
                                          const char* key) {
  if (key == nullptr) return JsonObjectConst();
  JsonArrayConst list = dashboardsArray(document);
  if (list.isNull()) return JsonObjectConst();
  for (JsonObjectConst dashboard : list) {
    if (std::strcmp(dashboard["key"] | "", key) == 0) return dashboard;
  }
  return JsonObjectConst();
}

JsonObject DashboardStore::findByKeyMutable(JsonDocument& document,
                                            const char* key) {
  JsonArray list = document["dashboards"].as<JsonArray>();
  if (list.isNull() || key == nullptr) return JsonObject();
  for (JsonObject dashboard : list) {
    if (std::strcmp(dashboard["key"] | "", key) == 0) return dashboard;
  }
  return JsonObject();
}

bool DashboardStore::removeByKey(JsonDocument& document, const char* key) {
  JsonArray list = document["dashboards"].as<JsonArray>();
  if (list.isNull() || key == nullptr) return false;
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (std::strcmp(list[i]["key"] | "", key) != 0) continue;
    list.remove(i);
    return true;
  }
  return false;
}

Status DashboardStore::validate(JsonObjectConst dashboard,
                                LabelString* offendingField) {
  if (dashboard.isNull()) return fail(ErrorCode::kInvalidArgument, "empty body");

  const char* key = dashboard["key"] | "";
  if (key[0] == '\0') {
    setField(offendingField, "key");
    return fail(ErrorCode::kInvalidArgument, "dashboard key is required");
  }
  KeyString parsedKey;
  if (!parsedKey.assign(key)) {
    setField(offendingField, "key");
    return fail(ErrorCode::kInvalidArgument, "dashboard key is too long");
  }
  NameString parsedName;
  if (!parsedName.assign(dashboard["name"] | "")) {
    setField(offendingField, "name");
    return fail(ErrorCode::kInvalidArgument, "dashboard name is too long");
  }

  const std::int32_t columns =
      dashboard["grid"]["columns"] | static_cast<std::int32_t>(limits::kDashboardGridColumns);
  if (columns < 1 || static_cast<std::size_t>(columns) > limits::kDashboardGridColumns) {
    setField(offendingField, "grid.columns");
    return fail(ErrorCode::kDashboardInvalid, "grid has 1..12 columns");
  }

  JsonArrayConst widgets = dashboard["widgets"].as<JsonArrayConst>();
  if (widgets.isNull()) return ok();  // an empty dashboard is a legal dashboard
  if (widgets.size() > limits::kMaxWidgetsPerDashboard) {
    setField(offendingField, "widgets");
    return fail(ErrorCode::kOutOfCapacity, "at most 24 widgets on a dashboard");
  }

  for (JsonObjectConst widget : widgets) {
    if (widget.isNull()) {
      setField(offendingField, "widgets");
      return fail(ErrorCode::kDashboardInvalid, "a widget is not an object");
    }
    if ((widget["id"] | "")[0] == '\0') {
      setField(offendingField, "widgets");
      return fail(ErrorCode::kDashboardInvalid, "every widget needs an id");
    }
    // `type` is checked for presence and NOT for meaning: the widget vocabulary
    // belongs to the web interface (ADR-0015).
    if ((widget["type"] | "")[0] == '\0') {
      setField(offendingField, "widgets");
      return fail(ErrorCode::kDashboardInvalid, "every widget needs a type");
    }

    const std::int32_t x = widget["x"] | 0;
    const std::int32_t y = widget["y"] | 0;
    const std::int32_t w = widget["w"] | 1;
    const std::int32_t h = widget["h"] | 1;
    if (w < 1 || h < 1) {
      setField(offendingField, "widgets");
      return fail(ErrorCode::kDashboardInvalid, "a widget is smaller than one cell");
    }
    if (x < 0 || y < 0 || x + w > columns) {
      setField(offendingField, "widgets");
      return fail(ErrorCode::kDashboardInvalid, "a widget sits outside the grid");
    }
    // y is not bounded above: a dashboard may be as tall as it likes, and the
    // file size limit is the real constraint.
  }

  // Duplicate widget ids would make "remove this one" ambiguous.
  for (std::size_t i = 0; i < widgets.size(); ++i) {
    for (std::size_t j = i + 1; j < widgets.size(); ++j) {
      if (std::strcmp(widgets[i]["id"] | "", widgets[j]["id"] | "") == 0) {
        setField(offendingField, "widgets");
        return fail(ErrorCode::kAlreadyExists, "two widgets share an id");
      }
    }
  }
  return ok();
}

Status DashboardStore::upsert(JsonDocument& document, JsonObjectConst dashboard) {
  JsonArray list = dashboardsArray(document);
  if (list.isNull()) return fail(ErrorCode::kStorageFailure, "dashboards");

  const char* key = dashboard["key"] | "";
  for (JsonObject existing : list) {
    if (std::strcmp(existing["key"] | "", key) != 0) continue;
    if (!existing.set(dashboard)) {
      return fail(ErrorCode::kOutOfCapacity, "dashboard is too large");
    }
    return ok();
  }

  if (list.size() >= limits::kMaxDashboards) {
    return fail(ErrorCode::kOutOfCapacity,
                "8 dashboards is the limit for this partition");
  }
  JsonObject added = list.add<JsonObject>();
  if (added.isNull() || !added.set(dashboard)) {
    return fail(ErrorCode::kOutOfCapacity, "dashboard is too large");
  }
  return ok();
}

DashboardReport DashboardStore::inspect(JsonObjectConst dashboard,
                                        const ChannelManager& channels) {
  DashboardReport report;
  JsonArrayConst widgets = dashboard["widgets"].as<JsonArrayConst>();
  if (widgets.isNull()) return report;

  for (JsonObjectConst widget : widgets) {
    ++report.widgets;

    // A widget may reference one channel directly, or several through a
    // "series" list (a chart).  Both forms are checked; anything else in
    // `config` is none of the firmware's business.
    JsonObjectConst config = widget["config"].as<JsonObjectConst>();
    if (config.isNull()) continue;

    const char* single = config["channel"] | "";
    if (single[0] != '\0' && channels.findByKey(single) == kInvalidChannel) {
      ++report.danglingChannels;
      if (report.danglingChannels == 1) {
        report.firstDanglingChannel.assign(single);
        report.firstDanglingWidget.assign(widget["id"] | "");
      }
    }

    JsonArrayConst series = config["series"].as<JsonArrayConst>();
    if (series.isNull()) continue;
    for (JsonObjectConst entry : series) {
      const char* key = entry["channel"] | "";
      if (key[0] == '\0' || channels.findByKey(key) != kInvalidChannel) continue;
      ++report.danglingChannels;
      if (report.danglingChannels == 1) {
        report.firstDanglingChannel.assign(key);
        report.firstDanglingWidget.assign(widget["id"] | "");
      }
    }
  }
  return report;
}

void DashboardStore::summarise(const JsonDocument& document,
                               const ChannelManager& channels, JsonArray out) {
  JsonArrayConst list = dashboardsArray(document);
  if (list.isNull()) return;
  for (JsonObjectConst dashboard : list) {
    JsonObject entry = out.add<JsonObject>();
    entry["key"] = jsonCopy(dashboard["key"] | "");
    entry["name"] = jsonCopy(dashboard["name"] | "");
    const DashboardReport report = inspect(dashboard, channels);
    entry["widgets"] = report.widgets;
    // Surfaced in the picker, so a dashboard that lost a sensor says so before
    // it is opened rather than after.
    if (report.danglingChannels > 0) {
      entry["dangling_channels"] = report.danglingChannels;
    }
  }
}

}  // namespace lc
