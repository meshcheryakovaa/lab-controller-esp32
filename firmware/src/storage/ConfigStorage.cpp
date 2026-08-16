#include "storage/ConfigStorage.h"

#include <cstdio>
#include <new>

namespace lc {
namespace {

struct SectionInfo {
  const char* name;
  const char* path;
};

constexpr SectionInfo kSections[] = {
    {"system",       "/config/system.json"},
    {"devices",      "/config/devices.json"},
    {"channels",     "/config/channels.json"},
    {"processing",   "/config/processing.json"},
    {"calibrations", "/config/calibrations.json"},
    {"virtual",      "/config/virtual.json"},
    {"control",      "/config/control.json"},
    {"dashboards",   "/config/dashboards.json"},
    {"experiments",  "/config/experiments.json"},
};

static_assert(sizeof(kSections) / sizeof(kSections[0]) ==
                  static_cast<std::size_t>(ConfigSection::kCount),
              "section table and enum are out of sync");

// What a section looks like when the instrument has never been configured.
//
// Milestone 12.  The first boot of a real board printed five error lines —
//
//     /littlefs/config/system.json does not exist
//     /littlefs/config/devices.json does not exist
//     ...
//
// — describing, at error level, the completely normal state of a device nobody
// has set up yet.  The code above already treats kNotFound as "use defaults",
// so nothing was WRONG; the log simply said something alarming about a healthy
// board, which is its own kind of bug: it teaches whoever reads it to ignore
// error lines.
//
// Writing the empty documents once, at first boot, is the better fix than
// filtering the message.  It costs five small files, and in exchange the
// filesystem afterwards contains exactly what a configured instrument contains
// — so export, import, backup and diff all work identically on a board that has
// been used and a board out of the box, and "the file is missing" stops being a
// state the rest of the firmware has to have an opinion about.
//
// Only the sections the boot sequence actually reads are seeded.  Creating
// files nothing loads would be tidiness for its own sake, and would put
// sections into an export that no importer expects.
struct SectionDefault {
  ConfigSection section;
  const char* body;  // merged with schemaVersion
};

constexpr SectionDefault kDefaults[] = {
    {ConfigSection::kSystem,       "{\"buses\":{\"i2c\":[]}}"},
    {ConfigSection::kDevices,      "{\"devices\":[]}"},
    {ConfigSection::kProcessing,   "{\"pipelines\":{}}"},
    {ConfigSection::kCalibrations, "{\"calibrations\":[]}"},
    {ConfigSection::kControl,      "{\"limits\":[],\"loops\":[],\"rules\":[]}"},
};

// A migration knows only how to move one version forward.  Chaining them means
// a device three firmware versions behind still upgrades correctly.
using MigrationFn = Status (*)(JsonDocument& document);

struct Migration {
  ConfigSection section;
  std::uint16_t from;
  MigrationFn apply;
};

// Empty at schema version 1.  The machinery exists now so that the first real
// migration is a two-line change and not a redesign.
//
// Example of what an entry will look like:
//   { ConfigSection::kDevices, 1, [](JsonDocument& d) -> Status {
//       for (JsonObject device : d["devices"].as<JsonArray>()) {
//         device["sample_interval_us"] = 1000000 / (device["rate_hz"] | 10);
//         device.remove("rate_hz");
//       }
//       return ok(); } },
constexpr const Migration* kMigrations = nullptr;
constexpr std::size_t kMigrationCount = 0;

}  // namespace

Status ConfigStorage::saveBackup(JsonDocument& document) {
  const std::size_t needed = measureJson(document) + 1;
  if (needed >= kMaxDocumentBytes * 2) {
    return fail(ErrorCode::kPayloadTooLarge, kBackupPath);
  }
  char* buffer = new (std::nothrow) char[needed];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const std::size_t written = serializeJson(document, buffer, needed);
  const Status saved = backend_.writeAtomic(kBackupPath, buffer, written);
  delete[] buffer;
  return saved;
}

Status ConfigStorage::loadBackup(JsonDocument& out) const {
  if (!backend_.exists(kBackupPath)) return fail(ErrorCode::kNotFound, kBackupPath);
  const Result<std::size_t> fileSize = backend_.size(kBackupPath);
  if (!fileSize.ok()) return fileSize.error();
  if (fileSize.value() >= kMaxDocumentBytes * 2) {
    return fail(ErrorCode::kPayloadTooLarge, kBackupPath);
  }
  char* buffer = new (std::nothrow) char[fileSize.value() + 1];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const Result<std::size_t> read =
      backend_.read(kBackupPath, buffer, fileSize.value() + 1);
  if (!read.ok()) {
    delete[] buffer;
    return read.error();
  }
  const DeserializationError parsed =
      deserializeJson(out, static_cast<const char*>(buffer), read.value());
  delete[] buffer;
  if (parsed) return fail(ErrorCode::kConfigCorrupt, parsed.c_str());
  return ok();
}

std::uint32_t ConfigStorage::fingerprint() {
  if (fingerprintValid_) return fingerprint_;

  // FNV-1a, over the raw file bytes rather than over parsed JSON: two files
  // that differ only in whitespace ARE the same configuration, but proving that
  // would mean re-serialising both, and the point of this number is to be cheap
  // and stable, not canonical.
  std::uint32_t hash = 2166136261u;
  for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ConfigSection::kCount); ++i) {
    const char* file = path(static_cast<ConfigSection>(i));
    if (file == nullptr || !backend_.exists(file)) continue;
    const Result<std::size_t> fileSize = backend_.size(file);
    if (!fileSize.ok() || fileSize.value() >= kMaxDocumentBytes) continue;

    char* buffer = new (std::nothrow) char[fileSize.value() + 1];
    if (buffer == nullptr) continue;
    const Result<std::size_t> read = backend_.read(file, buffer, fileSize.value() + 1);
    if (read.ok()) {
      for (std::size_t b = 0; b < read.value(); ++b) {
        hash ^= static_cast<std::uint8_t>(buffer[b]);
        hash *= 16777619u;
      }
    }
    delete[] buffer;
  }

  fingerprint_ = hash;
  fingerprintValid_ = true;
  return fingerprint_;
}

