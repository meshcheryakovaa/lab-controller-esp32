// =============================================================================
//  platform/esp32/CloudUploadTask.h — where the uploading actually happens
//  (M17 §14).
//
//  WHY THIS IS A SEPARATE FreeRTOS TASK AND NOT A SCHEDULER TICK.
//
//  Two reasons, and the first one has already cost this project a release:
//
//  1. IT MUST NOT RUN ON THE HTTP TASK.  Version 0.15.2 fixed a crash whose
//     signature was "Stack canary watchpoint triggered (httpd)", and that was
//     before anything did TLS.  A request handler that opened a TLS session,
//     streamed 100 KiB and parsed JSON on the web server's stack would be the
//     same mistake with more layers.  Every HTTP handler here therefore only
//     sets a flag and answers.
//
//  2. IT MUST NOT RUN ON THE SCHEDULER.  The scheduler is COOPERATIVE: a task
//     that blocks blocks the safety pass behind it.  A cloud request waits on a
//     network for up to fifteen seconds, and the one thing that may never wait
//     fifteen seconds is the loop that watches an interlock.
//
//  The task therefore does the slow work, and the sampling, logging and safety
//  passes carry on beside it — which is the whole point: a cloud problem must
//  never stop the experiment.
// =============================================================================
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "services/CloudManager.h"
#include "platform/esp32/YandexOAuthClient.h"

namespace lc {
namespace platform {

class CloudUploadTask {
 public:
  /** 12 KiB.  TLS handshakes are the deep part of this call chain — mbedTLS
   *  plus HTTPClient plus LittleFS — and §14 asks for at least 2 KiB of
   *  headroom after a load test.  The watermark is reported in diagnostics so
   *  the number can be judged rather than assumed. */
  static constexpr std::uint32_t kStackBytes = 12 * 1024;
  /** Below the scheduler's work and well below Wi-Fi: this is the least urgent
   *  thing the controller does. */
  static constexpr UBaseType_t kPriority = 1;
  /** Core 0, beside the network stack, leaving core 1 to the loop that samples
   *  and enforces safety. */
  static constexpr BaseType_t kCore = 0;

  /** How long the task sleeps when there is nothing to send.  Long enough to
   *  cost nothing, short enough that a segment closed now is on its way within
   *  a couple of seconds. */
  static constexpr std::uint32_t kIdleDelayMs = 2000;
  static constexpr std::uint32_t kBusyDelayMs = 20;

  CloudUploadTask(CloudManager& manager, YandexOAuthClient& oauth)
      : manager_(manager), oauth_(oauth) {}

  Status begin();
  void stop();

  /** Stack headroom in bytes, for diagnostics.  0 before the task starts. */
  std::uint32_t stackWatermarkBytes() const;
  bool running() const { return handle_ != nullptr; }

 private:
  static void trampoline(void* context);
  void run();

  CloudManager& manager_;
  YandexOAuthClient& oauth_;
  TaskHandle_t handle_ = nullptr;
  volatile bool stopping_ = false;
  volatile std::uint32_t watermark_ = 0;
};

}  // namespace platform
}  // namespace lc
