// =============================================================================
//  core/Types.h — fundamental value types shared by every subsystem.
//
//  This header is deliberately free of Arduino / ESP-IDF includes so that the
//  whole core layer can be compiled and unit-tested with a host compiler
//  (`pio test -e native`).  Nothing here allocates from the heap.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lc {

// ---------------------------------------------------------------------------
//  Time
// ---------------------------------------------------------------------------
//  Micros  — monotonic microseconds since boot.  Used for scheduling and for
//            measuring durations.  Never jumps when NTP corrects the clock.
//  Millis  — monotonic milliseconds since boot.
//  Epoch   — wall-clock milliseconds since 1970-01-01 UTC.  Used only for
//            timestamps written to logs and sent to the browser.  May be 0
//            before time synchronisation; loggers must record that fact.
// ---------------------------------------------------------------------------
using Micros = std::uint64_t;
using Millis = std::uint32_t;
using EpochMs = std::uint64_t;

// ---------------------------------------------------------------------------
//  Handles
// ---------------------------------------------------------------------------
//  Hot paths (acquisition, processing, logging, WebSocket serialisation) address
//  entities by small integer handles, never by string.  Strings exist only in
//  the descriptor records and at the REST/WebSocket boundary.
//  0 is reserved as "invalid"; see ADR-0002.
// ---------------------------------------------------------------------------
using DeviceHandle = std::uint16_t;
using ChannelHandle = std::uint16_t;
using ModuleTypeIndex = std::uint16_t;

inline constexpr DeviceHandle kInvalidDevice = 0;
inline constexpr ChannelHandle kInvalidChannel = 0;

// ---------------------------------------------------------------------------
//  Compile-time capacity limits
// ---------------------------------------------------------------------------
//  Every container in the core layer is fixed-capacity.  Bumping a limit is a
//  conscious, reviewable change with a known RAM cost, not an accident that
//  shows up as heap fragmentation three hours into an experiment.
// ---------------------------------------------------------------------------
// =============================================================================
//  CAPACITIES AND THE RAM BILL  (Milestone 12)
//
//  Every number in this namespace is multiplied by a record size and charged to
//  .bss at link time.  Nothing in the milestones that wrote them ever paid the
//  bill, because nothing had linked for the ESP32; the first attempt ended in
//
//      region `dram0_0_seg' overflowed by 10600 bytes
//
//  which names no object and offers no advice.  Three things changed here:
//
//  1. The four biggest limits came down to what a bench instrument on an ESP32
//     DevKit can actually be, rather than what looked generous in a header.
//  2. `tools/ram_report.cpp` prints the cost of each limit, so the next change
//     to a number in this file can be argued in bytes beforehand.
//  3. test_core enforces a static-footprint budget on the host, so exceeding it
//     fails a four-second test instead of a three-minute link on a toolchain
//     not everyone has installed.
//
//  The reductions are not free and the trade is written down next to each one.
// =============================================================================
namespace limits {
// Sixteen devices and forty-eight channels: an ESP32 DevKit has 34 GPIOs, two
// I2C buses and one ADC worth using, so a rig that needs a seventeenth device
// has outgrown the board rather than the firmware.  Together these two lines
// are 22 KB of the 10.6 KB that had to go.
inline constexpr std::size_t kMaxDevices = 16;
inline constexpr std::size_t kMaxChannels = 48;
inline constexpr std::size_t kMaxProcessorsPerChannel = 6;
inline constexpr std::size_t kMaxSchedulerTasks = 48;
inline constexpr std::size_t kMaxEventSubscribers = 24;
inline constexpr std::size_t kMaxChannelSubscribers = 8;
// One claim per GPIO, ADC input, PWM channel and I2C address in use.  Sixty-four
// covers every pin the chip has with room to spare; ninety-six could not be
// reached by any configuration this firmware will accept.
inline constexpr std::size_t kMaxResourceClaims = 64;
inline constexpr std::size_t kMaxModuleTypes = 64;
// Channels that may carry an ACTIVE calibration at the same time.  The version
// HISTORY is not counted here: it lives in calibrations.json and is read from
// the file when the editor asks for it (ADR-0014).  Keeping every past fit in
// RAM would cost kilobytes to serve a page nobody has open.
// Halved with kMaxChannels: a calibration belongs to a measuring channel, and
// half the channels on a real rig are outputs, states or digital inputs that
// have nothing to calibrate.
inline constexpr std::size_t kMaxActiveCalibrations = 24;
// Output channels the safety layer tracks.  Every one of them costs a fixed
// record and a pass through the kSafety task; the limit is what keeps that
// pass bounded no matter how the rig is configured.
inline constexpr std::size_t kMaxOutputs = 24;
// Safety limits and control loops.  Both are evaluated every 100 ms at
// priorities that run before everything else, so both are bounded on purpose.
inline constexpr std::size_t kMaxSafetyLimits = 16;
inline constexpr std::size_t kMaxControlLoops = 8;
inline constexpr std::size_t kMaxRules = 16;
// One experiment runs at a time and only its steps are held in RAM; the other
// scenarios stay in the file until somebody starts them.  Sixteen steps is
// still a long scenario — the evaporation run in the brief is eight — and it
// bounds the worst case a single tick can walk through.  A scenario longer than
// this is refused when it is STORED, with a message that says so, rather than
// truncated when it is run.
inline constexpr std::size_t kMaxExperimentSteps = 16;
// Run records kept on the device.  Not configuration and not the dataset:
// enough history to answer "what was the last thing this rig did, and how did
// it end" without turning the flash into a database (§48).
inline constexpr std::size_t kMaxRunRecords = 8;
inline constexpr std::size_t kMaxRunEvents = 16;
// Channels one dataset may carry.  A CSV row is written from a fixed buffer,
// and this is what makes that buffer a known size rather than a hope.
inline constexpr std::size_t kMaxLoggedChannels = 16;
// Datasets the index keeps track of.  The logger never deletes anybody's data
// to make room (§33), so this bound is reached by refusing to start a new
// session, not by quietly dropping an old one.
inline constexpr std::size_t kMaxLogSessions = 24;
// Dashboards are presentation, and presentation is stored, not run: nothing in
// the firmware holds one in RAM.  These bound the FILE, which shares a 640 KB
// LittleFS partition with the web interface itself (ADR-0015).
inline constexpr std::size_t kMaxDashboards = 8;
inline constexpr std::size_t kMaxWidgetsPerDashboard = 24;
inline constexpr std::size_t kDashboardGridColumns = 12;

inline constexpr std::size_t kKeyLength = 24;   // "temperature_01"
inline constexpr std::size_t kNameLength = 32;  // "Surface temperature"
inline constexpr std::size_t kUnitLength = 12;  // "degC", "g", "Pa"
inline constexpr std::size_t kLabelLength = 40; // free-form short label
// Error details are read by a human trying to fix a rig ("DOUT never went low;
// check wiring and power").  Sized so a useful sentence fits: a truncated
// diagnostic is worse than a short one, because it stops mid-instruction.
inline constexpr std::size_t kDetailLength = 64;
}  // namespace limits

