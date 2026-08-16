// =============================================================================
//  storage/LogStore.h — datasets on disk: the CSV, its header, and the index
//  (§33, §48, ADR-0019).
//
//  DataLogger formats rows and counts what it could not write.  This class owns
//  everything that touches the filesystem: the file, the header that makes the
//  dataset re-analysable in a year, the footer that says how it ended, and the
//  index the Logs page reads.
//
//  THE HEADER IS THE POINT.
//  A column of numbers is not a measurement.  What makes it one is the block
//  above it: which experiment, which operator, which sample, which
//  configuration revision, and which calibration produced every unit in the
//  column names.  Written once, at open, from the rig as it is at that moment.
//
//  THE FOOTER IS THE OTHER POINT.
//  Every dataset ends with a line that says how it ended — including
//  `truncated: …` when the medium filled.  A file that simply stops is a file
//  somebody will assume is complete.
//
//  THE LOGGER NEVER DELETES ANYBODY'S DATA.
//  Not to make room, not to keep the index tidy.  When the index is full or the
//  medium is at its reserve, new sessions are REFUSED and the operator decides
//  what goes.  That is the difference between an instrument and an appliance.
//
//  File layout:
//    /data/logs/<id>.csv     one dataset
//    /data/logs.json         the index (metadata only, never the samples)
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Error.h"
#include "services/CalibrationManager.h"
#include "services/DataLogger.h"
#include "storage/ConfigStorage.h"
#include "storage/IStorageBackend.h"

namespace lc {

class LogStore final : public ILogSink {
 public:
  static constexpr const char* kIndexPath = "/data/logs.json";
  static constexpr const char* kDirectory = "/data/logs";
  static constexpr std::size_t kMaxIndexBytes = 8 * 1024;
  // Space the log may never consume.  An instrument whose filesystem has been
  // eaten by its own dataset cannot serve its web interface or save its
  // configuration; that is not a full disk, that is a brick.
  static constexpr std::size_t kReserveBytes = 64 * 1024;
  // One dataset never grows past this: §33 asks for rotation at 16 MB, and on a
  // device whose whole filesystem is smaller than that, this is simply the
  // ceiling nothing may cross.
  static constexpr std::size_t kMaxSessionBytes = 16 * 1024 * 1024;

  LogStore(IStorageBackend& backend, ConfigStorage& storage,
           CalibrationManager* calibrations = nullptr)
      : backend_(backend), storage_(storage), calibrations_(calibrations) {}

  Status begin();

  // --- ILogSink ------------------------------------------------------------
  Status openSession(const LogSpec& spec, const char* const* columns,
                     std::size_t columnCount, KeyString& id) override;
  Status appendRows(const char* text, std::size_t bytes) override;
  void closeSession(const LogStatus& status) override;
  std::size_t writableBytes() const override;

  // --- the index -----------------------------------------------------------
  Status loadIndex(JsonDocument& out) const;
  // Removes a dataset and its file.  The only way data is ever deleted here is
  // somebody asking for it by name.
  Status removeSession(const char* id);
  // Path of a dataset's file, for the streaming export.  Empty if unknown.
  bool pathFor(const char* id, FixedString<64>& out) const;

  std::size_t sessionCount() const;

 private:
  Status saveIndex(JsonDocument& document);
  Status writeHeader(const LogSpec& spec, const char* const* columns,
                     std::size_t columnCount);

  IStorageBackend& backend_;
  ConfigStorage& storage_;
  CalibrationManager* calibrations_ = nullptr;

  FixedString<64> currentPath_;
  KeyString currentId_;
  bool open_ = false;
};

}  // namespace lc
