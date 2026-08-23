#include "platform/esp32/YandexOAuthClient.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>

#include <cstring>

#include "core/Format.h"
#include "platform/esp32/YandexCa.h"

namespace lc {
namespace platform {
namespace {

constexpr const char* kDeviceCodePath = "https://oauth.yandex.ru/device/code";
constexpr const char* kTokenPath      = "https://oauth.yandex.ru/token";
constexpr const char* kRevokePath     = "https://oauth.yandex.ru/revoke_token";
constexpr const char* kVerificationUrl = "https://oauth.yandex.ru/device";

/** Percent-encodes into a bounded buffer.  Client ids and device names are
 *  operator-supplied and go into a form body; encoding them is what stops a
 *  stray '&' turning one parameter into two. */
bool formEncode(const char* input, char* out, std::size_t capacity) {
  static const char kHex[] = "0123456789ABCDEF";
  std::size_t used = 0;
  for (const char* p = input; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~';
    if (unreserved) {
      if (used + 2 > capacity) return false;
      out[used++] = static_cast<char>(c);
    } else {
      if (used + 4 > capacity) return false;
      out[used++] = '%';
      out[used++] = kHex[(c >> 4) & 0x0F];
      out[used++] = kHex[c & 0x0F];
    }
  }
  if (used + 1 > capacity) return false;
  out[used] = '\0';
  return true;
}

}  // namespace

const char* toString(DeviceCodeState state) {
  switch (state) {
    case DeviceCodeState::kIdle:           return "IDLE";
    case DeviceCodeState::kRequestingCode: return "REQUESTING_CODE";
    case DeviceCodeState::kWaitingUser:    return "WAITING_USER";
    case DeviceCodeState::kAuthorized:     return "AUTHORIZED";
    case DeviceCodeState::kExpired:        return "EXPIRED";
    case DeviceCodeState::kFailed:         return "FAILED";
  }
  return "IDLE";
}

void YandexOAuthClient::reload() {
  if (!store_.loadTokens(tokens_)) tokens_ = CloudTokens{};
}

bool YandexOAuthClient::authorized() const {
  if (!tokens_.valid()) return false;
  if (tokens_.expiresAtEpochMs == 0) return true;
  return clock_.epochMillis() + CloudCredentialStore::kRenewBeforeMs <
         tokens_.expiresAtEpochMs;
}

std::uint32_t YandexOAuthClient::secondsRemaining() const {
  const EpochMs now = clock_.epochMillis();
  if (deviceCodeExpiresAt_ <= now) return 0;
  return static_cast<std::uint32_t>((deviceCodeExpiresAt_ - now) / 1000);
}

/**
 * One HTTPS form POST to Yandex, with a verified certificate.
 *
 * The client is a LOCAL: one TLS session at a time is a deliberate memory
 * decision on a device with about 100 KiB of usable heap, and holding a session
 * open between an operator's two button presses would be holding it for
 * minutes.
 */
CloudResult YandexOAuthClient::postForm(const char* url, const char* body,
                                        bool withBasicAuth, JsonDocument& out,
                                        int& httpStatus) {
  httpStatus = 0;
  if (clock_.epochMillis() < 1600000000000ull) {
    // Without a real date every certificate looks expired or not yet valid.
    // Waiting is the answer; setInsecure() would be the other one, and it would
    // throw away the only thing protecting the token in transit.
    return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                     "the clock is not set yet");
  }

  if (!cloudCertificateConfigured()) {
    // No CA means no way to know who is answering.  This is a refusal, not a
    // reason to fall back — see YandexCa.h.
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kCloudNotConfigured,
                     "no root certificate is built in; see YandexCa.h");
  }

  WiFiClientSecure client;
  client.setCACert(kYandexRootCa);
  client.setTimeout(kRequestTimeoutMs / 1000);

