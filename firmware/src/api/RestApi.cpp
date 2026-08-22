#include "api/RestApi.h"

#include <cstdlib>
#include <cstring>

#include "buses/I2cScanner.h"
#include "storage/CalibrationStore.h"
#include "storage/ControlStore.h"
#include "storage/DashboardStore.h"
#include "storage/ExperimentStore.h"
#include "services/AuthManager.h"
#include "storage/LogStore.h"
#include "storage/JsonConfigView.h"

namespace lc {
namespace {

constexpr const char* kApiVersion = "1";

bool isWrite(HttpMethod method) {
  return method != HttpMethod::kGet && method != HttpMethod::kOptions;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Entry point
// ---------------------------------------------------------------------------
void RestApi::handle(const ApiRequest& request, ApiResponse& response) {
  ++requests_;
  response.reset();

  const PathSegments path(request.path);
  if (path.truncated()) {
    response.setError(fail(ErrorCode::kInvalidArgument, "path is too long"));
    ++errors_;
    return;
  }
  if (!path.isApiV1()) {
    response.setError(404, fail(ErrorCode::kNotFound, "unknown API version"));
    ++errors_;
    return;
  }
  if (path.count() < 3) {
    // /api/v1 itself: a tiny index, useful when poking at the device by hand.
    response.body["version"] = kApiVersion;
    response.body["firmware"] = LC_FIRMWARE_VERSION;
    JsonArray resources = response.body["resources"].to<JsonArray>();
    for (const char* name :
         {"system", "diagnostics", "modules", "gpio", "buses", "devices",
          "channels", "processing", "calibrations", "outputs", "control",
          "experiments", "logs", "auth", "firmware", "dashboards", "config"}) {
      resources.add(name);
    }
    return;
  }

  const char* group = path.at(2);

  // One gate, before the route groups, so that no endpoint can be added later
  // that quietly forgets to check.  The exemptions are listed in exactly one
  // place (isSafetyExempt) and every one of them is an action that can only
  // make the rig safer.
  exemptRequest_ = isSafetyExempt(request, path);
  if (isWrite(request.method) && !exemptRequest_ &&
      std::strcmp(group, "auth") != 0 && !signedIn(request)) {
    response.setError(401, fail(ErrorCode::kUnauthorized,
                                "sign in to change settings"));
    ++errors_;
    return;
  }

  if (std::strcmp(group, "system") == 0) {
    handleSystem(request, path, response);
  } else if (std::strcmp(group, "diagnostics") == 0) {
    handleDiagnostics(response);
  } else if (std::strcmp(group, "modules") == 0) {
    handleModules(path, response);
  } else if (std::strcmp(group, "gpio") == 0) {
    handleGpio(response);
  } else if (std::strcmp(group, "buses") == 0) {
    handleBuses(request, path, response);
  } else if (std::strcmp(group, "devices") == 0) {
    handleDevices(request, path, response);
  } else if (std::strcmp(group, "channels") == 0) {
    handleChannels(request, path, response);
  } else if (std::strcmp(group, "processing") == 0) {
    handleProcessing(request, path, response);
  } else if (std::strcmp(group, "calibrations") == 0) {
    handleCalibrations(request, path, response);
  } else if (std::strcmp(group, "outputs") == 0) {
    handleOutputs(request, path, response);
  } else if (std::strcmp(group, "control") == 0) {
    handleControl(request, path, response);
  } else if (std::strcmp(group, "experiments") == 0) {
    handleExperiments(request, path, response);
  } else if (std::strcmp(group, "logs") == 0) {
    handleLogs(request, path, response);
  } else if (std::strcmp(group, "auth") == 0) {
    handleAuth(request, path, response);
  } else if (std::strcmp(group, "firmware") == 0) {
    handleFirmware(request, path, response);
  } else if (std::strcmp(group, "dashboards") == 0) {
    handleDashboards(request, path, response);
  } else if (std::strcmp(group, "config") == 0) {
    handleConfig(request, path, response);
  } else {
    response.setError(404, fail(ErrorCode::kNotFound, group));
  }

  if (response.isError()) ++errors_;
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
bool RestApi::signedIn(const ApiRequest& request) const {
  // A transport may assert it (the host development server does).  Otherwise
  // the cookie is resolved here, in the layer that is tested, rather than in an
  // adapter that is not.
  if (request.authenticated) return true;
  if (s_.auth == nullptr) return true;      // a build with no credentials
  if (!s_.auth->configured()) return true;  // no password set: see AuthManager.h
  SessionToken token;
  AuthManager::tokenFromCookie(request.cookie, token);
  return s_.auth->validate(token.c_str());
}

bool RestApi::requireWriteAccess(const ApiRequest& request,
                                 ApiResponse& response) {
  if (!isWrite(request.method)) return true;
  if (exemptRequest_) return true;   // the stop button; see isSafetyExempt()
  if (signedIn(request)) return true;
  response.setError(401, fail(ErrorCode::kUnauthorized,
                              "sign in to change settings"));
  return false;
}

bool RestApi::requireConfirmation(const ApiRequest& request,
                                  ApiResponse& response, const char* what) {
  if (s_.auth == nullptr || !s_.auth->configured()) return true;
  if (!signedIn(request)) {
    response.setError(401, fail(ErrorCode::kUnauthorized, "sign in first"));
    return false;
  }

  // The password again, in this request.  A session says "this browser was
  // trusted at some point"; that is not the same as "the person meant to do
  // THIS", and the difference matters exactly for the actions that remove a
  // protection.
  JsonDocument body;
  const char* password = nullptr;
  if (request.body != nullptr && request.bodyLength > 0 &&
      !deserializeJson(body, request.body, request.bodyLength)) {
    password = body["password"] | static_cast<const char*>(nullptr);
  }
  if (password != nullptr && s_.auth->verify(password)) return true;

  response.setError(403, fail(ErrorCode::kForbidden, what), "password",
                    "confirm with your password: this removes a protection");
  return false;
}

bool RestApi::isSafetyExempt(const ApiRequest& request,
                             const PathSegments& path) {
  // Stopping is never gated.  A person reaching for the stop button is not
  // going to sign in first, and nothing they can reach here makes the rig less
  // safe than it already is: the safe state is safe by definition (§49).
  //
  // CLEARING a stop is deliberately NOT in this list — that is an arming
  // action, and arming is exactly what a password is for.
  if (request.method != HttpMethod::kPost) return false;
  if (path.count() >= 4 && path.is(2, "outputs")) {
    if (path.is(3, "trip")) return true;
    if (path.count() >= 5 && path.is(4, "release")) return true;
  }
  if (path.count() >= 6 && path.is(2, "experiments") && path.is(4, "actions") &&
      path.is(5, "stop")) {
    return true;
  }
  return false;
}

bool RestApi::parseBody(const ApiRequest& request, JsonDocument& out,
                        ApiResponse& response) {
  if (request.body == nullptr || request.bodyLength == 0) {
    response.setError(400, fail(ErrorCode::kInvalidArgument, "empty body"));
    return false;
  }
  if (request.bodyLength >= ApiResponse::kMaxResponseBytes) {
    response.setError(fail(ErrorCode::kPayloadTooLarge, "request body"));
    return false;
  }
  const DeserializationError parsed =
      deserializeJson(out, request.body, request.bodyLength);
  if (parsed) {
    response.setError(400, fail(ErrorCode::kInvalidArgument, parsed.c_str()),
                      nullptr, "request body is not valid JSON");
    return false;
  }
  return true;
}

Status RestApi::persistDevices(JsonDocument& document) {
  return s_.storage->save(ConfigSection::kDevices, document);
}

void RestApi::describeDevice(const DeviceRecord& record, JsonObject out) const {
  out["handle"] = record.handle;
  out["key"] = jsonCopy(record.key.c_str());
  out["module"] = record.manifest->id;
  out["name"] = jsonCopy(record.name.c_str());
  out["state"] = toString(record.state);
  out["sample_interval_us"] = record.sampleIntervalUs;
  if (!record.lastError.ok()) {
    JsonObject error = out["error"].to<JsonObject>();
    error["code"] = record.lastError.symbol();
    error["numeric"] = static_cast<int>(record.lastError.code);
    error["detail"] = jsonCopy(record.lastError.detail.c_str());
  }
  if (record.geometry.isDefined()) {
    serializeGeometry(record.geometry, out["geometry"].to<JsonObject>());
  }

  JsonArray channels = out["channels"].to<JsonArray>();
  for (std::uint8_t i = 0; i < record.channelCount; ++i) {
    const ChannelDescriptor* descriptor =
        s_.channels->descriptor(record.channels[i]);
    if (descriptor == nullptr) continue;
    JsonObject channel = channels.add<JsonObject>();
    channel["handle"] = record.channels[i];
    channel["key"] = jsonCopy(descriptor->key.c_str());
    channel["unit"] = jsonCopy(descriptor->unit.c_str());
    channel["stages"] = s_.processing->stageCount(record.channels[i]);
  }
}

// ---------------------------------------------------------------------------
//  /system
// ---------------------------------------------------------------------------
void RestApi::handleSystem(const ApiRequest& request, const PathSegments& path,
                           ApiResponse& response) {
  if (path.count() >= 4 && path.is(3, "reboot")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;
    response.body["rebooting"] = true;
    if (s_.reboot != nullptr) s_.reboot(s_.rebootContext);
    return;
  }

  if (request.method != HttpMethod::kGet) {
    response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
    return;
  }

  JsonObject out = response.body.to<JsonObject>();
  out["firmware"] = LC_FIRMWARE_VERSION;
  // Which controller this is, independent of how it is reached.  Outside the
  // `metrics != nullptr` block below on purpose: a client that files local
  // recordings by controller must never be handed nothing and fall back to the
  // address, because in access-point mode every board is 192.168.4.1 (§M14).
  out["controller_id"] =
      (s_.metrics != nullptr) ? s_.metrics->controllerId() : "lc-000000000000";
  out["chip"] = s_.resources->chip().name;
  out["schema_version"] = ConfigStorage::kSchemaVersion;
  out["config_revision"] = s_.storage->revision();
  // Identifies the configuration itself, not how many times it has been saved
  // since boot.  It is what a dataset header carries, so the page an operator
  // is looking at and the file they downloaded can be compared (§48).
  out["config_fingerprint"] = s_.storage->fingerprint();
  out["uptime_ms"] = s_.clock->nowMicros() / 1000ULL;
  out["epoch_ms"] = s_.clock->epochMillis();
  out["time_synchronised"] = s_.clock->epochValid();
  out["boot_mode"] = toString(s_.system->mode());

  JsonObject auth = out["auth"].to<JsonObject>();
  // `configured: false` is the honest statement that this instrument is open,
  // and the interface is expected to say so where nobody can miss it.
  auth["configured"] = (s_.auth != nullptr) && s_.auth->configured();
  auth["signed_in"] = signedIn(request);
  if (s_.auth != nullptr) {
    const AuthState state = s_.auth->state();
    auth["sessions"] = state.sessions;
    auth["locked"] = state.lockedUntilUs > s_.clock->nowMicros();
  }

  if (s_.metrics != nullptr) {
    out["network_mode"] = s_.metrics->networkMode();
    out["ip"] = jsonCopy(s_.metrics->ipAddress());
    out["hostname"] = jsonCopy(s_.metrics->hostname());
  }

  JsonObject counts = out["counts"].to<JsonObject>();
  counts["devices"] = s_.devices->activeCount();
  counts["channels"] = s_.channels->activeCount();
  counts["modules"] = s_.registry->size();
  counts["devices_in_error"] =
      s_.devices->countInState(DeviceState::kError);

  // A device that is written in devices.json but refused to start is not in
  // any of the lists above: it has no record, no handle and no channel.  Until
  // this block existed it simply vanished, and the operator had no way to tell
  // "I never configured it" from "it failed and nobody said so" (§46).
  const BootReport& boot = s_.system->report();
  JsonObject bootOut = out["boot"].to<JsonObject>();
  bootOut["mode"] = toString(boot.mode);
  bootOut["storage_mounted"] = boot.storageMounted;
  bootOut["buses_started"] = boot.buses.applied;
  bootOut["buses_failed"] = boot.buses.failed;
  bootOut["devices_started"] = boot.devices.applied;
  bootOut["devices_failed"] = boot.devices.failed;
  bootOut["processing_applied"] = boot.processing.applied;
  bootOut["processing_failed"] = boot.processing.failed;
  if (boot.devices.failed > 0) {
    JsonObject failure = bootOut["first_failure"].to<JsonObject>();
    failure["device"] = jsonCopy(boot.devices.firstFailedKey.c_str());
    failure["field"] = jsonCopy(boot.devices.firstFailedField.c_str());
    failure["code"] = boot.devices.firstError.symbol();
    failure["numeric"] = static_cast<int>(boot.devices.firstError.code);
    failure["detail"] = jsonCopy(boot.devices.firstError.detail.c_str());
  }
  if (!boot.safeModeReason.ok()) {
    JsonObject safe = bootOut["safe_mode"].to<JsonObject>();
    safe["code"] = boot.safeModeReason.symbol();
    safe["detail"] = jsonCopy(boot.safeModeReason.detail.c_str());
  }
  if (!boot.storageError.ok()) {
    JsonObject storage = bootOut["storage_error"].to<JsonObject>();
    storage["code"] = boot.storageError.symbol();
    storage["detail"] = jsonCopy(boot.storageError.detail.c_str());
  }
}

// ---------------------------------------------------------------------------
//  /diagnostics
// ---------------------------------------------------------------------------
void RestApi::handleDiagnostics(ApiResponse& response) {
  JsonObject out = response.body.to<JsonObject>();
  out["uptime_ms"] = s_.clock->nowMicros() / 1000ULL;

  if (s_.metrics != nullptr) {
    const HeapMetrics heap = s_.metrics->heap();
    JsonObject memory = out["heap"].to<JsonObject>();
    memory["free"] = heap.freeBytes;
    // The low-water mark, not the current value, is what predicts a crash.
    memory["min_free"] = heap.minFreeBytes;
    memory["largest_block"] = heap.largestBlock;
    memory["total"] = heap.totalBytes;
    if (heap.hasPsram) memory["psram_free"] = heap.psramFreeBytes;

    JsonObject storage = out["filesystem"].to<JsonObject>();
    storage["used"] = s_.metrics->filesystemUsedBytes();
    storage["total"] = s_.metrics->filesystemTotalBytes();

    JsonObject sketch = out["firmware"].to<JsonObject>();
    sketch["used"] = s_.metrics->sketchUsedBytes();
    sketch["total"] = s_.metrics->sketchTotalBytes();

    JsonObject network = out["network"].to<JsonObject>();
    network["mode"] = s_.metrics->networkMode();
    network["ip"] = jsonCopy(s_.metrics->ipAddress());
    network["rssi"] = s_.metrics->wifiRssi();
  }

  JsonObject loop = out["loop"].to<JsonObject>();
  loop["passes"] = s_.scheduler->passCount();
  loop["max_pass_us"] = s_.scheduler->maxPassDurationUs();
  loop["budget_exhausted"] = s_.scheduler->budgetExhaustedCount();
  serializeSchedulerStats(*s_.scheduler, out["tasks"].to<JsonArray>());

  JsonObject data = out["data"].to<JsonObject>();
  data["published_samples"] = s_.channels->publishedSamples();
  data["suppressed_samples"] = s_.channels->suppressedSamples();
  data["active_channels"] = s_.channels->activeCount();
  data["processing_stages"] = s_.processing->totalStages();

  JsonObject events = out["events"].to<JsonObject>();
  events["published"] = s_.events->publishedCount();
  events["dropped"] = s_.events->droppedCount();

  JsonObject api = out["api"].to<JsonObject>();
  api["requests"] = requests_;
  api["errors"] = errors_;

  if (s_.buses != nullptr) {
    JsonArray buses = out["i2c"].to<JsonArray>();
    for (std::uint8_t i = 0; i < s_.buses->i2cBusCount(); ++i) {
      II2cBus* bus = s_.buses->i2c(i);
      if (bus == nullptr) continue;
      JsonObject entry = buses.add<JsonObject>();
      entry["index"] = i;
      entry["errors"] = bus->errorCount();
    }
  }

  serializeResourceClaims(*s_.resources, out["resources"].to<JsonArray>());
}

// ---------------------------------------------------------------------------
//  /modules
// ---------------------------------------------------------------------------
void RestApi::handleModules(const PathSegments& path, ApiResponse& response) {
  if (path.count() >= 4) {
    const ModuleManifest* manifest = s_.registry->manifestById(path.at(3));
    if (manifest == nullptr) {
      response.setError(404, fail(ErrorCode::kDriverNotRegistered, path.at(3)));
      return;
    }
    serializeManifest(*manifest, response.body.to<JsonObject>());
    return;
  }

  JsonArray out = response.body["modules"].to<JsonArray>();
  for (std::size_t i = 0; i < s_.registry->size(); ++i) {
    const ModuleDescriptor& descriptor = s_.registry->at(i);
    if (descriptor.manifest == nullptr) continue;
    serializeManifest(*descriptor.manifest, out.add<JsonObject>());
  }
}

// ---------------------------------------------------------------------------
//  /gpio
// ---------------------------------------------------------------------------
void RestApi::handleGpio(ApiResponse& response) {
  serializeGpioMap(*s_.resources, response.body.to<JsonObject>());
}

// ---------------------------------------------------------------------------
//  /buses
// ---------------------------------------------------------------------------
void RestApi::handleBuses(const ApiRequest& request, const PathSegments& path,
                          ApiResponse& response) {
  // POST /buses/i2c/{n}/scan
  if (path.count() >= 6 && path.is(3, "i2c") && path.is(5, "scan")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (s_.buses == nullptr) {
      response.setError(fail(ErrorCode::kBusNotConfigured, "no bus layer"));
      return;
    }
    const int index = std::atoi(path.at(4));
    II2cBus* bus = s_.buses->i2c(static_cast<std::uint8_t>(index));
    if (bus == nullptr) {
      response.setError(fail(ErrorCode::kBusNotConfigured, "bus not configured"));
      return;
    }

    I2cScanEntry entries[I2cScanner::kMaxResults];
    const std::size_t found = I2cScanner::scan(*bus, entries, I2cScanner::kMaxResults);

    JsonObject out = response.body.to<JsonObject>();
    out["bus"] = index;
    JsonArray list = out["found"].to<JsonArray>();
    for (std::size_t i = 0; i < found; ++i) {
      JsonObject device = list.add<JsonObject>();
      char address[6];
      std::snprintf(address, sizeof(address), "0x%02X", entries[i].address);
      device["address"] = jsonCopy(address);
      device["address_decimal"] = entries[i].address;

      const ResourceClaim* claim = s_.resources->find(
          i2cAddressResource(static_cast<std::uint8_t>(index), entries[i].address));
      if (claim != nullptr) device["claimed_by"] = jsonCopy(claim->label.c_str());

      // Candidates, never conclusions: 0x76 is BMP280 *and* BME280 *and*
      // several unrelated parts.  The driver's chip ID check decides.
      JsonArray candidates = device["candidates"].to<JsonArray>();
      for (std::uint8_t h = 0; h < entries[i].hintCount; ++h) {
        JsonObject hint = candidates.add<JsonObject>();
        hint["module"] = entries[i].hints[h].moduleId;
        hint["label"] = entries[i].hints[h].label;
        hint["confidence"] =
            entries[i].hints[h].confidence == HintConfidence::kLikely ? "likely"
                                                                     : "possible";
      }
    }
    return;
  }

  if (request.method != HttpMethod::kGet) {
    response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
    return;
  }

  JsonArray out = response.body["i2c"].to<JsonArray>();
  if (s_.buses == nullptr) return;
  for (std::uint8_t i = 0; i < s_.buses->i2cBusCount(); ++i) {
    JsonObject entry = out.add<JsonObject>();
    entry["index"] = i;
    II2cBus* bus = s_.buses->i2c(i);
    entry["configured"] = bus != nullptr;
    if (bus != nullptr) entry["errors"] = bus->errorCount();
  }
}

// ---------------------------------------------------------------------------
//  /devices
// ---------------------------------------------------------------------------
void RestApi::handleDevices(const ApiRequest& request, const PathSegments& path,
                            ApiResponse& response) {
  if (path.count() == 3) {
    if (request.method == HttpMethod::kPost) {
      createDevice(request, response);
      return;
    }
    if (request.method != HttpMethod::kGet) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET or POST"));
      return;
    }
    JsonArray out = response.body["devices"].to<JsonArray>();
    for (std::size_t i = 0; i < DeviceManager::capacity(); ++i) {
      const DeviceRecord& record = s_.devices->slot(i);
      if (!record.active) continue;
      describeDevice(record, out.add<JsonObject>());
    }
    return;
  }

  const char* key = path.at(3);

  // /devices/{key}/actions/{name}
  if (path.count() >= 6 && path.is(4, "actions")) {
    deviceAction(request, key, path.at(5), response);
    return;
  }

  switch (request.method) {
    case HttpMethod::kGet: {
      const DeviceRecord* record = s_.devices->findByKey(key);
      if (record == nullptr) {
        response.setError(404, fail(ErrorCode::kNotFound, key));
        return;
      }
      describeDevice(*record, response.body.to<JsonObject>());

      // The stored configuration lives in the file, not in the record
      // (ADR-0010), so the form is populated from there.
      JsonDocument stored;
      if (s_.storage->load(ConfigSection::kDevices, stored).ok()) {
        JsonObjectConst entry =
            ConfigApplier::findDevice(stored, key);
        if (!entry.isNull()) response.body["config"] = entry["config"];
      }
      return;
    }
    case HttpMethod::kPatch:
    case HttpMethod::kPut:
      patchDevice(request, key, response);
      return;
    case HttpMethod::kDelete:
      deleteDevice(key, response);
      return;
    default:
      response.setError(405, fail(ErrorCode::kNotSupported, "method"));
      return;
  }
}

void RestApi::createDevice(const ApiRequest& request, ApiResponse& response) {
  if (!requireWriteAccess(request, response)) return;

  JsonDocument entry;
  if (!parseBody(request, entry, response)) return;

  const char* moduleId = entry["module"] | "";
  if (moduleId[0] == '\0') {
    response.setError(422, fail(ErrorCode::kInvalidArgument, "module is missing"),
                      "module");
    return;
  }

  DeviceSpec spec;
  const Status parsed = ConfigApplier::parseDeviceSpec(entry.as<JsonObjectConst>(), spec);
  if (!parsed.ok()) {
    response.setError(parsed, "key");
    return;
  }

  JsonConfigView config(entry["config"].as<JsonObjectConst>());
  LabelString field;

  // The key check runs before the dry_run branch on purpose: a dry run that
  // approves a key the real create would reject with 409 turns live form
  // validation back into a guess (the same class of defect as ADR-0013 #1).
  if (s_.devices->findByKey(spec.key.c_str()) != nullptr) {
    response.setError(409, fail(ErrorCode::kAlreadyExists, spec.key.c_str()), "key",
                      "a device with this key already exists");
    return;
  }

  // ?dry_run=1 runs exactly the same validation the real create would, which is
  // what makes live form validation trustworthy instead of a second guess.
  if (request.queryFlag("dry_run")) {
    const Status valid = s_.devices->validate(moduleId, config, &field);
    if (!valid.ok()) {
      response.setError(valid, field.c_str());
      return;
    }
    response.body["valid"] = true;
    response.body["key"] = jsonCopy(spec.key.c_str());
    return;
  }

  // 1. write the file first: a device that runs but was never stored would
  //    disappear at the next reboot with no explanation.
  JsonDocument document;
  const Status loaded = s_.storage->load(ConfigSection::kDevices, document);
  if (!loaded.ok() && loaded.code != ErrorCode::kNotFound) {
    response.setError(loaded);
    return;
  }
  const Status upserted =
      ConfigApplier::upsertDevice(document, entry.as<JsonObjectConst>());
  if (!upserted.ok()) {
    response.setError(upserted, "key");
    return;
  }
  const Status saved = persistDevices(document);
  if (!saved.ok()) {
    response.setError(saved);
    return;
  }

  // 2. bring it up.
  const Result<DeviceHandle> added =
      s_.devices->add(moduleId, spec, config, &field);
  if (!added.ok()) {
    // 3. roll the file back, so the stored configuration and the running rig
    //    never disagree.
    ConfigApplier::removeDevice(document, spec.key.c_str());
    persistDevices(document);
    response.setError(added.error(), field.c_str());
    return;
  }

  response.status = 201;
  describeDevice(*s_.devices->find(added.value()), response.body.to<JsonObject>());
}

void RestApi::patchDevice(const ApiRequest& request, const char* key,
                          ApiResponse& response) {
  if (!requireWriteAccess(request, response)) return;

  const DeviceRecord* record = s_.devices->findByKey(key);
  if (record == nullptr) {
    response.setError(404, fail(ErrorCode::kNotFound, key));
    return;
  }
  const DeviceHandle handle = record->handle;

  JsonDocument patch;
  if (!parseBody(request, patch, response)) return;

  JsonDocument document;
  const Status loaded = s_.storage->load(ConfigSection::kDevices, document);
  if (!loaded.ok()) {
    response.setError(loaded);
    return;
  }

  // Merge the patch into the stored entry.  Top-level keys are replaced whole;
  // "config" is replaced whole too, because a half-updated driver
  // configuration is not a state anyone can reason about.
  JsonObject stored;
  for (JsonObject candidate : document["devices"].as<JsonArray>()) {
    if (std::strcmp(candidate["key"] | "", key) == 0) {
      stored = candidate;
      break;
    }
  }
  if (stored.isNull()) {
    response.setError(fail(ErrorCode::kConfigCorrupt,
                           "device is running but missing from devices.json"));
    return;
  }

  JsonDocument previous;
  previous.set(stored);

  for (JsonPairConst pair : patch.as<JsonObjectConst>()) {
    if (std::strcmp(pair.key().c_str(), "key") == 0) continue;  // key is identity
    stored[pair.key()] = pair.value();
  }

  const char* moduleId = stored["module"] | "";
  DeviceSpec spec;
  const Status parsed = ConfigApplier::parseDeviceSpec(stored, spec);
  if (!parsed.ok()) {
    response.setError(parsed);
    return;
  }
  JsonConfigView config(stored["config"].as<JsonObjectConst>());

  LabelString field;
  const Status valid = s_.devices->validate(moduleId, config, &field, handle);
  if (!valid.ok()) {
    response.setError(valid, field.c_str());
    return;
  }

  const Status saved = persistDevices(document);
  if (!saved.ok()) {
    response.setError(saved);
    return;
  }

  const Status reconfigured =
      s_.devices->reconfigure(handle, moduleId, spec, config, &field);
  if (!reconfigured.ok()) {
    // Put the old entry back and re-apply it, so the rig ends up in the state
    // the file describes either way.
    ConfigApplier::upsertDevice(document, previous.as<JsonObjectConst>());
    persistDevices(document);
    s_.system->reloadConfiguration();
    response.setError(reconfigured, field.c_str());
    return;
  }

  const DeviceRecord* updated = s_.devices->findByKey(key);
  if (updated != nullptr) {
    describeDevice(*updated, response.body.to<JsonObject>());
  }
}

void RestApi::deleteDevice(const char* key, ApiResponse& response) {
  const DeviceRecord* record = s_.devices->findByKey(key);
  if (record == nullptr) {
    response.setError(404, fail(ErrorCode::kNotFound, key));
    return;
  }
  const DeviceHandle handle = record->handle;

  JsonDocument document;
  if (s_.storage->load(ConfigSection::kDevices, document).ok()) {
    ConfigApplier::removeDevice(document, key);
    const Status saved = persistDevices(document);
    if (!saved.ok()) {
      response.setError(saved);
      return;
    }
  }

  const Status removed = s_.devices->remove(handle);
  if (!removed.ok()) {
    response.setError(removed);
    return;
  }
  response.body["deleted"] = jsonCopy(key);
}

void RestApi::deviceAction(const ApiRequest& request, const char* key,
                           const char* action, ApiResponse& response) {
  if (request.method != HttpMethod::kPost) {
    response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
    return;
  }
  if (!requireWriteAccess(request, response)) return;

  const DeviceRecord* record = s_.devices->findByKey(key);
  if (record == nullptr) {
    response.setError(404, fail(ErrorCode::kNotFound, key));
    return;
  }

  if (std::strcmp(action, "self-test") == 0) {
    if (record->instance == nullptr) {
      response.setError(fail(ErrorCode::kInvalidState, "device is not built"));
      return;
    }
    const Status result = record->instance->selfTest();
    response.body["passed"] = result.ok();
    if (!result.ok()) {
      // Not an HTTP error: the request succeeded, the sensor failed.  Mixing
      // the two would make a failing self-test look like a broken API.
      JsonObject error = response.body["error"].to<JsonObject>();
      error["code"] = result.symbol();
      error["numeric"] = static_cast<int>(result.code);
      error["detail"] = jsonCopy(result.detail.c_str());
    }
    return;
  }

  if (std::strcmp(action, "enable") == 0 || std::strcmp(action, "disable") == 0) {
    const bool enable = (action[0] == 'e');
    const Status status = s_.devices->setEnabled(record->handle, enable);
    if (!status.ok()) {
      response.setError(status);
      return;
    }
    const DeviceRecord* updated = s_.devices->findByKey(key);
    if (updated != nullptr) {
      describeDevice(*updated, response.body.to<JsonObject>());
    }
    return;
  }

  response.setError(404, fail(ErrorCode::kNotFound, action));
}

// ---------------------------------------------------------------------------
//  /channels
// ---------------------------------------------------------------------------
void RestApi::handleChannels(const ApiRequest& request, const PathSegments& path,
                             ApiResponse& response) {
  if (path.count() == 3) {
    if (request.method != HttpMethod::kGet) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
      return;
    }
    const bool withValues = request.queryFlag("values");
    JsonArray out = response.body["channels"].to<JsonArray>();
    for (std::size_t i = 1; i <= ChannelManager::capacity(); ++i) {
      const ChannelHandle handle = static_cast<ChannelHandle>(i);
      const ChannelDescriptor* descriptor = s_.channels->descriptor(handle);
      if (descriptor == nullptr) continue;
      JsonObject entry = out.add<JsonObject>();
      serializeChannel(handle, *descriptor, *s_.channels->value(handle),
                       entry, withValues);
      // An output's safety state travels with the channel everywhere: the
      // dashboard, the channel table and the write response all read the same
      // field, so there is no screen on which an actuator looks unattended.
      describeOutput(handle, entry);
    }
    return;
  }

  const char* key = path.at(3);
  const ChannelHandle handle = s_.channels->findByKey(key);
  if (handle == kInvalidChannel) {
    response.setError(404, fail(ErrorCode::kChannelNotFound, key));
    return;
  }

  // POST /channels/{key}/write
  if (path.count() >= 5 && path.is(4, "write")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;

    JsonDocument body;
    if (!parseBody(request, body, response)) return;
    if (!body["value"].is<float>()) {
      response.setError(422, fail(ErrorCode::kInvalidArgument, "value is required"),
                        "value");
      return;
    }
    // Through the safety layer, never straight at the channel.  Commanding an
    // output is the one operation in this API that can start a fire, and it has
    // exactly one door (ADR-0016).
    const Status written =
        (s_.outputs != nullptr)
            ? s_.outputs->command(handle, body["value"].as<float>())
            : fail(ErrorCode::kNotSupported, "this build has no output support");
    if (!written.ok()) {
      response.setError(written, "value");
      return;
    }
    JsonObject out = response.body.to<JsonObject>();
    serializeChannel(handle, *s_.channels->descriptor(handle),
                     *s_.channels->value(handle), out, true);
    describeOutput(handle, out);
    return;
  }

  if (request.method != HttpMethod::kGet) {
    response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
    return;
  }
  JsonObject single = response.body.to<JsonObject>();
  serializeChannel(handle, *s_.channels->descriptor(handle),
                   *s_.channels->value(handle), single,
                   true);
  describeOutput(handle, single);
}

// ---------------------------------------------------------------------------
//  /processing/{channelKey}
// ---------------------------------------------------------------------------
namespace {

// Adapts the stored pipeline description to what ProcessingManager expects.
class StoredPipeline final : public IPipelineSource {
 public:
  explicit StoredPipeline(JsonObjectConst pipeline)
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
    view_.bind(stages_[index]["config"].as<JsonObjectConst>());
    return view_;
  }
  std::int8_t calibrationStage() const override { return calibrationStage_; }

