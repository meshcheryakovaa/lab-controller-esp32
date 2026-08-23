// =============================================================================
//  tools/host_server.cpp — the firmware's REST API, running on a PC.
//
//  This is NOT a mock.  It links the real RestApi, DeviceManager,
//  ChannelManager, ConfigStorage and the real drivers, and serves them over a
//  plain socket.  The browser talks to the same code that runs on the ESP32;
//  only the transport and the filesystem are different.
//
//  What that buys:
//    * the whole web interface can be developed, and screenshotted, with no
//      board on the desk (§59, extended to the UI);
//    * a UI bug and a firmware bug are told apart immediately, because there is
//      no second implementation of the API to disagree with;
//    * `?dry_run=1` validation in the browser exercises the real
//      ResourceManager and the real manifests, including "GPIO34 has no output
//      driver".
//
//  Milestone 14 ADDED WebSocket telemetry (it used to be deliberately absent).
//  The reason it had to arrive: M13's chart and M14's recorder are both fed by
//  the socket, and their acceptance criteria are about what happens when it
//  DROPS.  A bench that cannot drop a connection cannot test the behaviour that
//  matters, and screenshots of an empty chart were the visible symptom.
//
//  What follows is the smallest RFC 6455 server that is honestly a WebSocket:
//  the handshake, text frames out, masked frames in, ping/pong and close.  It
//  serves the real TelemetryBatcher, so the browser receives the frames the
//  firmware composes, not a fixture.
//
//  Historical note — the handshake and
//  framing would be a hundred lines of transport code with nothing to learn
//  from; the dashboard seeds its values from GET /channels?values=1 instead and
//  simply shows the link as offline.
//
//  Build:  make -C tools host-server
//  Run:    ./tools/host_server [port] [config-dir] [static-dir]
// =============================================================================
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "api/RestApi.h"
#include "api/TelemetryBatcher.h"
#include "app/SystemManager.h"
#include "platform/host/HostBusProvider.h"
#include "platform/host/HostClock.h"
#include "platform/host/HostRandom.h"
#include "services/CloudManager.h"
#include "storage/CloudUploadQueue.h"
#include "storage/PosixBackend.h"

using namespace lc;

namespace {

// Milestone 14: a controller identity that is stable for a given configuration
// directory.  The real board derives it from the eFuse MAC; here two dev
// servers started on two config roots must still look like two rigs, because
// the browser files local recordings under this string and merging them would
// be silent data corruption.
class HostSystemMetrics final : public NullSystemMetrics {
 public:
  explicit HostSystemMetrics(const std::string& root) {
    std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a
    for (const unsigned char c : root) {
      hash ^= c;
      hash *= 1099511628211ULL;
    }
    std::snprintf(id_, sizeof(id_), "lc-%012llx",
                  static_cast<unsigned long long>(hash & 0xFFFFFFFFFFFFULL));
  }
  const char* controllerId() const override { return id_; }

