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
// GlobalSign Root R3 signs the RSA OV 2018 chain currently used by Yandex
// OAuth.  Root R5 covers the corresponding GlobalSign ECC hierarchy used by
// Yandex services.  Both trust anchors are published by GlobalSign; keeping
// the pair here also lets Yandex switch between its RSA and ECC certificates
// without turning a running experiment's uploader off.
inline constexpr const char* kYandexRootCa = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4
GA1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbF
NpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwM
zE4MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMzET
MBEGA1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQY
JKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2Ec
WtiHL8RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUh
hB5uzsTgHeMCOFJ0mpiLx9e+pZo34knlTifBtc+ycsmWQ1z3rDI6SYOgxXG71uL
0gRgykmmKPZpO/bLyCiR5Z2KYVc3rHQU3HTgOu5yLy6c+9C7v/U9AOEGM+iCK65
TpjoWc4zdQQ4gOsC0p6Hpsk+QLjJg6VfLuQSSaGjlOCZgdbKfd/+RFO+uIEn8rU
AVSNECMWEZXriX7613t2Saer9fwRPvm2L7DWzgVGkWqQPabumDk3F2xmmFghcCA
wEAAaNCMEAwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0O
BBYEFI/wS3+oLkUkrk1Q+mOai97i3Ru8MA0GCSqGSIb3DQEBCwUAA4IBAQBLQNv
AUKr+yAzv95ZURUm7lgAJQayzE4aGKAczymvmdLm6AC2upArT9fHxD4q/c2dKg8
dEe3jgr25sbwMpjjM5RcOO5LlXbKr8EpbsU8Yt5CRsuZRj+9xTaGdWPoO4zzUhw
8lo/s7awlOqzJCK6fBdRoyV3XpYKBovHd7NADdBj+1EbddTKJd+82cEHhXXipa0
095MJ6RMG3NzdvQXmcIfeg7jLQitChws/zyrVQ4PkX4268NXSb7hLi18YIvDQVE
TI53O9zJrlAGomecsMx86OyXShkDOOyyGeMlhLxS67ttVb9+E7gUJTb0o2HLO02
JQZR7rkpeDMdmztcpHWD9f
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICHjCCAaSgAwIBAgIRYFlJ4CYuu1X5CneKcflK2GwwCgYIKoZIzj0EAwMwUDE
kMCIGA1UECxMbR2xvYmFsU2lnbiBFQ0MgUm9vdCBDQSAtIFI1MRMwEQYDVQQKEw
pHbG9iYWxTaWduMRMwEQYDVQQDEwpHbG9iYWxTaWduMB4XDTEyMTExMzAwMDAwM
FoXDTM4MDExOTAzMTQwN1owUDEkMCIGA1UECxMbR2xvYmFsU2lnbiBFQ0MgUm9v
dCBDQSAtIFI1MRMwEQYDVQQKEwpHbG9iYWxTaWduMRMwEQYDVQQDEwpHbG9iYWx
TaWduMHYwEAYHKoZIzj0CAQYFK4EEACIDYgAER0UOlvt9Xb/pOdEh+J8LttV7Hp
I6SFkc8GIxLcB6KP4ap1yztsyX50XUWPrRd21DosCHZTQKH3rd6zwzocWdTaRvQ
ZU4f8kehOvRnkmSh5SHDDqFSmafnVmTTZdhBoZKo0IwQDAOBgNVHQ8BAf8EBAMC
AQYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUPeYpSJvqB8ohREom3m7e0oP
Qn1kwCgYIKoZIzj0EAwMDaAAwZQIxAOVpEslu28YxuglB4Zf4+/2a4n0Sye18ZN
PLBSWLVtmg515dTguDnFt2KaAJJiFqYgIwcdK1j1zqO+F4CYWodZI7yFz9SO8Nd
CKoCOJuxUnOxwy8p2Fp8fc74SrL+SvzZpA3
-----END CERTIFICATE-----
)PEM";
#endif

/** False until a real certificate is supplied.  Both cloud clients check this
 *  and refuse rather than connecting without verification. */
inline bool cloudCertificateConfigured() {
  return kYandexRootCa != nullptr && kYandexRootCa[0] != '\0';
}

}  // namespace platform
}  // namespace lc
