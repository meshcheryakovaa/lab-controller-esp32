// =============================================================================
//  platform/esp32/YandexAccount.h — ICloudAccount, wired to the real OAuth
//  client (M17).
//
//  A thin adapter, and deliberately thin: it exists so RestApi can talk about
//  linking an account without including WiFiClientSecure.h.  Everything it does
//  is delegate — the judgement lives in YandexOAuthClient, and the policy
//  ("never return a secret") lives in the interface it implements, which simply
//  has no method that could.
// =============================================================================
#pragma once

#include "platform/esp32/CloudCredentialStore.h"
#include "platform/esp32/YandexDiskClient.h"
#include "platform/esp32/YandexOAuthClient.h"
#include "services/CloudManager.h"
#include "services/ICloudProvider.h"

namespace lc {
namespace platform {

class YandexAccount final : public ICloudAccount {
 public:
  YandexAccount(CloudCredentialStore& store, YandexOAuthClient& oauth,
                YandexDiskClient& disk, CloudManager& manager)
      : store_(store), oauth_(oauth), disk_(disk), manager_(manager) {}

  bool configured() const override { return store_.configured(); }
  bool clientSecretSet() const override { return store_.clientSecretSet(); }
  bool authorized() const override { return oauth_.authorized(); }

  EpochMs tokenExpiresAtEpochMs() const override {
    CloudTokens tokens;
    if (!store_.loadTokens(tokens)) return 0;
    return tokens.expiresAtEpochMs;
  }

  Status setClientId(const char* clientId) override {
    return store_.setClientId(clientId);
  }
  Status setClientSecret(const char* secret) override {
    return store_.setClientSecret(secret);
  }
  Status clearClientSecret() override { return store_.clearAll(); }

  CloudResult beginLink() override { return oauth_.beginDeviceCode(); }

  CloudLinkState linkState() const override {
    switch (oauth_.deviceCodeState()) {
      case DeviceCodeState::kRequestingCode: return CloudLinkState::kRequestingCode;
      case DeviceCodeState::kWaitingUser:    return CloudLinkState::kWaitingUser;
      case DeviceCodeState::kAuthorized:     return CloudLinkState::kAuthorized;
      case DeviceCodeState::kExpired:        return CloudLinkState::kExpired;
      case DeviceCodeState::kFailed:         return CloudLinkState::kFailed;
      default:                               return CloudLinkState::kIdle;
    }
  }

  CloudLinkPrompt linkPrompt() const override {
    CloudLinkPrompt out;
    out.userCode = oauth_.prompt().userCode;
    out.verificationUrl = oauth_.prompt().verificationUrl;
    out.secondsRemaining = oauth_.secondsRemaining();
    return out;
  }

  CloudResult checkAccess() override {
    const CloudResult refreshed = oauth_.refreshIfNeeded();
    if (!refreshed.ok()) return refreshed;
    std::uint64_t total = 0;
    std::uint64_t used = 0;
    const CloudResult read = disk_.checkAccess(total, used);
    if (!read.ok()) return read;
    // Creates the folder if it is missing, and writes no test file: proving a
    // connection by leaving litter in somebody's Disk is a poor trade.
    return disk_.ensureDirectory(manager_.root().c_str());
  }

  CloudResult disconnect() override { return oauth_.disconnect(); }

  bool storageIsEncrypted() const override {
    // A stock ESP32 has no NVS encryption.  Reported honestly so the interface
    // can warn rather than imply a protection that is not there (ADR-0023).
#if defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION
    return true;
#else
    return false;
#endif
  }

 private:
  CloudCredentialStore& store_;
  YandexOAuthClient& oauth_;
  YandexDiskClient& disk_;
  CloudManager& manager_;
};

}  // namespace platform
}  // namespace lc
