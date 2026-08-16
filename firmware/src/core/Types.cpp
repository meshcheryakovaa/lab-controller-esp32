#include "core/Types.h"

namespace lc {

const char* toString(DeviceState state) {
  switch (state) {
    case DeviceState::kDisabled:     return "DISABLED";
    case DeviceState::kConfigured:   return "CONFIGURED";
    case DeviceState::kInitializing: return "INITIALIZING";
    case DeviceState::kRunning:      return "RUNNING";
    case DeviceState::kWarning:      return "WARNING";
    case DeviceState::kError:        return "ERROR";
  }
  return "UNKNOWN";
}

const char* toString(ChannelQuality quality) {
  switch (quality) {
    case ChannelQuality::kUnknown:    return "UNKNOWN";
    case ChannelQuality::kGood:       return "GOOD";
    case ChannelQuality::kStale:      return "STALE";
    case ChannelQuality::kOutOfRange: return "OUT_OF_RANGE";
    case ChannelQuality::kSaturated:  return "SATURATED";
    case ChannelQuality::kFaulted:    return "FAULTED";
  }
  return "UNKNOWN";
}

}  // namespace lc