 private:
  char id_[16] = {0};
};


// ---------------------------------------------------------------------------
//  A network manager with no radio, so the M16 page can be driven on a PC.
//
//  It models the parts that matter to the interface and to ADR-0022: a join
//  takes time (so "connecting" is a state the page has to render, not a blink),
//  a wrong password fails without disturbing the stored network, and the
//  fallback access point never goes away while an attempt is in flight.
//
//  The password is kept only so the fake can decide whether the join succeeds.
//  It is never handed back — like the real one, NetworkStatus has no field for
//  it.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  A cloud account and uploader with no cloud, so the M17 page can be driven
//  on a PC.  It models the parts the interface has to render: a queue that
//  drains, a Device Code prompt, and a job that stops with a conflict.
// ---------------------------------------------------------------------------
/** A cloud that stores objects in memory, so the M17 page can be exercised. */
class HostCloudProvider final : public lc::ICloudProvider {
 public:
  const char* name() const override { return "yandex"; }
  bool authorized() const override { return true; }
  lc::CloudResult refreshAuthorizationIfNeeded() override { return lc::cloudOk(); }
  lc::CloudResult ensureDirectory(const char*) override { return lc::cloudOk(); }
  lc::CloudResult stat(const char* path, lc::CloudObjectInfo& out) override {
    const auto found = objects_.find(path);
    if (found == objects_.end()) { out = lc::CloudObjectInfo{}; return lc::cloudOk(); }
    out.exists = true; out.isFile = true;
    out.size = found->second.first;
    out.md5.assign(found->second.second.c_str());
    return lc::cloudOk();
  }
  lc::CloudResult upload(const char* remotePath, lc::IStorageBackend& storage,
                         const char* localPath, std::uint64_t bytes,
                         lc::ICloudUploadObserver* observer) override {
    lc::Md5 md5;
    char buffer[4096];
    std::size_t offset = 0;
    while (offset < bytes) {
      const std::size_t want = static_cast<std::size_t>(
          (bytes - offset) < sizeof(buffer) ? (bytes - offset) : sizeof(buffer));
      const lc::Result<std::size_t> read =
          storage.readAt(localPath, offset, buffer, want);
      if (!read.ok() || read.value() == 0) break;
      md5.update(reinterpret_cast<const std::uint8_t*>(buffer), read.value());
      offset += read.value();
      if (observer) observer->onUploadProgress(offset, bytes);
    }
    char hex[lc::Md5::kTextBytes];
    md5.finishHex(hex);
    objects_[remotePath] = {offset, hex};
    return lc::cloudOk();
  }
  lc::CloudResult move(const char* from, const char* to) override {
    const auto found = objects_.find(from);
    if (found == objects_.end()) {
      return lc::cloudFail(lc::CloudFailure::kPermanent, lc::ErrorCode::kNotFound, "gone");
    }
    objects_[to] = found->second;
    objects_.erase(found);
    return lc::cloudOk();
  }
  lc::CloudResult remove(const char* path) override {
    objects_.erase(path);
    return lc::cloudOk();
  }
 private:
  std::map<std::string, std::pair<std::uint64_t, std::string>> objects_;
};

class HostCloudAccount final : public lc::ICloudAccount {
 public:
  bool configured() const override { return !clientId_.empty(); }
  bool clientSecretSet() const override { return !secret_.empty(); }
  bool authorized() const override {
    // Settles the pending link first: the status route reads authorized()
    // before linkState(), and a fake that answered from a stale flag produced a
    // screenshot claiming "linked" and "not linked yet" at the same time.
    linkState();
    return linked_;
  }
  lc::EpochMs tokenExpiresAtEpochMs() const override {
    return linked_ ? 1787482800000ull : 0;
  }
  lc::Status setClientId(const char* v) override { clientId_ = v ? v : ""; return lc::ok(); }
  lc::Status setClientSecret(const char* v) override { secret_ = v ? v : ""; return lc::ok(); }
  lc::Status clearClientSecret() override { secret_.clear(); return lc::ok(); }

  lc::CloudResult beginLink() override {
    state_ = lc::CloudLinkState::kWaitingUser;
    startedAt_ = std::chrono::steady_clock::now();
    return lc::cloudOk();
  }
  lc::CloudLinkState linkState() const override {
    if (state_ == lc::CloudLinkState::kWaitingUser) {
      const auto elapsed = std::chrono::steady_clock::now() - startedAt_;
      // The operator "types the code" after four seconds, so the page's
      // waiting state is actually visible in a screenshot.
      if (elapsed >= std::chrono::seconds(4)) {
        state_ = lc::CloudLinkState::kAuthorized;
        linked_ = true;
      }
    }
    return state_;
  }
  lc::CloudLinkPrompt linkPrompt() const override {
    lc::CloudLinkPrompt prompt;
    prompt.userCode.assign("ABCD-1234");
    prompt.verificationUrl.assign("https://oauth.yandex.ru/device");
    const auto elapsed = std::chrono::steady_clock::now() - startedAt_;
    const auto left = std::chrono::seconds(300) -
        std::chrono::duration_cast<std::chrono::seconds>(elapsed);
    prompt.secondsRemaining = left.count() > 0 ? static_cast<std::uint32_t>(left.count()) : 0;
    return prompt;
  }
  lc::CloudResult checkAccess() override { return lc::cloudOk(); }
  lc::CloudResult disconnect() override {
    secret_.clear(); linked_ = false; state_ = lc::CloudLinkState::kIdle;
    return lc::cloudOk();
  }
  bool storageIsEncrypted() const override { return false; }

 private:
  std::string clientId_;
  std::string secret_;
  mutable bool linked_ = false;
  mutable lc::CloudLinkState state_ = lc::CloudLinkState::kIdle;
  mutable std::chrono::steady_clock::time_point startedAt_;
};

class HostNetworkManager final : public lc::INetworkManager {
 public:
  HostNetworkManager() {
    status_.state = lc::NetworkState::kApOnly;
    status_.accessPointActive = true;
    status_.accessPointSsid.assign("LAB-CONTROLLER-A1B2C3");
    status_.accessPointIp.assign("192.168.4.1");
    status_.hostname.assign("lab-controller-a1b2c3");
  }

