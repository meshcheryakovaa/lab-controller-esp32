// =============================================================================
//  services/ICloudProvider.h — what the uploader is allowed to know about a
//  cloud (M17).
//
//  THE ONE RULE OF THIS MILESTONE:
//
//      A local file may be deleted only after the REMOTE object has been read
//      back and found to match.
//
//  Not "after the upload returned 200".  A connection can drop after the server
//  has stored the file and before the answer arrives, and it can equally drop
//  after a partial body — the two look identical from here.  So `stat()` exists
//  as a first-class operation and every acknowledgement goes through it.
//
//  WHY AN INTERFACE.
//  Everything worth testing about the uploader is a decision, not a protocol:
//  when to retry, when to give up, what to do when the remote file already
//  exists with a different hash, whether an unknown upload host is acceptable.
//  Behind this contract those are host tests with a fake provider.  Behind
//  WiFiClientSecure they would need a board, an account and the internet, and
//  would therefore never be run.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/Error.h"
#include "core/Md5.h"
#include "core/Types.h"
#include "storage/IStorageBackend.h"

namespace lc {

/** What a remote path holds, as far as it can be checked. */
struct CloudObjectInfo {
  bool exists = false;
  bool isFile = false;
  std::uint64_t size = 0;
  FixedString<Md5::kTextBytes> md5;
};

/** Why a cloud operation failed, in the only terms the state machine needs.
 *  The distinction that matters is RETRY vs GIVE UP vs ASK THE OPERATOR — an
 *  HTTP status code is the wrong vocabulary for that decision. */
enum class CloudFailure : std::uint8_t {
  kNone = 0,
  kNoNetwork,       // not a failure, and not counted as an attempt
  kTransient,       // DNS, TLS, timeout, 5xx — retry with backoff
  kRateLimited,     // 429 — retry, honouring Retry-After
  kUnauthorized,    // 401 — one refresh, then retry
  kAuthRevoked,     // invalid_grant — needs the operator to re-link
  kQuotaExceeded,   // permanent until somebody frees space
  kUntrustedHost,   // an upload URL outside the provider's own domains
  kPermanent,       // anything else that will not get better by itself
};

const char* toString(CloudFailure failure);

/** Both the code and enough context to act on it, which a bare Status is not
 *  quite enough for: "retry in a minute" and "stop and tell somebody" can carry
 *  the same ErrorCode. */
struct CloudResult {
  Status status;
  CloudFailure failure = CloudFailure::kNone;
  /** From a 429's Retry-After, in milliseconds.  0 when the server did not
   *  say, in which case the ordinary backoff applies. */
  std::uint32_t retryAfterMs = 0;

  bool ok() const { return status.ok(); }
};

inline CloudResult cloudOk() { return CloudResult{ok(), CloudFailure::kNone, 0}; }

inline CloudResult cloudFail(CloudFailure failure, ErrorCode code,
                             const char* detail) {
  return CloudResult{fail(code, detail), failure, 0};
}

/** Reports how far an upload has got, so the interface can show a bar and the
 *  worker can notice a stalled transfer. */
class ICloudUploadObserver {
 public:
  virtual ~ICloudUploadObserver() = default;
  virtual void onUploadProgress(std::uint64_t sent, std::uint64_t total) = 0;
};

class ICloudProvider {
 public:
  virtual ~ICloudProvider() = default;

  virtual const char* name() const = 0;
  virtual bool authorized() const = 0;

  /** Refreshes the access token if it is missing or close to expiry.  Called
   *  before an upload rather than on a timer, so a controller that uploads once
   *  a week does not have to have kept a token warm. */
  virtual CloudResult refreshAuthorizationIfNeeded() = 0;

  /** Creates a directory and every missing parent.  "Already there" is success
   *  — but only once the existing resource has been confirmed to be a
   *  directory, because a FILE at that path would fail every later step in a
   *  much more confusing way. */
  virtual CloudResult ensureDirectory(const char* path) = 0;

  /** What is at `path`.  A missing object is a successful answer with
   *  `exists == false`, not an error: "is it already there?" is a question the
   *  idempotency rules ask on purpose. */
  virtual CloudResult stat(const char* path, CloudObjectInfo& out) = 0;

  /**
   * Streams a local file to a remote path.
   *
   * Takes the backend and a path rather than a buffer, and that is the whole
   * point: there is no signature here through which a caller could hand over a
   * 100 KiB file in memory.  The implementation reads it in fixed-size blocks.
   */
  virtual CloudResult upload(const char* remotePath, IStorageBackend& storage,
                             const char* localPath, std::uint64_t bytes,
                             ICloudUploadObserver* observer) = 0;

  virtual CloudResult move(const char* from, const char* to) = 0;

  /** Deletes a remote object.  Used only to clear a `.uploading` leftover that
   *  this controller itself wrote and has proved does not match. */
  virtual CloudResult remove(const char* path) = 0;
};


/** Where linking an account has got to, mirrored from the OAuth client so the
 *  API layer does not have to know what OAuth is. */
enum class CloudLinkState : std::uint8_t {
  kIdle = 0,
  kRequestingCode,
  kWaitingUser,
  kAuthorized,
  kExpired,
  kFailed,
};

const char* toString(CloudLinkState state);

/** What the operator is shown during Device Code.  A short code and a URL —
 *  and never a Yandex password field, which is the entire reason this flow was
 *  chosen over one with a login form on the instrument. */
struct CloudLinkPrompt {
  FixedString<48> userCode;
  FixedString<96> verificationUrl;
  std::uint32_t secondsRemaining = 0;
};

/**
 * Linking, configuring and disconnecting a cloud account.
 *
 * Deliberately separate from ICloudProvider: that one moves files, this one
 * holds credentials.  Keeping them apart means the REST routes that touch
 * secrets are a visibly different set from the ones that report progress, and
 * the "no secret ever comes back" rule has one place to live.
 *
 * Note what is NOT here: any way to READ the client secret or a token.  The
 * interface cannot return what it has no method to return.
 */
class ICloudAccount {
 public:
  virtual ~ICloudAccount() = default;

  virtual bool configured() const = 0;      // a client id is stored
  virtual bool clientSecretSet() const = 0;
  virtual bool authorized() const = 0;
  virtual EpochMs tokenExpiresAtEpochMs() const = 0;

  virtual Status setClientId(const char* clientId) = 0;
  virtual Status setClientSecret(const char* secret) = 0;
  virtual Status clearClientSecret() = 0;

  /** Starts Device Code.  Returns as soon as the code is in hand; the waiting
   *  is done by the worker task, never by an HTTP handler. */
  virtual CloudResult beginLink() = 0;
  virtual CloudLinkState linkState() const = 0;
  virtual CloudLinkPrompt linkPrompt() const = 0;

  /** Reads the account, creating the root folder if it is missing.  Writes no
   *  test file: proving a connection by leaving litter in somebody's Disk is a
   *  poor trade. */
  virtual CloudResult checkAccess() = 0;

  /** Revokes remotely if it can, and deletes locally whatever happens. */
  virtual CloudResult disconnect() = 0;

  /** True when the flash holding these tokens is not encrypted, so the
   *  interface can say so.  A stock ESP32 is not secure storage against
   *  somebody holding the board, and pretending otherwise would be worse than
   *  the limitation itself. */
  virtual bool storageIsEncrypted() const = 0;
};

}  // namespace lc
