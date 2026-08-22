#include "api/esp32/PsychicHttpAdapter.h"

#include <LittleFS.h>

#include <cstring>

namespace lc {
namespace platform {
namespace {

const char* contentTypeFor(const char* path) {
  const char* dot = std::strrchr(path, '.');
  if (dot == nullptr) return "application/octet-stream";
  if (std::strcmp(dot, ".html") == 0) return "text/html";
  if (std::strcmp(dot, ".js") == 0)   return "text/javascript";
  if (std::strcmp(dot, ".css") == 0)  return "text/css";
  if (std::strcmp(dot, ".json") == 0) return "application/json";
  if (std::strcmp(dot, ".svg") == 0)  return "image/svg+xml";
  if (std::strcmp(dot, ".png") == 0)  return "image/png";
  if (std::strcmp(dot, ".woff2") == 0) return "font/woff2";
  if (std::strcmp(dot, ".ico") == 0)  return "image/x-icon";
  return "application/octet-stream";
}

HttpMethod translate(int method) {
  switch (method) {
    case HTTP_GET:    return HttpMethod::kGet;
    case HTTP_POST:   return HttpMethod::kPost;
    case HTTP_PUT:    return HttpMethod::kPut;
    case HTTP_PATCH:  return HttpMethod::kPatch;
    case HTTP_DELETE: return HttpMethod::kDelete;
    case HTTP_OPTIONS:return HttpMethod::kOptions;
    default:          return HttpMethod::kUnknown;
  }
}

// A path is refused rather than sanitised.  "/assets/../config/auth.json" is
// not a typo to be helpfully corrected; it is somebody trying, and the only
// answer that cannot be got around is no.
bool pathIsSafe(const String& path) {
  if (path.indexOf("..") >= 0) return false;
  for (unsigned i = 0; i < path.length(); ++i) {
    const char c = path.charAt(i);
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '/' || c == '.' ||
                         c == '-' || c == '_';
    if (!allowed) return false;
  }
  return true;
}

}  // namespace

Status PsychicHttpAdapter::begin() {
  if (started_) return ok();

  // Registration is idempotent; the START is what may be retried.  See the
  // header: doing both on every attempt was what made retrying unsafe.
  configureAndRegisterOnce();

  const esp_err_t started = server_.begin();
  if (started != ESP_OK) {
    // Almost always "no network interface available": the caller got here
    // before esp_netif finished coming up.  Nothing has leaked, so the caller
    // may wait and call again.
    return fail(ErrorCode::kInternal, "the web server did not start");
  }

  started_ = true;
  return ok();
}

void PsychicHttpAdapter::configureAndRegisterOnce() {
  if (configured_) return;
  configured_ = true;

  // Sockets: four WebSocket clients plus a few HTTP keep-alives.  Anything the
  // server itself derives (max_uri_handlers) is set inside start() and must not
  // be second-guessed here.
  server_.setPort(kPort);
  server_.config.max_open_sockets = 7;
  server_.config.lru_purge_enable = true;
  // Serving a file walks LittleFS -> VFS -> esp_littlefs, which is a deep call
  // chain; the default 4 KB HTTP task stack overflows partway through a large
  // asset and takes the board with it.
  server_.config.stack_size = 8192;
  // A configuration import is the biggest body this API accepts.
  server_.maxRequestBodySize = 16 * 1024;

  // --- liveness ------------------------------------------------------------
  // First, and deliberately first: /health must answer even when the frontend
  // was never uploaded, so it is the one route that never looks at a file.
  server_.on("/health", HTTP_GET,
             [this](PsychicRequest* request, PsychicResponse* response) {
               return handleHealth(request, response);
             });

  // --- REST ----------------------------------------------------------------
  // One wildcard handler rather than a route table: the router lives in
  // RestApi, where it is testable, and duplicating it here would guarantee the
  // two drift apart.
  server_.on("/api/*", HTTP_ANY,
             [this](PsychicRequest* request, PsychicResponse* response) {
               return handleApi(request, response);
             });

  // --- WebSocket -----------------------------------------------------------
  websocket_.onOpen([this](PsychicWebSocketClient* client) {
    sendHello(client);
  });
  websocket_.onFrame([this](PsychicWebSocketRequest* request,
                            httpd_ws_frame* frame) -> esp_err_t {
    if (frame->type == HTTPD_WS_TYPE_TEXT && frame->payload != nullptr) {
      handleWebSocketFrame(request, reinterpret_cast<const char*>(frame->payload),
                           frame->len);
    }
    return ESP_OK;
  });
  // A WebSocket needs a real esp-idf URI handler, which can only be registered
  // while the server is stopped.  Registering it after begin() is silently
  // useless — the library logs a warning and the socket never connects.
  server_.on("/ws/live", &websocket_);

  // --- the single-page app -------------------------------------------------
  auto shell = [this](PsychicRequest* request, PsychicResponse* response) {
    return sendShell(response);
  };
  server_.on("/", HTTP_GET, shell);
  server_.on("/index.html", HTTP_GET, shell);

  // Assets are answered from the filesystem or not at all.  Falling back to the
  // shell here is what makes a missing bundle look like a JavaScript parse
  // error instead of a missing file.
  server_.on("/assets/*", HTTP_GET,
             [this](PsychicRequest* request, PsychicResponse* response) {
               const String path = request->path();
               if (!pathIsSafe(path)) {
                 return sendJson(response, 400,
                                 "{\"error\":{\"code\":\"INVALID_ARGUMENT\","
                                 "\"numeric\":1,\"message\":\"bad asset path\"}}");
               }
               const String file = String(kWebRoot) + path;
               if (!LittleFS.exists(file) && !LittleFS.exists(file + ".gz")) {
                 return sendJson(
                     response, 404,
                     "{\"error\":{\"code\":\"NOT_FOUND\",\"numeric\":101,"
                     "\"message\":\"this asset is not on the filesystem; the "
                     "web interface may be out of date — run "
                     "'pio run -t uploadfs'\"}}");
               }
               // "no-cache" means REVALIDATE, not "do not store": the browser
               // keeps the bundle and spends one conditional request per load
               // to ask whether it changed.  Immutable caching would be free,
               // and would also be wrong — vite.config.ts deliberately emits
               // stable names (assets/app.js, not assets/app.a1b2c3.js) to fit
               // the SPA in a 640 KB partition, so the SAME url legitimately
               // has different contents after every firmware update.  A year of
               // immutable caching on that name is a browser pinned to a bundle
               // the device no longer has.
               return sendFile(response, file.c_str(),
                               contentTypeFor(path.c_str()), "no-cache");
             });

  // Anything else is a client-side route.
  server_.on("/*", HTTP_GET, shell);
}

void PsychicHttpAdapter::end() {
  if (!started_) return;
  server_.stop();
  started_ = false;
}

esp_err_t PsychicHttpAdapter::sendJson(PsychicResponse* response, int code,
                                       const char* json) {
  response->setCode(code);
  response->setContentType("application/json");
  response->addHeader("Cache-Control", "no-store");
  response->setContent(json);
  return response->send();
}

esp_err_t PsychicHttpAdapter::handleHealth(PsychicRequest* request,
                                           PsychicResponse* response) {
  // Deliberately hand-built rather than serialised: this endpoint has to work
  // when everything else does not, including when the heap is too fragmented
  // for a JsonDocument.
  char body[192];
  std::snprintf(body, sizeof(body),
                "{\"status\":\"ok\",\"firmware\":\"%s\",\"schema_version\":%d,"
                "\"config_revision\":%lu,\"uptime_s\":%lu}",
                LC_FIRMWARE_VERSION, static_cast<int>(ConfigStorage::kSchemaVersion),
                static_cast<unsigned long>(storage_.revision()),
                static_cast<unsigned long>(millis() / 1000));
  response->setCode(200);
  response->setContentType("application/json");
  response->addHeader("Cache-Control", "no-store");
  response->setContent(body);
  return response->send();
}

esp_err_t PsychicHttpAdapter::sendFile(PsychicResponse* response,
                                       const char* path, const char* contentType,
                                       const char* cacheControl) {
  // PsychicFileResponse streams anything over 8 KB in chunks, so a 300 KB
  // bundle costs a buffer rather than the whole file in RAM.
  //
  // setCode() BEFORE send() is not optional.  The chunked path calls
  // sendHeaders() directly, and sendHeaders() formats whatever code the
  // response happens to hold; a response nobody set is code 0, which becomes
  // the status line "HTTP/1.1 0 Unknown" and makes every client reject the
  // answer as malformed.  Small files took the other branch and worked, so the
  // failure looked like "large assets are broken" — which is how it was found,
  // the expensive way, on hardware.
  response->setCode(200);
  PsychicFileResponse file(response, LittleFS, path, contentType);
  // Set after construction: addHeader replaces a field it already holds, and
  // PsychicFileResponse adds its own Content-Disposition and (for a .gz twin)
  // Content-Encoding.
  file.addHeader("Cache-Control", cacheControl);
  return file.send();
}

esp_err_t PsychicHttpAdapter::sendShell(PsychicResponse* response) {
  const String shell = String(kWebRoot) + "/index.html";
  if (!LittleFS.exists(shell) && !LittleFS.exists(shell + ".gz")) {
    return sendJson(response, 404,
                    "{\"error\":{\"code\":\"NOT_FOUND\",\"numeric\":101,"
                    "\"message\":\"the web interface is not installed; run "
                    "'pio run -t uploadfs'\"}}");
  }
  // The shell is the one file that must never be cached: it is what points at
  // the hashed assets, so a stale copy pins the browser to a bundle that is no
  // longer on the device.
  return sendFile(response, shell.c_str(), "text/html", "no-cache");
}

esp_err_t PsychicHttpAdapter::handleApi(PsychicRequest* request,
                                        PsychicResponse* response) {
  // Every one of these is held in a NAMED local.  path() and header() return by
  // value; `request->path().c_str()` hands the router a pointer into a String
  // that is destroyed at the end of the statement, and the request that comes
  // back is whatever the allocator left behind.
  const String path = request->path();
  const String query = request->queryString();
  const String body = request->body();
  const String cookie = request->header("Cookie");

  ApiRequest incoming;
  incoming.method = translate(request->method());
  incoming.path = path.c_str();
  incoming.query = query.c_str();
  // The body is already buffered by PsychicHttp and bounded by
  // maxRequestBodySize; RestApi rejects anything oversized itself as well.
  incoming.body = body.c_str();
  incoming.bodyLength = body.length();
  // The session is resolved by RestApi from this header, and by nothing else.
  // The adapter deliberately does not assert authentication: a transport that
  // could grant it is a transport that will, one refactor from now.
  incoming.cookie = cookie.isEmpty() ? nullptr : cookie.c_str();

  ApiResponse outgoing;
  api_.handle(incoming, outgoing);

  if (outgoing.isStream()) {
    // A dataset is megabytes; the JSON path is bounded at twelve kilobytes on
    // purpose (ADR-0012).
    if (!LittleFS.exists(outgoing.stream.path.c_str())) {
      return sendJson(response, 404,
                      "{\"error\":{\"code\":\"NOT_FOUND\",\"numeric\":101,"
                      "\"message\":\"this dataset is not on the filesystem\"}}");
    }
    response->setCode(200);
    PsychicFileResponse file(response, LittleFS, outgoing.stream.path.c_str(),
                             outgoing.stream.contentType.c_str());
    String disposition = "attachment; filename=\"";
    disposition += outgoing.stream.filename.c_str();
    disposition += "\"";
    file.addHeader("Content-Disposition", disposition.c_str());
    file.addHeader("Cache-Control", "no-store");
    return file.send();
  }

  String payload;
  serializeJson(outgoing.body, payload);

  response->setCode(outgoing.status);
  response->setContentType("application/json");
  response->addHeader("Cache-Control", "no-store");
  if (!outgoing.setCookie.empty()) {
    response->addHeader("Set-Cookie", outgoing.setCookie.c_str());
  }
  response->setContent(payload.c_str());
  return response->send();
}

void PsychicHttpAdapter::sendHello(PsychicWebSocketClient* client) {
  JsonDocument hello;
  hello["type"] = "hello";
  hello["firmware"] = LC_FIRMWARE_VERSION;
  hello["schema_version"] = ConfigStorage::kSchemaVersion;
  hello["config_revision"] = storage_.revision();
  hello["controller_id"] = controllerId_;
  hello["max_rate_hz"] = TelemetryBatcher::kMaxRateHz;

  String payload;
  serializeJson(hello, payload);
  client->sendMessage(payload.c_str());
}

void PsychicHttpAdapter::handleWebSocketFrame(PsychicWebSocketRequest* request,
                                              const char* text,
                                              std::size_t length) {
  JsonDocument message;
  if (deserializeJson(message, text, length)) return;  // ignore junk quietly

  const char* type = message["type"] | "";

  if (std::strcmp(type, "subscribe") == 0) {
    ChannelHandle handles[limits::kMaxChannels];
    std::size_t count = 0;
    for (JsonVariantConst entry : message["channels"].as<JsonArrayConst>()) {
      if (count >= limits::kMaxChannels) break;
      handles[count++] = static_cast<ChannelHandle>(entry.as<unsigned>());
    }
    telemetry_.subscribe(handles, count);
    return;
  }

  if (std::strcmp(type, "rate") == 0) {
    telemetry_.setRateHz(message["hz"] | TelemetryBatcher::kDefaultRateHz);
    return;
  }

  if (std::strcmp(type, "ping") == 0) {
    request->reply("{\"type\":\"pong\"}");
    return;
  }
}

std::size_t PsychicHttpAdapter::clientCount() const {
  return const_cast<PsychicWebSocketHandler&>(websocket_).getClientList().size();
}

bool PsychicHttpAdapter::canSend() const {
  // esp_http_server sends synchronously from the HTTP task, so there is no
  // pending-frame state to inspect; the guard that matters is the client limit.
  return clientCount() > 0 && clientCount() <= kMaxWebSocketClients;
}

bool PsychicHttpAdapter::broadcast(const char* text, std::size_t length) {
  (void)length;
  websocket_.sendAll(text);
  return true;
}

}  // namespace platform
}  // namespace lc
