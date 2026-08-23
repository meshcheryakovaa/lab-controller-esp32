#include "platform/esp32/CloudUploadTask.h"

#include <Arduino.h>

namespace lc {
namespace platform {

Status CloudUploadTask::begin() {
  if (handle_ != nullptr) return ok();
  stopping_ = false;
  const BaseType_t created = xTaskCreatePinnedToCore(
      trampoline, "cloud-upload", kStackBytes, this, kPriority, &handle_, kCore);
  if (created != pdPASS) {
    handle_ = nullptr;
    return fail(ErrorCode::kOutOfCapacity, "the upload task could not start");
  }
  return ok();
}

void CloudUploadTask::stop() {
  if (handle_ == nullptr) return;
  // Asked to finish, not killed: vTaskDelete on a task holding a TLS session
  // and an open file would leak both.
  stopping_ = true;
}

void CloudUploadTask::trampoline(void* context) {
  static_cast<CloudUploadTask*>(context)->run();
}

std::uint32_t CloudUploadTask::stackWatermarkBytes() const { return watermark_; }

void CloudUploadTask::run() {
  while (!stopping_) {
    // The Device Code poll lives here too: it is a network request on a timer,
    // and the HTTP handler that started the flow answered long ago.
    oauth_.poll();

    const bool busy = manager_.tick();

    // Recorded every pass so §14's "at least 2 KiB spare" can be judged from
    // the diagnostics page rather than guessed at.
    watermark_ = static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr))
                 * sizeof(StackType_t);

    // Yielding is not politeness, it is what keeps the watchdog fed and lets
    // the logger's flush reach the filesystem between blocks.
    vTaskDelay(pdMS_TO_TICKS(busy ? kBusyDelayMs : kIdleDelayMs));
  }
  handle_ = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace platform
}  // namespace lc
