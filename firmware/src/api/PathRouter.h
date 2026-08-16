// =============================================================================
//  api/PathRouter.h — splitting "/api/v1/devices/hx711_01/actions/self-test"
//  into segments, without allocating.
//
//  A regex router would be shorter to write and would cost several kilobytes of
//  flash plus a heap allocation per request.  Paths in this API are at most six
//  segments deep and follow one convention, so a fixed-capacity splitter is
//  both smaller and easier to reason about.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstring>

#include "core/Types.h"

namespace lc {

class PathSegments {
 public:
  static constexpr std::size_t kMaxSegments = 8;
  static constexpr std::size_t kMaxSegmentLength = 40;

  // Splits `path`, ignoring leading/trailing slashes and empty segments.
  // Percent-decodes %XX so that a channel key with a space still matches.
  explicit PathSegments(const char* path);

  std::size_t count() const { return count_; }
  const char* at(std::size_t index) const {
    return index < count_ ? segments_[index] : "";
  }
  bool is(std::size_t index, const char* literal) const {
    return index < count_ && std::strcmp(segments_[index], literal) == 0;
  }
  bool truncated() const { return truncated_; }

  // Convenience: does the path start with "api/v1"?
  bool isApiV1() const { return count_ >= 2 && is(0, "api") && is(1, "v1"); }

 private:
  char segments_[kMaxSegments][kMaxSegmentLength];
  std::size_t count_ = 0;
  bool truncated_ = false;
};

}  // namespace lc