 private:
  JsonArrayConst stages_;
  std::int8_t calibrationStage_;
  mutable JsonConfigView view_;
};

}  // namespace

void RestApi::handleProcessing(const ApiRequest& request,
                               const PathSegments& path, ApiResponse& response) {
  if (path.count() < 4) {
    response.setError(404, fail(ErrorCode::kNotFound, "channel key required"));
    return;
  }
  const char* key = path.at(3);
  const ChannelHandle handle = s_.channels->findByKey(key);
  if (handle == kInvalidChannel) {
    response.setError(404, fail(ErrorCode::kChannelNotFound, key));
    return;
  }

  if (request.method == HttpMethod::kGet) {
    JsonDocument stored;
    if (s_.storage->load(ConfigSection::kProcessing, stored).ok()) {
      JsonObjectConst pipeline = stored["pipelines"][key];
      if (!pipeline.isNull()) {
        response.body.set(pipeline);
      }
    }
    // What is actually running, which may differ from the file if a stage was
    // rejected at boot.
    JsonArray active = response.body["active_stages"].to<JsonArray>();
    for (std::size_t i = 0; i < s_.processing->stageCount(handle); ++i) {
      active.add(s_.processing->stageType(handle, i));
    }
    return;
  }

  if (request.method != HttpMethod::kPut) {
    response.setError(405, fail(ErrorCode::kNotSupported, "use GET or PUT"));
    return;
  }
  if (!requireWriteAccess(request, response)) return;

  JsonDocument body;
  if (!parseBody(request, body, response)) return;

  // The stored calibration stage is a placeholder; the coefficients live in
  // calibrations.json.  Resolving it here is not optional: without it, saving
  // the pipeline from the browser would configure the calibration stage with
  // {"source":"active"} — which the processor reads as "identity" — and the
  // channel would silently go back to raw counts under a unit that still said
  // grams.  Found the first time the pipeline editor was used.
  JsonDocument calibrations;
  s_.storage->load(ConfigSection::kCalibrations, calibrations);
  JsonDocument resolved;
  const Status merged = CalibrationStore::resolvePipeline(
      body.as<JsonObjectConst>(),
      CalibrationStore::findActive(
          CalibrationStore::calibrationsArray(calibrations), key),
      resolved);
  if (!merged.ok()) {
    response.setError(merged, "stages");
    return;
  }

  // Apply first, persist second: an unbuildable pipeline must not end up in the
  // file where it would fail again at every boot.  What is persisted is the
  // body as sent — with the placeholder, never the resolved coefficients.
  const StoredPipeline pipeline(resolved.as<JsonObjectConst>());
  const Status applied = s_.processing->apply(handle, pipeline);
  if (!applied.ok()) {
    response.setError(applied, "stages");
    return;
  }

  JsonDocument document;
  const Status loaded = s_.storage->load(ConfigSection::kProcessing, document);
  if (!loaded.ok() && loaded.code != ErrorCode::kNotFound) {
    response.setError(loaded);
    return;
  }
  document["pipelines"][key] = body;
  const Status saved = s_.storage->save(ConfigSection::kProcessing, document);
  if (!saved.ok()) {
    response.setError(saved);
    return;
  }

  JsonArray active = response.body["active_stages"].to<JsonArray>();
  for (std::size_t i = 0; i < s_.processing->stageCount(handle); ++i) {
    active.add(s_.processing->stageType(handle, i));
  }
}


// ---------------------------------------------------------------------------
//  /calibrations  (§12)
// ---------------------------------------------------------------------------
//  Four verbs and one invariant: a record, once written, is never edited.
//  Recalibrating appends a version; rolling back activates one that is still
//  there.  That is what makes a dataset recorded last Tuesday attributable to
//  the exact numbers that produced it (§48).
// ---------------------------------------------------------------------------
void RestApi::handleCalibrations(const ApiRequest& request,
                                 const PathSegments& path,
                                 ApiResponse& response) {
  if (s_.calibrations == nullptr) {
    response.setError(fail(ErrorCode::kNotSupported,
                           "this build has no calibration support"));
    return;
  }

  // /calibrations/solve — fit and report, store nothing.
  if (path.count() >= 4 && path.is(3, "solve")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    solveCalibration(request, response);
    return;
  }

  if (path.count() < 4) {
    if (request.method == HttpMethod::kGet) {
      listCalibrations(request, response);
      return;
    }
    if (request.method == HttpMethod::kPost) {
      createCalibration(request, response);
      return;
    }
    response.setError(405, fail(ErrorCode::kNotSupported, "use GET or POST"));
    return;
  }

  const char* id = path.at(3);

  if (path.count() >= 5) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (path.is(4, "activate")) {
      activateCalibration(id, true, response);
      return;
    }
    if (path.is(4, "deactivate")) {
      activateCalibration(id, false, response);
      return;
    }
    response.setError(404, fail(ErrorCode::kNotFound, path.at(4)));
    return;
  }

