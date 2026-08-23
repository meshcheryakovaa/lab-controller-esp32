// =============================================================================
//  api/RestApi.h — REST v1, independent of any HTTP library.
//
//  handle(request, response) is a pure function of the system state: no
//  sockets, no Arduino, no threads.  That is what lets the whole API — routing,
//  validation, dry-run, error envelopes, the create/rollback dance — be tested
//  on a host, including the failure paths that matter most.
//
//  WRITES GO THROUGH THE CONFIGURATION FILE (ADR-0010).  Creating a device is:
//      parse → validate → write devices.json → apply → (on failure) roll the
//      file back.
//  Never the other way round.  A device that exists in RAM but not on disk
//  would vanish at the next reboot, which is the worst possible behaviour for
//  an instrument someone just configured.
// =============================================================================
#pragma once

#include "api/ApiTypes.h"
#include "api/PathRouter.h"
#include "api/Serializers.h"
#include "api/SystemMetrics.h"
#include "app/SystemManager.h"
#include "buses/IBusProvider.h"
#include "core/ModuleRegistry.h"
#include "services/AuthManager.h"
#include "services/CloudManager.h"
#include "services/INetworkManager.h"
#include "services/DeviceManager.h"
#include "services/ProcessingManager.h"
#include "storage/ConfigApplier.h"
#include "storage/ConfigStorage.h"

namespace lc {

class RestApi {
 public:
  struct Services {
    const IClock* clock = nullptr;
    ModuleRegistry* registry = nullptr;
    ResourceManager* resources = nullptr;
    ChannelManager* channels = nullptr;
    Scheduler* scheduler = nullptr;
    EventBus* events = nullptr;
    DeviceManager* devices = nullptr;
    ProcessingManager* processing = nullptr;
    CalibrationManager* calibrations = nullptr;
    OutputManager* outputs = nullptr;
    SafetyManager* safety = nullptr;
    ControlManager* control = nullptr;
    ExperimentEngine* experiments = nullptr;
    RunLog* runLog = nullptr;
    DataLogger* logger = nullptr;
    LogStore* logStore = nullptr;
    // Optional: without it every request is treated as signed in, which is what
    // a build with no credential support means.  With it, the policy in
    // RestApi.cpp decides — see requireWriteAccess() and requireConfirmation().
    AuthManager* auth = nullptr;
    ConfigStorage* storage = nullptr;
    ConfigApplier* applier = nullptr;
    SystemManager* system = nullptr;
    IBusProvider* buses = nullptr;
    const ISystemMetrics* metrics = nullptr;
    // M16.  Optional: a build without a radio simply has no /network routes,
    // which is honest — the alternative is an endpoint that reports a network
    // nobody can configure.
    INetworkManager* network = nullptr;
    // M17.  Optional in the same way and for the same reason: a build with no
    // cloud has no /cloud routes rather than an endpoint describing one.
    CloudManager* cloud = nullptr;
    // The account-linking half.  Split from CloudManager on purpose: the
    // manager decides what to upload, this holds credentials, and the routes
    // that touch secrets are therefore visibly a different set.
    ICloudAccount* cloudAccount = nullptr;

    // Called by POST /system/reboot.  Injected so a test can assert the request
    // was accepted without the test process exiting.
    void (*reboot)(void* context) = nullptr;
    void* rebootContext = nullptr;
  };

  explicit RestApi(const Services& services) : s_(services) {}

  // Entry point.  Always fills `response`; never throws, never blocks.
  void handle(const ApiRequest& request, ApiResponse& response);

  // When true, every non-GET request needs `request.authenticated`.
  // Off by default in Milestone 3; Milestone 11 turns it on with real sessions.
  void setRequireAuthForWrites(bool required) { requireAuth_ = required; }

  std::uint32_t requestCount() const { return requests_; }
  std::uint32_t errorCount() const { return errors_; }

