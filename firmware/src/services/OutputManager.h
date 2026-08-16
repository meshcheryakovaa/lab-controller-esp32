// =============================================================================
//  services/OutputManager.h — the safety layer for anything that acts (§27, §30).
//
//  This file exists before any output driver does, and that order is the point.
//
//  THE RULE
//  An output is never merely "a value".  It is a value the system is currently
//  CERTAIN somebody still wants.  The moment that certainty lapses — the command
//  expired, the device that carries it stopped running, the controller was
//  tripped — the output goes to its safe state.  Certainty is the default state
//  of nothing, not of everything.
//
//  WHY THIS IS ONE CLASS AND NOT A CONVENTION ACROSS DRIVERS
//  A driver that forgets its timeout is a driver that leaves a heater on
//  overnight.  §30 says a safety limit must not be implemented as a user Rule;
//  the same reasoning says it must not be implemented five times by five
//  drivers either.  Enforcement runs here, in one scheduler task at
//  TaskPriority::kSafety, which by construction runs before acquisition,
//  processing or telemetry can consume the pass budget.
//
//  WHAT "SAFE" MEANS
//  Whatever the rig says it means.  0 % for a heater; possibly 100 % for the
//  fan on a hot box; closed for one valve and open for another.  It is declared
//  per device and it is mandatory — there is no "unspecified" safe state,
//  because the first thing an unspecified safe state does is become zero on a
//  device where zero is wrong.
//
//  BOOT
//  Outputs come up in the safe state.  Always, and with no option to restore
//  the last commanded value: a heater that switches itself back on after a
//  power cut, in an empty laboratory, is the exact failure this milestone is
//  written to prevent.  A rig that wants its fan running at boot declares the
//  fan's safe value to be 30 % — one concept, no second mechanism.
// =============================================================================
#pragma once

#include "core/Clock.h"
#include "core/Error.h"
#include "core/EventBus.h"
#include "core/Scheduler.h"
#include "core/Types.h"
#include "services/ChannelManager.h"

namespace lc {

class DeviceManager;

// Why an output is not currently carrying a commanded value.
enum class OutputHoldState : std::uint8_t {
  kSafe = 0,      // at the safe value; nothing has been commanded
  kCommanded,     // carrying a command that is still valid
  kExpired,       // a command was given and its hold time ran out
  kDeviceFault,   // the device carrying it stopped running
  kTripped,       // a global fail-safe was raised
};

const char* toString(OutputHoldState state);

struct OutputRecord {
  ChannelHandle channel = kInvalidChannel;
  DeviceHandle device = kInvalidDevice;

  float safeValue = 0.0f;
  float commanded = 0.0f;
  float applied = 0.0f;

  // 0 means "no deadline", which is a decision the configuration has to make
  // explicitly rather than something that happens by omission.
  Micros holdTimeoutUs = 0;
  Micros commandedAtUs = 0;

  OutputHoldState state = OutputHoldState::kSafe;
  bool active = false;
  // The safe value has been written through to the channel at least once.
  // Registration happens before the driver instance exists, so the first write
  // has to wait for the device to reach RUNNING — until then the channel would
  // read 0 while the actuator sits at its (possibly non-zero) safe value.
  bool published = false;
};

class OutputManager {
 public:
  OutputManager(const IClock& clock, ChannelManager& channels,
                Scheduler& scheduler, EventBus& events)
      : clock_(clock), channels_(channels), scheduler_(scheduler), events_(events) {}

  static constexpr std::size_t capacity() { return limits::kMaxOutputs; }

  // Registers the safety contract of one output channel.  Called by a driver
  // from configure(); an output channel that never registers cannot be
  // commanded at all, which is the safe way round.
  Status registerOutput(ChannelHandle channel, DeviceHandle device,
                        float safeValue, Micros holdTimeoutUs);
  void forgetDevice(DeviceHandle device);
  void forgetChannel(ChannelHandle channel);

  // Starts the enforcement task.  Call once, after construction.
  Status begin(Micros periodUs = 100000);

  // The only way a value reaches an output.  Refusing while tripped is the
  // whole point of the trip.
  Status command(ChannelHandle channel, float value);

  // Renews the deadline without changing the value — what a controller or a
  // watching browser sends to say "still here".
  Status renew(ChannelHandle channel);

  // Drops one output to its safe value, with a reason for the log.
  Status release(ChannelHandle channel, OutputHoldState reason);

  // EVERYTHING to safe, and refuse further commands until cleared.  Raised by
  // safe-mode boot, by the SafetyManager (M8) and by the operator.
  void trip(const char* reason);
  void clearTrip();
  bool tripped() const { return tripped_; }
  const char* tripReason() const { return tripReason_; }

  // Called by the enforcement task; exposed so tests can step it directly.
  void tick(Micros now);

  // Notification that a device is no longer able to carry its outputs.
  void onDeviceStateChanged(DeviceHandle device, bool healthy);

  const OutputRecord* find(ChannelHandle channel) const;
  std::size_t count() const { return count_; }
  const OutputRecord& at(std::size_t index) const { return records_[index]; }

  std::uint32_t expiries() const { return expiries_; }
  std::uint32_t trips() const { return trips_; }

 private:
  static void tickTrampoline(void* context);
  OutputRecord* mutableFind(ChannelHandle channel);
  Status drive(OutputRecord& record, float value, OutputHoldState state);
  void publish(const OutputRecord& record, const char* detail,
               std::uint8_t severity);

  const IClock& clock_;
  ChannelManager& channels_;
  Scheduler& scheduler_;
  EventBus& events_;

  OutputRecord records_[limits::kMaxOutputs];
  std::size_t count_ = 0;

  bool tripped_ = false;
  const char* tripReason_ = "";
  TaskId task_ = kInvalidTask;
  std::uint32_t expiries_ = 0;
  std::uint32_t trips_ = 0;
};

}  // namespace lc