  HTTPClient http;
  if (!http.begin(client, url)) {
    return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                     "could not reach the authorisation service");
  }
  http.setTimeout(kRequestTimeoutMs);
  // Never follow a redirect: a 3xx from an auth endpoint is either a mistake or
  // somebody's idea, and following one could carry the Basic credentials below
  // to a host nobody vetted.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  if (withBasicAuth) {
    FixedString<kOAuthClientIdLength> id;
    FixedString<kOAuthSecretLength> secret;
    if (!store_.clientId(id) || !store_.clientSecret(secret)) {
      http.end();
      return cloudFail(CloudFailure::kPermanent, ErrorCode::kCloudNotConfigured,
                       "the OAuth application is not configured");
    }
    char pair[kOAuthClientIdLength + kOAuthSecretLength + 2];
    std::size_t used = 0;
    appendFormat(pair, sizeof(pair), used, "%s:%s", id.c_str(), secret.c_str());

    unsigned char encoded[256];
    std::size_t encodedLength = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded) - 1, &encodedLength,
                              reinterpret_cast<const unsigned char*>(pair),
                              used) != 0) {
      http.end();
      return cloudFail(CloudFailure::kPermanent, ErrorCode::kInternal,
                       "the credentials could not be encoded");
    }
    encoded[encodedLength] = '\0';
    char header[320];
    std::size_t headerUsed = 0;
    appendFormat(header, sizeof(header), headerUsed, "Basic %s",
                 reinterpret_cast<const char*>(encoded));
    http.addHeader("Authorization", header);
  }

  const int status = http.POST(
    const_cast<std::uint8_t*>(
        reinterpret_cast<const std::uint8_t*>(body)),
    std::strlen(body));

  httpStatus = status;
  if (status <= 0) {
    http.end();
    return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                     "the authorisation service did not answer");
  }

  // Bounded: these responses are small and known, and a device that read
  // whatever a server sent would be handing out its heap on request.
  const int length = http.getSize();
  if (length > static_cast<int>(kMaxResponseBytes)) {
    http.end();
    return cloudFail(CloudFailure::kTransient, ErrorCode::kPayloadTooLarge,
                     "the authorisation answer is unexpectedly large");
  }
  const String payload = http.getString();
  http.end();
  if (payload.length() > kMaxResponseBytes) {
    return cloudFail(CloudFailure::kTransient, ErrorCode::kPayloadTooLarge,
                     "the authorisation answer is unexpectedly large");
  }

  const DeserializationError parsed = deserializeJson(out, payload);
  if (parsed) {
    return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                     "the authorisation answer could not be read");
  }
  return cloudOk();
}

CloudResult YandexOAuthClient::beginDeviceCode() {
  FixedString<kOAuthClientIdLength> id;
  if (!store_.clientId(id)) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kCloudNotConfigured,
                     "enter the OAuth client id first");
  }
  FixedString<40> device;
  store_.deviceId(device);

  char encodedId[kOAuthClientIdLength * 3];
  char encodedDevice[128];
  if (!formEncode(id.c_str(), encodedId, sizeof(encodedId)) ||
      !formEncode(device.c_str(), encodedDevice, sizeof(encodedDevice))) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kInvalidArgument,
                     "the client id cannot be used");
  }

  char body[512];
  std::size_t used = 0;
  if (!appendFormat(body, sizeof(body), used,
                    "client_id=%s&device_id=%s&device_name=LabController",
                    encodedId, encodedDevice)) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kPayloadTooLarge,
                     "the request does not fit");
  }

  state_ = DeviceCodeState::kRequestingCode;
  JsonDocument response;
  int status = 0;
  const CloudResult sent = postForm(kDeviceCodePath, body, false, response, status);
  if (!sent.ok()) {
    state_ = DeviceCodeState::kFailed;
    lastError_ = sent.status;
    return sent;
  }
  if (status != 200) {
    state_ = DeviceCodeState::kFailed;
    // The message is Yandex's own — but only the "error_description" field,
    // never the whole body, which would risk echoing anything it contained.
    lastError_ = fail(ErrorCode::kCloudUnauthorized,
                      response["error_description"] | "the request was refused");
    return cloudFail(CloudFailure::kPermanent, lastError_.code,
                     lastError_.detail.c_str());
  }

  deviceCode_.assign(response["device_code"] | "");
  prompt_.userCode.assign(response["user_code"] | "");
  prompt_.verificationUrl.assign(response["verification_url"] | kVerificationUrl);
  prompt_.expiresInSeconds = response["expires_in"] | 300u;
  prompt_.pollIntervalSeconds = response["interval"] | 5u;
  if (deviceCode_.empty() || prompt_.userCode.empty()) {
    state_ = DeviceCodeState::kFailed;
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kInternal,
                     "the authorisation answer was incomplete");
  }

  deviceCodeExpiresAt_ =
      clock_.epochMillis() + prompt_.expiresInSeconds * 1000ull;
  nextPollAt_ = clock_.epochMillis() + prompt_.pollIntervalSeconds * 1000ull;
  state_ = DeviceCodeState::kWaitingUser;
  return cloudOk();
}

CloudResult YandexOAuthClient::applyTokenResponse(const JsonDocument& document) {
  CloudTokens fresh;
  fresh.accessToken.assign(document["access_token"] | "");
  fresh.refreshToken.assign(document["refresh_token"] | "");
  fresh.scope.assign(document["scope"] | "");
  const std::uint64_t expiresIn = document["expires_in"] | 0ull;
  fresh.expiresAtEpochMs =
      expiresIn > 0 ? clock_.epochMillis() + expiresIn * 1000ull : 0;
  fresh.generation = tokens_.generation + 1;

  if (fresh.accessToken.empty()) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kInternal,
                     "the answer contained no access token");
  }
  // Yandex does not always return a new refresh token; keeping the old one is
  // correct, and losing it would silently turn a working link into one that
  // expires for good in a year.
  if (fresh.refreshToken.empty()) fresh.refreshToken = tokens_.refreshToken;

  const Status saved = store_.saveTokens(fresh);
  if (!saved.ok()) {
    return cloudFail(CloudFailure::kPermanent, saved.code, saved.detail.c_str());
  }
  tokens_ = fresh;
  return cloudOk();
}

