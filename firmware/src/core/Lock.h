// =============================================================================
//  core/Lock.h — a mutex where there are tasks, and nothing where there are not.
//
//  WHY (0.15.1-m15).
//
//  The firmware's scheduler is cooperative, so for most of this codebase "two
//  things at once" does not happen and no locking is needed.  The web server is
//  the exception, and it is easy to forget: esp_http_server runs the request
//  handler on ITS OWN FreeRTOS task (PsychicHttpAdapter sets that task's stack
//  size explicitly, which is the clue), while the logger's flush runs on the
//  Arduino loop task.  M15 gave those two tasks the same object to write to:
//
//      loop task                        HTTP task
//      ---------                        ---------
//      appendRows()                     GET  /logs/{id}/segments
//        rotate: finalizeSegment()      POST /logs/{id}/segments/{n}/ack
//          loadIndex()                    loadIndex()
//          saveIndex()                    backend_.remove(path)
//          openSegment()                  saveIndex()
//
//  Both sides read /data/logs.json into a document, edit it, and write the whole
//  thing back.  Interleave them and the second save silently discards the first
//  one's edit — which, depending on which way round it happens, means a segment
//  that was acknowledged and deleted reappears in the queue as a file that is no
//  longer there, or a segment that was just closed is dropped from the index and
//  becomes an orphan.  Neither is detected at the time.
//
//  WHAT IS DELIBERATELY NOT LOCKED.
//  Sending a segment to the browser.  That is megabytes through
//  PsychicFileResponse and happens entirely outside LogStore; holding a lock
//  across it would stall the logger's flush for the length of a download and
//  turn a slow client into dropped rows.  Only the short metadata operations are
//  serialised, which is the whole reason they are short.
// =============================================================================
#pragma once

#if defined(LC_TARGET_ESP32) || defined(ARDUINO)
#define LC_HAS_FREERTOS 1
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#else
#define LC_HAS_FREERTOS 0
#endif

namespace lc {

/**
 * A recursive mutex.
 *
 * Recursive because the methods that need it call each other — acknowledging a
 * segment loads the index, closing a session finalises a segment which records
 * it in the index — and a plain mutex would deadlock the first time somebody
 * added a call that already held it.  Getting that wrong deadlocks an
 * instrument in the field; the cost of recursion is a counter.
 *
 * On the host it compiles to nothing at all.  The tests are single-threaded, and
 * a lock that only exists on one target is a lock whose absence the other target
 * never has to reason about.
 */
class RecursiveMutex {
 public:
  RecursiveMutex() {
#if LC_HAS_FREERTOS
    handle_ = xSemaphoreCreateRecursiveMutex();
#endif
  }
  ~RecursiveMutex() {
#if LC_HAS_FREERTOS
    if (handle_ != nullptr) vSemaphoreDelete(handle_);
#endif
  }

  RecursiveMutex(const RecursiveMutex&) = delete;
  RecursiveMutex& operator=(const RecursiveMutex&) = delete;

  void lock() {
#if LC_HAS_FREERTOS
    // portMAX_DELAY: there is no useful "could not get the lock" answer here.
    // Every holder does a bounded amount of filesystem work and then releases;
    // returning early would mean proceeding with the data unprotected, which is
    // the failure being prevented.
    if (handle_ != nullptr) xSemaphoreTakeRecursive(handle_, portMAX_DELAY);
#endif
  }
  void unlock() {
#if LC_HAS_FREERTOS
    if (handle_ != nullptr) xSemaphoreGiveRecursive(handle_);
#endif
  }

 private:
#if LC_HAS_FREERTOS
  SemaphoreHandle_t handle_ = nullptr;
#endif
};

/** Scope-bound so that an early `return` — of which the store has many — cannot
 *  leave the lock held. */
class LockGuard {
 public:
  explicit LockGuard(RecursiveMutex& mutex) : mutex_(mutex) { mutex_.lock(); }
  ~LockGuard() { mutex_.unlock(); }

  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

 private:
  RecursiveMutex& mutex_;
};

}  // namespace lc
