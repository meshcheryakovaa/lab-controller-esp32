#include "services/AuthManager.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <new>

namespace lc {

Status AuthManager::begin() {
  if (!backend_.exists(kPath)) {
    // No credential.  Not an error, and not a reason to refuse to work: see the
    // header.  The API says `configured: false` and the interface says it
    // louder.
    configured_ = false;
    return ok();
  }

  const Result<std::size_t> size = backend_.size(kPath);
  if (!size.ok() || size.value() > 512) {
    return fail(ErrorCode::kConfigCorrupt, kPath);
  }
  char buffer[513];
  const Result<std::size_t> read = backend_.read(kPath, buffer, sizeof(buffer));
  if (!read.ok()) return read.error();

  JsonDocument document;
  if (deserializeJson(document, static_cast<const char*>(buffer), read.value())) {
    return fail(ErrorCode::kConfigCorrupt, kPath);
  }

  iterations_ = document["iterations"] | configuredIterations_;
  const std::size_t saltBytes =
      fromHex(document["salt"] | "", salt_, sizeof(salt_));
  const std::size_t hashBytes =
      fromHex(document["hash"] | "", hash_, sizeof(hash_));
  if (saltBytes != kSaltBytes || hashBytes != Sha256::kDigestBytes ||
      iterations_ == 0) {
    // A credential file that cannot be parsed leaves the instrument OPEN rather
    // than locked: a lab controller nobody can log into is a lab controller
    // somebody has to take apart, and the alternative failure — an unlocked
    // instrument that says it is unlocked — is the recoverable one.
    configured_ = false;
    return fail(ErrorCode::kConfigCorrupt, "the credential file is unreadable");
  }
  configured_ = true;
  return ok();
}

Status AuthManager::save() {
  char saltHex[kSaltBytes * 2 + 1];
  char hashHex[Sha256::kDigestBytes * 2 + 1];
  toHex(salt_, sizeof(salt_), saltHex, sizeof(saltHex));
  toHex(hash_, sizeof(hash_), hashHex, sizeof(hashHex));

  JsonDocument document;
  document["schemaVersion"] = LC_CONFIG_SCHEMA_VERSION;
  document["iterations"] = iterations_;
  document["salt"] = saltHex;
  document["hash"] = hashHex;

  char buffer[512];
  const std::size_t written = serializeJson(document, buffer, sizeof(buffer));
  if (written == 0) return fail(ErrorCode::kInternal, "credential too large");
  return backend_.writeAtomic(kPath, buffer, written);
}

void AuthManager::publish(const char* detail, std::uint8_t severity) {
  Event event;
  event.type = EventType::kSystemMessage;
  event.severity = severity;
  event.detail = detail;  // static lifetime only
  event.timestamp = clock_.nowMicros();
  events_.publish(event);
}

AuthState AuthManager::state() const {
  AuthState state;
  state.configured = configured_;
  state.failures = failures_;
  state.lockedUntilUs = lockedUntilUs_;
  state.logins = logins_;
  state.rejections = rejections_;
  const Micros now = clock_.nowMicros();
  for (const Session& session : sessions_) {
    if (session.active && session.expiresAtUs > now) ++state.sessions;
  }
  return state;
}

Status AuthManager::setPassword(const char* current, const char* next) {
  if (next == nullptr || std::strlen(next) < kMinPasswordLength) {
    return fail(ErrorCode::kInvalidArgument,
                "a password needs at least 8 characters");
  }
  if (configured_ && !verify(current)) {
    // The current password, not the session.  A tab left open must not be
    // enough to replace the credential that the tab was authorised by.
    return fail(ErrorCode::kUnauthorized, "the current password does not match");
  }

  random_.fill(salt_, sizeof(salt_));
  iterations_ = configuredIterations_;
  pbkdf2Sha256(next, salt_, sizeof(salt_), iterations_, hash_, sizeof(hash_));

  const Status saved = save();
  if (!saved.ok()) return saved;
  configured_ = true;

  // Changing the password ends every session, including the one that changed
  // it.  If the reason for the change is "somebody else knows the old one",
  // leaving their browser logged in defeats the exercise.
  logoutAll();
  failures_ = 0;
  lockedUntilUs_ = 0;
  publish("the password was changed; all sessions ended", 2);
  return ok();
}

bool AuthManager::verify(const char* password) {
  if (!configured_) return true;   // nothing to verify against
  if (password == nullptr) return false;

  const Micros now = clock_.nowMicros();
  if (now < lockedUntilUs_) return false;

  std::uint8_t candidate[Sha256::kDigestBytes];
  pbkdf2Sha256(password, salt_, sizeof(salt_), iterations_, candidate,
               sizeof(candidate));
  const bool matches = equalsConstantTime(candidate, hash_, sizeof(candidate));

  if (matches) {
    failures_ = 0;
    lockedUntilUs_ = 0;
    return true;
  }

  ++rejections_;
  if (failures_ < 255) ++failures_;
  if (failures_ >= kMaxFailures) {
    // The lock is global rather than per client, because this API has no
    // reliable notion of one.  That is a denial of service an attacker can
    // trigger — and it is acceptable precisely because the emergency stop does
    // not go through here: the worst they can do is make an operator wait a
    // minute before changing a setpoint.
    lockedUntilUs_ = now + kLockoutUs;
    failures_ = 0;
    publish("too many failed sign-ins; sign-in is locked for a minute", 3);
  }
  return false;
}

Result<SessionToken> AuthManager::login(const char* password, bool* evicted) {
  if (evicted != nullptr) *evicted = false;
  const Micros now = clock_.nowMicros();
  if (now < lockedUntilUs_) {
    return fail(ErrorCode::kRateLimited, "too many attempts; wait a moment");
  }
  if (!verify(password)) {
    return fail(ErrorCode::kUnauthorized, "wrong password");
  }

  Session* slot = nullptr;
  for (Session& session : sessions_) {
    if (!session.active || session.expiresAtUs <= now) {
      slot = &session;
      break;
    }
  }
  if (slot == nullptr) {
    // The table is full of live sessions.  The oldest one goes: refusing the
    // person who is holding the password because four tabs were left open
    // somewhere is how an instrument becomes unusable without anything being
    // broken.  The eviction is REPORTED, so the browser that loses its session
    // can say why rather than start failing mysteriously.
    slot = &sessions_[0];
    for (Session& session : sessions_) {
      if (session.expiresAtUs < slot->expiresAtUs) slot = &session;
    }
    if (evicted != nullptr) *evicted = true;
    publish("a session was signed out to make room for a new one", 2);
  }

  std::uint8_t raw[16];
  random_.fill(raw, sizeof(raw));
  char hex[sizeof(raw) * 2 + 1];
  toHex(raw, sizeof(raw), hex, sizeof(hex));
  slot->token.assign(hex);
  slot->expiresAtUs = now + kSessionLifetimeUs;
  slot->active = true;
  ++logins_;
  return slot->token;
}

void AuthManager::logout(const char* token) {
  if (token == nullptr) return;
  for (Session& session : sessions_) {
    if (session.active && session.token.equals(token)) {
      session.active = false;
      session.token.assign("");
      return;
    }
  }
}

void AuthManager::logoutAll() {
  for (Session& session : sessions_) {
    session.active = false;
    session.token.assign("");
  }
}

bool AuthManager::validate(const char* token) const {
  if (token == nullptr || token[0] == '\0') return false;
  const std::size_t length = std::strlen(token);
  const Micros now = clock_.nowMicros();
  for (const Session& session : sessions_) {
    if (!session.active || session.expiresAtUs <= now) continue;
    // The LENGTH is not a secret — every token this firmware issues is the same
    // length — so comparing it first leaks nothing.  The bytes are compared
    // without an early exit.
    if (session.token.size() != length) continue;
    if (equalsConstantTime(
            reinterpret_cast<const std::uint8_t*>(session.token.c_str()),
            reinterpret_cast<const std::uint8_t*>(token), length)) {
      return true;
    }
  }
  return false;
}

void AuthManager::tokenFromCookie(const char* cookieHeader, SessionToken& out) {
  out.assign("");
  if (cookieHeader == nullptr) return;

  const char* name = cookieName();
  const std::size_t nameLength = std::strlen(name);
  for (const char* p = cookieHeader; *p != '\0'; ++p) {
    if (std::strncmp(p, name, nameLength) != 0) continue;
    const char* after = p + nameLength;
    while (*after == ' ') ++after;
    if (*after != '=') continue;
    ++after;
    char buffer[SessionToken::capacity() + 1];
    std::size_t written = 0;
    while (*after != '\0' && *after != ';' && *after != ' ' &&
           written < sizeof(buffer) - 1) {
      buffer[written++] = *after++;
    }
    buffer[written] = '\0';
    out.assign(buffer);
    return;
  }
}

}  // namespace lc
