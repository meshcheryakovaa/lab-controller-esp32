#include "core/Md5.h"

#include <cstring>

namespace lc {
namespace {

// The per-round shift amounts and the sine-derived constants, straight from
// RFC 1321.  Written out rather than computed so that the table itself can be
// read against the specification.
constexpr std::uint8_t kShift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

constexpr std::uint32_t kK[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

inline std::uint32_t rotateLeft(std::uint32_t value, std::uint8_t bits) {
  return (value << bits) | (value >> (32 - bits));
}

// Little-endian, unlike SHA-256 next door.  Getting this backwards produces a
// digest that is stable, plausible and wrong — which is why the host tests
// check the published vectors rather than only self-consistency.
inline std::uint32_t readLe32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void writeLe32(std::uint8_t* p, std::uint32_t value) {
  p[0] = static_cast<std::uint8_t>(value & 0xFF);
  p[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  p[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  p[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

}  // namespace

void Md5::reset() {
  state_[0] = 0x67452301;
  state_[1] = 0xefcdab89;
  state_[2] = 0x98badcfe;
  state_[3] = 0x10325476;
  buffered_ = 0;
  length_ = 0;
}

void Md5::compress(const std::uint8_t block[kBlockBytes]) {
  std::uint32_t m[16];
  for (std::size_t i = 0; i < 16; ++i) m[i] = readLe32(block + i * 4);

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];

  for (std::size_t i = 0; i < 64; ++i) {
    std::uint32_t f = 0;
    std::size_t g = 0;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    const std::uint32_t temp = d;
    d = c;
    c = b;
    b = b + rotateLeft(a + f + kK[i] + m[g], kShift[i]);
    a = temp;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
}

void Md5::update(const std::uint8_t* data, std::size_t bytes) {
  if (data == nullptr) return;
  length_ += bytes;
  while (bytes > 0) {
    const std::size_t room = kBlockBytes - buffered_;
    const std::size_t take = bytes < room ? bytes : room;
    std::memcpy(buffer_ + buffered_, data, take);
    buffered_ += take;
    data += take;
    bytes -= take;
    if (buffered_ == kBlockBytes) {
      compress(buffer_);
      buffered_ = 0;
    }
  }
}

void Md5::update(const char* text) {
  if (text == nullptr) return;
  update(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text));
}

void Md5::finish(std::uint8_t digest[kDigestBytes]) {
  // 0x80, zeroes, then the length in BITS as a little-endian 64-bit value.
  const std::uint64_t bits = length_ * 8;
  std::uint8_t padding = 0x80;
  update(&padding, 1);
  // update() has already counted that byte; the count is not used again except
  // through `bits`, captured above.
  padding = 0x00;
  while (buffered_ != 56) update(&padding, 1);

  std::uint8_t tail[8];
  writeLe32(tail, static_cast<std::uint32_t>(bits & 0xFFFFFFFFu));
  writeLe32(tail + 4, static_cast<std::uint32_t>((bits >> 32) & 0xFFFFFFFFu));
  std::memcpy(buffer_ + 56, tail, 8);
  compress(buffer_);
  buffered_ = 0;

  for (std::size_t i = 0; i < 4; ++i) writeLe32(digest + i * 4, state_[i]);
}

void Md5::finishHex(char out[kTextBytes]) {
  std::uint8_t digest[kDigestBytes];
  finish(digest);
  static const char kHex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < kDigestBytes; ++i) {
    out[i * 2] = kHex[(digest[i] >> 4) & 0x0F];
    out[i * 2 + 1] = kHex[digest[i] & 0x0F];
  }
  out[kDigestBytes * 2] = '\0';
}

void Md5::hash(const std::uint8_t* data, std::size_t bytes,
               std::uint8_t digest[kDigestBytes]) {
  Md5 md5;
  md5.update(data, bytes);
  md5.finish(digest);
}

void Md5::hashHex(const std::uint8_t* data, std::size_t bytes,
                  char out[kTextBytes]) {
  Md5 md5;
  md5.update(data, bytes);
  md5.finishHex(out);
}

}  // namespace lc
