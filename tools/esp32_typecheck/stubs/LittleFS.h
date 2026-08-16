#pragma once
#include "FS.h"
namespace fs {
class LittleFSFS : public FS {
 public:
  bool begin(bool formatOnFail = false, const char* basePath = "/littlefs",
             std::uint8_t maxOpenFiles = 10,
             const char* partitionLabel = "spiffs");
  void end();
  bool format();
  std::size_t totalBytes();
  std::size_t usedBytes();
};
}  // namespace fs
extern fs::LittleFSFS LittleFS;
