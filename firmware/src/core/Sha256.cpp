#include "core/Sha256.h"

#include <cstring>

namespace lc {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) {
  return (value >> bits) | (value << (32u - bits));
}

}  // namespace

void Sha256::reset() {
  state_[0] = 0x6a09e667u;
  state_[1] = 0xbb67ae85u;
  state_[2] = 0x3c6ef372u;
  state_[3] = 0xa54ff53au;
  state_[4] = 0x510e527fu;
  state_[5] = 0x9b05688cu;
  state_[6] = 0x1f83d9abu;
  state_[7] = 0x5be0cd19u;
  buffered_ = 0;
  totalBits_ = 0;
}

void Sha256::compress(const std::uint8_t block[kBlockBytes]) {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;

    h = g; g = f; f = e; e = d + temp1;
    d = c; c = b; b = a; a = temp1 + temp2;
  }

  state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
  state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t bytes) {
  totalBits_ += static_cast<std::uint64_t>(bytes) * 8u;
  while (bytes > 0) {
    const std::size_t room = kBlockBytes - buffered_;
    const std::size_t take = (bytes < room) ? bytes : room;
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

void Sha256::update(const char* text) {
  if (text == nullptr) return;
  update(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text));
}

void Sha256::finish(std::uint8_t digest[kDigestBytes]) {
  const std::uint64_t bits = totalBits_;
  const std::uint8_t one = 0x80u;
  update(&one, 1);
  totalBits_ = bits;  // padding does not count towards the length

  const std::uint8_t zero = 0x00u;
  while (buffered_ != 56) {
    update(&zero, 1);
    totalBits_ = bits;
  }

  std::uint8_t length[8];
  for (std::size_t i = 0; i < 8; ++i) {
    length[7 - i] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xffu);
  }
  update(length, 8);

  for (std::size_t i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xffu);
    digest[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xffu);
    digest[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xffu);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xffu);
  }
}

void Sha256::hash(const std::uint8_t* data, std::size_t bytes,
                  std::uint8_t digest[kDigestBytes]) {
  Sha256 sha;
  sha.update(data, bytes);
  sha.finish(digest);
}

void hmacSha256(const std::uint8_t* key, std::size_t keyBytes,
                const std::uint8_t* message, std::size_t messageBytes,
                std::uint8_t digest[Sha256::kDigestBytes]) {
  std::uint8_t block[Sha256::kBlockBytes] = {};
  if (keyBytes > Sha256::kBlockBytes) {
    Sha256::hash(key, keyBytes, block);
  } else {
    std::memcpy(block, key, keyBytes);
  }

  std::uint8_t inner[Sha256::kBlockBytes];
  std::uint8_t outer[Sha256::kBlockBytes];
  for (std::size_t i = 0; i < Sha256::kBlockBytes; ++i) {
    inner[i] = static_cast<std::uint8_t>(block[i] ^ 0x36u);
    outer[i] = static_cast<std::uint8_t>(block[i] ^ 0x5cu);
  }

  std::uint8_t innerDigest[Sha256::kDigestBytes];
  Sha256 sha;
  sha.update(inner, sizeof(inner));
  sha.update(message, messageBytes);
  sha.finish(innerDigest);

  Sha256 outerSha;
  outerSha.update(outer, sizeof(outer));
  outerSha.update(innerDigest, sizeof(innerDigest));
  outerSha.finish(digest);
}

void pbkdf2Sha256(const char* password, const std::uint8_t* salt,
                  std::size_t saltBytes, std::uint32_t iterations,
                  std::uint8_t* out, std::size_t outBytes) {
  if (password == nullptr || iterations == 0) return;
  const std::uint8_t* key = reinterpret_cast<const std::uint8_t*>(password);
  const std::size_t keyBytes = std::strlen(password);

  std::uint32_t blockIndex = 1;
  std::size_t produced = 0;
  while (produced < outBytes) {
    // U1 = HMAC(password, salt || INT(i))
    std::uint8_t input[64 + 4];
    const std::size_t saltUsed = (saltBytes > 64) ? 64 : saltBytes;
    std::memcpy(input, salt, saltUsed);
    input[saltUsed] = static_cast<std::uint8_t>((blockIndex >> 24) & 0xffu);
    input[saltUsed + 1] = static_cast<std::uint8_t>((blockIndex >> 16) & 0xffu);
    input[saltUsed + 2] = static_cast<std::uint8_t>((blockIndex >> 8) & 0xffu);
    input[saltUsed + 3] = static_cast<std::uint8_t>(blockIndex & 0xffu);

    std::uint8_t u[Sha256::kDigestBytes];
    std::uint8_t accumulator[Sha256::kDigestBytes];
    hmacSha256(key, keyBytes, input, saltUsed + 4, u);
    std::memcpy(accumulator, u, sizeof(u));

    for (std::uint32_t iteration = 1; iteration < iterations; ++iteration) {
      hmacSha256(key, keyBytes, u, sizeof(u), u);
      for (std::size_t i = 0; i < sizeof(u); ++i) accumulator[i] ^= u[i];
    }

    const std::size_t take = ((outBytes - produced) < sizeof(accumulator))
                                 ? (outBytes - produced)
                                 : sizeof(accumulator);
    std::memcpy(out + produced, accumulator, take);
    produced += take;
    ++blockIndex;
  }
}

bool equalsConstantTime(const std::uint8_t* a, const std::uint8_t* b,
                        std::size_t bytes) {
  std::uint8_t difference = 0;
  for (std::size_t i = 0; i < bytes; ++i) {
    difference = static_cast<std::uint8_t>(difference | (a[i] ^ b[i]));
  }
  return difference == 0;
}

void toHex(const std::uint8_t* data, std::size_t bytes, char* out,
           std::size_t outCapacity) {
  static const char kDigits[] = "0123456789abcdef";
  if (outCapacity == 0) return;
  std::size_t written = 0;
  for (std::size_t i = 0; i < bytes && written + 2 < outCapacity; ++i) {
    out[written++] = kDigits[(data[i] >> 4) & 0x0fu];
    out[written++] = kDigits[data[i] & 0x0fu];
  }
  out[written] = '\0';
}

std::size_t fromHex(const char* text, std::uint8_t* out,
                    std::size_t outCapacity) {
  if (text == nullptr) return 0;
  std::size_t written = 0;
  for (std::size_t i = 0; text[i] != '\0' && text[i + 1] != '\0'; i += 2) {
    if (written >= outCapacity) return 0;
    std::uint8_t value = 0;
    for (std::size_t half = 0; half < 2; ++half) {
      const char c = text[i + half];
      std::uint8_t nibble = 0;
      if (c >= '0' && c <= '9') nibble = static_cast<std::uint8_t>(c - '0');
      else if (c >= 'a' && c <= 'f') nibble = static_cast<std::uint8_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') nibble = static_cast<std::uint8_t>(c - 'A' + 10);
      else return 0;
      value = static_cast<std::uint8_t>((value << 4) | nibble);
    }
    out[written++] = value;
  }
  return written;
}

}  // namespace lc
