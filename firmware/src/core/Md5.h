// =============================================================================
//  core/Md5.h — RFC 1321, streaming (M17).
//
//  WHY MD5, AND WHY NOT AS SECURITY.
//  Yandex Disk reports an MD5 for every stored file, and that number is the only
//  thing the controller can compare its own bytes against.  It is used here for
//  exactly one purpose: deciding whether the copy in the cloud is the same file
//  as the copy on flash, and therefore whether the local one may be deleted.
//  MD5 is broken for anything adversarial, and nothing here is adversarial — the
//  question is "did this upload arrive intact", not "did somebody forge it".
//  CRC-32 stays as the local check (M15); this is the remote one.
//
//  STREAMING IS NOT AN OPTIMISATION HERE, IT IS THE REQUIREMENT.
//  A segment is ~100 KiB and the device has ~100 KiB of usable heap.  The digest
//  is computed 4 KiB at a time as the file is read for upload, so the file is
//  never in RAM whole — which is the same rule the upload itself follows.
//
//  Platform-independent on purpose: the host tests hash the official RFC 1321
//  vectors, so a mistake in the padding or the length encoding fails in a
//  second rather than as an upload that is silently never acknowledged.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace lc {

class Md5 {
 public:
  static constexpr std::size_t kDigestBytes = 16;
  static constexpr std::size_t kBlockBytes = 64;
  /** 32 hex characters plus the terminator. */
  static constexpr std::size_t kTextBytes = 33;

  Md5() { reset(); }
  void reset();
  void update(const std::uint8_t* data, std::size_t bytes);
  void update(const char* text);
  /** Finalises into `digest`; reset() before reusing the object. */
  void finish(std::uint8_t digest[kDigestBytes]);
  /** Finalises straight into lower-case hex, which is the form Yandex
   *  reports and therefore the only form anything here compares. */
  void finishHex(char out[kTextBytes]);

  static void hash(const std::uint8_t* data, std::size_t bytes,
                   std::uint8_t digest[kDigestBytes]);
  static void hashHex(const std::uint8_t* data, std::size_t bytes,
                      char out[kTextBytes]);

 private:
  void compress(const std::uint8_t block[kBlockBytes]);

  std::uint32_t state_[4];
  std::uint8_t buffer_[kBlockBytes];
  std::size_t buffered_ = 0;
  std::uint64_t length_ = 0;
};

}  // namespace lc