  if (request.method == HttpMethod::kDelete) {
    deleteCalibration(id, response);
    return;
  }
  if (request.method != HttpMethod::kGet) {
    response.setError(405, fail(ErrorCode::kNotSupported, "use GET or DELETE"));
    return;
  }

  JsonDocument stored;
  s_.storage->load(ConfigSection::kCalibrations, stored);
  JsonObjectConst record = CalibrationStore::findById(stored, id);
  if (record.isNull()) {
    response.setError(404, fail(ErrorCode::kNotFound, id));
    return;
  }
  response.body.set(record);
}

void RestApi::solveCalibration(const ApiRequest& request,
                               ApiResponse& response) {
  JsonDocument body;
  if (!parseBody(request, body, response)) return;

  CalibrationDraft draft;
  LabelString field;
  const Status parsed =
      CalibrationStore::parseDraft(body.as<JsonObjectConst>(), draft, &field);
  if (!parsed.ok()) {
    response.setError(parsed, field.c_str());
    return;
  }

  const Result<PolynomialFit> fit = CalibrationStore::solve(draft);
  if (!fit.ok()) {
    response.setError(fit.error(), "points");
    return;
  }

  response.body["kind"] = toString(draft.kind);
  CalibrationStore::serializeFit(fit.value(), response.body["fit"].to<JsonObject>());
  // Per-point residuals, not just the summary: an average of 0.02 g hides the
  // one weight that is out by 0.4 g, and that one point is the whole story.
  CalibrationStore::serializeResiduals(
      draft, fit.value(), response.body["residuals"].to<JsonArray>());
  response.body["stored"] = false;
}

