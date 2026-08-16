// =============================================================================
//  services/ProcessingManager.h — owns the IProcessor instances (§14).
//
//  ChannelManager knows how to RUN a pipeline; it does not own the stages.
//  This class does: it builds them from the registry, keeps the array alive for
//  as long as the channel exists, and destroys them when it goes away.
//
//  It subscribes to ChannelManager's lifecycle hook, so a pipeline can never
//  outlive its channel — that would leave ChannelManager holding dangling
//  IProcessor pointers, which is exactly the kind of failure that shows up
//  three hours into an experiment and nowhere else.
// =============================================================================
#pragma once

#include "core/ConfigView.h"
#include "core/Error.h"
#include "core/IModule.h"
#include "core/ModuleRegistry.h"
#include "services/ChannelManager.h"

namespace lc {

// Where a pipeline description comes from.  Implemented over ArduinoJson by
// storage/, and by a trivial struct in tests — which is the point.
class IPipelineSource {
 public:
  virtual ~IPipelineSource() = default;

  virtual std::size_t stageCount() const = 0;
  virtual const char* stageType(std::size_t index) const = 0;
  virtual const IConfigView& stageConfig(std::size_t index) const = 0;

  // Which stage's output is recorded as the channel's calibrated value.
  //   >= 0 : explicit index
  //   -1   : no calibration in this chain
  //   -2   : auto — the first stage whose typeId is "calibration"
  virtual std::int8_t calibrationStage() const { return kAutoCalibrationStage; }

  static constexpr std::int8_t kAutoCalibrationStage = -2;
};

class ProcessingManager {
 public:
  ProcessingManager(ModuleRegistry& registry, ChannelManager& channels);
  ~ProcessingManager();

  ProcessingManager(const ProcessingManager&) = delete;
  ProcessingManager& operator=(const ProcessingManager&) = delete;

  // Subscribes to channel lifecycle events.  Call once, after construction.
  Status begin();

  // Replaces the channel's pipeline.  On any failure nothing is changed:
  // the new chain is built completely before the old one is swapped in, so a
  // bad filter configuration never leaves a channel half-processed.
  Status apply(ChannelHandle handle, const IPipelineSource& source);

  Status clear(ChannelHandle handle);
  void clearAll();

  std::size_t stageCount(ChannelHandle handle) const;
  const char* stageType(ChannelHandle handle, std::size_t index) const;
  std::size_t totalStages() const;

 private:
  struct Chain {
    IProcessor* stages[limits::kMaxProcessorsPerChannel] = {nullptr};
    std::uint8_t count = 0;
    std::int8_t calibrationStage = -1;
  };

  static void lifecycleTrampoline(ChannelHandle handle, bool created,
                                  void* context);
  void destroy(Chain& chain);

  ModuleRegistry& registry_;
  ChannelManager& channels_;
  Chain chains_[limits::kMaxChannels];
};

}  // namespace lc