  lc::NetworkStatus status() const override {
    std::lock_guard<std::mutex> held(mutex_);
    if (status_.testing) {
      const auto elapsed = std::chrono::steady_clock::now() - startedAt_;
      if (elapsed >= std::chrono::seconds(3)) {
        // "password" is the fake's shared secret; anything else times out, so
        // the failure path is reachable from the interface on a PC.
        if (pendingPassword_ == "password" || pendingPassword_.empty()) {
          status_.testing = false;
          status_.configured = true;
          status_.ssid = pendingSsid_;
          status_.state = lc::NetworkState::kStationConnected;
          status_.stationConnected = true;
          status_.stationIp.assign("192.168.1.74");
          status_.rssi = -57;
          ++status_.reconnects;
        } else {
          status_.testing = false;
          status_.state = status_.configured
              ? lc::NetworkState::kApStationFallback : lc::NetworkState::kApOnly;
          // The station really is down, so every field that describes it has to
          // say so.  Leaving a stale address behind produced a card that showed
          // AP_STA_FALLBACK and a live IP at the same time — a real controller
          // reports WiFi.status() != WL_CONNECTED and none of these.
          status_.stationConnected = false;
          status_.stationIp.assign("");
          status_.rssi = 0;
          status_.lastError = lc::fail(lc::ErrorCode::kTimeout,
              "could not join that network; check the password");
        }
      }
    }
    return status_;
  }

  lc::Status beginScan() override {
    std::lock_guard<std::mutex> held(mutex_);
    if (status_.testing) {
      return lc::fail(lc::ErrorCode::kResourceBusy, "a connection test is running");
    }
    scan_ = lc::ScanState::kComplete;
    return lc::ok();
  }
  lc::ScanState scanState() const override { return scan_; }

  std::size_t scanResults(lc::NetworkCandidate* out,
                          std::size_t capacity) const override {
    static const struct { const char* ssid; int rssi; int channel; bool secured; }
        kFound[] = {
          {"HomeWiFi", -48, 6, true},
          {"Lab-5GHz", -61, 36, true},
          {"Guest", -71, 11, true},
          {"neighbour-2.4", -83, 1, true},
        };
    const std::size_t count =
        (sizeof(kFound) / sizeof(kFound[0])) < capacity
            ? (sizeof(kFound) / sizeof(kFound[0])) : capacity;
    for (std::size_t i = 0; i < count; ++i) {
      out[i].ssid.assign(kFound[i].ssid);
      out[i].rssi = kFound[i].rssi;
      out[i].channel = static_cast<std::uint8_t>(kFound[i].channel);
      out[i].secured = kFound[i].secured;
    }
    return count;
  }

  lc::Status testCredentials(const char* ssid, const char* password) override {
    std::lock_guard<std::mutex> held(mutex_);
    if (status_.testing) {
      return lc::fail(lc::ErrorCode::kResourceBusy,
                      "a connection test is already running");
    }
    pendingSsid_.assign(ssid);
    pendingPassword_ = password != nullptr ? password : "";
    startedAt_ = std::chrono::steady_clock::now();
    status_.testing = true;
    status_.state = lc::NetworkState::kStationConnecting;
    status_.lastError = lc::Error{};
    // The access point stays up throughout — that is the promise being modelled.
    status_.accessPointActive = true;
    return lc::ok();
  }

  lc::Status clearCredentials() override {
    std::lock_guard<std::mutex> held(mutex_);
    const lc::FixedString<lc::kHostnameLength> keptHostname = status_.hostname;
    status_ = lc::NetworkStatus{};
    status_.state = lc::NetworkState::kApOnly;
    status_.accessPointActive = true;
    status_.accessPointSsid.assign("LAB-CONTROLLER-A1B2C3");
    status_.accessPointIp.assign("192.168.4.1");
    status_.hostname = keptHostname;
    return lc::ok();
  }

  lc::Status setHostname(const char* hostname) override {
    if (!lc::INetworkManager::hostnameIsValid(hostname)) {
      return lc::fail(lc::ErrorCode::kInvalidArgument, "bad hostname");
    }
    std::lock_guard<std::mutex> held(mutex_);
    status_.hostname.assign(hostname);
    return lc::ok();
  }

