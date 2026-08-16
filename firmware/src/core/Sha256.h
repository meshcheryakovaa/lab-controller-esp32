// =============================================================================
//  core/Sha256.h — SHA-256, HMAC-SHA256 and PBKDF2, with no dependencies.
//
//  Written out rather than pulled from mbedtls for one reason: the password
//  path has to be testable on the host against the published vectors.  A
//  credential check that only exists on the target is a credential check nobody
//  has ever verified — and "we assume the platform library is right" is exactly
//  the assumption that produces a device where every password matches.
//
//  The vectors from RFC 6234 (SHA-256), RFC 4231 (HMAC) and RFC 6070 (PBKDF2,
//  adapted to SHA-256) are in test_core.  If they pass on the host and the code
//  is this one, they pass on the board.
//
//  Not constant-time in the hashing itself, which does not matter: the input is
//  a password, the output is compared with `equalsConstantTime()`, and the
//  attacker who can measure a hash on an ESP32 over Wi-Fi can also unplug it.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace lc {

class Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;
  static constexpr std::size_t kBlockBytes = 64;

  Sha256() { reset(); }
  void reset();
  void update(const std::uint8_t* data, std::size_t bytes);
  void update(const char* text);
  // Finalises into `digest`; the object must be reset() before reuse.
  void finish(std::uint8_t digest[kDigestBytes]);

  static void hash(const std::uint8_t* data, std::size_t bytes,
                   std::uint8_t digest[kDigestBytes]);

 private:
  void compress(const std::uint8_t block[kBlockBytes]);

  std::uint32_t state_[8];
  std::uint8_t buffer_[kBlockBytes];
  std::size_t buffered_ = 0;
  std::uint64_t totalBits_ = 0;
};

void hmacSha256(const std::uint8_t* key, std::size_t keyBytes,
                const std::uint8_t* message, std::size_t messageBytes,
                std::uint8_t digest[Sha256::kDigestBytes]);

// PBKDF2-HMAC-SHA256.  `iterations` is a cost knob, not a magic number: it is
// stored beside the hash so that raising it later does not invalidate existing
// credentials.
void pbkdf2Sha256(const char* password, const std::uint8_t* salt,
                  std::size_t saltBytes, std::uint32_t iterations,
                  std::uint8_t* out, std::size_t outBytes);

// Compares without an early exit.  Used for every credential comparison; the
// cost of getting this wrong is a timing oracle, and the cost of doing it right
// is four lines.
bool equalsConstantTime(const std::uint8_t* a, const std::uint8_t* b,
                        std::size_t bytes);

void toHex(const std::uint8_t* data, std::size_t bytes, char* out,
           std::size_t outCapacity);
// Returns the number of bytes decoded, or 0 if `text` is not valid hex.
std::size_t fromHex(const char* text, std::uint8_t* out, std::size_t outCapacity);

}  // namespace lc
