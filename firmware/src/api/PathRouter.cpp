#include "api/PathRouter.h"

namespace lc {
namespace {

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

PathSegments::PathSegments(const char* path) {
  if (path == nullptr) return;

  const char* cursor = path;
  while (*cursor != '\0' && count_ < kMaxSegments) {
    while (*cursor == '/') ++cursor;
    if (*cursor == '\0') break;

    char* out = segments_[count_];
    std::size_t written = 0;
    while (*cursor != '\0' && *cursor != '/' && *cursor != '?') {
      char decoded = *cursor;
      if (decoded == '%' && cursor[1] != '\0' && cursor[2] != '\0') {
        const int high = hexValue(cursor[1]);
        const int low = hexValue(cursor[2]);
        if (high >= 0 && low >= 0) {
          decoded = static_cast<char>((high << 4) | low);
          cursor += 2;
        }
      }
      if (written + 1 < kMaxSegmentLength) {
        out[written++] = decoded;
      } else {
        // A segment longer than a channel key can ever be is not a path we
        // serve; flagging it keeps the router from matching a truncated key.
        truncated_ = true;
      }
      ++cursor;
    }
    out[written] = '\0';
    if (written > 0) ++count_;
    if (*cursor == '?') break;
  }

  if (*cursor != '\0' && *cursor != '?') truncated_ = true;
}

}  // namespace lc