const char* toString(ConfigSection section) {
  const std::size_t index = static_cast<std::size_t>(section);
  return (index < static_cast<std::size_t>(ConfigSection::kCount))
             ? kSections[index].name
             : "unknown";
}

const char* ConfigStorage::path(ConfigSection section) {
  const std::size_t index = static_cast<std::size_t>(section);
  return (index < static_cast<std::size_t>(ConfigSection::kCount))
             ? kSections[index].path
             : nullptr;
}

Status ConfigStorage::begin() {
  const Status created = backend_.ensureDirectory("/config");
  if (!created.ok()) return created;
  backend_.ensureDirectory("/config/backup");

  // A leftover *.tmp means a previous write was interrupted.  The valid data is
  // the file that was never renamed over, so the temporary is simply dropped.
  const std::size_t purged = backend_.purgeTemporaries("/config");
  if (purged > 0) {
    Event event;
    event.type = EventType::kSystemMessage;
    event.severity = 2;
    event.code = ErrorCode::kStorageFailure;
    event.detail = "discarded an interrupted configuration write";
    events_.publish(event);
  }

  ensureDefaults();
  return ok();
}

std::size_t ConfigStorage::ensureDefaults() {
  std::size_t created = 0;
  for (const SectionDefault& entry : kDefaults) {
    const char* file = path(entry.section);
    if (file == nullptr || backend_.exists(file)) continue;

    JsonDocument document;
    if (deserializeJson(document, entry.body)) continue;  // unreachable
    document["schemaVersion"] = kSchemaVersion;

    char buffer[256];
    const std::size_t written =
        serializeJson(document, buffer, sizeof(buffer));
    if (written == 0) continue;
    // Best effort on purpose.  A board that cannot write its defaults still
    // runs — the loaders have always coped with a missing file, and refusing to
    // boot over a cosmetic improvement to a log would be a poor trade.
    if (backend_.writeAtomic(file, buffer, written).ok()) ++created;
  }
  if (created > 0) {
    Event event;
    event.type = EventType::kSystemMessage;
    event.severity = 1;
    event.detail = "first boot: empty configuration files created";
    events_.publish(event);
  }
  return created;
}

Status ConfigStorage::backup(ConfigSection section, const char* data,
                             std::size_t bytes, std::uint16_t version) {
  char target[96];
  std::snprintf(target, sizeof(target), "/config/backup/%s.v%u.json",
                toString(section), static_cast<unsigned>(version));
  return backend_.writeAtomic(target, data, bytes);
}