 private:
  mutable std::mutex mutex_;
  mutable lc::NetworkStatus status_;
  lc::ScanState scan_ = lc::ScanState::kIdle;
  lc::FixedString<lc::kSsidLength> pendingSsid_;
  std::string pendingPassword_;
  std::chrono::steady_clock::time_point startedAt_;
};

struct Rig {
  // Declared first so `metrics` below can be initialised from it: members are
  // constructed in declaration order, and reading one that is not yet built is
  // the kind of bug this project spends its afternoons not having.
  std::string root;
  platform::HostClock clock;
  platform::PosixBackend backend;
  MemoryBootCounter bootCounter;
  ModuleRegistry registry;
  ResourceManager resources{ChipProfile::esp32()};
  ChannelManager channels{clock};
  Scheduler scheduler{clock};
  EventBus events;
  DeviceManager devices{clock, registry, resources, channels, scheduler, events};
  ProcessingManager processing{registry, channels};
  OutputManager outputs{clock, channels, scheduler, events};
  SafetyManager safety{clock, channels, outputs, scheduler, events};
  ControlManager control{clock, channels, outputs, scheduler, events};
  ConfigStorage storage{backend, events};
  CalibrationManager calibrations;
  ExperimentEngine experiments{clock,   channels, outputs, control,
                               devices, scheduler, events};
  RunLog runLog{backend, storage, devices, &calibrations};
  DataLogger logger{clock, channels, scheduler, events};
  LogStore logStore{backend, storage, &calibrations};
  platform::HostRandom random;
  AuthManager auth{clock, random, backend, events};
  ConfigApplier applier{devices, processing, channels, &calibrations};
  HostSystemMetrics metrics{root};
  HostNetworkManager network;
  lc::CloudUploadQueue cloudQueue{backend};
  lc::CloudManager cloud{clock, cloudQueue, events};
  HostCloudAccount cloudAccount;
  HostCloudProvider cloudProvider;
  platform::HostBusProvider buses;
  SystemManager system;
  RestApi api;

  explicit Rig(std::string configRoot)
      : root(configRoot),
        backend(std::move(configRoot)),
        system(makeSystemServices()),
        api(makeApiServices()) {
    applier.setControl(&control);
    applier.setSafety(&safety);
    cloud.setProvider(&cloudProvider);
    cloud.setNetwork(&network);
    cloud.setStorage(&backend);
    cloud.setControllerId("esp32-a1b2c3");
    cloud.setEnabled(true);
    cloud.begin();
  }

  SystemManager::Services makeSystemServices() {
    SystemManager::Services services;
    services.clock = &clock;
    services.registry = &registry;
    services.resources = &resources;
    services.channels = &channels;
    services.scheduler = &scheduler;
    services.events = &events;
    services.devices = &devices;
    services.processing = &processing;
    services.calibrations = &calibrations;
    services.outputs = &outputs;
    services.safety = &safety;
    services.control = &control;
    services.experiments = &experiments;
    services.runLog = &runLog;
    services.logger = &logger;
    services.logStore = &logStore;
    services.auth = &auth;
    services.storage = &storage;
    services.applier = &applier;
    services.bootCounter = &bootCounter;
    services.buses = &buses;
    return services;
  }

