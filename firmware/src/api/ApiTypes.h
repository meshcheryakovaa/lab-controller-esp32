// =============================================================================
//  api/ApiTypes.h — the request/response model the REST layer works with.
//
//  Deliberately free of any HTTP library.  RestApi takes an ApiRequest and
//  fills an ApiResponse; the PsychicHttp adapter does nothing but translate.
//  That is what makes the entire API — routing, validation, error envelopes,
//  dry-run behaviour — testable on a host with no network stack in sight.
//
//  RESPONSE MODEL: a response is built into a JsonDocument and serialised by
//  the adapter.  Bounded by kMaxResponseBytes.  Endpoints that will eventually
//  return megabytes (CSV log export, Milestone 10) get a streaming path of
//  their own rather than distorting this one.  See ADR-0012.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstring>

#include "core/Error.h"
#include "core/Types.h"
#include "storage/JsonUtils.h"   // jsonCopy — read the header, it matters

namespace lc {

enum class HttpMethod : std::uint8_t {
  kGet = 0,
  kPost,
  kPut,
  kPatch,
  kDelete,
  kOptions,
  kUnknown,
};

const char* toString(HttpMethod method);
HttpMethod parseHttpMethod(const char* text);

struct ApiRequest {
  HttpMethod method = HttpMethod::kGet;
  const char* path = "";        // "/api/v1/devices/hx711_01"
  const char* query = "";       // "dry_run=1" — raw, without '?'
  const char* body = nullptr;   // request body, NUL-terminated
  std::size_t bodyLength = 0;
  // The raw Cookie header.  RestApi resolves the session from it ITSELF rather
  // than trusting a flag from the transport: one place decides who is signed
  // in, and it is the place that is unit-tested (ADR-0020).
  const char* cookie = nullptr;
  // Set only by transports that are trusted by construction — the host
  // development server, and nothing on the board.
  bool authenticated = false;

  // "dry_run=1" / "dry_run=true" / bare "dry_run".
  bool queryFlag(const char* name) const;
  // Raw value of a query parameter, or `fallback`.  Points into `query`, so it
  // is valid only for the lifetime of the request.
  const char* queryValue(const char* name, char* buffer, std::size_t capacity,
                         const char* fallback = nullptr) const;
};

// A response that is a FILE rather than a document.  The REST layer describes
// the stream — path, type, filename — and the transport adapter does the
// streaming; RestApi stays free of sockets, and a test can assert that the
// right dataset was offered without a network stack (ADR-0012, ADR-0019).
//
// It exists because a dataset is megabytes and the JSON path is bounded at
// twelve kilobytes on purpose: the fix for "the export does not fit" is a
// second path, not a bigger buffer.
struct StreamSpec {
  bool active = false;
  FixedString<64> path;              // file on the storage backend
  FixedString<32> contentType{"text/csv"};
  FixedString<64> filename;          // what the browser should call it
};

class ApiResponse {
 public:
  // Largest response we are willing to build in RAM.  A configuration export
  // of a full rig is ~8 KB; going beyond this means an endpoint needs
  // streaming, not a bigger buffer.
  static constexpr std::size_t kMaxResponseBytes = 12 * 1024;

  int status = 200;
  JsonDocument body;
  StreamSpec stream;
  // A Set-Cookie header value, or empty.  The REST layer decides what the
  // cookie should be; the adapter puts it on the wire.
  FixedString<128> setCookie;

  // The uniform error envelope (§46).  `field` names the offending
  // configuration key so the UI can highlight the right input.
  void setError(const Error& error, const char* field = nullptr,
                const char* message = nullptr);

  // Same, but choosing the HTTP status from the error code.
  void setError(int httpStatus, const Error& error, const char* field = nullptr,
                const char* message = nullptr);

  bool isError() const { return status >= 400; }
  bool isStream() const { return stream.active; }
  void reset();
};

// Maps a domain error onto the HTTP status that describes it honestly.
int httpStatusFor(ErrorCode code);

}  // namespace lc