 private:
  // --- route groups --------------------------------------------------------
  void handleSystem(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleDiagnostics(ApiResponse&);
  void handleModules(const PathSegments&, ApiResponse&);
  void handleGpio(ApiResponse&);
  void handleBuses(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleDevices(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleChannels(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleProcessing(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleCalibrations(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleDashboards(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleOutputs(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleControl(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleExperiments(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleLogs(const ApiRequest&, const PathSegments&, ApiResponse&);
  void describeLogging(JsonObject out) const;
  // M15: the offload queue of one continuous session.
  void describeSegments(const char* id, ApiResponse& response);
  void handleAuth(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleFirmware(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleConfig(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleNetwork(const ApiRequest&, const PathSegments&, ApiResponse&);
  void handleCloud(const ApiRequest&, const PathSegments&, ApiResponse&);
  void describeCloud(JsonObject out) const;

  // --- experiments ---------------------------------------------------------
  void describeRunState(JsonObject out) const;
  void saveExperiment(const ApiRequest&, const char* key, ApiResponse&);
  void experimentAction(const ApiRequest&, const char* key, const char* action,
                        ApiResponse&);

  // --- control operations --------------------------------------------------
  void listControl(ApiResponse&);
  void replaceControl(const ApiRequest&, ApiResponse&);
  void loopAction(const ApiRequest&, const char* id, const char* action,
                  ApiResponse&);
  // Writes the running configuration back to control.json.  Called after a
  // live edit so the number on the screen and the number in the file are the
  // same number after a reboot.
  Status persistControl();
  void describeLoop(const ControlLoop& loop, JsonObject out) const;
  void describeRule(const ControlRule& rule, JsonObject out) const;
  void describeLimit(const SafetyLimit& limit, JsonObject out) const;

  // --- device operations ---------------------------------------------------
  void createDevice(const ApiRequest&, ApiResponse&);
  void patchDevice(const ApiRequest&, const char* key, ApiResponse&);
  void deleteDevice(const char* key, ApiResponse&);
  void deviceAction(const ApiRequest&, const char* key, const char* action,
                    ApiResponse&);
  void describeDevice(const DeviceRecord& record, JsonObject out) const;

  // --- calibration operations ----------------------------------------------
  void solveCalibration(const ApiRequest&, ApiResponse&);
  void listCalibrations(const ApiRequest&, ApiResponse&);
  void createCalibration(const ApiRequest&, ApiResponse&);
  void activateCalibration(const char* id, bool active, ApiResponse&);
  void deleteCalibration(const char* id, ApiResponse&);
  // Rebuilds the channel's pipeline from the stored documents.  Called after
  // anything that changes which calibration is active, because the running
  // pipeline holds resolved coefficients and would otherwise keep the old ones.
  Status reapplyChannel(const char* channelKey, JsonDocument& calibrations);

  // Adds the safety state of an output channel to its descriptor.  Every
  // reader of a channel sees it, because "what is this actuator doing and for
  // how long is it allowed to keep doing it" is not a separate screen.
  void describeOutput(ChannelHandle handle, JsonObject out) const;

  // --- dashboard operations ------------------------------------------------
  void saveDashboard(const ApiRequest&, JsonDocument& stored, const char* pathKey,
                     ApiResponse&);
  void describeDashboardHealth(JsonObjectConst dashboard, JsonObject out) const;

  // --- helpers -------------------------------------------------------------
  bool parseBody(const ApiRequest&, JsonDocument& out, ApiResponse&);
  bool requireWriteAccess(const ApiRequest&, ApiResponse&);
  // True when the caller proved they are the person, not just the browser.
  // Required by everything that REMOVES a protection: replacing the firmware,
  // importing a configuration, changing the password, disabling an interlock.
  bool requireConfirmation(const ApiRequest&, ApiResponse&, const char* what);
  // Whether this exact request is exempt from authentication because refusing
  // it could only ever make the rig less safe (§49, ADR-0020).
  static bool isSafetyExempt(const ApiRequest&, const PathSegments&);
  bool signedIn(const ApiRequest&) const;
  Status persistDevices(JsonDocument& document);

  Services s_;
  // Whether the request being handled right now is one of the safety
  // exemptions.  Set once in handle(); the per-route checks read it instead of
  // each re-deriving the rule from the path — one place decides, and the other
  // places cannot disagree with it.
  bool exemptRequest_ = false;
  bool requireAuth_ = false;
  std::uint32_t requests_ = 0;
  std::uint32_t errors_ = 0;
};

}  // namespace lc