void RestApi::listCalibrations(const ApiRequest& request,
                               ApiResponse& response) {
  char buffer[limits::kKeyLength + 1];
  const char* channel =
      request.queryValue("channel", buffer, sizeof(buffer), nullptr);

  JsonDocument stored;
  s_.storage->load(ConfigSection::kCalibrations, stored);
  JsonArrayConst records = CalibrationStore::calibrationsArray(stored);

  JsonArray out = response.body["calibrations"].to<JsonArray>();
  if (!records.isNull()) {
    for (JsonObjectConst record : records) {
      if (channel != nullptr &&
          std::strcmp(record["channel"] | "", channel) != 0) {
        continue;
      }
      out.add(record);
    }
  }
}

void RestApi::createCalibration(const ApiRequest& request,
                                ApiResponse& response) {
  if (!requireWriteAccess(request, response)) return;

  JsonDocument body;
  if (!parseBody(request, body, response)) return;

  CalibrationDraft draft;
  LabelString field;
  Status status =
      CalibrationStore::parseDraft(body.as<JsonObjectConst>(), draft, &field);
  if (!status.ok()) {
    response.setError(status, field.c_str());
    return;
  }

  const ChannelHandle handle = s_.channels->findByKey(draft.channel.c_str());
  if (handle == kInvalidChannel) {
    response.setError(404, fail(ErrorCode::kChannelNotFound, draft.channel.c_str()),
                      "channel");
    return;
  }

  const Result<PolynomialFit> fit = CalibrationStore::solve(draft);
  if (!fit.ok()) {
    response.setError(fit.error(), "points");
    return;
  }

  JsonDocument stored;
  const Status loaded = s_.storage->load(ConfigSection::kCalibrations, stored);
  if (!loaded.ok() && loaded.code != ErrorCode::kNotFound) {
    response.setError(loaded);
    return;
  }

  const std::uint16_t version =
      CalibrationStore::nextVersion(stored, draft.channel.c_str());
  CalibrationIdString id;
  if (!CalibrationManager::makeId(draft.channel.c_str(), version, id)) {
    response.setError(fail(ErrorCode::kInvalidArgument,
                           "channel key leaves no room for a version"),
                      "channel");
    return;
  }

  const bool activate = body["activate"] | true;
  if (activate) CalibrationStore::deactivateChannel(stored, draft.channel.c_str());

  status = CalibrationStore::append(stored, draft, fit.value(), id.c_str(),
                                    version, s_.clock->epochMillis(), activate,
                                    nullptr);
  if (!status.ok()) {
    response.setError(status);
    return;
  }

  status = s_.storage->save(ConfigSection::kCalibrations, stored);
  if (!status.ok()) {
    response.setError(status);
    return;
  }

  if (activate) {
    status = reapplyChannel(draft.channel.c_str(), stored);
    if (!status.ok()) {
      response.setError(status, "channel");
      return;
    }
  }

  response.status = 201;
  response.body["id"] = jsonCopy(id.c_str());
  response.body["channel"] = jsonCopy(draft.channel.c_str());
  response.body["version"] = version;
  response.body["active"] = activate;
  CalibrationStore::serializeFit(fit.value(), response.body["fit"].to<JsonObject>());
  CalibrationStore::serializeResiduals(
      draft, fit.value(), response.body["residuals"].to<JsonArray>());
}

void RestApi::activateCalibration(const char* id, bool active,
                                  ApiResponse& response) {
  JsonDocument stored;
  const Status loaded = s_.storage->load(ConfigSection::kCalibrations, stored);
  if (!loaded.ok()) {
    response.setError(404, fail(ErrorCode::kNotFound, id));
    return;
  }

  JsonObjectConst record = CalibrationStore::findById(stored, id);
  if (record.isNull()) {
    response.setError(404, fail(ErrorCode::kNotFound, id));
    return;
  }

  KeyString channel;
  channel.assign(record["channel"] | "");
  if (active) {
    // One active calibration per channel is an invariant, not a convention:
    // two of them means the number on screen depends on which one the pipeline
    // happened to pick up.
    CalibrationStore::deactivateChannel(stored, channel.c_str());
    CalibrationStore::findByIdMutable(stored, id)["active"] = true;
  } else {
    CalibrationStore::findByIdMutable(stored, id)["active"] = false;
  }

  Status status = s_.storage->save(ConfigSection::kCalibrations, stored);
  if (!status.ok()) {
    response.setError(status);
    return;
  }

  status = reapplyChannel(channel.c_str(), stored);
  if (!status.ok()) {
    response.setError(status, "channel");
    return;
  }

  response.body["id"] = jsonCopy(id);
  response.body["channel"] = jsonCopy(channel.c_str());
  response.body["active"] = active;
}

void RestApi::deleteCalibration(const char* id, ApiResponse& response) {
  JsonDocument stored;
  if (!s_.storage->load(ConfigSection::kCalibrations, stored).ok()) {
    response.setError(404, fail(ErrorCode::kNotFound, id));
    return;
  }

  JsonObjectConst record = CalibrationStore::findById(stored, id);
  if (record.isNull()) {
    response.setError(404, fail(ErrorCode::kNotFound, id));
    return;
  }
  if (record["active"] | false) {
    // Deleting the running calibration would silently change every reading on
    // that channel.  Deactivate it first, deliberately.
    response.setError(409,
                      fail(ErrorCode::kResourceBusy, "calibration is active"),
                      "id",
                      "deactivate this calibration before deleting it");
    return;
  }

  CalibrationStore::removeById(stored, id);
  const Status saved = s_.storage->save(ConfigSection::kCalibrations, stored);
  if (!saved.ok()) {
    response.setError(saved);
    return;
  }
  response.body["deleted"] = jsonCopy(id);
}

Status RestApi::reapplyChannel(const char* channelKey,
                               JsonDocument& calibrations) {
  const ChannelHandle handle = s_.channels->findByKey(channelKey);
  if (handle == kInvalidChannel) {
    return fail(ErrorCode::kChannelNotFound, channelKey);
  }

  JsonArrayConst records = CalibrationStore::calibrationsArray(calibrations);
  JsonObjectConst active = CalibrationStore::findActive(records, channelKey);

  // --- the in-RAM half -----------------------------------------------------
  if (active.isNull()) {
    s_.calibrations->clearActive(channelKey);
    // Put the unit back to what the channel was created with.  Leaving it at
    // "g" once the grams have been taken away would show raw ADC counts under
    // a unit that makes them look like a real mass.
    const Status restored = s_.channels->resetPresentation(handle);
    if (!restored.ok()) return restored;
  } else {
    ActiveCalibration record;
    Status status = CalibrationStore::toActive(active, record);
    if (!status.ok()) return status;
    status = s_.calibrations->setActive(record);
    if (!status.ok()) return status;
    status = s_.channels->setPresentation(
        handle, record.unit.empty() ? nullptr : record.unit.c_str(),
        static_cast<std::uint8_t>(active["precision"] | 0),
        active["min"] | 0.0f, active["max"] | 0.0f);
    if (!status.ok()) return status;
  }

  // --- the pipeline --------------------------------------------------------
  JsonDocument processing;
  const Status loaded = s_.storage->load(ConfigSection::kProcessing, processing);
  if (!loaded.ok() && loaded.code != ErrorCode::kNotFound) return loaded;

  JsonObject pipeline = processing["pipelines"][channelKey].to<JsonObject>();
  bool changed = false;
  if (!active.isNull()) {
    const Result<bool> installed = CalibrationStore::installStage(pipeline);
    if (!installed.ok()) return installed.error();
    changed = installed.value();
  }

  JsonDocument resolved;
  Status status = CalibrationStore::resolvePipeline(pipeline, active, resolved);
  if (!status.ok()) return status;

  const StoredPipeline source(resolved.as<JsonObjectConst>());
  status = s_.processing->apply(handle, source);
  if (!status.ok()) return status;

  // Persist only the placeholder, and only when the document actually changed:
  // rewriting processing.json on every activation would burn flash for nothing.
  if (changed) return s_.storage->save(ConfigSection::kProcessing, processing);
  return ok();
}


// ---------------------------------------------------------------------------
//  /dashboards  (§22–§26)
// ---------------------------------------------------------------------------
//  The firmware stores these and checks their shape; it does not know what a
//  widget is (ADR-0015).  The one thing it does understand is a channel
//  reference, and a broken one is reported rather than removed.
// ---------------------------------------------------------------------------
void RestApi::handleDashboards(const ApiRequest& request,
                               const PathSegments& path, ApiResponse& response) {
  JsonDocument stored;
  const Status loaded = s_.storage->load(ConfigSection::kDashboards, stored);
  if (!loaded.ok() && loaded.code != ErrorCode::kNotFound) {
    response.setError(loaded);
    return;
  }

  // --- the list ------------------------------------------------------------
  if (path.count() < 4) {
    if (request.method == HttpMethod::kGet) {
      // Summaries only: eight dashboards of twenty-four widgets do not fit in
      // one response, and the picker needs names, not layouts.
      DashboardStore::summarise(stored, *s_.channels,
                                response.body["dashboards"].to<JsonArray>());
      response.body["limits"]["dashboards"] = limits::kMaxDashboards;
      response.body["limits"]["widgets"] = limits::kMaxWidgetsPerDashboard;
      response.body["limits"]["columns"] = limits::kDashboardGridColumns;
      return;
    }
    if (request.method == HttpMethod::kPost) {
      saveDashboard(request, stored, nullptr, response);
      return;
    }
    response.setError(405, fail(ErrorCode::kNotSupported, "use GET or POST"));
    return;
  }

  const char* key = path.at(3);

  switch (request.method) {
    case HttpMethod::kGet: {
      JsonObjectConst dashboard = DashboardStore::findByKey(stored, key);
      if (dashboard.isNull()) {
        response.setError(404, fail(ErrorCode::kNotFound, key));
        return;
      }
      response.body.set(dashboard);
      // as<>, NOT to<>: to<JsonObject>() CLEARS the document and hands back a
      // fresh empty object, which would silently discard the dashboard that
      // was just copied into it.  Same family of trap as jsonCopy().
      describeDashboardHealth(dashboard, response.body.as<JsonObject>());
      return;
    }
    case HttpMethod::kPut:
      saveDashboard(request, stored, key, response);
      return;
    case HttpMethod::kDelete: {
      if (!requireWriteAccess(request, response)) return;
      if (!DashboardStore::removeByKey(stored, key)) {
        response.setError(404, fail(ErrorCode::kNotFound, key));
        return;
      }
      const Status saved = s_.storage->save(ConfigSection::kDashboards, stored);
      if (!saved.ok()) {
        response.setError(saved);
        return;
      }
      response.body["deleted"] = jsonCopy(key);
      return;
    }
    default:
      response.setError(405, fail(ErrorCode::kNotSupported, "method"));
      return;
  }
}

void RestApi::describeDashboardHealth(JsonObjectConst dashboard,
                                      JsonObject out) const {
  const DashboardReport report =
      DashboardStore::inspect(dashboard, *s_.channels);
  JsonObject health = out["health"].to<JsonObject>();
  health["widgets"] = report.widgets;
  health["dangling_channels"] = report.danglingChannels;
  if (report.danglingChannels > 0) {
    // Named, not counted: "1 widget points at a channel that is gone" sends the
    // operator hunting; "w3 points at mass_01" does not.
    health["first_dangling"]["widget"] =
        jsonCopy(report.firstDanglingWidget.c_str());
    health["first_dangling"]["channel"] =
        jsonCopy(report.firstDanglingChannel.c_str());
  }
}

void RestApi::saveDashboard(const ApiRequest& request, JsonDocument& stored,
                            const char* pathKey, ApiResponse& response) {
  if (!requireWriteAccess(request, response)) return;

  JsonDocument body;
  if (!parseBody(request, body, response)) return;

  JsonObject dashboard = body.as<JsonObject>();
  if (dashboard.isNull()) {
    response.setError(400, fail(ErrorCode::kInvalidArgument,
                                "body must be a dashboard object"));
    return;
  }
  // PUT /dashboards/{key} wins over a key in the body: the URL is what the
  // browser addressed, and silently writing to a different dashboard because
  // the body disagreed would be the worst possible resolution.
  if (pathKey != nullptr) dashboard["key"] = jsonCopy(pathKey);

  LabelString field;
  Status status = DashboardStore::validate(dashboard, &field);
  if (!status.ok()) {
    response.setError(status, field.c_str());
    return;
  }

  status = DashboardStore::upsert(stored, dashboard);
  if (!status.ok()) {
    response.setError(status, "key");
    return;
  }

  status = s_.storage->save(ConfigSection::kDashboards, stored);
  if (!status.ok()) {
    // Almost always the document budget: the web interface shares this
    // partition, so the message says which limit was hit.
    response.setError(status, "widgets");
    return;
  }

  response.status = (pathKey == nullptr) ? 201 : 200;
  JsonObject out = response.body.to<JsonObject>();
  out["key"] = jsonCopy(dashboard["key"] | "");
  describeDashboardHealth(dashboard, out);
}

