#pragma once
// Used by PsychicHttp 3.x for HTTP digest authentication.
#include <cstddef>
#include <cstdint>

typedef struct {
  std::uint32_t state[4];
  std::uint32_t total[2];
  unsigned char buffer[64];
} mbedtls_md5_context;

extern "C" {
void mbedtls_md5_init(mbedtls_md5_context* ctx);
void mbedtls_md5_free(mbedtls_md5_context* ctx);
int mbedtls_md5_starts(mbedtls_md5_context* ctx);
int mbedtls_md5_update(mbedtls_md5_context* ctx, const unsigned char* input, std::size_t ilen);
int mbedtls_md5_finish(mbedtls_md5_context* ctx, unsigned char output[16]);
int mbedtls_md5(const unsigned char* input, std::size_t ilen, unsigned char output[16]);
}
