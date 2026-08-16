// =============================================================================
//  storage/IStorageBackend.h — the only thing ConfigStorage knows about files.
//
//  Two implementations:
//    * LittleFsBackend — on the device;
//    * PosixBackend    — on the host, so the whole configuration layer
//                        (schema versions, migrations, atomic writes, corrupt
//                        files, "disk full") is unit-tested with a real
//                        filesystem in a temporary directory.
//
//  Atomicity is part of the CONTRACT, not an implementation detail: a power cut
//  in the middle of saving a dashboard must not destroy the calibration file.
// =============================================================================
#pragma once

#include <cstddef>

#include "core/Error.h"

namespace lc {

class IStorageBackend {
 public:
  virtual ~IStorageBackend() = default;

  virtual bool exists(const char* path) const = 0;

  // Size in bytes, or an error if the file is missing.
  virtual Result<std::size_t> size(const char* path) const = 0;

  // Reads the whole file into `buffer` and NUL-terminates it.
  // Fails with kPayloadTooLarge if it does not fit in `capacity - 1`.
  virtual Result<std::size_t> read(const char* path, char* buffer,
                                   std::size_t capacity) const = 0;

  // Writes via "<path>.tmp" and renames.  Implementations MUST make the rename
  // the last step so that an interrupted write leaves the old file intact.
  virtual Status writeAtomic(const char* path, const char* data,
                             std::size_t size) = 0;

  // Appends to a file, creating it if needed.  NOT atomic, and cannot be: a
  // dataset is a stream, not a document, and the honest outcome of a power cut
  // in the middle of a row is a truncated last line — not a lost file, and
  // certainly not the previous version of one (Milestone 10).
  virtual Status append(const char* path, const char* data, std::size_t bytes) = 0;

  virtual Status remove(const char* path) = 0;
  virtual Status ensureDirectory(const char* path) = 0;

  // Deletes leftover "*.tmp" files in a directory.  Called once at boot: their
  // presence means a previous write was interrupted, and the valid data is the
  // file that was NOT renamed.
  virtual std::size_t purgeTemporaries(const char* directory) = 0;

  virtual std::size_t totalBytes() const = 0;
  virtual std::size_t freeBytes() const = 0;
};

}  // namespace lc
