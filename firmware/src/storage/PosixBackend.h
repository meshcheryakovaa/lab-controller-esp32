// =============================================================================
//  storage/PosixBackend.h — IStorageBackend on a normal filesystem.
//
//  Used by `pio test -e native` (and by desktop prototyping of the services
//  layer).  Everything under `root` is treated as the device's LittleFS.
// =============================================================================
#pragma once

#include <string>

#include "storage/IStorageBackend.h"

namespace lc {
namespace platform {

class PosixBackend final : public IStorageBackend {
 public:
  explicit PosixBackend(std::string root) : root_(std::move(root)) {}

  bool exists(const char* path) const override;
  Result<std::size_t> size(const char* path) const override;
  Result<std::size_t> read(const char* path, char* buffer,
                           std::size_t capacity) const override;
  Result<std::size_t> readAt(const char* path, std::size_t offset, char* buffer,
                             std::size_t bytes) const override;
  Status writeAtomic(const char* path, const char* data,
                     std::size_t bytes) override;
  Status append(const char* path, const char* data, std::size_t bytes) override;
  Status remove(const char* path) override;
  Status ensureDirectory(const char* path) override;
  std::size_t purgeTemporaries(const char* directory) override;
  std::size_t totalBytes() const override { return quotaBytes_; }
  std::size_t freeBytes() const override;

  // Lets a test simulate a full filesystem without filling a real disk.
  void setQuota(std::size_t bytes) { quotaBytes_ = bytes; }

 private:
  std::string absolute(const char* path) const;

  std::string root_;
  std::size_t quotaBytes_ = 640 * 1024;  // mirrors the 4 MB partition table
};

}  // namespace platform
}  // namespace lc
