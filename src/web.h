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
//
// It also replaces the base class's chunk writer. WebServer::sendContent() sends ONE chunk as
// THREE socket writes -- the size line, the data, the CRLF -- after a malloc for the size line.
// Three writes is three TCP segments, and it is worse than it sounds on this board: with Nagle
// on, each write after the first waits for the previous segment to be ACKed over WiFi (20-300
// ms of taskWeb doing nothing, per write); with Nagle off, a 173 KB page went out as ~500
// runt packets and took 6-20 s. Either way the one-connection server was serving nobody else.
//
// So chunks are FRAMED into a buffer and the buffer is written when it fills or when the
// response ends: a handler that calls sendContent() five times for 2.7 KB now costs one or two
// full-size segments instead of fifteen runts. The methods shadow the base class's (they are
// not virtual, but `server` is declared as this type, so every call site resolves here), which
// is why none of the handlers had to change. chunkReset() is called at the start of each
// request so a response abandoned half-way (client gone) cannot leak into the next one.
class HealWebServer : public WebServer {
public:
  using WebServer::WebServer;
  bool listening() { return (bool)_server; }

  void chunkReset() { _pend = 0; _dead = false; }
  // True once a write to the current client has failed: the rest of the response is discarded.
  // Handlers that loop over something expensive (a 4 MB trace file) can stop early on it.
  bool clientDead() const { return _dead; }

  void sendContent(const char* content, size_t len) {
    if (_dead) return;
    if (!_chunked) {                       // HTTP/1.0, or a fixed-length response: as the base
      if (len) writeOrDie((const uint8_t*)content, len);
      return;
    }
    if (len == 0) {                        // end of the response: flush, with the terminator
      if (_pend + 5 > sizeof(_buf)) chunkFlush();
      memcpy(_buf + _pend, "0\r\n\r\n", 5); _pend += 5;
      chunkFlush();
      _chunked = false;
      return;
    }
    while (len) {
      // Room for this piece's own frame: up to 8 hex digits + CRLF before, CRLF after.
      if (_pend + 16 >= sizeof(_buf)) chunkFlush();
      size_t room = sizeof(_buf) - _pend - 12;
      size_t take = len < room ? len : room;
      _pend += (size_t)snprintf(_buf + _pend, 11, "%x\r\n", (unsigned)take);
      memcpy(_buf + _pend, content, take); _pend += take;
      _buf[_pend++] = '\r'; _buf[_pend++] = '\n';
      content += take; len -= take;
    }
  }
  void sendContent(const char* content)   { sendContent(content, content ? strlen(content) : 0); }
  void sendContent(const String& content) { sendContent(content.c_str(), content.length()); }

private:
  // NetworkClient::write() does not honour the connection timeout: it waits for the socket in
  // 1 s select()s, ten times, and on giving up leaves the socket OPEN. So a client that stopped
  // reading -- a phone that slept mid-response, a tab on a network that went away -- cost ten
  // seconds on EVERY following write, with the web watchdog fed in between: a module list was
  // 20 s of a frozen server, a long monitor backlog minutes. One short write now ends the
  // response: the client is closed and everything after it is dropped, so the worst a dead
  // peer can cost is a single stall.
  void writeOrDie(const uint8_t* p, size_t n) {
    if (_currentClient.write(p, n) != n) { _dead = true; _currentClient.stop(); }
  }
  void chunkFlush() {
    if (_pend && !_dead) writeOrDie((const uint8_t*)_buf, _pend);
    _pend = 0;
  }
  bool   _dead = false;
  // Two segments' worth. One instance, one task (taskWeb), so no lock and no stack cost.
  char   _buf[2872];
  size_t _pend = 0;
};

// ---- owned globals (defined in globals.cpp) ----
extern HealWebServer server;

void webInit();
// Re-establish port 80 if its listen socket is silently down. No-op when already listening, so it
// is safe to call often; called every 20s from taskWeb. Returns true if the listener is live.
bool webEnsureListening();

#endif // SFGW_WEB_H