// ---------------------------------------------------------------------------
//  /outputs  (§27, §30)
// ---------------------------------------------------------------------------
void RestApi::describeOutput(ChannelHandle handle, JsonObject out) const {
  if (s_.outputs == nullptr) return;
  const OutputRecord* record = s_.outputs->find(handle);
  if (record == nullptr) return;

  JsonObject output = out["output"].to<JsonObject>();
  output["state"] = toString(record->state);
  output["safe_value"] = record->safeValue;
  output["commanded"] = record->commanded;
  output["applied"] = record->applied;
  output["hold_s"] = static_cast<double>(record->holdTimeoutUs) / 1e6;

  // Seconds left, not the deadline: an operator watching a heater wants to know
  // how long it has, and computing that from two timestamps in the browser is
  // one more place for the clocks to disagree.
  if (record->state == OutputHoldState::kCommanded && record->holdTimeoutUs > 0) {
    const Micros now = s_.clock->nowMicros();
    const Micros elapsed = (now > record->commandedAtUs)
                               ? (now - record->commandedAtUs) : 0;
    const double remaining =
        (elapsed >= record->holdTimeoutUs)
            ? 0.0
            : static_cast<double>(record->holdTimeoutUs - elapsed) / 1e6;
    output["expires_in_s"] = remaining;
  }
}

void RestApi::handleOutputs(const ApiRequest& request, const PathSegments& path,
                            ApiResponse& response) {
  if (s_.outputs == nullptr) {
    response.setError(fail(ErrorCode::kNotSupported,
                           "this build has no output support"));
    return;
  }

  // POST /outputs/trip  and  POST /outputs/clear — the master stop.
  if (path.count() >= 4 && (path.is(3, "trip") || path.is(3, "clear"))) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;
    if (path.is(3, "trip")) {
      // A literal, because the reason is held as a pointer with static
      // lifetime: whatever the caller sent has gone by the time it is read.
      s_.outputs->trip("stopped from the web interface");
    } else {
      // Clearing an operator's stop is one button.  Clearing a stop that an
      // interlock raised is not: the limit has to be reset by name, and a limit
      // whose cause has not gone away trips again immediately.  Without this,
      // "clear" would be a way to switch an interlock off (§30).
      if (s_.safety != nullptr && s_.safety->latchedCount() > 0) {
        response.setError(
            fail(ErrorCode::kSafetyInterlock,
                 "reset the latched safety limits first"),
            nullptr, "a safety limit is still latched");
        return;
      }
      s_.outputs->clearTrip();
    }
    response.body["tripped"] = s_.outputs->tripped();
    response.body["reason"] = jsonCopy(s_.outputs->tripReason());
    return;
  }

  // POST /outputs/{key}/renew — "still here", without changing the value.
  if (path.count() >= 5 && path.is(4, "renew")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;
    const ChannelHandle handle = s_.channels->findByKey(path.at(3));
    if (handle == kInvalidChannel) {
      response.setError(404, fail(ErrorCode::kChannelNotFound, path.at(3)));
      return;
    }
    const Status renewed = s_.outputs->renew(handle);
    if (!renewed.ok()) {
      response.setError(renewed);
      return;
    }
    describeOutput(handle, response.body.to<JsonObject>());
    return;
  }

  // POST /outputs/{key}/release — back to the safe value, deliberately.
  if (path.count() >= 5 && path.is(4, "release")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;
    const ChannelHandle handle = s_.channels->findByKey(path.at(3));
    if (handle == kInvalidChannel) {
      response.setError(404, fail(ErrorCode::kChannelNotFound, path.at(3)));
      return;
    }
    const Status released =
        s_.outputs->release(handle, OutputHoldState::kSafe);
    if (!released.ok()) {
      response.setError(released);
      return;
    }
    describeOutput(handle, response.body.to<JsonObject>());
    return;
  }

  if (request.method != HttpMethod::kGet) {
    response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
    return;
  }

  JsonObject out = response.body.to<JsonObject>();
  out["tripped"] = s_.outputs->tripped();
  out["reason"] = jsonCopy(s_.outputs->tripReason());
  out["expiries"] = s_.outputs->expiries();
  out["trips"] = s_.outputs->trips();

  JsonArray list = out["outputs"].to<JsonArray>();
  for (std::size_t i = 0; i < s_.outputs->count(); ++i) {
    const OutputRecord& record = s_.outputs->at(i);
    const ChannelDescriptor* descriptor = s_.channels->descriptor(record.channel);
    JsonObject entry = list.add<JsonObject>();
    entry["channel"] = jsonCopy(descriptor != nullptr ? descriptor->key.c_str() : "");
    entry["unit"] = jsonCopy(descriptor != nullptr ? descriptor->unit.c_str() : "");
    entry["handle"] = record.channel;
    describeOutput(record.channel, entry);
  }
}

// ---------------------------------------------------------------------------
//  /config
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Control: loops, rules and limits (§28, §29, §30)
// ---------------------------------------------------------------------------
namespace {

void describeFault(const Error& error, JsonObject out) {
  if (error.ok()) return;
  JsonObject fault = out["fault"].to<JsonObject>();
  fault["code"] = error.symbol();
  fault["numeric"] = static_cast<int>(error.code);
  fault["detail"] = jsonCopy(error.detail.c_str());
}

}  // namespace

void RestApi::describeLoop(const ControlLoop& loop, JsonObject out) const {
  ControlStore::serializeLoop(loop, out);
  out["mode"] = toString(loop.mode);
  out["state"] = toString(loop.state);
  // What the loop ASKED for.  Not the same thing as what the actuator is doing,
  // and the difference is the whole story when a loop is blocked or its output
  // is power-limited — see `output_applied` below.
  out["output_value"] = loop.lastOutput;
  out["last_error"] = loop.lastError;
  out["integral"] = loop.integral;

  // Whether the channels a loop names still exist is the first question
  // somebody has when a loop will not start, and it is not answerable from the
  // configuration alone — the device may have failed to come up.
  const ChannelHandle input = s_.channels->findByKey(loop.inputKey.c_str());
  const ChannelHandle output = s_.channels->findByKey(loop.outputKey.c_str());
  out["input_present"] = input != kInvalidChannel;
  out["output_present"] = output != kInvalidChannel;
  const ChannelValue* value =
      (input != kInvalidChannel) ? s_.channels->value(input) : nullptr;
  if (value != nullptr) {
    out["measured"] = value->processed;
    out["quality"] = toString(value->quality);
  }
  const ChannelDescriptor* descriptor =
      (input != kInvalidChannel) ? s_.channels->descriptor(input) : nullptr;
  if (descriptor != nullptr) out["unit"] = jsonCopy(descriptor->unit.c_str());

  // And what the actuator is ACTUALLY doing.  A blocked loop still computes a
  // demand — that is what makes it blocked rather than off — and showing that
  // number alone next to a heater sitting at zero is the same lie the output
  // channel used to tell before Milestone 7 separated the two.
  const ChannelValue* applied =
      (output != kInvalidChannel) ? s_.channels->value(output) : nullptr;
  if (applied != nullptr) out["output_applied"] = applied->processed;
  describeFault(loop.lastFault, out);
}

void RestApi::describeRule(const ControlRule& rule, JsonObject out) const {
  ControlStore::serializeRule(rule, out);
  out["engaged"] = rule.engaged;
  out["holding"] = rule.owns;
  out["activations"] = rule.activations;
  out["input_present"] =
      s_.channels->findByKey(rule.inputKey.c_str()) != kInvalidChannel;
  out["output_present"] =
      s_.channels->findByKey(rule.outputKey.c_str()) != kInvalidChannel;
}

void RestApi::describeLimit(const SafetyLimit& limit, JsonObject out) const {
  ControlStore::serializeLimit(limit, out);
  out["violating"] = limit.violating;
  out["latched"] = limit.latched;
  out["trips"] = limit.trips;
  out["channel_present"] =
      s_.channels->findByKey(limit.channelKey.c_str()) != kInvalidChannel;
  describeFault(limit.lastReason, out);
}

Status RestApi::persistControl() {
  JsonDocument document;
  ControlStore::serializeAll(*s_.control, s_.safety, document);
  return s_.storage->save(ConfigSection::kControl, document);
}

void RestApi::listControl(ApiResponse& response) {
  JsonObject out = response.body.to<JsonObject>();

  JsonArray limits = out["limits"].to<JsonArray>();
  if (s_.safety != nullptr) {
    for (std::size_t i = 0; i < s_.safety->count(); ++i) {
      describeLimit(s_.safety->at(i), limits.add<JsonObject>());
    }
    out["latched"] = s_.safety->latchedCount();
  }

  JsonArray loops = out["loops"].to<JsonArray>();
  for (std::size_t i = 0; i < s_.control->loopCount(); ++i) {
    describeLoop(s_.control->loopAt(i), loops.add<JsonObject>());
  }

  JsonArray rules = out["rules"].to<JsonArray>();
  for (std::size_t i = 0; i < s_.control->ruleCount(); ++i) {
    describeRule(s_.control->ruleAt(i), rules.add<JsonObject>());
  }

  if (s_.outputs != nullptr) {
    // A page full of loops that are commanding nothing needs to say why in the
    // one case where the reason is global.
    out["tripped"] = s_.outputs->tripped();
    out["trip_reason"] = jsonCopy(s_.outputs->tripReason());
  }
  JsonObject capacity = out["limits_max"].to<JsonObject>();
  capacity["loops"] = ControlManager::loopCapacity();
  capacity["rules"] = ControlManager::ruleCapacity();
  capacity["limits"] = SafetyManager::capacity();
}

void RestApi::replaceControl(const ApiRequest& request, ApiResponse& response) {
  JsonDocument body;
  if (!parseBody(request, body, response)) return;

  // --- validate the WHOLE document before touching anything ---------------
  // A half-applied control configuration is a rig with three of its four
  // interlocks.  Every entry is parsed here, and only if all of them parse is
  // anything installed.
  LabelString field;

  // Two entries with the same id are not a duplicate row, they are a silently
  // missing one: the second overwrites the first on install, and the operator
  // is left looking at a document with two interlocks and a rig with one.
  for (const char* list : {"limits", "loops", "rules"}) {
    JsonArrayConst entries = body[list].as<JsonArrayConst>();
    for (JsonObjectConst a : entries) {
      std::size_t seen = 0;
      for (JsonObjectConst b : entries) {
        if (std::strcmp(a["id"] | "", b["id"] | "") == 0) ++seen;
      }
      if (seen > 1) {
        response.setError(fail(ErrorCode::kInvalidArgument, a["id"] | ""), "id",
                          "two entries share this id");
        return;
      }
    }
  }
  for (JsonObjectConst entry : body["limits"].as<JsonArrayConst>()) {
    SafetyLimit limit;
    const Status parsed = ControlStore::parseLimit(entry, limit, field);
    if (!parsed.ok()) {
      response.setError(parsed, field.c_str(), "a safety limit is not valid");
      return;
    }
  }
  for (JsonObjectConst entry : body["loops"].as<JsonArrayConst>()) {
    ControlLoop loop;
    const Status parsed = ControlStore::parseLoop(entry, loop, field);
    if (!parsed.ok()) {
      response.setError(parsed, field.c_str(), "a control loop is not valid");
      return;
    }
  }
  for (JsonObjectConst entry : body["rules"].as<JsonArrayConst>()) {
    ControlRule rule;
    const Status parsed = ControlStore::parseRule(entry, rule, field);
    if (!parsed.ok()) {
      response.setError(parsed, field.c_str(), "a rule is not valid");
      return;
    }
  }

  if (body["loops"].as<JsonArrayConst>().size() > ControlManager::loopCapacity() ||
      body["rules"].as<JsonArrayConst>().size() > ControlManager::ruleCapacity() ||
      body["limits"].as<JsonArrayConst>().size() > SafetyManager::capacity()) {
    response.setError(fail(ErrorCode::kOutOfCapacity,
                           "more loops, rules or limits than this build holds"));
    return;
  }

  // Does this edit REMOVE a protection?  Adding a loop is a write; deleting an
  // interlock — or switching one off — is a different kind of act, and it is
  // confirmed by the person rather than by their browser (ADR-0020).
  bool removesProtection = false;
  if (s_.safety != nullptr) {
    for (std::size_t i = 0; i < s_.safety->count(); ++i) {
      const SafetyLimit& existing = s_.safety->at(i);
      if (!existing.active || !existing.enabled) continue;
      bool survives = false;
      for (JsonObjectConst entry : body["limits"].as<JsonArrayConst>()) {
        if (!existing.id.equals(entry["id"] | "")) continue;
        survives = (entry["enabled"] | true);
        break;
      }
      if (!survives) removesProtection = true;
    }
  }
  if (removesProtection &&
      !requireConfirmation(request, response, "removing a safety limit")) {
    return;
  }

  if (request.queryFlag("dry_run")) {
    response.body["dry_run"] = true;
    response.body["valid"] = true;
    response.body["removes_protection"] = removesProtection;
    return;
  }

  body["schemaVersion"] = LC_CONFIG_SCHEMA_VERSION;
  const Status saved = s_.storage->save(ConfigSection::kControl, body);
  if (!saved.ok()) {
    response.setError(saved);
    return;
  }

  // Everything currently running stops and lets go of its outputs.  Editing
  // the control configuration of a running rig is not a background operation,
  // and pretending otherwise would leave a loop commanding a heater with gains
  // that no longer exist anywhere.
  s_.control->clearAll();
  if (s_.safety != nullptr) s_.safety->clearAll();
  const ApplyReport report = s_.applier->applyControl(body.as<JsonObjectConst>());

  listControl(response);
  JsonObject applied = response.body["applied"].to<JsonObject>();
  applied["ok"] = report.applied;
  applied["failed"] = report.failed;
  if (report.failed > 0) {
    applied["first_error"] = jsonCopy(report.firstError.detail.c_str());
    applied["first_id"] = jsonCopy(report.firstFailedKey.c_str());
  }
}

