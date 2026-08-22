#pragma once
// PsychicHttp 3.x base64-encodes the WebSocket handshake key with mbedtls,
// where 2.x used libb64.  Only the signature matters for the host typecheck.
#include <cstddef>

extern "C" {
int mbedtls_base64_encode(unsigned char* dst, std::size_t dlen, std::size_t* olen,
                          const unsigned char* src, std::size_t slen);
int mbedtls_base64_decode(unsigned char* dst, std::size_t dlen, std::size_t* olen,
                          const unsigned char* src, std::size_t slen);
}