Status ConfigStorage::migrate(ConfigSection section, JsonDocument& document,
                              std::uint16_t from) {
  std::uint16_t current = from;
  std::size_t guard = 0;

  while (current < kSchemaVersion) {
    bool advanced = false;
    for (std::size_t i = 0; i < kMigrationCount; ++i) {
      if (kMigrations[i].section != section || kMigrations[i].from != current) {
        continue;
      }
      const Status applied = kMigrations[i].apply(document);
      if (!applied.ok()) return applied;
      ++current;
      advanced = true;
      break;
    }
    if (!advanced) {
      return fail(ErrorCode::kConfigMigrationFailed, "no migration available");
    }
    if (++guard > 64) {
      return fail(ErrorCode::kConfigMigrationFailed, "migration loop");
    }
  }

  document["schemaVersion"] = kSchemaVersion;
  return ok();
}

Status ConfigStorage::load(ConfigSection section, JsonDocument& out) {
  const char* file = path(section);
  if (file == nullptr) return fail(ErrorCode::kInvalidArgument, "section");

  const Result<std::size_t> fileSize = backend_.size(file);
  if (!fileSize.ok()) return fileSize.error();
  if (fileSize.value() == 0) return fail(ErrorCode::kConfigCorrupt, "empty file");
  if (fileSize.value() >= kMaxDocumentBytes) {
    return fail(ErrorCode::kPayloadTooLarge, file);
  }

  // Configuration-phase allocation (ADR-0007): never on the acquisition path.
  const std::size_t capacity = fileSize.value() + 1;
  char* buffer = new (std::nothrow) char[capacity];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");

  const Result<std::size_t> readBytes = backend_.read(file, buffer, capacity);
  if (!readBytes.ok()) {
    delete[] buffer;
    return readBytes.error();
  }

  // static_cast to const char* matters: given a mutable char* ArduinoJson
  // parses in zero-copy mode and stores pointers INTO the buffer, which is
  // freed three lines below.  Every loaded configuration would hold dangling
  // strings.  The const overload copies.
  const DeserializationError parsed =
      deserializeJson(out, static_cast<const char*>(buffer), readBytes.value());
  if (parsed) {
    delete[] buffer;
    return fail(ErrorCode::kConfigCorrupt, parsed.c_str());
  }

  const std::uint16_t version = out["schemaVersion"].as<std::uint16_t>();
  if (version == 0) {
    delete[] buffer;
    return fail(ErrorCode::kConfigCorrupt, "missing schemaVersion");
  }
  lastLoadedVersion_ = version;

  if (version > kSchemaVersion) {
    // Written by newer firmware.  Guessing at a format from the future is how
    // an OTA rollback destroys a user's calibrations.
    delete[] buffer;
    return fail(ErrorCode::kConfigSchemaTooNew, file);
  }

  if (version < kSchemaVersion) {
    backup(section, buffer, readBytes.value(), version);
    const Status migrated = migrate(section, out, version);
    delete[] buffer;
    if (!migrated.ok()) return migrated;
    return save(section, out);
  }

  delete[] buffer;
  return ok();
}

Status ConfigStorage::save(ConfigSection section, JsonDocument& document) {
  const char* file = path(section);
  if (file == nullptr) return fail(ErrorCode::kInvalidArgument, "section");

  document["schemaVersion"] = kSchemaVersion;

  const std::size_t needed = measureJson(document) + 1;
  if (needed >= kMaxDocumentBytes) {
    return fail(ErrorCode::kPayloadTooLarge, file);
  }

  char* buffer = new (std::nothrow) char[needed];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");

  const std::size_t written = serializeJson(document, buffer, needed);
  const Status status = backend_.writeAtomic(file, buffer, written);
  delete[] buffer;
  if (!status.ok()) return status;

  ++revision_;
  fingerprintValid_ = false;

  Event event;
  event.type = EventType::kConfigChanged;
  event.integer = static_cast<std::int32_t>(section);
  event.detail = toString(section);
  events_.publish(event);
  return ok();
}

Status ConfigStorage::remove(ConfigSection section) {
  const char* file = path(section);
  if (file == nullptr) return fail(ErrorCode::kInvalidArgument, "section");
  const Status status = backend_.remove(file);
  if (status.ok()) ++revision_;
  return status;
}

}  // namespace lc
