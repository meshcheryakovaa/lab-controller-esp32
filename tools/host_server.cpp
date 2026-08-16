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
//  Deliberately NOT implemented: WebSocket telemetry.  The handshake and
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

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "api/RestApi.h"
#include "app/SystemManager.h"
#include "platform/host/HostBusProvider.h"
#include "platform/host/HostClock.h"
#include "platform/host/HostRandom.h"
#include "storage/PosixBackend.h"

using namespace lc;

namespace {

struct Rig {
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
  NullSystemMetrics metrics;
  platform::HostBusProvider buses;
  SystemManager system;
  RestApi api;

  explicit Rig(std::string configRoot)
      : backend(std::move(configRoot)),
        system(makeSystemServices()),
        api(makeApiServices()) {
    applier.setControl(&control);
    applier.setSafety(&safety);
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
      // See the header comment: telemetry is not served here on purpose.
      send(client, 404, "application/json",
           "{\"error\":{\"code\":\"NOT_SUPPORTED\",\"numeric\":104,"
           "\"message\":\"the host server does not serve WebSocket telemetry\"}}");
      ::close(client);
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
