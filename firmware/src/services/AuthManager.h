// =============================================================================
//  services/AuthManager.h — who is allowed to change this instrument (§44,
//  ADR-0020).
//
//  WHAT THE PASSWORD IS FOR, AND WHAT IT IS NOT FOR
//  This is a box on a bench in a room with a door.  Anyone standing next to it
//  can unplug it, press its reset button, or flash it over USB.  A password
//  that claimed to defend against that person would be a lie told to whoever
//  installed it.
//
//  What it actually defends against is real and worth defending against:
//    * the accident — a browser tab left open on a shared screen, somebody
//      dragging a setpoint while an experiment is running;
//    * the stranger on the same Wi-Fi who opens the page out of curiosity;
//    * the well-meaning colleague who imports their own configuration over a
//      run that is six hours in.
//
//  THE EMERGENCY STOP NEVER ASKS FOR A PASSWORD.
//  §49 puts Safety above everything, and a person reaching for "stop all
//  outputs" is not going to type a password first.  Nor should they: an
//  unauthenticated stop cannot make a rig less safe — the safe state is safe by
//  definition.  Stopping a run and releasing an output are exempt for the same
//  reason.  CLEARING a stop is not: that is an arming action.
//
//  DANGEROUS IS NOT THE SAME AS "A WRITE".
//  A session is enough to change a setpoint.  It is not enough to remove an
//  interlock, import a configuration over a running rig, replace the firmware
//  or change the password: those require the password again, in the request,
//  whatever the session says.  The rule is that anything which REMOVES A
//  PROTECTION is confirmed by the person, not by their browser.
//
//  OUT OF THE BOX IT IS OPEN, AND IT SAYS SO.
//  A device that ships with a default password is a device with no password.
//  A device that refuses to work until one is set is a device that cannot be
//  used at 2 a.m. by the person who just unpacked it.  So: no credential means
//  no restrictions, `GET /system` says `auth.configured = false`, and the web
//  interface carries a banner that does not go away until somebody decides.
// =============================================================================
#pragma once

#include "core/Clock.h"
#include "core/Error.h"
#include "core/EventBus.h"
#include "core/IRandom.h"
#include "core/Sha256.h"
#include "core/Types.h"
#include "storage/IStorageBackend.h"

namespace lc {

// Long enough that guessing is hopeless, short enough for a cookie.
using SessionToken = FixedString<33>;

struct AuthState {
  bool configured = false;
  std::size_t sessions = 0;
  std::uint8_t failures = 0;
  Micros lockedUntilUs = 0;
  std::uint32_t logins = 0;
  std::uint32_t rejections = 0;
};

class AuthManager {
 public:
  // Where the credential lives.  NOT a ConfigSection, and that is the point:
  // the export walks the sections, so a hash that is not one cannot end up in
  // an exported file by anybody's oversight (§44).
  static constexpr const char* kPath = "/config/auth.json";
  static constexpr std::size_t kMaxSessions = 4;
  static constexpr std::uint32_t kIterations = 20000;
  static constexpr std::size_t kSaltBytes = 16;
  static constexpr Micros kSessionLifetimeUs = 12ULL * 3600ULL * 1000000ULL;
  // Five wrong answers and the door stops opening for a while.  A four-digit
  // password on an open network is otherwise a five-second problem.
  static constexpr std::uint8_t kMaxFailures = 5;
  static constexpr Micros kLockoutUs = 60ULL * 1000000ULL;
  static constexpr std::size_t kMinPasswordLength = 8;

  // `iterations` is a cost knob, not a secret: it is stored beside the hash, so
  // raising it later leaves existing credentials working.  Tests lower it so
  // that a suite which signs in fifty times does not spend its life in PBKDF2 —
  // the algorithm under test is the same one either way.
  AuthManager(const IClock& clock, IRandom& random, IStorageBackend& backend,
              EventBus& events, std::uint32_t iterations = kIterations)
      : clock_(clock), random_(random), backend_(backend), events_(events),
        iterations_(iterations), configuredIterations_(iterations) {}

  Status begin();

  bool configured() const { return configured_; }
  AuthState state() const;

  // Sets or changes the password.  When one is already configured, `current`
  // must match — a session is not enough to replace the credential that
  // protects the instrument.
  Status setPassword(const char* current, const char* next);

  // Verifies a password without creating a session.  This is what the
  // "confirm with your password" path uses for dangerous actions.
  bool verify(const char* password);

  // `evicted` is set when signing in had to take somebody else's slot.  It is
  // reported rather than hidden: a browser that stops working deserves an
  // explanation, and an instrument that refuses the only person holding the
  // password because four stale tabs exist is worse than either.
  Result<SessionToken> login(const char* password, bool* evicted = nullptr);
  void logout(const char* token);
  void logoutAll();

  // True when the token names a live session.  Extends nothing: a session has
  // a fixed lifetime, so a browser left open in an empty room stops being a
  // key at some point without anybody having to remember.
  bool validate(const char* token) const;

  // Reads the session token out of a Cookie header.  Empty when absent.
  static void tokenFromCookie(const char* cookieHeader, SessionToken& out);
  static const char* cookieName() { return "lc_session"; }

 private:
  struct Session {
    SessionToken token;
    Micros expiresAtUs = 0;
    bool active = false;
  };

  Status save();
  void publish(const char* detail, std::uint8_t severity);

  const IClock& clock_;
  IRandom& random_;
  IStorageBackend& backend_;
  EventBus& events_;

  bool configured_ = false;
  std::uint32_t iterations_ = kIterations;
  std::uint32_t configuredIterations_ = kIterations;
  std::uint8_t salt_[kSaltBytes] = {};
  std::uint8_t hash_[Sha256::kDigestBytes] = {};

  Session sessions_[kMaxSessions];
  std::uint8_t failures_ = 0;
  Micros lockedUntilUs_ = 0;
  std::uint32_t logins_ = 0;
  std::uint32_t rejections_ = 0;
};

}  // namespace lc
