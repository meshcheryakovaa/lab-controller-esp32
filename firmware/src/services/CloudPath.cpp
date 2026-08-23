#include "services/CloudPath.h"

#include <cstring>

#include "core/Format.h"

namespace lc {
namespace {

constexpr const char* kDiskPrefix = "disk:";

/** The domains an upload URL may point at.  Suffix-matched against the host,
 *  with a leading dot so that "evil-disk.yandex.net" cannot pass as
 *  "disk.yandex.net" — the classic way a suffix check is defeated. */
constexpr const char* kTrustedUploadDomains[] = {
    ".disk.yandex.net",
    ".yandex.net",
};

bool isAlphanumeric(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9');
}

bool isAllowedChar(char c) {
  return isAlphanumeric(c) || c == '_' || c == '-';
}

/** Splits the host out of an https URL.  Returns false for anything that is
 *  not https, has no host, or carries credentials — "user@host" is a shape
 *  worth refusing outright rather than parsing carefully. */
bool hostOf(const char* url, char* out, std::size_t capacity) {
  constexpr const char* kScheme = "https://";
  const std::size_t schemeLength = std::strlen(kScheme);
  if (url == nullptr || std::strncmp(url, kScheme, schemeLength) != 0) {
    return false;
  }
  const char* start = url + schemeLength;
  std::size_t length = 0;
  while (start[length] != '\0' && start[length] != '/' &&
         start[length] != '?' && start[length] != '#') {
    if (start[length] == '@') return false;  // credentials in the authority
    ++length;
  }
  if (length == 0 || length >= capacity) return false;
  // A port is allowed but not part of the name being matched.
  std::size_t nameLength = length;
  for (std::size_t i = 0; i < length; ++i) {
    if (start[i] == ':') { nameLength = i; break; }
  }
  if (nameLength == 0) return false;
  std::memcpy(out, start, nameLength);
  out[nameLength] = '\0';
  return true;
}

bool endsWith(const char* text, const char* suffix) {
  const std::size_t textLength = std::strlen(text);
  const std::size_t suffixLength = std::strlen(suffix);
  if (suffixLength > textLength) return false;
  return std::strcmp(text + textLength - suffixLength, suffix) == 0;
}

}  // namespace

bool sanitiseCloudComponent(const char* input, char* out,
                            std::size_t capacity) {
  if (input == nullptr || out == nullptr || capacity < 2) return false;

  std::size_t written = 0;
  bool lastWasDash = false;
  for (const char* p = input; *p != '\0'; ++p) {
    if (written + 1 >= capacity) break;

    // A dot is kept ONLY as an extension separator — that is, directly after an
    // alphanumeric.  A leading dot, or a second dot in a row, becomes a dash.
    //
    // That one rule removes every shape of traversal at once: "..", "../..",
    // "...", and the leading-dot hidden-file names, without needing a blacklist
    // to keep up with them.  An earlier version filtered the characters and
    // then rejected exactly "." and "..", which let "../../etc" through as
    // "..-..-etc" — safe in practice, but a name with ".." still visible in it
    // is not something to have to reason about twice.
    const bool keepDot =
        (*p == '.') && written > 0 && isAlphanumeric(out[written - 1]);
    if (isAllowedChar(*p) || keepDot) {
      out[written++] = *p;
      lastWasDash = false;
      continue;
    }
    // Everything else — slashes, spaces, control characters, anything above
    // ASCII — collapses to a single dash.  Dropping them silently would let
    // "a/b" and "ab" become the same folder.
    if (lastWasDash) continue;
    out[written++] = '-';
    lastWasDash = true;
  }
  // Trim the padding characters from both ends, so a name cannot begin with a
  // separator either.
  std::size_t start = 0;
  while (start < written && (out[start] == '-' || out[start] == '.')) ++start;
  while (written > start && (out[written - 1] == '-' || out[written - 1] == '.')) {
    --written;
  }
  if (start > 0) {
    std::memmove(out, out + start, written - start);
    written -= start;
  }
  out[written] = '\0';

  return written > 0;
}

bool normaliseCloudRoot(const char* input, CloudPathString& out) {
  if (input == nullptr) return false;
  const char* cursor = input;
  if (std::strncmp(cursor, kDiskPrefix, std::strlen(kDiskPrefix)) == 0) {
    cursor += std::strlen(kDiskPrefix);
  }

  char path[kCloudPathLength];
  std::size_t used = 0;
  bool complete = appendFormat(path, sizeof(path), used, "%s/", kDiskPrefix);

  std::size_t parts = 0;
  while (*cursor != '\0') {
    while (*cursor == '/') ++cursor;
    if (*cursor == '\0') break;
    char raw[64];
    std::size_t length = 0;
    while (*cursor != '\0' && *cursor != '/' && length + 1 < sizeof(raw)) {
      raw[length++] = *cursor++;
    }
    raw[length] = '\0';
    while (*cursor != '\0' && *cursor != '/') ++cursor;  // skip an overlong part

    char clean[64];
    if (!sanitiseCloudComponent(raw, clean, sizeof(clean))) return false;
    complete &= appendFormat(path, sizeof(path), used, "%s%s",
                             parts == 0 ? "" : "/", clean);
    ++parts;
  }

  if (parts == 0 || !complete) return false;
  return out.assign(path);
}

bool buildCloudSessionPath(const char* root, const char* controllerId,
                           const char* sessionId, CloudPathString& out) {
  CloudPathString normalisedRoot;
  if (!normaliseCloudRoot(root, normalisedRoot)) return false;

  char controller[48];
  char session[48];
  if (!sanitiseCloudComponent(controllerId, controller, sizeof(controller))) {
    return false;
  }
  if (!sanitiseCloudComponent(sessionId, session, sizeof(session))) return false;

  char path[kCloudPathLength];
  std::size_t used = 0;
  if (!appendFormat(path, sizeof(path), used, "%s/%s/%s",
                    normalisedRoot.c_str(), controller, session)) {
    return false;
  }
  return out.assign(path);
}

bool buildCloudSegmentPath(const char* sessionPath, const char* fileName,
                           CloudPathString& out) {
  if (sessionPath == nullptr || sessionPath[0] == '\0') return false;
  char name[64];
  if (!sanitiseCloudComponent(fileName, name, sizeof(name))) return false;

  char path[kCloudPathLength];
  std::size_t used = 0;
  if (!appendFormat(path, sizeof(path), used, "%s/%s", sessionPath, name)) {
    return false;
  }
  return out.assign(path);
}

bool buildCloudTemporaryPath(const char* finalPath, CloudPathString& out) {
  if (finalPath == nullptr || finalPath[0] == '\0') return false;
  char path[kCloudPathLength];
  std::size_t used = 0;
  if (!appendFormat(path, sizeof(path), used, "%s%s", finalPath,
                    kUploadingSuffix)) {
    return false;
  }
  return out.assign(path);
}

bool isTrustedUploadUrl(const char* url) {
  char host[128];
  if (!hostOf(url, host, sizeof(host))) return false;
  for (const char* domain : kTrustedUploadDomains) {
    if (endsWith(host, domain)) return true;
  }
  return false;
}

}  // namespace lc
