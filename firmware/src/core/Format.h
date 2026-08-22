// =============================================================================
//  core/Format.h — appending to a fixed buffer without walking off the end.
//
//  WHY THIS FILE EXISTS (0.15.1-m15).
//
//  The controller rebooted with:
//
//      Backtrace: 0xfffffffe:0x8008e498 |<-CORRUPTED
//
//  and the cause was four characters of arithmetic repeated all over the
//  firmware:
//
//      used += std::snprintf(buffer + used, sizeof(buffer) - used, ...);
//
//  `snprintf` returns the length the text WOULD have needed, not the length it
//  wrote.  One truncated fragment therefore pushes `used` past `sizeof(buffer)`,
//  and from that moment on every further call in the same function is handed
//
//      buffer + used        — a pointer past the end of the array, and
//      sizeof(buffer) - used — an unsigned subtraction that has wrapped, so the
//                              "capacity" is about four billion bytes
//
//  which is a licence to write anywhere.  In DataLogger::appendRow the buffer
//  was a 512-byte local, so the overrun landed on the task's own return address
//  — hence a backtrace the panic handler could not even decode.
//
//  The rule this header enforces is simple: `used` never exceeds `capacity`, and
//  a fragment that did not fit says so instead of being rounded up into a
//  pointer.  Callers must LOOK at the answer.  Silent truncation is how a
//  dataset ends up with a column line cut through the middle of a channel name,
//  which is a file nobody can parse and everybody assumes is fine.
// =============================================================================
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace lc {

/**
 * Append `format` to `buffer` at offset `used`, advancing `used`.
 *
 * Returns true when the whole fragment fitted.  On false the buffer still holds
 * a valid NUL-terminated string — as much of it as fitted — and `used` is left
 * at the capacity, so a caller that ignores the answer produces a short string
 * rather than a corrupted one.  It still produces a WRONG string, which is why
 * every caller in this firmware checks.
 */
inline bool appendFormat(char* buffer, std::size_t capacity, std::size_t& used,
                         const char* format, ...) {
  // Room for at least one byte plus the terminator, or there is nothing honest
  // to do.  Note `used >= capacity` rather than `>`: at exactly capacity the
  // only byte left is the one holding the NUL.
  if (buffer == nullptr || capacity == 0 || used >= capacity) {
    if (capacity > 0) used = capacity - 1;
    return false;
  }

  va_list args;
  va_start(args, format);
  const int written =
      std::vsnprintf(buffer + used, capacity - used, format, args);
  va_end(args);

  if (written < 0) {
    // An encoding error.  The buffer contents are unspecified past `used`, so
    // terminate where we were and report the failure.
    buffer[used] = '\0';
    return false;
  }

  const std::size_t required = static_cast<std::size_t>(written);
  if (required >= capacity - used) {
    // Truncated.  vsnprintf has already NUL-terminated at the end of the space
    // it had; parking `used` there keeps a later append from re-writing over
    // the terminator and pretending the text is longer than it is.
    used = capacity - 1;
    return false;
  }

  used += required;
  return true;
}

}  // namespace lc