  RestApi::Services makeApiServices() {
    RestApi::Services services;
    services.clock = &clock;
    services.registry = &registry;
    services.resources = &resources;
    services.channels = &channels;
    services.scheduler = &scheduler;
    services.events = &events;
    services.devices = &devices;
    services.processing = &processing;
    services.calibrations = &calibrations;
    services.outputs = &outputs;
    services.safety = &safety;
    services.control = &control;
    services.experiments = &experiments;
    services.runLog = &runLog;
    services.logger = &logger;
    services.logStore = &logStore;
    services.auth = &auth;
    services.storage = &storage;
    services.applier = &applier;
    services.system = &system;
    services.metrics = &metrics;
    services.network = &network;
    services.cloud = &cloud;
    services.cloudAccount = &cloudAccount;
    services.buses = &buses;
    return services;
  }
};

// Finds a header by name, ignoring case.  Returns the offset of the name.
std::size_t findHeader(const std::string& request, const char* lowercaseName) {
  const std::size_t length = std::strlen(lowercaseName);
  for (std::size_t i = 0; i + length <= request.size(); ++i) {
    // Only at the start of a line: "Set-Cookie" inside a body must not match.
    if (i != 0 && !(request[i - 1] == '\n')) continue;
    bool matches = true;
    for (std::size_t j = 0; j < length; ++j) {
      const char c = request[i + j];
      const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
      if (lower != lowercaseName[j]) {
        matches = false;
        break;
      }
    }
    if (matches) return i;
  }
  return std::string::npos;
}

std::string readAll(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

const char* mimeFor(const std::string& path) {
  if (path.size() > 5 && path.compare(path.size() - 5, 5, ".html") == 0) return "text/html";
  if (path.size() > 3 && path.compare(path.size() - 3, 3, ".js") == 0) return "text/javascript";
  if (path.size() > 4 && path.compare(path.size() - 4, 4, ".css") == 0) return "text/css";
  if (path.size() > 5 && path.compare(path.size() - 5, 5, ".json") == 0) return "application/json";
  return "application/octet-stream";
}

void send(int socketFd, int status, const char* contentType,
          const std::string& body, const std::string& setCookie = {}) {
  const char* reason = (status == 200) ? "OK"
                     : (status == 201) ? "Created"
                     : (status == 404) ? "Not Found"
                                       : "Error";
  std::ostringstream head;
  head << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
       << "Content-Type: " << contentType << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Methods: GET,POST,PUT,PATCH,DELETE,OPTIONS\r\n"
       << "Access-Control-Allow-Headers: Content-Type\r\n";
  if (!setCookie.empty()) head << "Set-Cookie: " << setCookie << "\r\n";
  head << "Connection: close\r\n\r\n";
  const std::string header = head.str();
  ::send(socketFd, header.data(), header.size(), 0);
  ::send(socketFd, body.data(), body.size(), 0);
}


// -----------------------------------------------------------------------------
//  A small, real WebSocket server (RFC 6455) — Milestone 14.
//
//  Only what /ws/live needs: the handshake, unmasked text frames out, masked
//  frames in, ping/pong and close.  No extensions, no fragmentation of outgoing
//  messages (a telemetry batch is far below any sane limit).
// -----------------------------------------------------------------------------

// SHA-1, needed only by the handshake.  The firmware ships SHA-256 for
// passwords; RFC 6455 specifies SHA-1 for the accept key and nothing else, and
// it is not being used as a security primitive here.
class Sha1 {
 public:
  void update(const std::uint8_t* data, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
      buffer_[bufferLength_++] = data[i];
      if (bufferLength_ == 64) { block(buffer_); bufferLength_ = 0; }
      ++total_;
    }
  }

  void finish(std::uint8_t out[20]) {
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t one = 0x80;
    update(&one, 1);
    total_ -= 1;                       // padding is not message length
    const std::uint8_t zero = 0;
    while (bufferLength_ != 56) { update(&zero, 1); total_ -= 1; }
    std::uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(tail, 8);
    for (int i = 0; i < 5; ++i) {
      out[i * 4 + 0] = static_cast<std::uint8_t>(state_[i] >> 24);
      out[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
      out[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
      out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
  }

 private:
  static std::uint32_t rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

  void block(const std::uint8_t* chunk) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(chunk[i * 4]) << 24)
           | (static_cast<std::uint32_t>(chunk[i * 4 + 1]) << 16)
           | (static_cast<std::uint32_t>(chunk[i * 4 + 2]) << 8)
           | static_cast<std::uint32_t>(chunk[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
      else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
      const std::uint32_t temp = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d; state_[4] += e;
  }

  std::uint32_t state_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::uint8_t buffer_[64] = {0};
  std::size_t bufferLength_ = 0;
  std::uint64_t total_ = 0;
};

std::string base64(const std::uint8_t* data, std::size_t length) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < length; i += 3) {
    const std::uint32_t a = data[i];
    const std::uint32_t b = (i + 1 < length) ? data[i + 1] : 0;
    const std::uint32_t c = (i + 2 < length) ? data[i + 2] : 0;
    const std::uint32_t triple = (a << 16) | (b << 8) | c;
    out += kAlphabet[(triple >> 18) & 0x3F];
    out += kAlphabet[(triple >> 12) & 0x3F];
    out += (i + 1 < length) ? kAlphabet[(triple >> 6) & 0x3F] : '=';
    out += (i + 2 < length) ? kAlphabet[triple & 0x3F] : '=';
  }
  return out;
}

std::string headerValue(const std::string& request, const std::string& name) {
  // Header names are case-insensitive, and browsers do vary.
  std::string lowered;
  lowered.reserve(request.size());
  for (const char c : request) lowered += static_cast<char>(std::tolower(c));
  std::string needle = name;
  for (char& c : needle) c = static_cast<char>(std::tolower(c));
  needle += ":";
  const std::size_t at = lowered.find(needle);
  if (at == std::string::npos) return "";
  std::size_t start = at + needle.size();
  while (start < request.size() && (request[start] == ' ' || request[start] == '\t')) ++start;
  const std::size_t end = request.find("\r\n", start);
  return request.substr(start, end - start);
}

/**
 * Every connected browser, and the sink the TelemetryBatcher broadcasts into.
 *
 * One mutex around the whole registry: this is a bench tool serving a handful
 * of tabs, and a lock-free design here would buy nothing but bugs.
 */
class WebSocketRegistry final : public IWebSocketSink {
 public:
  bool upgrade(int fd, const std::string& request, Rig& rig) {
    const std::string key = headerValue(request, "Sec-WebSocket-Key");
    if (key.empty()) return false;
    static const char* kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 sha;
    sha.update(reinterpret_cast<const std::uint8_t*>(key.data()), key.size());
    sha.update(reinterpret_cast<const std::uint8_t*>(kMagic), std::strlen(kMagic));
    std::uint8_t digest[20];
    sha.finish(digest);

    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n"
             << "Upgrade: websocket\r\n"
             << "Connection: Upgrade\r\n"
             << "Sec-WebSocket-Accept: " << base64(digest, 20) << "\r\n\r\n";
    const std::string head = response.str();
    if (::send(fd, head.data(), head.size(), MSG_NOSIGNAL)
        != static_cast<ssize_t>(head.size())) {
      return false;
    }

    {
      std::lock_guard<std::mutex> guard(mutex_);
      clients_.push_back(fd);
    }

    // The same hello the ESP32 sends, including controller_id (M14 §7).
    std::ostringstream hello;
    hello << "{\"type\":\"hello\",\"firmware\":\"" << LC_FIRMWARE_VERSION << "\","
          << "\"schema_version\":" << ConfigStorage::kSchemaVersion << ","
          << "\"config_revision\":" << rig.storage.revision() << ","
          << "\"controller_id\":\"" << rig.metrics.controllerId() << "\","
          << "\"max_rate_hz\":" << TelemetryBatcher::kMaxRateHz << "}";
    sendTo(fd, hello.str());

    // Each client gets a reader thread: it only has to notice close frames and
    // answer pings, and it keeps a stalled browser from blocking the others.
    std::thread([this, fd]() { read(fd); }).detach();
    return true;
  }

  // --- IWebSocketSink ------------------------------------------------------
  std::size_t clientCount() const override {
    std::lock_guard<std::mutex> guard(mutex_);
    return clients_.size();
  }

  bool canSend() const override { return clientCount() > 0; }

  bool broadcast(const char* text, std::size_t length) override {
    std::vector<int> targets;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      targets = clients_;
    }
    for (const int fd : targets) sendTo(fd, std::string(text, length));
    return true;
  }

 private:
  void drop(int fd) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      for (std::size_t i = 0; i < clients_.size(); ++i) {
        if (clients_[i] != fd) continue;
        clients_.erase(clients_.begin() + static_cast<long>(i));
        break;
      }
    }
    ::close(fd);
  }

  void sendTo(int fd, const std::string& payload) {
    std::string frame;
    frame += static_cast<char>(0x81);                    // FIN + text
    const std::size_t n = payload.size();
    if (n < 126) {
      frame += static_cast<char>(n);
    } else if (n <= 0xFFFF) {
      frame += static_cast<char>(126);
      frame += static_cast<char>((n >> 8) & 0xFF);
      frame += static_cast<char>(n & 0xFF);
    } else {
      frame += static_cast<char>(127);
      for (int i = 7; i >= 0; --i) frame += static_cast<char>((n >> (8 * i)) & 0xFF);
    }
    frame += payload;
    std::lock_guard<std::mutex> guard(sendMutex_);
    if (::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL)
        != static_cast<ssize_t>(frame.size())) {
      // The browser went away mid-write; the reader thread will clean up.
    }
  }

  void read(int fd) {
    std::string buffer;
    char chunk[2048];
    for (;;) {
      const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
      if (got <= 0) break;
      buffer.append(chunk, static_cast<std::size_t>(got));
      for (;;) {
        if (buffer.size() < 2) break;
        const std::uint8_t opcode = static_cast<std::uint8_t>(buffer[0]) & 0x0F;
        const bool masked = (static_cast<std::uint8_t>(buffer[1]) & 0x80) != 0;
        std::uint64_t length = static_cast<std::uint8_t>(buffer[1]) & 0x7F;
        std::size_t offset = 2;
        if (length == 126) {
          if (buffer.size() < 4) break;
          length = (static_cast<std::uint8_t>(buffer[2]) << 8)
                 | static_cast<std::uint8_t>(buffer[3]);
          offset = 4;
        } else if (length == 127) {
          if (buffer.size() < 10) break;
          length = 0;
          for (int i = 0; i < 8; ++i) {
            length = (length << 8) | static_cast<std::uint8_t>(buffer[2 + i]);
          }
          offset = 10;
        }
        std::uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
          if (buffer.size() < offset + 4) break;
          for (int i = 0; i < 4; ++i) {
            mask[i] = static_cast<std::uint8_t>(buffer[offset + static_cast<std::size_t>(i)]);
          }
          offset += 4;
        }
        if (buffer.size() < offset + length) break;
        std::string payload = buffer.substr(offset, static_cast<std::size_t>(length));
        if (masked) {
          for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]);
          }
        }
        buffer.erase(0, offset + static_cast<std::size_t>(length));

        if (opcode == 0x8) { drop(fd); return; }        // close
        if (opcode == 0x9) {                             // ping -> pong
          std::string pong;
          pong += static_cast<char>(0x8A);
          pong += static_cast<char>(payload.size());
          pong += payload;
          std::lock_guard<std::mutex> guard(sendMutex_);
          ::send(fd, pong.data(), pong.size(), MSG_NOSIGNAL);
        }
        // Subscription messages are accepted and ignored: this bench serves
        // every channel, and pretending to filter would hide a real bug in the
        // firmware's own subscription handling rather than exercise it.
      }
    }
    drop(fd);
  }

  mutable std::mutex mutex_;
  std::mutex sendMutex_;
  std::vector<int> clients_;
};