void RestApi::loopAction(const ApiRequest& request, const char* id,
                         const char* action, ApiResponse& response) {
  JsonDocument body;
  if (!parseBody(request, body, response)) return;

  if (std::strcmp(action, "mode") == 0) {
    LoopMode mode = LoopMode::kOff;
    if (!parseLoopMode(body["mode"] | "", mode)) {
      response.setError(fail(ErrorCode::kInvalidArgument,
                             "mode: off | manual | automatic"), "mode");
      return;
    }
    if (mode == LoopMode::kManual && !body["value"].isNull()) {
      const Status set = s_.control->setManual(id, body["value"] | 0.0f);
      if (!set.ok()) {
        response.setError(set, "value");
        return;
      }
    }
    const Status changed = s_.control->setMode(id, mode);
    if (!changed.ok()) {
      response.setError(changed);
      return;
    }
    // The mode is deliberately NOT persisted: a loop comes up OFF after every
    // reboot, whatever it was doing before (ADR-0017).
  } else if (std::strcmp(action, "setpoint") == 0) {
    if (body["value"].isNull()) {
      response.setError(fail(ErrorCode::kInvalidArgument, "value is required"),
                        "value");
      return;
    }
    const Status changed = s_.control->setSetpoint(id, body["value"] | 0.0f);
    if (!changed.ok()) {
      response.setError(changed, "value");
      return;
    }
    const Status saved = persistControl();
    if (!saved.ok()) {
      response.setError(saved);
      return;
    }
  } else if (std::strcmp(action, "manual") == 0) {
    if (body["value"].isNull()) {
      response.setError(fail(ErrorCode::kInvalidArgument, "value is required"),
                        "value");
      return;
    }
    const Status changed = s_.control->setManual(id, body["value"] | 0.0f);
    if (!changed.ok()) {
      response.setError(changed, "value");
      return;
    }
    const Status saved = persistControl();
    if (!saved.ok()) {
      response.setError(saved);
      return;
    }
  } else {
    response.setError(404, fail(ErrorCode::kNotFound, action));
    return;
  }

  const ControlLoop* loop = s_.control->findLoop(id);
  if (loop == nullptr) {
    response.setError(404, fail(ErrorCode::kNotFound, id));
    return;
  }
  describeLoop(*loop, response.body.to<JsonObject>());
}

void RestApi::handleControl(const ApiRequest& request, const PathSegments& path,
                            ApiResponse& response) {
  if (s_.control == nullptr) {
    response.setError(fail(ErrorCode::kNotSupported,
                           "this build has no control support"));
    return;
  }

  // POST /control/limits/reset — clear every latch, deliberately and by hand.
  if (path.count() == 5 && path.is(3, "limits") && path.is(4, "reset")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;
    if (s_.safety == nullptr) {
      response.setError(fail(ErrorCode::kNotSupported, "no safety limits"));
      return;
    }
    s_.safety->resetAll();
    listControl(response);
    return;
  }

  // POST /control/limits/{id}/reset
  if (path.count() >= 6 && path.is(3, "limits") && path.is(5, "reset")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;
    if (s_.safety == nullptr) {
      response.setError(fail(ErrorCode::kNotSupported, "no safety limits"));
      return;
    }
    const Status reset = s_.safety->reset(path.at(4));
    if (!reset.ok()) {
      response.setError(404, reset);
      return;
    }
    const SafetyLimit* limit = s_.safety->find(path.at(4));
    describeLimit(*limit, response.body.to<JsonObject>());
    return;
  }

  // POST /control/loops/{id}/{mode|setpoint|manual}
  if (path.count() >= 6 && path.is(3, "loops")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;
    loopAction(request, path.at(4), path.at(5), response);
    return;
  }

  if (path.count() > 3) {
    response.setError(404, fail(ErrorCode::kNotFound, path.at(3)));
    return;
  }

  switch (request.method) {
    case HttpMethod::kGet:
      listControl(response);
      return;
    case HttpMethod::kPut:
      if (!requireWriteAccess(request, response)) return;
      replaceControl(request, response);
      return;
    default:
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET or PUT"));
      return;
  }
}

// ---------------------------------------------------------------------------
//  Experiments: scenarios, runs and the record of what happened (§31, §48)
// ---------------------------------------------------------------------------
void RestApi::describeRunState(JsonObject out) const {
  const ExperimentEngine& engine = *s_.experiments;
  const RunRecord& run = engine.run();

  out["state"] = toString(engine.state());
  out["experiment"] = jsonCopy(run.experimentKey.c_str());
  out["name"] = jsonCopy(run.name.c_str());
  out["step"] = engine.busy() ? engine.stepIndex() + 1 : 0;
  out["steps"] = engine.current().stepCount;
  out["operator"] = jsonCopy(run.metadata.operatorName.c_str());
  out["sample"] = jsonCopy(run.metadata.sample.c_str());

  if (engine.busy() && engine.stepIndex() < engine.current().stepCount) {
    const ExperimentStep& step = engine.current().steps[engine.stepIndex()];
    JsonObject current = out["current"].to<JsonObject>();
    current["op"] = toString(step.op);
    current["target"] = jsonCopy(step.target.c_str());
    current["channel"] = jsonCopy(step.channel.c_str());
    current["label"] = jsonCopy(step.label.c_str());
    // What the operator actually wants to know while looking at a running rig:
    // how long this step can still take before something happens.
    current["remaining_s"] = engine.remainingSeconds(s_.clock->nowMicros());
  }

  if (engine.state() == ExperimentState::kFinished ||
      engine.state() == ExperimentState::kAborted) {
    // The last run stays visible after it ends, with the same three fields the
    // record carries — the screen must not be able to disagree with the file.
    out["reason"] = toString(run.reason);
    out["step_reached"] = run.stepReached;
    if (!run.detail.ok()) {
      JsonObject error = out["error"].to<JsonObject>();
      error["code"] = run.detail.symbol();
      error["numeric"] = static_cast<int>(run.detail.code);
      error["detail"] = jsonCopy(run.detail.detail.c_str());
    }
  }

  JsonArray events = out["events"].to<JsonArray>();
  for (std::size_t i = 0; i < run.eventCount; ++i) {
    JsonObject event = events.add<JsonObject>();
    event["at_s"] = static_cast<double>(run.events[i].atUs) / 1e6;
    event["step"] = run.events[i].step;
    event["label"] = jsonCopy(run.events[i].label.c_str());
  }
  out["events_dropped"] = run.eventsDropped;

  if (s_.runLog != nullptr) out["record_pending"] = s_.runLog->pending();
}

void RestApi::saveExperiment(const ApiRequest& request, const char* key,
                             ApiResponse& response) {
  if (!requireWriteAccess(request, response)) return;

  JsonDocument body;
  if (!parseBody(request, body, response)) return;
  if (key != nullptr) body["key"] = key;

  // Parsed into the typed form first: what is stored has to be something the
  // engine can actually run, not JSON that merely looks like a scenario.
  Experiment experiment;
  std::size_t offendingStep = 0;
  LabelString field;
  const Status parsed = ExperimentStore::parse(body.as<JsonObjectConst>(),
                                               experiment, offendingStep, field);
  if (!parsed.ok()) {
    response.setError(parsed, field.empty() ? nullptr : field.c_str(),
                      "this step is not valid");
    response.body["step"] = offendingStep;
    return;
  }

  // Targets are checked against the rig as well, but NOT fatally: a scenario
  // may legitimately be written before the device it drives is wired up, and
  // refusing to save it would mean editing scenarios only on a finished rig.
  // Starting it is what requires everything to resolve.
  std::size_t unresolved = 0;
  const Status runnable = s_.experiments->validate(experiment, unresolved);
  response.body["runnable"] = runnable.ok();
  if (!runnable.ok()) {
    response.body["blocking_step"] = unresolved;
    response.body["blocking_reason"] = jsonCopy(runnable.detail.c_str());
  }

  if (request.queryFlag("dry_run")) {
    response.body["dry_run"] = true;
    response.body["valid"] = true;
    response.body["steps"] = experiment.stepCount;
    return;
  }

  JsonDocument stored;
  const Status loaded = s_.storage->load(ConfigSection::kExperiments, stored);
  if (!loaded.ok() && loaded.code != ErrorCode::kNotFound) {
    response.setError(loaded);
    return;
  }

  JsonDocument normalised;
  ExperimentStore::serialize(experiment, normalised.to<JsonObject>());
  const Status upserted =
      ExperimentStore::upsert(stored, normalised.as<JsonObjectConst>());
  if (!upserted.ok()) {
    response.setError(upserted);
    return;
  }
  const Status saved = s_.storage->save(ConfigSection::kExperiments, stored);
  if (!saved.ok()) {
    response.setError(saved);
    return;
  }

  response.status = (key == nullptr) ? 201 : 200;
  response.body["key"] = jsonCopy(experiment.key.c_str());
  response.body["steps"] = experiment.stepCount;
}

void RestApi::experimentAction(const ApiRequest& request, const char* key,
                               const char* action, ApiResponse& response) {
  if (!requireWriteAccess(request, response)) return;

  if (std::strcmp(action, "stop") == 0) {
    const Status stopped = s_.experiments->stop(StopReason::kOperator);
    if (!stopped.ok()) {
      response.setError(stopped);
      return;
    }
    describeRunState(response.body.to<JsonObject>());
    return;
  }
  if (std::strcmp(action, "pause") == 0 || std::strcmp(action, "resume") == 0) {
    const Status changed = (action[1] == 'a') ? s_.experiments->pause()
                                              : s_.experiments->resume();
    if (!changed.ok()) {
      response.setError(changed);
      return;
    }
    describeRunState(response.body.to<JsonObject>());
    return;
  }
  if (std::strcmp(action, "start") != 0) {
    response.setError(404, fail(ErrorCode::kNotFound, action));
    return;
  }

  JsonDocument stored;
  const Status loaded = s_.storage->load(ConfigSection::kExperiments, stored);
  if (!loaded.ok()) {
    response.setError(404, fail(ErrorCode::kNotFound, "no experiments stored"));
    return;
  }
  JsonObjectConst entry = ExperimentStore::find(stored, key);
  if (entry.isNull()) {
    response.setError(404, fail(ErrorCode::kNotFound, key));
    return;
  }

  Experiment experiment;
  std::size_t offendingStep = 0;
  LabelString field;
  const Status parsed =
      ExperimentStore::parse(entry, experiment, offendingStep, field);
  if (!parsed.ok()) {
    response.setError(parsed, field.empty() ? nullptr : field.c_str());
    response.body["step"] = offendingStep;
    return;
  }

  // Run metadata comes with the request, not with the scenario: the sample and
  // the person are properties of THIS run.  A stored default is a convenience,
  // never a substitute.
  JsonDocument body;
  if (request.body != nullptr && request.bodyLength > 0) {
    if (!parseBody(request, body, response)) return;
    if (!body["operator"].isNull()) {
      experiment.metadata.operatorName.assign(body["operator"] | "");
    }
    if (!body["sample"].isNull()) {
      experiment.metadata.sample.assign(body["sample"] | "");
    }
    if (!body["notes"].isNull()) {
      experiment.metadata.notes.assign(body["notes"] | "");
    }
  }
  if (experiment.metadata.operatorName.empty()) {
    // §48: a dataset nobody can attribute is a dataset nobody can ask about.
    response.setError(fail(ErrorCode::kInvalidArgument,
                           "a run needs an operator"),
                      "operator",
                      "who is running this? the record keeps it forever");
    return;
  }

  const Status started = s_.experiments->start(experiment);
  if (!started.ok()) {
    response.setError(started);
    return;
  }
  response.status = 202;
  describeRunState(response.body.to<JsonObject>());
}

