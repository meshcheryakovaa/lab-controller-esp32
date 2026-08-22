// =============================================================================
//  core/Crc32.h — the checksum a transferred segment is judged by (M15).
//
//  Milestone 15 deletes a CSV from the controller once the browser says it has
//  it.  That sentence is only safe if "has it" means "has the same bytes", so
//  both ends compute CRC-32 (the ordinary zlib/PNG one, polynomial 0xEDB88320,
//  reflected, init and final xor 0xFFFFFFFF) and compare.
//
//  A nibble table rather than a 1 KiB byte table: the ESP32 pays for the table
//  in flash and the difference at 100 KiB per segment is well under a
//  millisecond, while the browser's implementation (frontend/src/lib/
//  log-offload/crc32.ts) uses the byte table because there RAM is free.  Both
//  are checked against the same vectors, which is the part that matters — two
//  implementations that agree by assertion are how a mismatch becomes a
//  deleted file.
//
//  This is not a security primitive.  It catches truncation and corruption in
//  transit, which is exactly what it is asked to do.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace lc {

class Crc32 {
 public:
  /** Fold more bytes in.  Call as many times as there are batches: the result
   *  is identical to hashing the concatenation in one go, which is what lets a
   *  segment be checksummed as it is written rather than re-read at the end. */
  void update(const void* data, std::size_t bytes) {
    static constexpr std::uint32_t kNibble[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    const std::uint8_t* bytesIn = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = state_;
    for (std::size_t i = 0; i < bytes; ++i) {
      crc ^= bytesIn[i];
      crc = kNibble[crc & 0x0F] ^ (crc >> 4);
      crc = kNibble[crc & 0x0F] ^ (crc >> 4);
    }
    state_ = crc;
  }

  /** The checksum of everything folded in so far.  Reading it does not end the
   *  computation, so a running total can be reported while a segment is still
   *  being written. */
  std::uint32_t value() const { return state_ ^ 0xFFFFFFFFu; }

  void reset() { state_ = 0xFFFFFFFFu; }

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

inline std::uint32_t crc32(const void* data, std::size_t bytes) {
  Crc32 crc;
  crc.update(data, bytes);
  return crc.value();
}

}  // namespace lc
