#include "storage/PosixBackend.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace lc {
namespace platform {
namespace {

std::size_t directorySize(const std::string& path) {
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) return 0;
  std::size_t total = 0;
  while (dirent* entry = readdir(dir)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    const std::string child = path + "/" + entry->d_name;
    struct stat info {};
    if (stat(child.c_str(), &info) != 0) continue;
    total += S_ISDIR(info.st_mode) ? directorySize(child)
                                   : static_cast<std::size_t>(info.st_size);
  }
  closedir(dir);
  return total;
}

}  // namespace

std::string PosixBackend::absolute(const char* path) const {
  if (path == nullptr) return root_;
  return (path[0] == '/') ? (root_ + path) : (root_ + "/" + path);
}

bool PosixBackend::exists(const char* path) const {
  struct stat info {};
  return stat(absolute(path).c_str(), &info) == 0;
}

Result<std::size_t> PosixBackend::size(const char* path) const {
  struct stat info {};
  if (stat(absolute(path).c_str(), &info) != 0) {
    return fail(ErrorCode::kNotFound, path);
  }
  return static_cast<std::size_t>(info.st_size);
}

Result<std::size_t> PosixBackend::read(const char* path, char* buffer,
                                       std::size_t capacity) const {
  if (buffer == nullptr || capacity == 0) {
    return fail(ErrorCode::kInvalidArgument, "no buffer");
  }
  FILE* file = std::fopen(absolute(path).c_str(), "rb");
  if (file == nullptr) return fail(ErrorCode::kNotFound, path);

  const std::size_t bytes = std::fread(buffer, 1, capacity - 1, file);
  const bool overflow = (std::fgetc(file) != EOF);
  std::fclose(file);

  if (overflow) return fail(ErrorCode::kPayloadTooLarge, path);
  buffer[bytes] = '\0';
  return bytes;
}

Status PosixBackend::writeAtomic(const char* path, const char* data,
                                 std::size_t bytes) {
  if (freeBytes() < bytes) return fail(ErrorCode::kFilesystemFull, path);

  const std::string target = absolute(path);
  const std::string temporary = target + ".tmp";

  // Create the parent directory on demand: a fresh device has no /config yet.
  const std::size_t slash = target.find_last_of('/');
  if (slash != std::string::npos) {
    std::string parent = target.substr(0, slash);
    for (std::size_t i = root_.size() + 1; i <= parent.size(); ++i) {
      if (i == parent.size() || parent[i] == '/') {
        mkdir(parent.substr(0, i).c_str(), 0775);
      }
    }
  }

  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (file == nullptr) return fail(ErrorCode::kStorageFailure, "cannot open temp");

  const std::size_t written = std::fwrite(data, 1, bytes, file);
  const bool flushed = (std::fflush(file) == 0);
  std::fclose(file);

  if (written != bytes || !flushed) {
    ::remove(temporary.c_str());
    return fail(ErrorCode::kStorageFailure, "short write");
  }
  if (::rename(temporary.c_str(), target.c_str()) != 0) {
    ::remove(temporary.c_str());
    return fail(ErrorCode::kStorageFailure, "rename failed");
  }
  return ok();
}

Status PosixBackend::append(const char* path, const char* data,
                            std::size_t bytes) {
  if (freeBytes() < bytes) return fail(ErrorCode::kFilesystemFull, path);

  const std::string target = absolute(path);
  const std::size_t slash = target.find_last_of('/');
  if (slash != std::string::npos) {
    std::string parent = target.substr(0, slash);
    for (std::size_t i = root_.size() + 1; i <= parent.size(); ++i) {
      if (i == parent.size() || parent[i] == '/') {
        mkdir(parent.substr(0, i).c_str(), 0775);
      }
    }
  }

  FILE* file = std::fopen(target.c_str(), "ab");
  if (file == nullptr) return fail(ErrorCode::kStorageFailure, "cannot open");
  const std::size_t written = std::fwrite(data, 1, bytes, file);
  std::fflush(file);
  std::fclose(file);
  if (written != bytes) return fail(ErrorCode::kFilesystemFull, path);
  return ok();
}

Status PosixBackend::remove(const char* path) {
  if (::remove(absolute(path).c_str()) != 0) {
    return fail(ErrorCode::kNotFound, path);
  }
  return ok();
}

Status PosixBackend::ensureDirectory(const char* path) {
  const std::string full = absolute(path);
  for (std::size_t i = root_.size() + 1; i <= full.size(); ++i) {
    if (i == full.size() || full[i] == '/') {
      mkdir(full.substr(0, i).c_str(), 0775);
    }
  }
  return exists(path) ? ok() : fail(ErrorCode::kStorageFailure, path);
}

std::size_t PosixBackend::purgeTemporaries(const char* directory) {
  const std::string full = absolute(directory);
  DIR* dir = opendir(full.c_str());
  if (dir == nullptr) return 0;

  std::size_t purged = 0;
  while (dirent* entry = readdir(dir)) {
    const std::size_t length = std::strlen(entry->d_name);
    if (length < 4 || std::strcmp(entry->d_name + length - 4, ".tmp") != 0) continue;
    if (::remove((full + "/" + entry->d_name).c_str()) == 0) ++purged;
  }
  closedir(dir);
  return purged;
}

std::size_t PosixBackend::freeBytes() const {
  const std::size_t used = directorySize(root_);
  return (used >= quotaBytes_) ? 0 : (quotaBytes_ - used);
}

}  // namespace platform
}  // namespace lc
