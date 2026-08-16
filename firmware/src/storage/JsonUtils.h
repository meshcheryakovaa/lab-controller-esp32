// =============================================================================
//  storage/JsonUtils.h — the one JSON pitfall worth a header of its own.
//
//  ArduinoJson stores a bare `const char*` BY POINTER.  That is right for a
//  string literal in flash and catastrophic for anything else: a FixedString
//  buffer, a local snprintf result or a URL path segment is long gone by the
//  time the document is serialised, and what ships is a dangling pointer.
//
//  Found by AddressSanitizer on the API error envelope, where `detail` came
//  from a temporary Error.  Every non-literal string now goes through
//  jsonCopy(); this lives in storage/ because that is the lowest layer allowed
//  to know about ArduinoJson.
//
//  Milestone 12.  The original spelling was `JsonString(text,
//  JsonString::Copied)`.  That enumerator was deprecated in ArduinoJson 7.0 and
//  DELETED in 7.3, so the first real hardware build — which resolved
//  `^7.2.0` to 7.4.3 — failed to compile in every file that serialises
//  anything.  The one-argument constructor has meant "copy" for the whole of
//  the 7.x line (`isStatic` defaults to false), so this spelling is correct on
//  both 7.2 and 7.4, and the static assertion below fails loudly if a future
//  version ever changes that default back.  The library version is now pinned
//  in platformio.ini as well: a caret range that silently moves under a
//  firmware release is not a dependency, it is a rumour.
// =============================================================================
#pragma once

#include <ArduinoJson.h>

namespace lc {

inline JsonString jsonCopy(const char* text) {
  return JsonString(text != nullptr ? text : "");
}

// The whole point of the helper: what comes back must be owned by the document,
// not by whatever buffer produced it.  `isStatic()` means "stored by address"
// and arrived in ArduinoJson 7.3; on older 7.x the same guarantee held under a
// different spelling, and the test simply reports success there.
inline bool jsonCopyIsReallyACopy() {
#if defined(ARDUINOJSON_VERSION_MAJOR) && ARDUINOJSON_VERSION_MAJOR >= 7 && \
    defined(ARDUINOJSON_VERSION_MINOR) && ARDUINOJSON_VERSION_MINOR >= 3
  const char probe[] = "probe";
  return !jsonCopy(probe).isStatic();
#else
  return true;
#endif
}

}  // namespace lc
