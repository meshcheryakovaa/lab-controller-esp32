#include "services/INetworkManager.h"

#include <cstring>

namespace lc {

const char* toString(NetworkState state) {
  switch (state) {
    case NetworkState::kOff:               return "OFF";
    case NetworkState::kApOnly:            return "AP_ONLY";
    case NetworkState::kStationConnecting: return "STA_CONNECTING";
    case NetworkState::kStationConnected:  return "STA_CONNECTED";
    case NetworkState::kApStationFallback: return "AP_STA_FALLBACK";
    case NetworkState::kError:             return "NETWORK_ERROR";
  }
  return "OFF";
}

const char* toString(ScanState state) {
  switch (state) {
    case ScanState::kIdle:     return "IDLE";
    case ScanState::kRunning:  return "SCANNING";
    case ScanState::kComplete: return "COMPLETE";
    case ScanState::kFailed:   return "FAILED";
  }
  return "IDLE";
}

/**
 * The hostname rule, in one place.
 *
 * It lives here rather than in the REST layer because BOTH sides need it: the
 * API rejects a bad name with a 400, and WifiManager refuses to hand one to
 * mDNS.  Two copies of a validation rule is two rules, and the one that gets
 * updated is never the one being violated.
 *
 * Deliberately stricter than DNS: lower case only, no dots.  A hostname that
 * differs from what the operator typed only by case is a hostname they will
 * type wrong, and a dot would turn "lab.reactor" into a subdomain that mDNS
 * does not resolve the way anybody expects.
 */
bool INetworkManager::hostnameIsValid(const char* hostname) {
  if (hostname == nullptr) return false;
  const std::size_t length = std::strlen(hostname);
  if (length == 0 || length >= kHostnameLength) return false;
  if (hostname[0] == '-' || hostname[length - 1] == '-') return false;
  for (std::size_t i = 0; i < length; ++i) {
    const char c = hostname[i];
    const bool allowed =
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!allowed) return false;
  }
  return true;
}

}  // namespace lc