void RestApi::handleExperiments(const ApiRequest& request,
                                const PathSegments& path, ApiResponse& response) {
  if (s_.experiments == nullptr) {
    response.setError(fail(ErrorCode::kNotSupported,
                           "this build has no experiment engine"));
    return;
  }

  // GET /experiments/state — the running scenario, polled while a run is open.
  if (path.count() == 4 && path.is(3, "state")) {
    if (request.method != HttpMethod::kGet) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
      return;
    }
    describeRunState(response.body.to<JsonObject>());
    return;
  }

  // GET /experiments/runs — the record of what this rig has actually done.
  if (path.count() == 4 && path.is(3, "runs")) {
    if (request.method != HttpMethod::kGet) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
      return;
    }
    if (s_.runLog == nullptr) {
      response.setError(fail(ErrorCode::kNotSupported,
                             "this build keeps no run records"));
      return;
    }
    JsonDocument runs;
    const Status loaded = s_.runLog->load(runs);
    if (!loaded.ok()) {
      response.setError(loaded);
      return;
    }
    response.body.set(runs.as<JsonObjectConst>());
    return;
  }

  JsonDocument stored;
  const Status loaded = s_.storage->load(ConfigSection::kExperiments, stored);
  if (!loaded.ok() && loaded.code != ErrorCode::kNotFound) {
    response.setError(loaded);
    return;
  }

  if (path.count() < 4) {
    if (request.method == HttpMethod::kGet) {
      JsonObject out = response.body.to<JsonObject>();
      ExperimentStore::summarise(stored, out["experiments"].to<JsonArray>());
      describeRunState(out["run"].to<JsonObject>());
      JsonObject capacity = out["limits"].to<JsonObject>();
      capacity["steps"] = limits::kMaxExperimentSteps;
      capacity["events"] = limits::kMaxRunEvents;
      capacity["records"] = limits::kMaxRunRecords;
      return;
    }
    if (request.method == HttpMethod::kPost) {
      saveExperiment(request, nullptr, response);
      return;
    }
    response.setError(405, fail(ErrorCode::kNotSupported, "use GET or POST"));
    return;
  }

  const char* key = path.at(3);

  // POST /experiments/{key}/actions/{start|stop|pause|resume}
  if (path.count() >= 6 && path.is(4, "actions")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    experimentAction(request, key, path.at(5), response);
    return;
  }

  switch (request.method) {
    case HttpMethod::kGet: {
      JsonObjectConst entry = ExperimentStore::find(stored, key);
      if (entry.isNull()) {
        response.setError(404, fail(ErrorCode::kNotFound, key));
        return;
      }
      response.body.set(entry);
      return;
    }
    case HttpMethod::kPut:
      saveExperiment(request, key, response);
      return;
    case HttpMethod::kDelete: {
      if (!requireWriteAccess(request, response)) return;
      if (s_.experiments->busy() &&
          s_.experiments->run().experimentKey.equals(key)) {
        // Deleting the scenario that is running would leave a run whose record
        // points at nothing.  Stop it first, deliberately.
        response.setError(fail(ErrorCode::kResourceBusy,
                               "this experiment is running"));
        return;
      }
      if (!ExperimentStore::removeByKey(stored, key)) {
        response.setError(404, fail(ErrorCode::kNotFound, key));
        return;
      }
      const Status saved = s_.storage->save(ConfigSection::kExperiments, stored);
      if (!saved.ok()) {
        response.setError(saved);
        return;
      }
      response.body["deleted"] = jsonCopy(key);
      return;
    }
    default:
      response.setError(405, fail(ErrorCode::kNotSupported, "method"));
      return;
  }
}

// ---------------------------------------------------------------------------
//  Logs: datasets, their index, and the one endpoint that is not JSON (§33)
// ---------------------------------------------------------------------------
void RestApi::describeLogging(JsonObject out) const {
  const LogStatus& status = s_.logger->status();
  out["recording"] = status.recording;
  out["id"] = jsonCopy(status.id.c_str());
  out["name"] = jsonCopy(status.name.c_str());
  out["rows"] = status.rows;
  out["dropped_rows"] = status.droppedRows;
  out["bytes"] = status.bytesWritten;
  out["rate_hz"] = status.rateHz;
  out["channels"] = status.channelCount;
  // The two numbers that decide whether a run can be recorded at all, next to
  // each other so nobody has to compute the difference.
  if (s_.logStore != nullptr) {
    out["writable_bytes"] = s_.logStore->writableBytes();
    out["reserve_bytes"] = LogStore::kReserveBytes;
  }
  if (!status.recording && status.stopReason != LogStopReason::kNone) {
    out["last_stop"] = toString(status.stopReason);
    out["last_truncated"] = status.truncated;
    if (!status.lastError.ok()) {
      out["last_error"] = jsonCopy(status.lastError.detail.c_str());
    }
  }
}

/**
 * The offload queue of one session (M15 §14.2).
 *
 * REST is the source of truth here and the WebSocket event is only a nudge: a
 * collector that missed a notification must be able to rebuild its whole to-do
 * list from this one response, or a lost frame would become a lost segment.
 */
void RestApi::describeSegments(const char* id, ApiResponse& response) {
  JsonDocument index;
  if (!s_.logStore->loadIndex(index).ok()) {
    response.setError(fail(ErrorCode::kStorageFailure, "log index"));
    return;
  }
  for (JsonObjectConst entry : index["logs"].as<JsonArrayConst>()) {
    if (std::strcmp(entry["id"] | "", id) != 0) continue;

    JsonObject out = response.body.to<JsonObject>();
    out["session_id"] = jsonCopy(id);
    out["state"] = jsonCopy(entry["state"] | "");
    out["mode"] = jsonCopy(entry["mode"] | "single");
    out["collector_id"] = jsonCopy(entry["collector_id"] | "");
    out["segments_completed"] = entry["segments_completed"] | 0u;
    out["segments_acked"] = entry["segments_acked"] | 0u;
    out["acked_through"] = entry["acked_through"] | 0u;
    out["rows"] = entry["rows"] | 0u;
    out["dropped"] = entry["dropped"] | 0u;
    JsonObjectConst active = entry["active"].as<JsonObjectConst>();
    if (!active.isNull()) {
      out["active_segment"] = active["sequence"] | 0u;
      out["active_bytes"] = active["bytes"] | 0u;
    }
    out["segment_bytes"] = entry["segment_bytes"] | LogStore::kDefaultSegmentBytes;

    std::size_t pendingBytes = 0;
    JsonArray pending = out["pending"].to<JsonArray>();
    LogStore::SegmentInfo waiting[LogStore::kMaxPendingSegments];
    const std::size_t count =
        s_.logStore->listSegments(id, waiting, LogStore::kMaxPendingSegments);
    for (std::size_t i = 0; i < count; ++i) {
      JsonObject item = pending.add<JsonObject>();
      item["sequence"] = waiting[i].sequence;
      item["bytes"] = waiting[i].bytes;
      item["rows"] = waiting[i].rows;
      item["first_row"] = waiting[i].firstRow;
      item["last_row"] = waiting[i].lastRow;
      char crc[16];
      std::snprintf(crc, sizeof(crc), "%08x",
                    static_cast<unsigned>(waiting[i].payloadCrc32));
      item["payload_crc32"] = jsonCopy(crc);
      item["state"] = jsonCopy(waiting[i].state.c_str());
      pendingBytes += waiting[i].bytes;
    }
    out["pending_bytes"] = pendingBytes;
    // How much room the queue still has.  The interface turns this into the
    // warning that tells an operator to reconnect the collector BEFORE the log
    // has to stop (§21).
    out["writable_bytes"] = s_.logStore->writableBytes();
    return;
  }
  response.setError(404, fail(ErrorCode::kNotFound, id));
}

void RestApi::handleLogs(const ApiRequest& request, const PathSegments& path,
                         ApiResponse& response) {
  if (s_.logger == nullptr || s_.logStore == nullptr) {
    response.setError(fail(ErrorCode::kNotSupported,
                           "this build cannot record data"));
    return;
  }

  // POST /logs/start — manual recording, outside any experiment.
  if (path.count() == 4 && path.is(3, "start")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;

    JsonDocument body;
    if (!parseBody(request, body, response)) return;

    LogSpec spec;
    spec.name.assign(body["name"] | "manual recording");
    spec.operatorName.assign(body["operator"] | "");
    spec.sample.assign(body["sample"] | "");
    spec.rateHz = body["rate_hz"] | 1.0f;
    spec.includeRaw = body["raw"] | true;
    // M15.  Absent means `single`, which is what every existing client sends
    // and what every existing session behaves like.
    if (!parseStorageMode(body["storage_mode"] | "", spec.storageMode)) {
      response.setError(fail(ErrorCode::kInvalidArgument,
                             "storage_mode is single or continuous_offload"),
                        "storage_mode");
      return;
    }
    spec.segmentBytes = body["segment_bytes"] | 0u;
    spec.collectorId.assign(body["collector_id"] | "");
    if (spec.storageMode == LogStorageMode::kContinuousOffload
        && spec.collectorId.empty()) {
      // Without an owner nothing may acknowledge, so the segments would queue
      // until the filesystem filled — a failure it is kinder to refuse now.
      response.setError(fail(ErrorCode::kInvalidArgument,
                             "continuous_offload needs a collector_id"),
                        "collector_id");
      return;
    }
    for (JsonVariantConst key : body["channels"].as<JsonArrayConst>()) {
      if (spec.channelCount >= limits::kMaxLoggedChannels) {
        response.setError(fail(ErrorCode::kOutOfCapacity,
                               "at most 16 channels in one dataset"), "channels");
        return;
      }
      const ChannelHandle handle = s_.channels->findByKey(key | "");
      if (handle == kInvalidChannel) {
        response.setError(404, fail(ErrorCode::kChannelNotFound, key | ""),
                          "channels");
        return;
      }
      spec.channels[spec.channelCount++] = handle;
    }

    const Status started =
        s_.logger->start(spec, body["expected_s"] | 0.0);
    if (!started.ok()) {
      response.setError(started);
      return;
    }
    describeLogging(response.body.to<JsonObject>());
    return;
  }

  if (path.count() == 4 && path.is(3, "stop")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;
    const Status stopped = s_.logger->stop(LogStopReason::kOperator);
    if (!stopped.ok()) {
      response.setError(stopped);
      return;
    }
    describeLogging(response.body.to<JsonObject>());
    return;
  }

  // --- M15: the offload queue ---------------------------------------------
  // GET /logs/{id}/segments — what is waiting to be collected.
  if (path.count() == 5 && path.is(4, "segments")) {
    if (request.method != HttpMethod::kGet) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
      return;
    }
    describeSegments(path.at(3), response);
    return;
  }

  // GET /logs/{id}/segments/{n}/export.csv — one part, streamed from the file.
  if (path.count() >= 7 && path.is(4, "segments") && path.is(6, "export.csv")) {
    if (request.method != HttpMethod::kGet) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
      return;
    }
    const std::uint32_t sequence =
        static_cast<std::uint32_t>(std::strtoul(path.at(5), nullptr, 10));
    LogStore::SegmentInfo segment;
    if (!s_.logStore->segmentInfo(path.at(3), sequence, segment)) {
      response.setError(404, fail(ErrorCode::kNotFound, "no such segment"));
      return;
    }
    response.stream.active = true;
    response.stream.path = segment.path;
    response.stream.contentType.assign("text/csv");
    char filename[80];
    std::snprintf(filename, sizeof(filename), "%s_p%06u.csv", path.at(3),
                  static_cast<unsigned>(sequence));
    response.stream.filename.assign(filename);
    // The checksum travels with the file so a collector can verify without a
    // second request, and so a proxy cannot serve a stale part as a fresh one.
    char etag[24];
    std::snprintf(etag, sizeof(etag), "crc32-%08x",
                  static_cast<unsigned>(segment.payloadCrc32));
    response.stream.etag.assign(etag);
    response.stream.segmentCrc32 = segment.payloadCrc32;
    response.stream.sequence = sequence;
    return;
  }

  // POST /logs/{id}/segments/{n}/ack — the only thing that deletes a segment.
  if (path.count() >= 7 && path.is(4, "segments") && path.is(6, "ack")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    // Deleting measurements needs the same right as deleting a dataset.
    if (!requireWriteAccess(request, response)) return;
    JsonDocument body;
    if (!parseBody(request, body, response)) return;

    const std::uint32_t sequence =
        static_cast<std::uint32_t>(std::strtoul(path.at(5), nullptr, 10));
    const std::uint32_t crc = static_cast<std::uint32_t>(
        std::strtoul(body["payload_crc32"] | "0", nullptr, 16));
    bool already = false;
    const Status acknowledged = s_.logStore->acknowledgeSegment(
        path.at(3), sequence, body["collector_id"] | "",
        body["bytes"] | 0u, crc, already);
    if (!acknowledged.ok()) {
      response.setError(acknowledged);
      return;
    }
    JsonObject out = response.body.to<JsonObject>();
    out["acknowledged"] = true;
    out["deleted"] = true;
    out["sequence"] = sequence;
    if (already) out["already_acknowledged"] = true;
    LogStore::SegmentInfo waiting[LogStore::kMaxPendingSegments];
    out["pending_segments"] =
        s_.logStore->listSegments(path.at(3), waiting, LogStore::kMaxPendingSegments);
    return;
  }

  // GET /logs/{id}/export.csv — the streaming path.  The response describes
  // the file; the transport adapter sends it (see StreamSpec).
  if (path.count() >= 5 && path.is(4, "export.csv")) {
    if (request.method != HttpMethod::kGet) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
      return;
    }
    FixedString<64> file;
    if (!s_.logStore->pathFor(path.at(3), file)) {
      response.setError(404, fail(ErrorCode::kNotFound, path.at(3)));
      return;
    }
    response.stream.active = true;
    response.stream.path = file;
    response.stream.contentType.assign("text/csv");
    char filename[64];
    std::snprintf(filename, sizeof(filename), "%s.csv", path.at(3));
    response.stream.filename.assign(filename);
    return;
  }

  if (path.count() < 4) {
    if (request.method != HttpMethod::kGet) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
      return;
    }
    JsonDocument index;
    const Status loaded = s_.logStore->loadIndex(index);
    if (!loaded.ok()) {
      response.setError(loaded);
      return;
    }
    JsonObject out = response.body.to<JsonObject>();
    out["logs"] = index["logs"];
    describeLogging(out["recording"].to<JsonObject>());
    out["limits"]["sessions"] = limits::kMaxLogSessions;
    out["limits"]["channels"] = limits::kMaxLoggedChannels;
    out["limits"]["rate_hz"] = DataLogger::kMaxRateHz;
    return;
  }

  const char* id = path.at(3);

  switch (request.method) {
    case HttpMethod::kGet: {
      JsonDocument index;
      if (!s_.logStore->loadIndex(index).ok()) {
        response.setError(fail(ErrorCode::kStorageFailure, "log index"));
        return;
      }
      for (JsonObjectConst entry : index["logs"].as<JsonArrayConst>()) {
        if (std::strcmp(entry["id"] | "", id) != 0) continue;
        response.body.set(entry);
        return;
      }
      response.setError(404, fail(ErrorCode::kNotFound, id));
      return;
    }
    case HttpMethod::kDelete: {
      if (!requireWriteAccess(request, response)) return;
      // The only way a dataset is ever deleted: somebody asking for it by name.
      const Status removed = s_.logStore->removeSession(id);
      if (!removed.ok()) {
        response.setError(removed);
        return;
      }
      response.body["deleted"] = jsonCopy(id);
      return;
    }
    default:
      response.setError(405, fail(ErrorCode::kNotSupported, "method"));
      return;
  }
}

