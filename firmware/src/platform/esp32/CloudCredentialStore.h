// =============================================================================
//  platform/esp32/CloudCredentialStore.h — the cloud secrets, in NVS (M17).
//
//  Four things live here: the OAuth client id, the client secret, the token
//  pair, and a device id that identifies this controller to the Device Code
//  flow.  None of them ever leaves through an API, an event, a log line, a
//  configuration export or a manifest.
//
//  TOKENS ARE SAVED AS ONE RECORD, NEVER FIELD BY FIELD.
//  An access token and the refresh token that renews it are only meaningful
//  together.  Writing them as separate NVS keys means a power cut between the
//  two writes leaves a NEW access token beside an OLD refresh token — a pair
//  that works until the access token expires and then cannot be renewed, which
//  is the worst kind of failure: delayed, silent, and needing the operator to
//  re-link an account they were never told had broken.  So the pair is
//  serialised into one blob with a generation counter and a CRC, and the whole
//  record is replaced atomically or not at all.
//
//  A KNOWN LIMIT, STATED RATHER THAN HIDDEN.
//  A stock ESP32 without Flash Encryption and NVS Encryption is not secure
//  storage against someone holding the board.  Anyone who can read the flash can
//  read these tokens.  That is acceptable for a bench instrument in a lab and
//  NOT acceptable for a device that leaves one, so the interface says so and
//  ADR-0023 records the decision.  The mitigation is a Yandex OAuth app scoped
//  to Disk alone: the worst case is somebody else's lab CSVs, not an account.
// =============================================================================
#pragma once

#include <Preferences.h>

#include "core/Error.h"
#include "core/Types.h"

namespace lc {
namespace platform {

/** Sizes taken from what Yandex actually issues, with room to spare. */
inline constexpr std::size_t kOAuthTokenLength = 256;
inline constexpr std::size_t kOAuthClientIdLength = 64;
inline constexpr std::size_t kOAuthSecretLength = 64;
inline constexpr std::size_t kOAuthScopeLength = 96;

struct CloudTokens {
  FixedString<kOAuthTokenLength> accessToken;
  FixedString<kOAuthTokenLength> refreshToken;
  FixedString<kOAuthScopeLength> scope;
  EpochMs expiresAtEpochMs = 0;
  std::uint32_t generation = 0;

  bool valid() const { return !accessToken.empty(); }
};

class CloudCredentialStore {
 public:
  static constexpr const char* kNamespace = "lc-cloud";
  /** Renew this long before expiry rather than after a 401: a refresh that
   *  happens while the old token still works costs one request, and one that
   *  happens after it stopped costs a failed upload first. */
  static constexpr EpochMs kRenewBeforeMs = 10ull * 60ull * 1000ull;

  Status begin();

  bool configured() const;
  bool clientSecretSet() const;

  Status setClientId(const char* clientId);
  bool clientId(FixedString<kOAuthClientIdLength>& out) const;

  Status setClientSecret(const char* secret);
  bool clientSecret(FixedString<kOAuthSecretLength>& out) const;

  /** Replaces the whole token record.  There is deliberately no way to write
   *  half of one. */
  Status saveTokens(const CloudTokens& tokens);
  bool loadTokens(CloudTokens& out) const;
  Status clearTokens();

  /** Removes everything, including the client secret.  Used by "disconnect". */
  Status clearAll();

  /** A stable id for this controller in the Device Code flow.  Generated once
   *  and kept, so re-authorising does not look like a different device. */
  bool deviceId(FixedString<40>& out);

  Status setRootPath(const char* path);
  bool rootPath(FixedString<160>& out) const;
  Status setEnabled(bool enabled);
  bool enabled() const;

 private:
  bool open() const;
  mutable Preferences preferences_;
};

}  // namespace platform
}  // namespace lc
