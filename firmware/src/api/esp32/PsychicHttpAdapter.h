// =============================================================================
//  api/esp32/PsychicHttpAdapter.h — the thin layer between PsychicHttp and
//  RestApi / TelemetryBatcher.
//
//  NOTE ON VERIFICATION.  This is the one part of the API layer that cannot be
//  unit-tested on a host: it is bound to esp_http_server through PsychicHttp.
//  That is exactly why it is kept as small as it is — it translates a request
//  into an ApiRequest, calls handle(), and serialises the answer.  Every
//  decision worth testing lives on the other side of that call.
//
//  It is, however, COMPILED on the host: tools/esp32_typecheck/check.sh builds
//  this file against the real PsychicHttp headers at the pinned version.  Every
//  API break below was found by hardware bring-up before that check existed, and
//  none of them can reach a board again.
//
//  Static assets are served pre-compressed: the build hook gzips the SPA into
//  /www, and this handler answers "/assets/app.js" with "/www/assets/app.js.gz"
//  plus Content-Encoding.  The MCU never compresses anything at request time.
//
//  ROUTING (Milestone 12).  There is no serveStatic() here, and that is
//  deliberate.  A catch-all static handler answers /health with "no such file"
//  and answers a missing /assets/app.js with index.html — an HTML page arriving
//  where JavaScript was expected, which the browser reports as a syntax error
//  on line 1 and which costs an evening to trace back to routing.  Routes are
//  therefore explicit and ORDERED, because PsychicHttp matches endpoints in
//  registration order:
//
//      /health      → liveness, plain and cheap, never touches the filesystem
//      /api/*       → RestApi
//      /ws/live     → telemetry socket
//      /            → the SPA shell
//      /index.html  → the SPA shell
//      /assets/*    → a file, or a 404 that says so.  NEVER the shell.
//      /*           → the SPA shell (a client-side route)
// =============================================================================
#pragma once

#include <PsychicHttp.h>

#include "api/IWebSocketSink.h"
#include "api/RestApi.h"
#include "api/TelemetryBatcher.h"

namespace lc {
namespace platform {

class PsychicHttpAdapter final : public IWebSocketSink {
 public:
  static constexpr std::uint16_t kPort = 80;
  // Each WebSocket client costs a control block and a send buffer.  Four is
  // generous for a lab instrument and keeps the memory bill predictable.
  static constexpr std::size_t kMaxWebSocketClients = 4;
  // Where the frontend lives on LittleFS.  The build hook writes here and
  // `pio run -t uploadfs` puts it on the board.
  static constexpr const char* kWebRoot = "/www";

  // `controllerId` is the same string GET /api/v1/system reports.  It is passed
  // in rather than read from the metrics here so that the hello a client sees
  // on connect and the one it read over REST cannot disagree (§M14).
  PsychicHttpAdapter(RestApi& api, TelemetryBatcher& telemetry,
                     ConfigStorage& storage, const char* controllerId)
      : api_(api), telemetry_(telemetry), storage_(storage),
        controllerId_(controllerId != nullptr ? controllerId : "lc-000000000000") {}

  // Registers every route and starts the server.  MUST be called after the
  // network is up: PsychicHttpServer::start() refuses to run without a
  // connected interface, and it says so rather than crashing later.
  //
  // SAFE TO RETRY (0.15.1-m15).  Route registration happens once and the start
  // is what repeats, so a caller may keep trying while the interface is still
  // coming up.  The version before this one registered every route again on
  // each attempt, which leaked a handler and a lambda per try and would
  // eventually take the heap with it — so the only safe number of attempts was
  // one, and one attempt is exactly what left the controller with no web
  // interface after a reset.
  Status begin();
  void end();
  bool running() const { return started_; }

  // --- IWebSocketSink ------------------------------------------------------
  std::size_t clientCount() const override;
  bool canSend() const override;
  bool broadcast(const char* text, std::size_t length) override;

 private:
  esp_err_t handleApi(PsychicRequest* request, PsychicResponse* response);
  esp_err_t handleHealth(PsychicRequest* request, PsychicResponse* response);
  esp_err_t sendFile(PsychicResponse* response, const char* path,
                     const char* contentType, const char* cacheControl);
  esp_err_t sendShell(PsychicResponse* response);
  esp_err_t sendJson(PsychicResponse* response, int code, const char* json);
  // Configuration and route registration: exactly once, however many times
  // begin() is called.  Both must happen while the server is STOPPED — a
  // WebSocket needs a real esp-idf URI handler, and those can only be added
  // before start().
  void configureAndRegisterOnce();
  void handleWebSocketFrame(PsychicWebSocketRequest* request, const char* text,
                            std::size_t length);
  void sendHello(PsychicWebSocketClient* client);

  RestApi& api_;
  TelemetryBatcher& telemetry_;
  ConfigStorage& storage_;
  const char* controllerId_;

  PsychicHttpServer server_{kPort};
  PsychicWebSocketHandler websocket_;
  bool started_ = false;
  bool configured_ = false;
};

}  // namespace platform
}  // namespace lc
