// =============================================================================
//  platform/esp32/YandexCa.h — the certificate authority the cloud client
//  trusts (M17).
//
//  READ THIS BEFORE FLASHING: THIS FILE IS DELIBERATELY EMPTY.
//
//  A root CA certificate has to be the REAL one, taken from the chain that
//  oauth.yandex.ru and cloud-api.yandex.net actually present, at the time you
//  build.  It cannot be guessed, it cannot be written from memory, and a
//  plausible-looking certificate that is not the right one is worse than none
//  at all: it would look configured and fail at the first connection, which is
//  the point at which somebody reaches for setInsecure() to make the error go
//  away.
//
//  So the default here is empty, and an empty CA is a REFUSAL, not a fallback.
//  YandexOAuthClient and YandexDiskClient check this before they open a socket
//  and report CLOUD_NOT_CONFIGURED with an explanation.  Nothing in this
//  firmware calls WiFiClientSecure::setInsecure(), and nothing should: the
//  token being carried is an authorisation to write to somebody's Disk.
//
//  HOW TO FILL IT IN
//
//      openssl s_client -showcerts -connect cloud-api.yandex.net:443 </dev/null
//
//  Take the LAST certificate in the chain (the root), and check that the same
//  root also terminates the chain for oauth.yandex.ru.  Then either paste it
//  below, or — better for a build you will repeat — pass it at build time:
//
//      build_flags = -D LC_YANDEX_ROOT_CA='"-----BEGIN CERTIFICATE-----\n..."'
//
//  Certificates expire.  When this one does, the uploader stops with a TLS
//  error and the fix is to replace it — which is the correct behaviour for an
//  instrument that is holding somebody's cloud credentials.
// =============================================================================
#pragma once

namespace lc {
namespace platform {

#ifdef LC_YANDEX_ROOT_CA
inline constexpr const char* kYandexRootCa = LC_YANDEX_ROOT_CA;
#else
inline constexpr const char* kYandexRootCa = "";
#endif

/** False until a real certificate is supplied.  Both cloud clients check this
 *  and refuse rather than connecting without verification. */
inline bool cloudCertificateConfigured() {
  return kYandexRootCa != nullptr && kYandexRootCa[0] != '\0';
}

}  // namespace platform
}  // namespace lc
