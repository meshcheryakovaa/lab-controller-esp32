// =============================================================================
//  platform/esp32/YandexDiskClient.h — the REST side of Yandex Disk (M17).
//
//  Implements ICloudProvider, so everything above it is testable without a
//  network.  What lives HERE is only the part that genuinely needs one: URLs,
//  headers, JSON shapes and a streaming PUT.
//
//  THREE RULES THIS FILE ENFORCES AND NOTHING ELSE CAN.
//
//  1. The OAuth token goes to cloud-api.yandex.net and NOWHERE else.  The
//     upload itself is a PUT to a one-time URL on a different host, and that
//     request carries no Authorization header — the URL is the capability.
//     Attaching the token there would hand it to whatever answered.
//
//  2. That one-time URL is checked before a byte is sent.  It is the only
//     destination in this feature chosen by a remote answer rather than by us
//     (isTrustedUploadUrl, tested in test_cloud).
//
//  3. The file is streamed.  4 KiB at a time, from a buffer owned by this
//     object rather than the stack, because the whole feature exists to move
//     100 KiB files on a device with about that much usable heap.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Clock.h"
#include "platform/esp32/YandexOAuthClient.h"
#include "services/ICloudProvider.h"

namespace lc {
namespace platform {

class YandexDiskClient final : public ICloudProvider {
 public:
  static constexpr const char* kApiBase = "https://cloud-api.yandex.net/v1/disk";
  static constexpr std::size_t kUploadBufferBytes = 4096;
  static constexpr std::uint32_t kRequestTimeoutMs = 15000;
  static constexpr std::size_t kMaxResponseBytes = 4096;

  YandexDiskClient(const IClock& clock, YandexOAuthClient& oauth)
      : clock_(clock), oauth_(oauth) {}

  const char* name() const override { return "yandex"; }
  bool authorized() const override { return oauth_.authorized(); }

  CloudResult refreshAuthorizationIfNeeded() override;
  CloudResult ensureDirectory(const char* path) override;
  CloudResult stat(const char* path, CloudObjectInfo& out) override;
  CloudResult upload(const char* remotePath, IStorageBackend& storage,
                     const char* localPath, std::uint64_t bytes,
                     ICloudUploadObserver* observer) override;
  CloudResult move(const char* from, const char* to) override;
  CloudResult remove(const char* path) override;

  /** Reads the account's disk info.  Used by "test access", which deliberately
   *  writes no file: creating one to prove a connection leaves litter in
   *  somebody's Disk. */
  CloudResult checkAccess(std::uint64_t& totalBytes, std::uint64_t& usedBytes);

 private:
  CloudResult request(const char* method, const char* url,
                      JsonDocument* out, int& httpStatus);
  CloudResult requestUploadUrl(const char* remotePath,
                               FixedString<256>& href);
  CloudResult classify(int httpStatus, const JsonDocument& body,
                       std::uint32_t retryAfterMs);

  const IClock& clock_;
  YandexOAuthClient& oauth_;
  /** Owned by the object, not by the stack: the worker task's stack is the one
   *  place this must not be. */
  char buffer_[kUploadBufferBytes];
};

}  // namespace platform
}  // namespace lc
