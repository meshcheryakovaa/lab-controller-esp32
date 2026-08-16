// Declaration-only Arduino FS, matching the ESP32 core's fs namespace.
#pragma once
#include "Arduino.h"
namespace fs {
class File;
class FS;

class File : public Stream {
 public:
  File() = default;
  std::size_t write(std::uint8_t) override;
  std::size_t write(const std::uint8_t* buf, std::size_t size) override;
  int available() override;
  int read() override;
  int peek() override;
  int read(std::uint8_t* buf, std::size_t size);
  void flush();
  bool seek(std::uint32_t pos);
  std::size_t position() const;
  std::size_t size() const;
  void close();
  operator bool() const;
  const char* name() const;
  const char* path() const;
  bool isDirectory();
  File openNextFile(const char* mode = "r");
  void rewindDirectory();
};

class FS {
 public:
  File open(const char* path, const char* mode = "r", bool create = false);
  File open(const String& path, const char* mode = "r", bool create = false);
  bool exists(const char* path);
  bool exists(const String& path);
  bool remove(const char* path);
  bool remove(const String& path);
  bool rename(const char* from, const char* to);
  bool mkdir(const char* path);
  bool rmdir(const char* path);
};
}  // namespace fs
using fs::File;
using fs::FS;