CloudResult YandexOAuthClient::poll() {
  if (state_ != DeviceCodeState::kWaitingUser) return cloudOk();
  const EpochMs now = clock_.epochMillis();
  if (now < nextPollAt_) return cloudOk();
  if (now >= deviceCodeExpiresAt_) {
    state_ = DeviceCodeState::kExpired;
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kTimeout,
                     "the code expired; start again");
  }
  nextPollAt_ = now + prompt_.pollIntervalSeconds * 1000ull;

  char encodedCode[192];
  if (!formEncode(deviceCode_.c_str(), encodedCode, sizeof(encodedCode))) {
    state_ = DeviceCodeState::kFailed;
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kInternal, "bad code");
  }
  char body[256];
  std::size_t used = 0;
  appendFormat(body, sizeof(body), used, "grant_type=device_code&code=%s",
               encodedCode);

  JsonDocument response;
  int status = 0;
  const CloudResult sent = postForm(kTokenPath, body, true, response, status);
  if (!sent.ok()) return sent;  // a network hiccup: keep waiting

  if (status == 200) {
    const CloudResult applied = applyTokenResponse(response);
    if (!applied.ok()) {
      state_ = DeviceCodeState::kFailed;
      return applied;
    }
    state_ = DeviceCodeState::kAuthorized;
    return cloudOk();
  }

  const char* error = response["error"] | "";
  if (std::strcmp(error, "authorization_pending") == 0) {
    // The person has not finished yet.  Normal, expected, and deliberately
    // silent: an event per poll would fill the log with nothing happening.
    return cloudOk();
  }
  if (std::strcmp(error, "slow_down") == 0) {
    prompt_.pollIntervalSeconds += 5;
    return cloudOk();
  }
  if (std::strcmp(error, "expired_token") == 0) {
    state_ = DeviceCodeState::kExpired;
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kTimeout,
                     "the code expired; start again");
  }
  state_ = DeviceCodeState::kFailed;
  lastError_ = fail(ErrorCode::kCloudUnauthorized,
                    response["error_description"] | "authorisation was refused");
  return cloudFail(CloudFailure::kPermanent, lastError_.code,
                   lastError_.detail.c_str());
}

CloudResult YandexOAuthClient::refreshIfNeeded(bool force) {
  if (!force && authorized()) return cloudOk();
  if (refreshing_) {
    return cloudFail(CloudFailure::kTransient, ErrorCode::kResourceBusy,
                     "a token refresh is already running");
  }
  if (tokens_.refreshToken.empty()) {
    return cloudFail(CloudFailure::kAuthRevoked, ErrorCode::kCloudAuthRevoked,
                     "the account is not linked");
  }

  refreshing_ = true;
  char encoded[kOAuthTokenLength * 3];
  if (!formEncode(tokens_.refreshToken.c_str(), encoded, sizeof(encoded))) {
    refreshing_ = false;
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kInternal, "bad token");
  }
  char body[kOAuthTokenLength * 3 + 64];
  std::size_t used = 0;
  appendFormat(body, sizeof(body), used,
               "grant_type=refresh_token&refresh_token=%s", encoded);

  JsonDocument response;
  int status = 0;
  const CloudResult sent = postForm(kTokenPath, body, true, response, status);
  refreshing_ = false;
  if (!sent.ok()) return sent;

  if (status == 200) return applyTokenResponse(response);

  const char* error = response["error"] | "";
  if (std::strcmp(error, "invalid_grant") == 0) {
    // The account was unlinked at the far end.  Not retryable by any amount of
    // waiting; it needs a person, and the queue says so rather than spinning.
    return cloudFail(CloudFailure::kAuthRevoked, ErrorCode::kCloudAuthRevoked,
                     "the cloud account needs to be linked again");
  }
  return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudUnauthorized,
                   "the token could not be renewed");
}

CloudResult YandexOAuthClient::disconnect() {
  CloudResult result = cloudOk();
  if (tokens_.valid()) {
    char encoded[kOAuthTokenLength * 3];
    if (formEncode(tokens_.accessToken.c_str(), encoded, sizeof(encoded))) {
      char body[kOAuthTokenLength * 3 + 32];
      std::size_t used = 0;
      appendFormat(body, sizeof(body), used, "access_token=%s", encoded);
      JsonDocument response;
      int status = 0;
      result = postForm(kRevokePath, body, true, response, status);
    }
  }

  // The local deletion happens WHATEVER the network said.  An operator who
  // pressed "disconnect" has decided; a device that kept the tokens because a
  // request timed out would be disobeying them.
  store_.clearAll();
  tokens_ = CloudTokens{};
  state_ = DeviceCodeState::kIdle;
  return result;
}

}  // namespace platform
}  // namespace lc
