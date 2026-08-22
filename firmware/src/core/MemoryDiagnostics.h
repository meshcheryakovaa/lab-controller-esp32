// =============================================================================
//  core/MemoryDiagnostics.h — heap and stack, at the moments that matter.
//
//  Built only when LC_MEM_DIAGNOSTICS=1 (see the esp32dev-debug environment in
//  platformio.ini).  In every other build every call here compiles to nothing.
//
//  WHY IT IS OFF BY DEFAULT.
//  heap_caps_check_integrity_all() walks the whole heap.  Calling it around
//  every segment rotation costs milliseconds that the logger does not have at
//  50 Hz, and an instrument should not pay for a net it is not falling into.
//
//  WHY IT EXISTS AT ALL.
//  Memory corruption is discovered late.  A stray write inside the logger shows
//  up as a crash inside Wi-Fi or LittleFS twenty seconds later, with a backtrace
//  that points at the innocent party — which is exactly how 0.15.1 began, and
//  why the first real clue came from a sanitizer on the host rather than from
//  the panic on the board.  Checking integrity at named points turns "it
//  rebooted somewhere" into "it was intact before the rotation and broken
//  after", which is a question with an answer.
// =============================================================================
#pragma once

#if defined(LC_MEM_DIAGNOSTICS) && LC_MEM_DIAGNOSTICS

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace lc {

/** free / lowest-ever-free / largest contiguous block / this task's stack
 *  headroom.  The third number is the one that matters for fragmentation: a
 *  heap with 40 KB free and a 2 KB largest block cannot serve a request. */
inline void memoryReport(const char* stage) {
  Serial.printf("[MEM] %-28s free=%u min=%u largest=%u stack=%u\n", stage,
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMinFreeHeap()),
                static_cast<unsigned>(
                    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

/** Stops the board AT the damage rather than wherever it next surfaces.  An
 *  abort() here produces a panic whose backtrace still means something, which a
 *  corrupted-heap crash three subsystems later does not. */
inline void memoryCheck(const char* stage) {
  if (!heap_caps_check_integrity_all(true)) {
    Serial.printf("[MEM] HEAP CORRUPTION DETECTED at %s\n", stage);
    Serial.flush();
    abort();
  }
}

}  // namespace lc

#define LC_MEM_REPORT(stage) ::lc::memoryReport(stage)
#define LC_MEM_CHECK(stage)  ::lc::memoryCheck(stage)

#else

#define LC_MEM_REPORT(stage) ((void)0)
#define LC_MEM_CHECK(stage)  ((void)0)

#endif
