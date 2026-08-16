// =============================================================================
//  api/IWebSocketSink.h — where telemetry frames go.
//
//  One method matters: canSend().  If the previous frame has not gone out yet,
//  the batcher DROPS the new one instead of queueing it.  Real-time telemetry
//  has no use for a stale frame, and a queue on a device with 300 KB of heap is
//  just a slower way to run out of memory.
// =============================================================================
#pragma once

#include <cstddef>

namespace lc {

class IWebSocketSink {
 public:
  virtual ~IWebSocketSink() = default;

  virtual std::size_t clientCount() const = 0;

  // False when the transport is still busy with the previous frame.
  virtual bool canSend() const = 0;

  virtual bool broadcast(const char* text, std::size_t length) = 0;
};

}  // namespace lc
