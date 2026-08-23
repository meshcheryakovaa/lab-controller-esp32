// =============================================================================
//  platform/esp32/YandexOAuthClient.h — linking an account without a browser
//  in the middle (M17).
//
//  WHY DEVICE CODE AND NOT AUTHORIZATION CODE.
//  Authorization Code needs a redirect back to the application, which means the
//  browser has to be part of the flow — and this controller has to keep
//  uploading with every browser closed, for days.  Device Code is designed for
//  exactly this shape: the controller asks for a short code, a person types it
//  on whatever device they have, and from then on the controller holds its own
//  tokens.
//
//  THE YANDEX PASSWORD IS NEVER TYPED INTO THIS INSTRUMENT.
//  That is the point of the flow and worth stating plainly, because the obvious
//  alternative — a login form on the controller's own page — would be asking an
//  operator to hand their account password to a device on a lab bench.
//
//  WHAT THIS CLASS WILL NOT DO.
//  It will not talk to anything but oauth.yandex.ru, it will not accept a
//  certificate it cannot verify, and it will not run before the clock is set:
//  without a real date, certificate validity cannot be judged at all, and the
//  tempting escape (setInsecure) would throw away the only thing protecting the
//  token in transit.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include "core/Clock.h"
#include "core/Error.h"
#include "platform/esp32/CloudCredentialStore.h"
#include "services/ICloudProvider.h"

namespace lc {
namespace platform {

/** Where the Device Code flow has got to. */
enum class DeviceCodeState : std::uint8_t {
  kIdle = 0,
  kRequestingCode,
  kWaitingUser,
  kAuthorized,
  kExpired,
  kFailed,
};

const char* toString(DeviceCodeState state);

struct DeviceCodePrompt {
  FixedString<48> userCode;
  FixedString<96> verificationUrl;
  std::uint32_t expiresInSeconds = 0;
  std::uint32_t pollIntervalSeconds = 5;
};

class YandexOAuthClient {
 public:
  static constexpr const char* kOAuthHost = "oauth.yandex.ru";
  static constexpr std::uint32_t kRequestTimeoutMs = 15000;
  /** Bounded because these responses are small and known.  An answer larger
   *  than this is not a token, and reading it would be a way to make the device
   *  allocate whatever a server felt like. */
  static constexpr std::size_t kMaxResponseBytes = 4096;

  YandexOAuthClient(const IClock& clock, CloudCredentialStore& store)
      : clock_(clock), store_(store) {}

  /** Asks Yandex for a user code.  Returns immediately — the waiting is done by
   *  poll(), from the worker task, never from an HTTP handler. */
  CloudResult beginDeviceCode();

  /** One poll of the token endpoint.  Called no more often than the interval
   *  Yandex asked for; "not yet authorised" is a normal answer and deliberately
   *  does not produce an event, or the log would fill with it. */
  CloudResult poll();

  DeviceCodeState deviceCodeState() const { return state_; }
  const DeviceCodePrompt& prompt() const { return prompt_; }
  std::uint32_t secondsRemaining() const;

  /** True when there is a usable access token that is not about to expire. */
  bool authorized() const;

  /** Renews if the token is missing, expiring or has just been refused.  At
   *  most one refresh at a time: a token that will not work is a reason to stop
   *  and say so, not to hammer the endpoint. */
  CloudResult refreshIfNeeded(bool force = false);

  const char* accessToken() const { return tokens_.accessToken.c_str(); }

  /** Best-effort revoke, then local deletion.  The local part happens even if
   *  the network part fails — an operator who pressed "disconnect" has decided,
   *  and a device that kept the tokens because a request timed out would be
   *  disobeying them. */
  CloudResult disconnect();

  void reload();

 private:
  CloudResult postForm(const char* path, const char* body, bool withBasicAuth,
                       JsonDocument& out, int& httpStatus);
  CloudResult applyTokenResponse(const JsonDocument& document);

  const IClock& clock_;
  CloudCredentialStore& store_;
  CloudTokens tokens_;

  DeviceCodeState state_ = DeviceCodeState::kIdle;
  DeviceCodePrompt prompt_;
  FixedString<96> deviceCode_;
  EpochMs deviceCodeExpiresAt_ = 0;
  EpochMs nextPollAt_ = 0;
  bool refreshing_ = false;
  Error lastError_;
};

}  // namespace platform
}  // namespace lc
