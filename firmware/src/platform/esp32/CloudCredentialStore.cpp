#include "platform/esp32/CloudCredentialStore.h"

#include <Arduino.h>
#include <esp_random.h>

#include <cstring>

#include "core/Crc32.h"
#include "core/Format.h"

namespace lc {
namespace platform {
namespace {

// NVS keys are capped at 15 characters, hence the abbreviations.
constexpr const char* kKeyClientId = "cid";
constexpr const char* kKeySecret   = "secret";
constexpr const char* kKeyToken    = "token";
constexpr const char* kKeyDeviceId = "devid";
constexpr const char* kKeyRoot     = "root";
constexpr const char* kKeyEnabled  = "on";

constexpr std::uint16_t kTokenSchemaVersion = 1;

/**
 * The token blob.
 *
 * Length-prefixed strings rather than a struct of fixed arrays, so the record
 * stays the size of what it holds; a CRC over everything before it, so a
 * half-written or bit-rotted record is REJECTED rather than used.  A token that
 * has silently lost a character fails at the worst possible moment — mid-upload,
 * with no obvious cause — and this is what turns that into "not authorised".
 */
struct TokenHeader {
  std::uint16_t schemaVersion;
  std::uint32_t generation;
  std::uint64_t expiresAtEpochMs;
  std::uint16_t accessLength;
  std::uint16_t refreshLength;
  std::uint16_t scopeLength;
};

constexpr std::size_t kTokenBlobMax =
    sizeof(TokenHeader) + kOAuthTokenLength * 2 + kOAuthScopeLength + sizeof(std::uint32_t);

}  // namespace

bool CloudCredentialStore::open() const {
  // Read/write even for reads: opening a namespace that has never been written
  // read-only fails with NOT_FOUND, and the first boot would log an error
  // describing a completely normal state.  Same lesson as lc-wifi in M12.
  return preferences_.begin(kNamespace, /*readOnly=*/false);
}

Status CloudCredentialStore::begin() {
  if (!open()) return fail(ErrorCode::kStorageFailure, "NVS is not available");
  preferences_.end();
  return ok();
}

bool CloudCredentialStore::configured() const {
  if (!open()) return false;
  const bool present = preferences_.isKey(kKeyClientId);
  preferences_.end();
  return present;
}

bool CloudCredentialStore::clientSecretSet() const {
  if (!open()) return false;
  const bool present = preferences_.isKey(kKeySecret);
  preferences_.end();
  return present;
}

Status CloudCredentialStore::setClientId(const char* clientId) {
  if (clientId == nullptr || clientId[0] == '\0') {
    return fail(ErrorCode::kInvalidArgument, "the client id is empty");
  }
  if (std::strlen(clientId) >= kOAuthClientIdLength) {
    return fail(ErrorCode::kPayloadTooLarge, "the client id is too long");
  }
  if (!open()) return fail(ErrorCode::kStorageFailure, "NVS is not available");
  preferences_.putString(kKeyClientId, clientId);
  preferences_.end();
  return ok();
}

bool CloudCredentialStore::clientId(FixedString<kOAuthClientIdLength>& out) const {
  if (!open()) return false;
  const String value = preferences_.getString(kKeyClientId, "");
  preferences_.end();
  if (value.length() == 0) return false;
  return out.assign(value.c_str());
}

Status CloudCredentialStore::setClientSecret(const char* secret) {
  if (secret == nullptr || secret[0] == '\0') {
    return fail(ErrorCode::kInvalidArgument, "the client secret is empty");
  }
  if (std::strlen(secret) >= kOAuthSecretLength) {
    return fail(ErrorCode::kPayloadTooLarge, "the client secret is too long");
  }
  if (!open()) return fail(ErrorCode::kStorageFailure, "NVS is not available");
  preferences_.putString(kKeySecret, secret);
  preferences_.end();
  return ok();
}

bool CloudCredentialStore::clientSecret(FixedString<kOAuthSecretLength>& out) const {
  if (!open()) return false;
  const String value = preferences_.getString(kKeySecret, "");
  preferences_.end();
  if (value.length() == 0) return false;
  return out.assign(value.c_str());
}

Status CloudCredentialStore::saveTokens(const CloudTokens& tokens) {
  std::uint8_t blob[kTokenBlobMax];
  TokenHeader header{};
  header.schemaVersion = kTokenSchemaVersion;
  header.generation = tokens.generation;
  header.expiresAtEpochMs = tokens.expiresAtEpochMs;
  header.accessLength = static_cast<std::uint16_t>(tokens.accessToken.size());
  header.refreshLength = static_cast<std::uint16_t>(tokens.refreshToken.size());
  header.scopeLength = static_cast<std::uint16_t>(tokens.scope.size());

  std::size_t used = 0;
  std::memcpy(blob + used, &header, sizeof(header));
  used += sizeof(header);
  std::memcpy(blob + used, tokens.accessToken.c_str(), header.accessLength);
  used += header.accessLength;
  std::memcpy(blob + used, tokens.refreshToken.c_str(), header.refreshLength);
  used += header.refreshLength;
  std::memcpy(blob + used, tokens.scope.c_str(), header.scopeLength);
  used += header.scopeLength;

  const std::uint32_t crc = crc32(reinterpret_cast<const char*>(blob), used);
  std::memcpy(blob + used, &crc, sizeof(crc));
  used += sizeof(crc);

  if (!open()) return fail(ErrorCode::kStorageFailure, "NVS is not available");
  // One putBytes.  The pair is replaced or it is not; there is no state in
  // which a new access token sits beside an old refresh token.
  const std::size_t written = preferences_.putBytes(kKeyToken, blob, used);
  preferences_.end();
  if (written != used) {
    return fail(ErrorCode::kStorageFailure, "the tokens could not be saved");
  }
  return ok();
}

bool CloudCredentialStore::loadTokens(CloudTokens& out) const {
  if (!open()) return false;
  std::uint8_t blob[kTokenBlobMax];
  const std::size_t read = preferences_.getBytes(kKeyToken, blob, sizeof(blob));
  preferences_.end();
  if (read < sizeof(TokenHeader) + sizeof(std::uint32_t)) return false;

  TokenHeader header{};
  std::memcpy(&header, blob, sizeof(header));
  if (header.schemaVersion != kTokenSchemaVersion) return false;

  const std::size_t payload = sizeof(header) + header.accessLength +
                              header.refreshLength + header.scopeLength;
  if (payload + sizeof(std::uint32_t) != read) return false;
  if (header.accessLength >= kOAuthTokenLength ||
      header.refreshLength >= kOAuthTokenLength ||
      header.scopeLength >= kOAuthScopeLength) {
    return false;
  }

  std::uint32_t stored = 0;
  std::memcpy(&stored, blob + payload, sizeof(stored));
  if (stored != crc32(reinterpret_cast<const char*>(blob), payload)) {
    // A record that does not check out is treated as no record at all.  Using
    // half a token pair fails later, mid-upload, with no obvious cause.
    return false;
  }

  char text[kOAuthTokenLength];
  std::size_t offset = sizeof(header);
  std::memcpy(text, blob + offset, header.accessLength);
  text[header.accessLength] = '\0';
  out.accessToken.assign(text);
  offset += header.accessLength;

  std::memcpy(text, blob + offset, header.refreshLength);
  text[header.refreshLength] = '\0';
  out.refreshToken.assign(text);
  offset += header.refreshLength;

  char scope[kOAuthScopeLength];
  std::memcpy(scope, blob + offset, header.scopeLength);
  scope[header.scopeLength] = '\0';
  out.scope.assign(scope);

  out.expiresAtEpochMs = header.expiresAtEpochMs;
  out.generation = header.generation;
  return true;
}

Status CloudCredentialStore::clearTokens() {
  if (!open()) return fail(ErrorCode::kStorageFailure, "NVS is not available");
  preferences_.remove(kKeyToken);
  preferences_.end();
  return ok();
}

Status CloudCredentialStore::clearAll() {
  if (!open()) return fail(ErrorCode::kStorageFailure, "NVS is not available");
  preferences_.remove(kKeyToken);
  preferences_.remove(kKeySecret);
  preferences_.remove(kKeyClientId);
  // The device id and the root path are kept: neither is a secret, and keeping
  // the device id means re-linking later looks like the same controller rather
  // than a new one in the account's device list.
  preferences_.end();
  return ok();
}

bool CloudCredentialStore::deviceId(FixedString<40>& out) {
  if (!open()) return false;
  String value = preferences_.getString(kKeyDeviceId, "");
  if (value.length() == 0) {
    char generated[40];
    std::size_t used = 0;
    appendFormat(generated, sizeof(generated), used, "lc-%08x%08x",
                 static_cast<unsigned>(esp_random()),
                 static_cast<unsigned>(esp_random()));
    preferences_.putString(kKeyDeviceId, generated);
    value = String(generated);
  }
  preferences_.end();
  return out.assign(value.c_str());
}

Status CloudCredentialStore::setRootPath(const char* path) {
  if (!open()) return fail(ErrorCode::kStorageFailure, "NVS is not available");
  preferences_.putString(kKeyRoot, path != nullptr ? path : "");
  preferences_.end();
  return ok();
}

bool CloudCredentialStore::rootPath(FixedString<160>& out) const {
  if (!open()) return false;
  const String value = preferences_.getString(kKeyRoot, "");
  preferences_.end();
  if (value.length() == 0) return false;
  return out.assign(value.c_str());
}

Status CloudCredentialStore::setEnabled(bool enabled) {
  if (!open()) return fail(ErrorCode::kStorageFailure, "NVS is not available");
  preferences_.putBool(kKeyEnabled, enabled);
  preferences_.end();
  return ok();
}

bool CloudCredentialStore::enabled() const {
  if (!open()) return false;
  const bool value = preferences_.getBool(kKeyEnabled, false);
  preferences_.end();
  return value;
}

}  // namespace platform
}  // namespace lc
