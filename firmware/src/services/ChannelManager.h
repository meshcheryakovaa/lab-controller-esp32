// =============================================================================
//  services/ChannelManager.h — the centre of the platform (§10).
//
//  A Channel is a named, typed, timestamped stream of numbers.  Everything
//  downstream of a driver — dashboards, charts, PID, rules, experiments,
//  logging, virtual channels — talks to channels and never to a sensor.  That
//  single indirection is what makes "add a sensor without reflashing" possible.
//
//  THREE VALUES PER SAMPLE (§48)
//    raw        — exactly what the driver produced (ADC counts, HX711 units)
//    calibrated — after the calibration stage
//    processed  — after the full pipeline; this is what the UI shows
//  Twelve bytes per channel buys scientific reproducibility: a stored log can
//  be re-processed later with corrected coefficients.
//
//  HANDLES, NOT STRINGS
//    Channel keys ("mass_01") exist for the API, the configuration files and
//    formulas.  Everything on the hot path uses ChannelHandle, a uint16 that is
//    simply index+1 into a fixed array.  Handles are stable for the lifetime of
//    a configuration; removing a channel leaves a hole rather than renumbering.
//
//  DATA PLANE, NOT EVENT BUS
//    Sample delivery goes through this class's own listener list, not through
//    EventBus.  See the note at the top of core/EventBus.h.
// =============================================================================
#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/Error.h"
#include "core/IModule.h"
#include "core/Types.h"

namespace lc {

struct ChannelValue {
  float raw = 0.0f;
  float calibrated = 0.0f;
  float processed = 0.0f;
  Micros timestampUs = 0;   // monotonic — use for rates and intervals
  EpochMs epochMs = 0;      // wall clock — use for logs; 0 if unsynced
  std::uint32_t sequence = 0;
  ChannelQuality quality = ChannelQuality::kUnknown;
};

struct ChannelDescriptor {
  KeyString key;        // "mass_01" — unique, stable, referenced by formulas
  NameString name;      // "Sample mass"
  UnitString unit;      // "g"
  FixedString<16> quantity{"raw"};  // "mass", "temperature", ...

  DeviceHandle source = kInvalidDevice;  // kInvalidDevice => virtual channel
  ChannelDirection direction = ChannelDirection::kInput;

  float minimum = 0.0f;
  float maximum = 0.0f;
  std::uint8_t precision = 2;
  std::uint32_t color = 0x4C9AFFu;  // UI hint only

  bool logged = true;
  bool visible = true;

  // Used for staleness detection; 0 disables the check.
  Micros expectedIntervalUs = 0;

  Geometry geometry;
};

// Fires after every accepted sample.  Keep it short: it runs inside the
// acquisition path.  Used by DataLogger, RuleEngine, the WebSocket batcher and
// virtual-channel recomputation.
using ChannelListener = void (*)(ChannelHandle handle, const ChannelValue& value,
                                 void* context);

// Installed by DeviceManager so that writing to an output channel reaches the
// actual actuator without ChannelManager depending on DeviceManager.
// `applied` is an OUT parameter: the driver reports what it actually did, which
// is not always what was asked.  A heater with a 60 % power limit commanded to
// 100 % applies 60, and the channel has to say 60 — a limit that is invisible is
// a limit nobody knows is active (ADR-0016).
using OutputSink = Status (*)(ChannelHandle handle, float value, float* applied,
                              void* context);

// Fires when a channel appears or disappears.  ProcessingManager uses it to
// destroy the processors of a channel whose device was removed; dashboards and
// the logger use it to invalidate their references.  Without this hook a
// removed channel would leave dangling IProcessor* behind.
using ChannelLifecycleListener = void (*)(ChannelHandle handle, bool created,
                                          void* context);

class ChannelManager {
 public:
  explicit ChannelManager(const IClock& clock) : clock_(clock) {}

  // --- lifecycle -----------------------------------------------------------
  Result<ChannelHandle> create(const ChannelDescriptor& descriptor);
  Status remove(ChannelHandle handle);
  std::size_t removeAllFrom(DeviceHandle device);

  // --- lookup --------------------------------------------------------------
  ChannelHandle findByKey(const char* key) const;
  bool exists(ChannelHandle handle) const;
  const ChannelDescriptor* descriptor(ChannelHandle handle) const;
  const ChannelValue* value(ChannelHandle handle) const;

  // Iteration for the API layer.  Handles run 1..capacity(); holes are skipped
  // by exists().
  static constexpr std::size_t capacity() { return limits::kMaxChannels; }
  std::size_t activeCount() const { return activeCount_; }

