// =============================================================================
//  core/Error.h — machine-readable error model (§46).
//
//  Rules of the project:
//    * every failure has a stable numeric code AND a stable string symbol;
//    * the string symbol is what the REST API and the UI switch on, so it must
//      never be reworded once released;
//    * a human-readable detail may be attached, but the UI must remain usable
//      if it is empty.
// =============================================================================
#pragma once

#include <cstdint>

#include "core/Types.h"

namespace lc {

// ---------------------------------------------------------------------------
//  Error codes are grouped by subsystem in blocks of 100 so that a code alone
//  tells you where to look.  Never renumber an existing code.
// ---------------------------------------------------------------------------
enum class ErrorCode : std::uint16_t {
  kOk = 0,

  // 1xx — generic / argument validation
  kInvalidArgument = 100,
  kNotFound = 101,
  kAlreadyExists = 102,
  kOutOfCapacity = 103,
  kNotSupported = 104,
  kInvalidState = 105,
  kTimeout = 106,
  kInternal = 107,
  kNameTooLong = 108,

  // 2xx — hardware resources
  kResourceBusy = 200,
  kGpioInvalid = 201,
  kGpioInputOnly = 202,
  kGpioReserved = 203,       // flash/PSRAM pins, never usable
  kGpioStrapping = 204,      // usable, but with a warning
  kBusNotConfigured = 205,
  kI2cAddressBusy = 206,
  kAdcChannelInvalid = 207,
  kPwmChannelExhausted = 208,

  // 3xx — devices / drivers
  kDeviceInitFailed = 300,
  kDeviceNotResponding = 301,
  kDeviceCrcError = 302,
  kDeviceOutOfRange = 303,
  kDriverNotRegistered = 304,
  kDeviceConfigInvalid = 305,

  // 4xx — channels / processing
  kChannelNotFound = 400,
  kChannelTypeMismatch = 401,
  kFormulaParseError = 402,
  kFormulaCycle = 403,
  kCalibrationInsufficientPoints = 404,
  kCalibrationSingular = 405,
  kProcessorChainTooLong = 406,
  kDashboardInvalid = 407,   // layout is well-formed JSON but not a layout

  // 5xx — storage / configuration
  kStorageFailure = 500,
  kConfigSchemaTooNew = 501,
  kConfigMigrationFailed = 502,
  kConfigCorrupt = 503,
  kFilesystemFull = 504,

  // 6xx — control / safety
  kSafetyInterlock = 600,
  kExperimentAborted = 601,
  kRuleInvalid = 602,

  // 7xx — API / auth
  kUnauthorized = 700,
  kForbidden = 701,
  kPayloadTooLarge = 702,
  kRateLimited = 703,
};

// Stable UPPER_SNAKE symbol, e.g. "GPIO_ALREADY_IN_USE".  Used verbatim in
// REST error envelopes and in the WebSocket alarm stream.
const char* errorSymbol(ErrorCode code);

// ---------------------------------------------------------------------------
//  Error — code plus optional human detail.  Trivially copyable, no heap.
// ---------------------------------------------------------------------------
struct Error {
  ErrorCode code = ErrorCode::kOk;
  DetailString detail;

  Error() = default;
  explicit Error(ErrorCode c) : code(c) {}
  Error(ErrorCode c, const char* d) : code(c) { detail.assign(d); }

  bool ok() const { return code == ErrorCode::kOk; }
  const char* symbol() const { return errorSymbol(code); }
};

inline Error ok() { return Error{}; }
inline Error fail(ErrorCode code) { return Error{code}; }
inline Error fail(ErrorCode code, const char* detail) { return Error{code, detail}; }

// ---------------------------------------------------------------------------
//  Result<T> — value-or-error.  Deliberately minimal; no exceptions anywhere
//  in the firmware (exceptions are disabled in the ESP32 build).
// ---------------------------------------------------------------------------
template <typename T>
class Result {
 public:
  Result(const T& value) : value_(value) {}          // NOLINT(runtime/explicit)
  Result(const Error& error) : error_(error) {}      // NOLINT(runtime/explicit)

  bool ok() const { return error_.ok(); }
  explicit operator bool() const { return ok(); }

  // Only valid when ok(); callers must check first.
  const T& value() const { return value_; }
  T& value() { return value_; }
  const T& valueOr(const T& fallback) const { return ok() ? value_ : fallback; }

  const Error& error() const { return error_; }
  ErrorCode code() const { return error_.code; }

 private:
  T value_{};
  Error error_{};
};

// `Status` is the void-returning counterpart of Result<T>.
using Status = Error;

}  // namespace lc
