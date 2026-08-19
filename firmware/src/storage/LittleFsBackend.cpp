#include "storage/LittleFsBackend.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_log.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace lc {
namespace platform {

Status LittleFsBackend::mount(bool formatOnFail) {
  if (mounted_) return ok();
  // The partition label is passed explicitly.  It is "spiffs" — see the comment
  // in partitions_4mb.csv — and spelling it out here means the mount no longer
  // depends on an Arduino default agreeing with a partition table, which is
  // exactly the pair that silently disagreed during Milestone 12 bring-up.
  if (!LittleFS.begin(formatOnFail, kMountPoint, kMaxOpenFiles, kPartitionLabel)) {
    return fail(ErrorCode::kStorageFailure, "LittleFS mount failed");
  }
  // esp_littlefs logs "<path> does not exist" at ERROR level for every stat of
  // a file that is not there — and asking whether a file is there is a normal,
  // frequent question: the HTTP layer probes for a pre-compressed twin of every
  // asset it serves.  The result is a console where errors are the background
  // noise, which is a console nobody reads.  Exactly one component is turned
  // down, by name, and nothing else in the log changes.
  esp_log_level_set("esp_littlefs", ESP_LOG_WARN);

  mounted_ = true;
  return ok();
}

 bool LittleFsBackend::exists(const char* path) const {
  if (!mounted_ || path == nullptr || path[0] != '/') return false;

  char fullPath[160];
  const int written = std::snprintf(fullPath, sizeof(fullPath), "%s%s",
                                    kMountPoint, path);
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof(fullPath)) {
    return false;
  }

  struct stat info {};
  return ::stat(fullPath, &info) == 0;
 }

Result<std::size_t> LittleFsBackend::size(const char* path) const {
  if (!mounted_) return fail(ErrorCode::kStorageFailure, "not mounted");
  if (path == nullptr || path[0] != '/') {
    return fail(ErrorCode::kInvalidArgument, "path");
  }

  char fullPath[160];
  const int written = std::snprintf(fullPath, sizeof(fullPath), "%s%s",
                                    kMountPoint, path);
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof(fullPath)) {
    return fail(ErrorCode::kInvalidArgument, "path too long");
  }

  struct stat info {};
  if (::stat(fullPath, &info) != 0) {
    return (errno == ENOENT)
               ? fail(ErrorCode::kNotFound, path)
               : fail(ErrorCode::kStorageFailure, path);
  }
  if (S_ISDIR(info.st_mode)) {
    return fail(ErrorCode::kInvalidArgument, "path is a directory");
  }
  return static_cast<std::size_t>(info.st_size);
}

Result<std::size_t> LittleFsBackend::read(const char* path, char* buffer,
                                          std::size_t capacity) const {
  if (!mounted_) return fail(ErrorCode::kStorageFailure, "not mounted");
  if (buffer == nullptr || capacity == 0) {
    return fail(ErrorCode::kInvalidArgument, "no buffer");
  }
  if (!exists(path)) return fail(ErrorCode::kNotFound, path);
  File file = LittleFS.open(path, "r");
  if (!file) return fail(ErrorCode::kNotFound, path);

  const std::size_t bytes = file.size();
  if (bytes >= capacity) {
    file.close();
    return fail(ErrorCode::kPayloadTooLarge, path);
  }
  const std::size_t read = file.readBytes(buffer, bytes);
  file.close();
  buffer[read] = '\0';
  return read;
}

Status LittleFsBackend::append(const char* path, const char* data,
                               std::size_t bytes) {
  if (!mounted_) return fail(ErrorCode::kStorageFailure, "not mounted");
  // The same 512-byte margin the atomic write keeps: LittleFS needs room for
  // metadata, and a filesystem that cannot even record a rename is not "nearly
  // full", it is full.
  if (freeBytes() < bytes + 512) return fail(ErrorCode::kFilesystemFull, path);

  File file = LittleFS.open(path, "a", /*create=*/true);
  if (!file) return fail(ErrorCode::kStorageFailure, "cannot open");
  const std::size_t written =
      file.write(reinterpret_cast<const std::uint8_t*>(data), bytes);
  file.flush();
  file.close();
  if (written != bytes) return fail(ErrorCode::kFilesystemFull, path);
  return ok();
}

Status LittleFsBackend::writeAtomic(const char* path, const char* data,
                                    std::size_t bytes) {
  if (!mounted_) return fail(ErrorCode::kStorageFailure, "not mounted");
  if (freeBytes() < bytes + 512) return fail(ErrorCode::kFilesystemFull, path);

  char temporary[96];
  std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);

  {
    File file = LittleFS.open(temporary, "w", /*create=*/true);
    if (!file) return fail(ErrorCode::kStorageFailure, "cannot open temp");
    const std::size_t written = file.write(
        reinterpret_cast<const std::uint8_t*>(data), bytes);
    file.flush();
    file.close();
    if (written != bytes) {
      LittleFS.remove(temporary);
      return fail(ErrorCode::kStorageFailure, "short write");
    }
  }

  // rename() replaces an existing target atomically.  Do not remove the
  // target first: besides producing a false error on first save, that would
  // introduce a power-loss window in which neither version exists.

  if (!LittleFS.rename(temporary, path)) {
    LittleFS.remove(temporary);
    return fail(ErrorCode::kStorageFailure, "rename failed");
  }
  return ok();
}

Status LittleFsBackend::remove(const char* path) {
  if (!mounted_) return fail(ErrorCode::kStorageFailure, "not mounted");
  if (!exists(path)) return fail(ErrorCode::kNotFound, path);
  return LittleFS.remove(path) ? ok() : fail(ErrorCode::kNotFound, path);
}

Status LittleFsBackend::ensureDirectory(const char* path) {
  if (!mounted_) return fail(ErrorCode::kStorageFailure, "not mounted");
  if (LittleFS.exists(path)) return ok();
  return LittleFS.mkdir(path) ? ok() : fail(ErrorCode::kStorageFailure, path);
}

std::size_t LittleFsBackend::purgeTemporaries(const char* directory) {
  if (!mounted_) return 0;
  File dir = LittleFS.open(directory, "r");
  if (!dir || !dir.isDirectory()) return 0;

  std::size_t purged = 0;
  char victims[8][96];
  std::size_t victimCount = 0;

  // Collect first, delete after closing the directory handle: deleting while
  // iterating is undefined behaviour in LittleFS.
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const char* name = entry.path();
    const std::size_t length = std::strlen(name);
    if (length >= 4 && std::strcmp(name + length - 4, ".tmp") == 0 &&
        victimCount < 8) {
      std::snprintf(victims[victimCount++], sizeof(victims[0]), "%s", name);
    }
    entry.close();
  }
  dir.close();

  for (std::size_t i = 0; i < victimCount; ++i) {
    if (LittleFS.remove(victims[i])) ++purged;
  }
  return purged;
}

std::size_t LittleFsBackend::totalBytes() const {
  return mounted_ ? LittleFS.totalBytes() : 0;
}

std::size_t LittleFsBackend::freeBytes() const {
  if (!mounted_) return 0;
  const std::size_t total = LittleFS.totalBytes();
  const std::size_t used = LittleFS.usedBytes();
  return (used >= total) ? 0 : (total - used);
}

}  // namespace platform
}  // namespace lc
