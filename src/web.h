// web.h -- HTTP server entry point (webInit). Handlers are file-private.

#ifndef SFGW_WEB_H
#define SFGW_WEB_H

#include "common.h"

// A WebServer that can be asked whether its listen socket is actually up. WebServer::begin()
// returns void and swallows a bind/listen failure, so a silent failure at boot (seen once after
// NVS corruption left the stack able to associate and pass traffic but unable to open a new inbound
// listener) otherwise refuses every request for the whole uptime, with nothing to detect or retry
// it. NetworkServer::operator bool() is the ground truth (_listening, protected in WebServer),
// surfaced here so taskWeb can re-establish a dead listener. See webEnsureListening().
class HealWebServer : public WebServer {
public:
  using WebServer::WebServer;
  bool listening() { return (bool)_server; }
};

// ---- owned globals (defined in globals.cpp) ----
extern HealWebServer server;

void webInit();
// Re-establish port 80 if its listen socket is silently down. No-op when already listening, so it
// is safe to call often; called every 20s from taskWeb. Returns true if the listener is live.
bool webEnsureListening();

#endif // SFGW_WEB_H
