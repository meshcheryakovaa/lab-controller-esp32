#include "api/ApiTypes.h"

namespace lc {

const char* toString(HttpMethod method) {
  switch (method) {
    case HttpMethod::kGet:     return "GET";
    case HttpMethod::kPost:    return "POST";
    case HttpMethod::kPut:     return "PUT";
    case HttpMethod::kPatch:   return "PATCH";
    case HttpMethod::kDelete:  return "DELETE";
    case HttpMethod::kOptions: return "OPTIONS";
    case HttpMethod::kUnknown: break;
  }
  return "UNKNOWN";
}

HttpMethod parseHttpMethod(const char* text) {
  if (text == nullptr) return HttpMethod::kUnknown;
  if (std::strcmp(text, "GET") == 0) return HttpMethod::kGet;
  if (std::strcmp(text, "POST") == 0) return HttpMethod::kPost;
  if (std::strcmp(text, "PUT") == 0) return HttpMethod::kPut;
  if (std::strcmp(text, "PATCH") == 0) return HttpMethod::kPatch;
  if (std::strcmp(text, "DELETE") == 0) return HttpMethod::kDelete;
  if (std::strcmp(text, "OPTIONS") == 0) return HttpMethod::kOptions;
  return HttpMethod::kUnknown;
}

namespace {

// Finds "name=" or a bare "name" in a & separated query string.
const char* findParam(const char* query, const char* name) {
  if (query == nullptr || name == nullptr) return nullptr;
  const std::size_t length = std::strlen(name);

  const char* cursor = query;
  while (*cursor != '\0') {
    if (std::strncmp(cursor, name, length) == 0) {
      const char terminator = cursor[length];
      if (terminator == '=' || terminator == '&' || terminator == '\0') {
        return cursor + length;
      }
    }
    const char* next = std::strchr(cursor, '&');
    if (next == nullptr) break;
    cursor = next + 1;
  }
  return nullptr;
}

}  // namespace

bool ApiRequest::queryFlag(const char* name) const {
  const char* found = findParam(query, name);
  if (found == nullptr) return false;
  if (*found != '=') return true;  // bare "?dry_run" means yes
  ++found;
  return std::strncmp(found, "0", 1) != 0 &&
         std::strncmp(found, "false", 5) != 0;
}

const char* ApiRequest::queryValue(const char* name, char* buffer,
                                   std::size_t capacity,
                                   const char* fallback) const {
  const char* found = findParam(query, name);
  if (found == nullptr || *found != '=' || buffer == nullptr || capacity == 0) {
    return fallback;
  }
  ++found;
  std::size_t written = 0;
  while (*found != '\0' && *found != '&' && written + 1 < capacity) {
    buffer[written++] = *found++;
  }
  buffer[written] = '\0';
  return buffer;
}

// ---------------------------------------------------------------------------
//  Errors
// ---------------------------------------------------------------------------
int httpStatusFor(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk:
      return 200;

    case ErrorCode::kNotFound:
    case ErrorCode::kChannelNotFound:
    case ErrorCode::kDriverNotRegistered:
      return 404;

    case ErrorCode::kAlreadyExists:
    case ErrorCode::kResourceBusy:
    case ErrorCode::kI2cAddressBusy:
    case ErrorCode::kInvalidState:
      return 409;

    case ErrorCode::kPayloadTooLarge:
      return 413;

    // "Syntactically fine, semantically wrong" — the form should stay filled in
    // and the offending field highlighted, which is exactly what 422 means.
    case ErrorCode::kDeviceConfigInvalid:
    case ErrorCode::kGpioInvalid:
    case ErrorCode::kGpioInputOnly:
    case ErrorCode::kGpioReserved:
    case ErrorCode::kAdcChannelInvalid:
    case ErrorCode::kBusNotConfigured:
    case ErrorCode::kChannelTypeMismatch:
    case ErrorCode::kFormulaParseError:
    case ErrorCode::kFormulaCycle:
    case ErrorCode::kCalibrationInsufficientPoints:
    case ErrorCode::kCalibrationSingular:
    case ErrorCode::kProcessorChainTooLong:
    case ErrorCode::kDashboardInvalid:
    case ErrorCode::kRuleInvalid:
    case ErrorCode::kNameTooLong:
    case ErrorCode::kOutOfCapacity:
      return 422;

    case ErrorCode::kRateLimited:
      return 429;

    case ErrorCode::kUnauthorized:
      return 401;
    case ErrorCode::kForbidden:
    case ErrorCode::kSafetyInterlock:
      return 403;

    case ErrorCode::kFilesystemFull:
      return 507;

    case ErrorCode::kInvalidArgument:
    case ErrorCode::kNotSupported:
      return 400;

    case ErrorCode::kTimeout:
    case ErrorCode::kDeviceNotResponding:
      return 504;

    default:
      return 500;
  }
}

void ApiResponse::setError(const Error& error, const char* field,
                           const char* message) {
  setError(httpStatusFor(error.code), error, field, message);
}

void ApiResponse::setError(int httpStatus, const Error& error, const char* field,
                           const char* message) {
  status = httpStatus;
  body.clear();
  JsonObject envelope = body["error"].to<JsonObject>();
  envelope["code"] = error.symbol();
  envelope["numeric"] = static_cast<int>(error.code);
  // `message` is for humans and may be reworded or translated freely;
  // `code` is the contract and must never change.
  envelope["message"] = jsonCopy((message != nullptr) ? message : error.symbol());
  if (!error.detail.empty()) envelope["detail"] = jsonCopy(error.detail.c_str());
  if (field != nullptr && field[0] != '\0') envelope["field"] = jsonCopy(field);
}

void ApiResponse::reset() {
  status = 200;
  body.clear();
  stream = StreamSpec{};
  setCookie.assign("");
}

}  // namespace lc