  // --- data plane ----------------------------------------------------------
  // Called by drivers.  Runs the processing pipeline and notifies listeners.
  // Returns false when the pipeline suppressed the sample (e.g. a median
  // filter still filling its window) — not an error.
  bool publishRaw(ChannelHandle handle, float raw, Micros now);

  // For virtual channels and for anything that computes a final value itself.
  bool publishProcessed(ChannelHandle handle, float processed, Micros now);

  // Commands an output channel; routed to the installed OutputSink.
  Status write(ChannelHandle handle, float value);
  void setOutputSink(OutputSink sink, void* context);

  // --- processing pipeline (§14) ------------------------------------------
  // `stages` is owned by the caller (ProcessingManager) and must outlive the
  // channel.  `calibrationStage` marks which stage's output is recorded as the
  // calibrated value, or -1 if the chain has no calibration.
  Status setPipeline(ChannelHandle handle, IProcessor* const* stages,
                     std::uint8_t stageCount, std::int8_t calibrationStage);
  Status clearPipeline(ChannelHandle handle);

  // --- presentation --------------------------------------------------------
  // Attaching a calibration converts a channel from ADC counts to grams.  The
  // unit, the displayed precision and the range used for the OUT_OF_RANGE
  // check all describe the value AFTER processing, so all three have to move
  // with it: a channel that reads grams while its descriptor still says
  // "counts" is wrong on screen, wrong in the log and wrong in the range check.
  // `unit`/`precision` of nullptr / 0 leave the current value alone; a range
  // with minimum >= maximum means "no declared range".
  Status setPresentation(ChannelHandle handle, const char* unit,
                         std::uint8_t precision, float minimum, float maximum);

  // Puts unit, precision and range back to what the channel was CREATED with.
  // Deactivating a calibration has to do this: leaving the channel reading
  // "498311 g" after the grams were taken away is worse than never having
  // calibrated it, because the number now looks plausible and is not.
  Status resetPresentation(ChannelHandle handle);

  // --- listeners -----------------------------------------------------------
  Status addListener(ChannelListener listener, void* context);
  Status removeListener(ChannelListener listener, void* context);
  Status addLifecycleListener(ChannelLifecycleListener listener, void* context);

  // --- housekeeping --------------------------------------------------------
  // Marks channels stale when no sample arrived within 3 expected intervals,
  // and propagates a source device's failure to its channels.
  void tick(Micros now);
  void setSourceQuality(DeviceHandle device, ChannelQuality quality);

  // --- diagnostics ---------------------------------------------------------
  std::uint32_t publishedSamples() const { return publishedSamples_; }
  std::uint32_t suppressedSamples() const { return suppressedSamples_; }

 private:
  // What the channel was declared as, kept so a calibration can be undone.
  // Fourteen bytes per channel; the alternative is re-deriving it from
  // devices.json and the module manifest, which duplicates DeviceManager's
  // naming rules in a second place.
  struct Declared {
    UnitString unit;
    float minimum = 0.0f;
    float maximum = 0.0f;
    std::uint8_t precision = 2;
  };

  struct Slot {
    bool active = false;
    ChannelDescriptor descriptor;
    Declared declared;
    ChannelValue value;
    IProcessor* const* stages = nullptr;
    std::uint8_t stageCount = 0;
    std::int8_t calibrationStage = -1;
  };

  struct Listener {
    ChannelListener fn = nullptr;
    void* context = nullptr;
  };

  struct LifecycleListener {
    ChannelLifecycleListener fn = nullptr;
    void* context = nullptr;
  };

  static constexpr std::size_t kMaxLifecycleListeners = 4;

  void notifyLifecycle(ChannelHandle handle, bool created);
  // Delivers a channel value to every listener.  Called for a new sample AND
  // for a quality transition that arrives without one (stale, faulted).
  void notify(ChannelHandle handle, const Slot& slot);

  Slot* slotFor(ChannelHandle handle);
  const Slot* slotFor(ChannelHandle handle) const;
  ChannelQuality classify(const ChannelDescriptor& descriptor, float value) const;
  void commit(Slot& slot, ChannelHandle handle, Micros now);

  const IClock& clock_;
  Slot slots_[limits::kMaxChannels];
  std::size_t activeCount_ = 0;

  Listener listeners_[limits::kMaxChannelSubscribers];
  std::size_t listenerCount_ = 0;

  LifecycleListener lifecycleListeners_[kMaxLifecycleListeners];
  std::size_t lifecycleListenerCount_ = 0;

  OutputSink outputSink_ = nullptr;
  void* outputSinkContext_ = nullptr;

  std::uint32_t publishedSamples_ = 0;
  std::uint32_t suppressedSamples_ = 0;
};

}  // namespace lc
