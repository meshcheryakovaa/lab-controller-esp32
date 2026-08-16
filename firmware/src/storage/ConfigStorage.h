// =============================================================================
//  storage/ConfigStorage.h — reads and writes the configuration sections.
//
//  Responsibilities, and nothing else:
//    * one file per section, so moving a dashboard widget never rewrites the
//      calibration file (flash wear, and the risk of losing calibrations to a
//      power cut during an unrelated save);
//    * schemaVersion on every file, with a migration chain;
//    * refusal to read a file from the future (`CONFIG_SCHEMA_TOO_NEW`) —
//      an OTA rollback must not silently mangle the user's configuration;
//    * atomic writes delegated to the backend;
//    * a monotonic `revision` the WebSocket layer uses to tell clients their
//      cached descriptors are stale.
//
//  Turning documents into live devices is ConfigApplier's job, not this class's.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Error.h"
#include "core/EventBus.h"
#include "storage/IStorageBackend.h"

namespace lc {

enum class ConfigSection : std::uint8_t {
  kSystem = 0,
  kDevices,
  kChannels,
  kProcessing,
  kCalibrations,
  kVirtual,
  kControl,
  kDashboards,
  // Appended rather than inserted: sections are exported and imported BY NAME,
  // but the enum's numeric order is what the boot sequence and the export loop
  // walk, and renumbering an existing section is a change nobody would think to
  // look for when something goes wrong.
  kExperiments,
  kCount
};

const char* toString(ConfigSection section);

class ConfigStorage {
 public:
  // Bumped only when a change breaks compatibility; additive changes with a
  // manifest-provided default do not bump it.
  static constexpr std::uint16_t kSchemaVersion = LC_CONFIG_SCHEMA_VERSION;

  // Largest configuration file we are willing to parse.  A document bigger than
  // this means either corruption or a runaway writer; failing loudly beats
  // exhausting the heap.
  static constexpr std::size_t kMaxDocumentBytes = 16 * 1024;

  ConfigStorage(IStorageBackend& backend, EventBus& events)
      : backend_(backend), events_(events) {}

  // Creates /config, removes leftover *.tmp from an interrupted write, and
  // writes empty documents for the sections the boot sequence reads.
  Status begin();

  // Writes an empty, valid document for every section the boot sequence loads
  // that does not have a file yet.  Returns how many were created; zero on
  // every boot after the first.  Called by begin(); public so a test can watch
  // it happen twice and see the second call do nothing.
  std::size_t ensureDefaults();

  // kNotFound is a normal answer for a device that has never been configured;
  // callers treat it as "use defaults", not as an error.
  Status load(ConfigSection section, JsonDocument& out);
  Status save(ConfigSection section, JsonDocument& document);
  Status remove(ConfigSection section);

  static const char* path(ConfigSection section);

  // The configuration as it was before the last import.  One file, replaced
  // each time: an undo for the single most destructive thing the API can do,
  // not a version-control system living in 640 KB of flash.
  static constexpr const char* kBackupPath = "/config/backup/pre-import.json";
  Status saveBackup(JsonDocument& document);
  Status loadBackup(JsonDocument& out) const;

  std::uint32_t revision() const { return revision_; }

  // A stable identifier for the CONFIGURATION ITSELF: FNV-1a over the bytes of
  // every section file, in a fixed order.  `revision()` counts saves since
  // boot, which means two different rigs both answer 0 the morning after a
  // power cut — useless in the header of a dataset that has to say what it was
  // measured with (§48).  The fingerprint answers that question instead.
  // Recomputed on demand and cached until the next save.
  std::uint32_t fingerprint();
  std::uint16_t lastLoadedVersion() const { return lastLoadedVersion_; }

 private:
  Status migrate(ConfigSection section, JsonDocument& document,
                 std::uint16_t from);
  Status backup(ConfigSection section, const char* data, std::size_t bytes,
                std::uint16_t version);

  IStorageBackend& backend_;
  EventBus& events_;
  std::uint32_t revision_ = 0;
  std::uint32_t fingerprint_ = 0;
  bool fingerprintValid_ = false;
  std::uint16_t lastLoadedVersion_ = 0;
};

}  // namespace lc
