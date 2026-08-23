// =============================================================================
//  storage/LittleFsBackend.h — IStorageBackend on the device.
//
//  LittleFS rather than SPIFFS: it has wear levelling and survives power loss
//  mid-write, which for a file holding calibration coefficients is not
//  optional.  The partition is separate from the OTA slots, so a firmware
//  update never touches the user's profiles, calibrations or dashboards (§45).
// =============================================================================
#pragma once

#include "storage/IStorageBackend.h"

namespace lc {
namespace platform {

class LittleFsBackend final : public IStorageBackend {
 public:
  // The label in partitions_*.csv.  Arduino's LittleFS defaults to this name,
  // and the partition table now says it out loud rather than relying on the
  // default; both halves of that agreement are written down on purpose.
  static constexpr const char* kPartitionLabel = "spiffs";
  static constexpr const char* kMountPoint = "/littlefs";
  static constexpr std::uint8_t kMaxOpenFiles = 10;

  // `formatOnFail` formats the partition if it cannot be mounted — correct for
  // a brand-new board, and reported as a warning so it is never silent.
  Status mount(bool formatOnFail = true);
  bool mounted() const { return mounted_; }

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
  std::size_t totalBytes() const override;
  std::size_t freeBytes() const override;

 private:
  bool mounted_ = false;
};

}  // namespace platform
}  // namespace lc