// ---------------------------------------------------------------------------
//  Auth: sessions and the password (§44, ADR-0020)
// ---------------------------------------------------------------------------
void RestApi::handleAuth(const ApiRequest& request, const PathSegments& path,
                         ApiResponse& response) {
  if (s_.auth == nullptr) {
    response.setError(fail(ErrorCode::kNotSupported,
                           "this build has no credential support"));
    return;
  }

  // GET /auth — what the browser needs to decide what to show.  Deliberately
  // says nothing about WHY a sign-in failed beyond "locked": an endpoint that
  // distinguishes "no such password" from "wrong password" is an endpoint that
  // answers questions nobody should be asking it.
  if (path.count() == 3 && request.method == HttpMethod::kGet) {
    const AuthState state = s_.auth->state();
    JsonObject out = response.body.to<JsonObject>();
    out["configured"] = state.configured;
    out["signed_in"] = signedIn(request);
    out["sessions"] = state.sessions;
    out["locked"] = state.lockedUntilUs > s_.clock->nowMicros();
    out["min_password_length"] = AuthManager::kMinPasswordLength;
    return;
  }

  if (path.count() < 4 || request.method != HttpMethod::kPost) {
    response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
    return;
  }

  if (path.is(3, "login")) {
    JsonDocument body;
    if (!parseBody(request, body, response)) return;
    bool evicted = false;
    const Result<SessionToken> session =
        s_.auth->login(body["password"] | "", &evicted);
    if (!session.ok()) {
      response.setError(401, session.error());
      return;
    }
    // HttpOnly so that a cross-site script cannot read it, SameSite=Strict so
    // that another page cannot spend it, Path=/ because the whole instrument is
    // one application.  No Secure flag: this device speaks plain HTTP on a lab
    // network, and setting a flag that would make the cookie unusable is worse
    // than admitting the transport is what it is.
    char cookie[128];
    std::snprintf(cookie, sizeof(cookie),
                  "%s=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%llu",
                  AuthManager::cookieName(), session.value().c_str(),
                  static_cast<unsigned long long>(
                      AuthManager::kSessionLifetimeUs / 1000000ULL));
    response.setCookie.assign(cookie);
    response.body["signed_in"] = true;
    response.body["evicted_a_session"] = evicted;
    return;
  }

  if (path.is(3, "logout")) {
    SessionToken token;
    AuthManager::tokenFromCookie(request.cookie, token);
    s_.auth->logout(token.c_str());
    char cookie[96];
    std::snprintf(cookie, sizeof(cookie),
                  "%s=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0",
                  AuthManager::cookieName());
    response.setCookie.assign(cookie);
    response.body["signed_in"] = false;
    return;
  }

  if (path.is(3, "password")) {
    JsonDocument body;
    if (!parseBody(request, body, response)) return;
    // Changing the credential is confirmed by the credential, never by the
    // session — AuthManager::setPassword() enforces that, and this route only
    // has to pass the old one through.
    const Status changed = s_.auth->setPassword(body["current"] | "",
                                                body["password"] | "");
    if (!changed.ok()) {
      response.setError(changed, "password");
      return;
    }
    char cookie[96];
    std::snprintf(cookie, sizeof(cookie),
                  "%s=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0",
                  AuthManager::cookieName());
    response.setCookie.assign(cookie);
    response.body["configured"] = true;
    response.body["sessions_ended"] = true;
    return;
  }

  response.setError(404, fail(ErrorCode::kNotFound, path.at(3)));
}

// ---------------------------------------------------------------------------
//  Firmware: the one upload, and the reasons it is refused (§44)
// ---------------------------------------------------------------------------
void RestApi::handleFirmware(const ApiRequest& request, const PathSegments& path,
                             ApiResponse& response) {
  if (path.count() < 4 || !path.is(3, "ota")) {
    response.setError(404, fail(ErrorCode::kNotFound, "firmware"));
    return;
  }
  if (request.method != HttpMethod::kPost) {
    response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
    return;
  }

  // The order of these checks is the order of §49.  A firmware upload reboots
  // the controller; a reboot with a heater under command is the failure this
  // project has spent four milestones closing, and no amount of authorisation
  // makes it acceptable.
  if (s_.experiments != nullptr && s_.experiments->busy()) {
    response.setError(409, fail(ErrorCode::kResourceBusy,
                                "an experiment is running; stop it first"));
    return;
  }
  if (s_.logger != nullptr && s_.logger->recording()) {
    response.setError(409, fail(ErrorCode::kResourceBusy,
                                "a dataset is being recorded; stop it first"));
    return;
  }
  if (s_.outputs != nullptr) {
    for (std::size_t i = 0; i < s_.outputs->count(); ++i) {
      if (s_.outputs->at(i).state != OutputHoldState::kCommanded) continue;
      response.setError(409, fail(ErrorCode::kSafetyInterlock,
                                  "an output is commanded; release it first"));
      return;
    }
  }

  if (!requireConfirmation(request, response, "replacing the firmware")) return;

  // The upload itself belongs to the transport: the image arrives as a stream
  // of megabytes and the REST layer has never seen one of those (ADR-0012).
  // What this endpoint owns is the POLICY, and the policy is what is tested.
  response.setError(fail(ErrorCode::kNotSupported,
                         "the host build has no partition to flash"),
                    nullptr,
                    "checks passed; on the device the image is written here");
}

void RestApi::handleConfig(const ApiRequest& request, const PathSegments& path,
                           ApiResponse& response) {
  // GET /config/backup — the configuration as it was before the last import.
  if (path.count() >= 4 && path.is(3, "backup")) {
    if (request.method != HttpMethod::kGet) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
      return;
    }
    JsonDocument backup;
    const Status loaded = s_.storage->loadBackup(backup);
    if (!loaded.ok()) {
      response.setError(404, fail(ErrorCode::kNotFound,
                                  "nothing has been overwritten yet"));
      return;
    }
    response.body.set(backup.as<JsonObjectConst>());
    return;
  }

  if (path.count() >= 4 && path.is(3, "export")) {
    if (request.method != HttpMethod::kGet) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use GET"));
      return;
    }
    JsonObject out = response.body.to<JsonObject>();
    out["schemaVersion"] = ConfigStorage::kSchemaVersion;
    out["firmware"] = LC_FIRMWARE_VERSION;
    out["chip"] = s_.resources->chip().name;
    JsonObject sections = out["sections"].to<JsonObject>();
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ConfigSection::kCount); ++i) {
      const ConfigSection section = static_cast<ConfigSection>(i);
      JsonDocument document;
      if (!s_.storage->load(section, document).ok()) continue;
      sections[toString(section)] = document;
    }
    return;
  }

  if (path.count() >= 4 && path.is(3, "import")) {
    if (request.method != HttpMethod::kPost) {
      response.setError(405, fail(ErrorCode::kNotSupported, "use POST"));
      return;
    }
    if (!requireWriteAccess(request, response)) return;
    // An import replaces every section, interlocks included, on a rig that may
    // be running.  It is the single most destructive thing this API can do.
    if (!requireConfirmation(request, response, "importing a configuration")) return;

    JsonDocument body;
    if (!parseBody(request, body, response)) return;

    const std::uint16_t version = body["schemaVersion"] | 0;
    if (version == 0) {
      response.setError(fail(ErrorCode::kConfigCorrupt, "missing schemaVersion"));
      return;
    }
    if (version > ConfigStorage::kSchemaVersion) {
      response.setError(fail(ErrorCode::kConfigSchemaTooNew,
                             "exported by newer firmware"));
      return;
    }

    JsonObjectConst sections = body["sections"].as<JsonObjectConst>();
    if (sections.isNull()) {
      response.setError(fail(ErrorCode::kConfigCorrupt, "no sections"));
      return;
    }

    // What is here NOW, kept before it is overwritten.  One file, replaced by
    // each import: enough to undo the mistake somebody is about to make, and
    // not a version-control system pretending to live in 640 KB of flash.
    {
      JsonDocument previous;
      JsonObject snapshot = previous.to<JsonObject>();
      snapshot["schemaVersion"] = ConfigStorage::kSchemaVersion;
      snapshot["firmware"] = LC_FIRMWARE_VERSION;
      snapshot["fingerprint"] = s_.storage->fingerprint();
      JsonObject saved = snapshot["sections"].to<JsonObject>();
      for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ConfigSection::kCount); ++i) {
        const ConfigSection section = static_cast<ConfigSection>(i);
        JsonDocument document;
        if (!s_.storage->load(section, document).ok()) continue;
        saved[toString(section)] = document;
      }
      const Status backedUp = s_.storage->saveBackup(previous);
      response.body["backup_saved"] = backedUp.ok();
      if (!backedUp.ok()) {
        response.body["backup_error"] = jsonCopy(backedUp.detail.c_str());
      }
    }

    std::size_t written = 0;
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ConfigSection::kCount); ++i) {
      const ConfigSection section = static_cast<ConfigSection>(i);
      JsonVariantConst content = sections[toString(section)];
      if (content.isNull()) continue;
      JsonDocument document;
      document.set(content);
      const Status saved = s_.storage->save(section, document);
      if (!saved.ok()) {
        response.setError(saved);
        return;
      }
      ++written;
    }

    // Rebuild the rig from what was just imported.  Partial failure is normal
    // when moving a configuration between boards with different pinouts, so the
    // report says what did not come up rather than failing the whole import.
    s_.system->reloadConfiguration();
    const BootReport& report = s_.system->report();

    response.body["sections_written"] = written;
    response.body["devices_started"] = report.devices.applied;
    response.body["devices_failed"] = report.devices.failed;
    if (report.devices.failed > 0) {
      JsonObject failure = response.body["first_failure"].to<JsonObject>();
      failure["device"] = jsonCopy(report.devices.firstFailedKey.c_str());
      failure["field"] = jsonCopy(report.devices.firstFailedField.c_str());
      failure["code"] = report.devices.firstError.symbol();
      failure["detail"] = jsonCopy(report.devices.firstError.detail.c_str());
    }
    return;
  }

  response.setError(404, fail(ErrorCode::kNotFound, "config endpoint"));
}

}  // namespace lc
