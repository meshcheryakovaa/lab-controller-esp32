#include "core/Error.h"

namespace lc {

const char* errorSymbol(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk:                return "OK";

    case ErrorCode::kInvalidArgument:   return "INVALID_ARGUMENT";
    case ErrorCode::kNotFound:          return "NOT_FOUND";
    case ErrorCode::kAlreadyExists:     return "ALREADY_EXISTS";
    case ErrorCode::kOutOfCapacity:     return "OUT_OF_CAPACITY";
    case ErrorCode::kNotSupported:      return "NOT_SUPPORTED";
    case ErrorCode::kInvalidState:      return "INVALID_STATE";
    case ErrorCode::kTimeout:           return "TIMEOUT";
    case ErrorCode::kInternal:          return "INTERNAL";
    case ErrorCode::kNameTooLong:       return "NAME_TOO_LONG";

    case ErrorCode::kResourceBusy:      return "RESOURCE_BUSY";
    case ErrorCode::kGpioInvalid:       return "GPIO_INVALID";
    case ErrorCode::kGpioInputOnly:     return "GPIO_INPUT_ONLY";
    case ErrorCode::kGpioReserved:      return "GPIO_RESERVED";
    case ErrorCode::kGpioStrapping:     return "GPIO_STRAPPING";
    case ErrorCode::kBusNotConfigured:  return "BUS_NOT_CONFIGURED";
    case ErrorCode::kI2cAddressBusy:    return "I2C_ADDRESS_BUSY";
    case ErrorCode::kAdcChannelInvalid: return "ADC_CHANNEL_INVALID";
    case ErrorCode::kPwmChannelExhausted: return "PWM_CHANNEL_EXHAUSTED";

    case ErrorCode::kDeviceInitFailed:  return "DEVICE_INIT_FAILED";
    case ErrorCode::kDeviceNotResponding: return "DEVICE_NOT_RESPONDING";
    case ErrorCode::kDeviceCrcError:    return "DEVICE_CRC_ERROR";
    case ErrorCode::kDeviceOutOfRange:  return "DEVICE_OUT_OF_RANGE";
    case ErrorCode::kDriverNotRegistered: return "DRIVER_NOT_REGISTERED";
    case ErrorCode::kDeviceConfigInvalid: return "DEVICE_CONFIG_INVALID";

    case ErrorCode::kChannelNotFound:   return "CHANNEL_NOT_FOUND";
    case ErrorCode::kChannelTypeMismatch: return "CHANNEL_TYPE_MISMATCH";
    case ErrorCode::kFormulaParseError: return "FORMULA_PARSE_ERROR";
    case ErrorCode::kFormulaCycle:      return "FORMULA_CYCLE";
    case ErrorCode::kCalibrationInsufficientPoints:
      return "CALIBRATION_INSUFFICIENT_POINTS";
    case ErrorCode::kCalibrationSingular: return "CALIBRATION_SINGULAR";
    case ErrorCode::kProcessorChainTooLong: return "PROCESSOR_CHAIN_TOO_LONG";
    case ErrorCode::kDashboardInvalid:      return "DASHBOARD_INVALID";

    case ErrorCode::kStorageFailure:    return "STORAGE_FAILURE";
    case ErrorCode::kConfigSchemaTooNew: return "CONFIG_SCHEMA_TOO_NEW";
    case ErrorCode::kConfigMigrationFailed: return "CONFIG_MIGRATION_FAILED";
    case ErrorCode::kConfigCorrupt:     return "CONFIG_CORRUPT";
    case ErrorCode::kFilesystemFull:    return "FILESYSTEM_FULL";

    case ErrorCode::kSafetyInterlock:   return "SAFETY_INTERLOCK";
    case ErrorCode::kExperimentAborted: return "EXPERIMENT_ABORTED";
    case ErrorCode::kRuleInvalid:       return "RULE_INVALID";

    case ErrorCode::kUnauthorized:      return "UNAUTHORIZED";
    case ErrorCode::kForbidden:         return "FORBIDDEN";
    case ErrorCode::kPayloadTooLarge:   return "PAYLOAD_TOO_LARGE";
    case ErrorCode::kRateLimited:       return "RATE_LIMITED";
    case ErrorCode::kCloudNotConfigured:    return "CLOUD_NOT_CONFIGURED";
    case ErrorCode::kCloudUnauthorized:     return "CLOUD_UNAUTHORIZED";
    case ErrorCode::kCloudAuthRevoked:      return "CLOUD_AUTH_REVOKED";
    case ErrorCode::kCloudQuotaExceeded:    return "CLOUD_QUOTA_EXCEEDED";
    case ErrorCode::kCloudChecksumMismatch: return "CLOUD_CHECKSUM_MISMATCH";
    case ErrorCode::kCloudRemoteConflict:   return "CLOUD_REMOTE_CONFLICT";
    case ErrorCode::kCloudUntrustedHost:    return "CLOUD_UNTRUSTED_UPLOAD_URL";
    case ErrorCode::kCloudTransient:        return "CLOUD_TRANSIENT";
  }
  return "UNKNOWN";
}

}  // namespace lc
