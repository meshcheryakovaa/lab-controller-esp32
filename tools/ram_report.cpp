// =============================================================================
//  tools/ram_report.cpp — where the static RAM actually goes.
//
//  Every long-lived object in this firmware is a fixed-capacity container sized
//  by a constant in core/Types.h.  That is a deliberate trade (ADR-0007: no
//  allocation on the acquisition path), and its bill arrives all at once, in
//  .bss, at link time — which is the worst possible moment to discover it:
//
//      region `dram0_0_seg' overflowed by 10600 bytes
//
//  That message names no object.  This program does.  It prints the size of
//  every manager and the cost of one more device, channel, output and so on, so
//  a capacity change can be argued about in bytes before anyone waits for a
//  link to fail.
//
//  HONESTY ABOUT THE NUMBERS.  This runs on the host, where a pointer is eight
//  bytes and the ESP32's is four.  Every figure below is therefore an
//  UPPER BOUND for the target; structures that are mostly arrays of PODs (the
//  big ones — channels, devices, experiments) come out within a few percent,
//  and structures that are mostly pointers and vtables come out noticeably
//  high.  Use it to compare, to find the fat, and to sanity-check a change;
//  use `pio run` for the number that decides whether it fits.
// =============================================================================
#include <cstdio>

#include "api/RestApi.h"
#include "api/TelemetryBatcher.h"
#include "app/SystemManager.h"
#include "core/EventBus.h"
#include "core/ModuleRegistry.h"
#include "core/ResourceManager.h"
#include "core/Scheduler.h"
#include "services/AuthManager.h"
#include "services/CalibrationManager.h"
#include "services/ChannelManager.h"
#include "services/ControlManager.h"
#include "services/DataLogger.h"
#include "services/DeviceManager.h"
#include "services/ExperimentEngine.h"
#include "services/OutputManager.h"
#include "services/ProcessingManager.h"
#include "services/SafetyManager.h"
#include "storage/ConfigApplier.h"
#include "storage/ConfigStorage.h"
#include "storage/DashboardStore.h"
#include "storage/ExperimentStore.h"
#include "storage/LogStore.h"
#include "storage/RunLog.h"

using namespace lc;

namespace {

struct Row {
  const char* name;
  std::size_t bytes;
  std::size_t capacity;  // 0 when the object does not scale with a limit
  const char* limit;
};

void print(const Row* rows, std::size_t count) {
  std::size_t total = 0;
  std::printf("%-22s %10s %10s  %s\n", "object", "bytes", "per unit", "limit");
  std::printf("%-22s %10s %10s  %s\n", "----------------------", "----------",
              "----------", "-----");
  for (std::size_t i = 0; i < count; ++i) {
    total += rows[i].bytes;
    if (rows[i].capacity > 0) {
      std::printf("%-22s %10zu %10zu  %s = %zu\n", rows[i].name, rows[i].bytes,
                  rows[i].bytes / rows[i].capacity, rows[i].limit,
                  rows[i].capacity);
    } else {
      std::printf("%-22s %10zu %10s  %s\n", rows[i].name, rows[i].bytes, "-",
                  rows[i].limit);
    }
  }
  std::printf("%-22s %10zu\n", "TOTAL", total);
}

}  // namespace

int main() {
  const Row rows[] = {
      {"ChannelManager", sizeof(ChannelManager), limits::kMaxChannels,
       "kMaxChannels"},
      {"DeviceManager", sizeof(DeviceManager), limits::kMaxDevices,
       "kMaxDevices"},
      {"ExperimentEngine", sizeof(ExperimentEngine), limits::kMaxExperimentSteps,
       "kMaxExperimentSteps"},
      {"CalibrationManager", sizeof(CalibrationManager),
       limits::kMaxActiveCalibrations, "kMaxActiveCalibrations"},
      {"ResourceManager", sizeof(ResourceManager), limits::kMaxResourceClaims,
       "kMaxResourceClaims"},
      {"ControlManager", sizeof(ControlManager), limits::kMaxControlLoops,
       "kMaxControlLoops"},
      {"DataLogger", sizeof(DataLogger), limits::kMaxLoggedChannels,
       "kMaxLoggedChannels"},
      {"SafetyManager", sizeof(SafetyManager), limits::kMaxSafetyLimits,
       "kMaxSafetyLimits"},
      {"Scheduler", sizeof(Scheduler), limits::kMaxSchedulerTasks,
       "kMaxSchedulerTasks"},
      {"ProcessingManager", sizeof(ProcessingManager), 0, "-"},
      {"OutputManager", sizeof(OutputManager), limits::kMaxOutputs,
       "kMaxOutputs"},
      {"TelemetryBatcher", sizeof(TelemetryBatcher), 0, "-"},
      {"ModuleRegistry", sizeof(ModuleRegistry), limits::kMaxModuleTypes,
       "kMaxModuleTypes"},
      {"EventBus", sizeof(EventBus), limits::kMaxEventSubscribers,
       "kMaxEventSubscribers"},
      {"RunLog", sizeof(RunLog), limits::kMaxRunRecords, "kMaxRunRecords"},
      {"LogStore", sizeof(LogStore), limits::kMaxLogSessions, "kMaxLogSessions"},
      {"DashboardStore", sizeof(DashboardStore), limits::kMaxDashboards,
       "kMaxDashboards"},
      {"ExperimentStore", sizeof(ExperimentStore), 0, "-"},
      {"ConfigStorage", sizeof(ConfigStorage), 0, "-"},
      {"ConfigApplier", sizeof(ConfigApplier), 0, "-"},
      {"AuthManager", sizeof(AuthManager), 0, "-"},
      {"SystemManager", sizeof(SystemManager), 0, "-"},
      {"RestApi", sizeof(RestApi), 0, "-"},
  };
  print(rows, sizeof(rows) / sizeof(rows[0]));

  std::printf("\nrecord sizes\n");
  std::printf("  ChannelDescriptor %zu\n", sizeof(ChannelDescriptor));
  std::printf("  ChannelValue      %zu\n", sizeof(ChannelValue));
  std::printf("\nThe ESP32 DevKit has about 176 KB of DRAM for .data + .bss;\n"
              "everything above competes with the heap, the Wi-Fi stack and\n"
              "the HTTP task's 8 KB stack for it.\n");
  return 0;
}