// ---------------------------------------------------------------------------
//  FixedString — bounded, heap-free string with value semantics.
// ---------------------------------------------------------------------------
//  Replaces Arduino `String` everywhere inside the core.  Truncation is
//  reported through the return value of assign() so callers can turn it into a
//  validation error instead of silently corrupting an identifier.
// ---------------------------------------------------------------------------
template <std::size_t N>
class FixedString {
 public:
  static_assert(N >= 2, "FixedString needs room for at least one character");

  FixedString() { buf_[0] = '\0'; }
  explicit FixedString(const char* text) { assign(text); }

  // Returns false if `text` had to be truncated to fit.
  bool assign(const char* text) {
    if (text == nullptr) {
      buf_[0] = '\0';
      len_ = 0;
      return true;
    }
    const std::size_t incoming = std::strlen(text);
    const bool fits = incoming < N;
    len_ = fits ? incoming : (N - 1);
    std::memcpy(buf_, text, len_);
    buf_[len_] = '\0';
    return fits;
  }

  const char* c_str() const { return buf_; }
  std::size_t size() const { return len_; }
  bool empty() const { return len_ == 0; }
  static constexpr std::size_t capacity() { return N - 1; }

  bool equals(const char* other) const {
    return other != nullptr && std::strcmp(buf_, other) == 0;
  }
  bool operator==(const FixedString& other) const { return equals(other.buf_); }
  bool operator!=(const FixedString& other) const { return !(*this == other); }

 private:
  char buf_[N];
  std::size_t len_ = 0;
};

using KeyString = FixedString<limits::kKeyLength>;
using NameString = FixedString<limits::kNameLength>;
using UnitString = FixedString<limits::kUnitLength>;
using LabelString = FixedString<limits::kLabelLength>;
using DetailString = FixedString<limits::kDetailLength>;
// "mass_01#3" — channel key plus version.  Stamped into logged datasets so a
// recorded run can always be traced back to the numbers that produced it (§48).
using CalibrationIdString = FixedString<limits::kKeyLength + 8>;

// ---------------------------------------------------------------------------
//  Lifecycle state of a Device (see docs/architecture.md, "Module lifecycle")
// ---------------------------------------------------------------------------
enum class DeviceState : std::uint8_t {
  kDisabled = 0,   // exists in configuration, intentionally not running
  kConfigured,     // configuration validated, resources claimed, not begun
  kInitializing,   // begin() in progress (may span several scheduler ticks)
  kRunning,        // producing samples
  kWarning,        // producing samples, but something is degraded
  kError,          // not producing samples; lastError explains why
};

const char* toString(DeviceState state);

// ---------------------------------------------------------------------------
//  Per-sample quality flag of a Channel
// ---------------------------------------------------------------------------
enum class ChannelQuality : std::uint8_t {
  kUnknown = 0,  // never sampled since boot
  kGood,         // fresh, in range, device healthy
  kStale,        // no new sample within the expected period
  kOutOfRange,   // outside [minimum, maximum] declared for the channel
  kSaturated,    // sensor/ADC clipping
  kFaulted,      // source device is in ERROR
};

const char* toString(ChannelQuality quality);

// ---------------------------------------------------------------------------
//  Direction of a channel: measured input vs. commanded output.
// ---------------------------------------------------------------------------
enum class ChannelDirection : std::uint8_t {
  kInput = 0,   // sensor / virtual / processed value
  kOutput,      // setpoint written to an actuator
};

// ---------------------------------------------------------------------------
//  Geometry — physical placement of a sensor inside the rig (§11).
// ---------------------------------------------------------------------------
enum class CoordinateSystem : std::uint8_t {
  kNone = 0,
  kCartesian,    // x, y, z   [mm]
  kCylindrical,  // r, phi, z [mm, deg, mm]
};

struct Geometry {
  CoordinateSystem system = CoordinateSystem::kNone;
  float a = 0.0f;  // x  or r
  float b = 0.0f;  // y  or phi
  float c = 0.0f;  // z  or z
  LabelString group;  // "Thermocouples"
  LabelString role;   // "Temperature above sample"

  bool isDefined() const { return system != CoordinateSystem::kNone; }
};

}  // namespace lc