WebSocketRegistry* g_sockets = nullptr;

}  // namespace

int main(int argc, char** argv) {
  const int port = (argc > 1) ? std::atoi(argv[1]) : 8080;
  const std::string configRoot = (argc > 2) ? argv[2] : "/tmp/lc-host";
  const std::string staticRoot = (argc > 3) ? argv[3] : "../frontend/dist";

  Rig rig(configRoot);
  const BootReport& report = rig.system.begin();
  std::printf("host server: boot mode %s, %zu devices, %zu channels\n",
              toString(report.mode), rig.devices.activeCount(),
              rig.channels.activeCount());

  // Milestone 14: the real TelemetryBatcher, over a real WebSocket.  The
  // browser now receives the frames the firmware composes — which is what makes
  // "pull the plug and check the recording shows a gap" a test rather than a
  // hope.
  WebSocketRegistry sockets;
  g_sockets = &sockets;
  TelemetryBatcher telemetry(rig.channels, rig.clock);
  telemetry.begin(rig.scheduler, rig.events, sockets);
  telemetry.subscribeAll();

  // Keep the rig running: the simulator publishes, filters filter, staleness is
  // detected — the browser sees a live system, not a frozen snapshot.
  std::thread ticker([&rig]() {
    while (true) {
      rig.system.loop();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  ticker.detach();

  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  int reuse = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    std::perror("bind");
    return 1;
  }
  ::listen(listener, 16);
  std::printf("listening on http://127.0.0.1:%d  (static: %s)\n", port,
              staticRoot.c_str());

  while (true) {
    const int client = ::accept(listener, nullptr, nullptr);
    if (client < 0) continue;

    std::string request;
    char chunk[4096];
    ssize_t received = 0;
    // Read until the headers are complete, then until Content-Length is met.
    while ((received = ::recv(client, chunk, sizeof(chunk), 0)) > 0) {
      request.append(chunk, static_cast<std::size_t>(received));
      const std::size_t headerEnd = request.find("\r\n\r\n");
      if (headerEnd == std::string::npos) continue;
      const std::size_t lengthPos = request.find("Content-Length:");
      std::size_t expected = 0;
      if (lengthPos != std::string::npos) {
        expected = static_cast<std::size_t>(
            std::atoi(request.c_str() + lengthPos + 15));
      }
      if (request.size() >= headerEnd + 4 + expected) break;
    }
    if (request.empty()) {
      ::close(client);
      continue;
    }

    std::istringstream head(request);
    std::string method, target, version;
    head >> method >> target >> version;

    const std::size_t bodyStart = request.find("\r\n\r\n");
    const std::string body =
        (bodyStart == std::string::npos) ? std::string()
                                         : request.substr(bodyStart + 4);

    std::string path = target;
    std::string query;
    const std::size_t questionMark = target.find('?');
    if (questionMark != std::string::npos) {
      path = target.substr(0, questionMark);
      query = target.substr(questionMark + 1);
    }

    if (method == "OPTIONS") {
      send(client, 200, "text/plain", "");
      ::close(client);
      continue;
    }

    // The routes below mirror PsychicHttpAdapter::begin() deliberately, in the
    // same order.  Milestone 12 started because the development server and the
    // device disagreed about routing: the browser worked here and served
    // index.html for a missing bundle there, so the failure only ever appeared
    // on hardware, as a JavaScript syntax error.  Two servers that answer the
    // same URL differently are two servers, and only one of them is tested.
    if (path == "/health") {
      char health[192];
      std::snprintf(health, sizeof(health),
                    "{\"status\":\"ok\",\"firmware\":\"%s\","
                    "\"schema_version\":%d,\"config_revision\":%lu}",
                    LC_FIRMWARE_VERSION,
                    static_cast<int>(ConfigStorage::kSchemaVersion),
                    static_cast<unsigned long>(rig.storage.revision()));
      send(client, 200, "application/json", health);
      ::close(client);
      continue;
    }

    if (path.rfind("/api/", 0) == 0) {
      ApiRequest incoming;
      incoming.method = parseHttpMethod(method.c_str());
      incoming.path = path.c_str();
      incoming.query = query.c_str();
      incoming.body = body.c_str();
      incoming.bodyLength = body.size();
      // The session comes from the cookie, exactly as it does on the board.
      // The development server does NOT wave requests through: an interface
      // that is only tested against an API which trusts everyone is an
      // interface whose sign-in flow has never run (ADR-0020).
      // Case-insensitively: HTTP header names are, and clients disagree —
      // curl sends "Cookie:", Playwright sends "cookie:", and a server that
      // only understands one of them has an authentication bug that looks like
      // a browser bug.
      std::string cookie;
      const std::size_t cookieAt = findHeader(request, "cookie:");
      if (cookieAt != std::string::npos) {
        const std::size_t valueAt = cookieAt + 7;
        const std::size_t end = request.find("\r\n", valueAt);
        cookie = request.substr(valueAt,
                                (end == std::string::npos ? request.size() : end) -
                                    valueAt);
        while (!cookie.empty() && cookie.front() == ' ') cookie.erase(0, 1);
      }
      incoming.cookie = cookie.empty() ? nullptr : cookie.c_str();

      ApiResponse outgoing;
      rig.api.handle(incoming, outgoing);

      if (outgoing.isStream()) {
        // The same two-path model the firmware uses: the REST layer described a
        // file, and the transport sends it in chunks instead of building it in
        // memory (ADR-0019).
        const std::string file = configRoot + outgoing.stream.path.c_str();
        std::ifstream data(file, std::ios::binary);
        if (!data) {
          send(client, 404, "application/json",
               "{\"error\":{\"code\":\"NOT_FOUND\",\"numeric\":101,"
               "\"message\":\"this dataset is not on the filesystem\"}}");
          ::close(client);
          continue;
        }
        data.seekg(0, std::ios::end);
        const std::size_t bytes = static_cast<std::size_t>(data.tellg());
        data.seekg(0, std::ios::beg);

        std::ostringstream head;
        head << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: " << outgoing.stream.contentType.c_str() << "\r\n"
             << "Content-Length: " << bytes << "\r\n"
             << "Content-Disposition: attachment; filename=\""
             << outgoing.stream.filename.c_str() << "\"\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Connection: close\r\n\r\n";
        const std::string header = head.str();
        ::send(client, header.data(), header.size(), 0);

        char chunk[8192];
        while (data.read(chunk, sizeof(chunk)) || data.gcount() > 0) {
          ::send(client, chunk, static_cast<std::size_t>(data.gcount()), 0);
          if (data.eof()) break;
        }
        std::printf("%-6s %-48s -> stream %zu bytes\n", method.c_str(),
                    target.c_str(), bytes);
        ::close(client);
        continue;
      }

      std::string payload;
      serializeJson(outgoing.body, payload);
      std::printf("%-6s %-48s -> %d\n", method.c_str(), target.c_str(),
                  outgoing.status);
      send(client, outgoing.status, "application/json", payload,
           outgoing.setCookie.c_str());
      ::close(client);
      continue;
    }

    if (path.rfind("/ws/", 0) == 0) {
      if (!g_sockets->upgrade(client, request, rig)) ::close(client);
      // Owned by the socket registry now; do not close it here.
      continue;
    }

    std::string file = staticRoot + (path == "/" ? "/index.html" : path);
    std::string content = readAll(file);
    if (content.empty() && path.rfind("/assets/", 0) == 0) {
      // An asset that is not there is a 404, never the shell.  See above.
      send(client, 404, "application/json",
           "{\"error\":{\"code\":\"NOT_FOUND\",\"numeric\":101,"
           "\"message\":\"this asset is not in frontend/dist\"}}");
      ::close(client);
      continue;
    }
    if (content.empty()) {
      file = staticRoot + "/index.html";
      content = readAll(file);
    }
    send(client, content.empty() ? 404 : 200, mimeFor(file),
         content.empty() ? "not found" : content);
    ::close(client);
  }
}
