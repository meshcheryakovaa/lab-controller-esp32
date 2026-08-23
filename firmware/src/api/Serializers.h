// =============================================================================
//  api/Serializers.h — turning firmware structures into the JSON the browser
//  consumes.
//
//  The manifest serialiser is the important one: it is the mechanism by which
//  "add a driver, change nothing in the frontend" actually works (§63, ADR-0004).
//  Whatever a ParamSpec can express, the form can render.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/ModuleManifest.h"
#include "core/ResourceManager.h"
#include "core/Scheduler.h"
#include "services/ChannelManager.h"
#include "services/INetworkManager.h"

namespace lc {

const char* toString(ParamType type);
const char* toString(PinUse use);
const char* toString(BusRequirement bus);
const char* toString(ChannelDirection direction);
const char* toString(CoordinateSystem system);

// One module type, complete enough to build its whole configuration form.
void serializeManifest(const ModuleManifest& manifest, JsonObject out);

// The pin picker's data source: capability, current owner, and the advisory
// text for strapping pins.  The frontend renders this and never re-derives it.
void serializeGpioMap(const ResourceManager& resources, JsonObject out);

// Every claimed resource, for the Hardware page and for debugging.
void serializeResourceClaims(const ResourceManager& resources, JsonArray out);

void serializeChannel(ChannelHandle handle, const ChannelDescriptor& descriptor,
                      const ChannelValue& value, JsonObject out,
                      bool includeValue);

void serializeGeometry(const Geometry& geometry, JsonObject out);

// Per-task scheduler statistics — the fastest way to find what is stalling the
// control loop.
void serializeSchedulerStats(const Scheduler& scheduler, JsonArray out);

/**
 * The network, as the interface and the diagnostics page both see it (M16).
 *
 * One function, two callers, and that is on purpose: /network and
 * /diagnostics have to agree, and the surest way to keep a Wi-Fi password out
 * of BOTH is for neither to assemble its own object.  There is nothing to
 * redact here — NetworkStatus has no password field.
 */
void serializeNetwork(const INetworkManager& network, JsonObject out);

}  // namespace lc
