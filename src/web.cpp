#include "gateway.h"
#include "web_ui.h"



// web.cpp -- HTTP server: the dashboard page and the REST API.
// Runs entirely on taskWeb (one request at a time). handleRoot streams the
// static dashboard from web_ui.h; each handleApi* function serves one REST
// route (the HTTP method + path is noted above each, and registered in
// webInit). Handlers are static -- only webInit is exported.
// ---- file-private forward declarations ----
static void handleApiAll();
static void handleApiAutoHome();
static void handleApiCalibrate();
static void handleApiCalibrateStatus();
static void handleApiChar();
static void handleApiConfigGet();
static void handleApiConfigMqtt();
static void handleApiConfigRS485();
static void handleApiConfigSettings();
static void handleApiConfigWifi();
static void handleApiDeprovision();
static void handleApiDiag();
static void handleApiDiagMech();
static void handleApiDiagStatus();
static void handleApiDisplayState();
static void handleApiDisplayCells();
static void handleApiDump();
static void handleApiDumpBySN();
static void handleApiErase();
static void handleApiFactoryReset();
static void handleApiFactoryResetBySN();
static void handleApiFlapConfig();
static void handleApiGoto();
static void handleApiHome();
static void handleApiHomeBySN();
static void handleApiHomeOffset();
static void handleApiIdentify();
static void handleApiIndex();
static void handleApiMaintenance();
static void handleApiMessages();
static void handleApiModules();
static void handleApiMqttTest();
static void handleApiNudge();
static void handleApiProvision();
static void handleApiQuiet();
static void handleApiQuietSchedule();
static void handleApiCompanion();
static void handleApiCompanionSettingsGet();
static void handleApiCompanionSettingsPut();
static void handleApiCompanionSettingsRaw();
static void handleApiRestoreBySN();
static void handleApiSend();
static void handleApiSendBatch();
static void handleApiCapabilities();
static void handleApiStatus();
static void handleApiText();
static void handleApiTotalSteps();
static void handleApiVersion();
static void handleApiWritePos();
static void handleFavicon();
static void handleLogo();
static void handleOptions();
static void handleRoot();
static void sendJsonError(int code, const char* msg);
static const char* sfValidateCharSet(const char* charSet, char* out, size_t outLen);

/* ----------------------------------------------------------
   Web server
---------------------------------------------------------- */
static void sendJsonError(int code, const char* msg) {
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
  server.send(code, "application/json", buf);
}

// -- GET /  (main dashboard)
// Browser tab icon (favicon): a split-flap tile -- two flaps, the signature
// horizontal seam with axle pivots, and a character bisected by it. Served at
// /favicon.svg and linked from each page <head>. SVG keeps it crisp at any size
// with no binary blob; single-quoted attributes let it sit in a plain C string.
const char FAVICON_SVG[] =
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64' role='img' aria-label='Split-Flap Gateway'><defs><linearGradient id='sfTop' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#3c424c'/><stop offset='1' stop-color='#2d323b'/></linearGradient><linearGradient id='sfBot' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#272b32'/><stop offset='1' stop-color='#181b20'/></linearGradient><clipPath id='sfTile'><rect x='7' y='7' width='50' height='50' rx='10'/></clipPath></defs><rect x='7' y='8.5' width='50' height='50' rx='10' fill='#000' opacity='0.35'/><g clip-path='url(#sfTile)'><rect x='7' y='7' width='50' height='25' fill='url(#sfTop)'/><rect x='7' y='32' width='50' height='25' fill='url(#sfBot)'/><text x='32' y='46' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='40' fill='#f3eee3'>S</text><rect x='7' y='30.9' width='50' height='2.2' fill='#0c0d10'/><rect x='7' y='33.1' width='50' height='0.8' fill='#565c68' opacity='0.7'/></g><rect x='4.5' y='29.5' width='4' height='5' rx='1.6' fill='#0c0d10'/><rect x='55.5' y='29.5' width='4' height='5' rx='1.6' fill='#0c0d10'/><rect x='7' y='7' width='50' height='50' rx='10' fill='none' stroke='#0a0b0d' stroke-width='1'/></svg>";
// GET /favicon.svg
static void handleFavicon() {
  static const char* ETAG = "\"" __DATE__ "-" __TIME__ "\"";
  server.sendHeader("ETag", ETAG);
  server.sendHeader("Cache-Control", "no-cache");
  if (server.header("If-None-Match") == ETAG) { server.send(304, "image/svg+xml", ""); return; }
  server.send(200, "image/svg+xml", FAVICON_SVG);
}

// Web UI wordmark (header logo): the app name on a split-flap board -- the same
// two-flap tiles, seam and pivots as the favicon, one cell per character. Served
// at /logo.svg and used in <h1> in place of the title text.
const char LOGO_SVG[] =
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 256 44' role='img' aria-label='Split-Flap'><defs><linearGradient id='sfTop' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#3c424c'/><stop offset='1' stop-color='#2d323b'/></linearGradient><linearGradient id='sfBot' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#272b32'/><stop offset='1' stop-color='#181b20'/></linearGradient></defs><rect x='3' y='2' width='250' height='40' rx='6' fill='#000' opacity='0.30'/><clipPath id='clip1'><rect x='3' y='0' width='250' height='40' rx='6'/></clipPath><g clip-path='url(#clip1)'><rect x='3' y='0' width='250' height='20' fill='url(#sfTop)'/><rect x='3' y='20' width='250' height='20' fill='url(#sfBot)'/><rect x='27.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='28.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='52.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='53.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='77.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='78.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='102.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='103.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='127.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='128.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='152.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='153.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='177.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='178.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='202.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='203.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='227.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='228.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><text x='15.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>S</text><text x='40.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>P</text><text x='65.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>L</text><text x='90.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>I</text><text x='115.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>T</text><text x='140.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>-</text><text x='165.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>F</text><text x='190.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>L</text><text x='215.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>A</text><text x='240.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>P</text><rect x='3' y='18.9' width='250' height='2.2' fill='#0c0d10'/><rect x='3' y='21.1' width='250' height='0.8' fill='#565c68' opacity='0.7'/></g><rect x='1.2' y='17.5' width='3.6' height='5' rx='1.4' fill='#0c0d10'/><rect x='251.2' y='17.5' width='3.6' height='5' rx='1.4' fill='#0c0d10'/><rect x='3' y='0' width='250' height='40' rx='6' fill='none' stroke='#0a0b0d' stroke-width='0.8'/></svg>";
// GET /logo.svg
static void handleLogo() {
  // ETag = build time, revalidated every request (like the page and /lang). A plain 7-day
  // max-age with NO validator was a bug: when the logo changed, the browser kept serving its
  // OLD cached copy for a week -- the header text updated but the wordmark did not.
  static const char* ETAG = "\"" __DATE__ "-" __TIME__ "\"";
  server.sendHeader("ETag", ETAG);
  server.sendHeader("Cache-Control", "no-cache");
  if (server.header("If-None-Match") == ETAG) { server.send(304, "image/svg+xml", ""); return; }
  server.send(200, "image/svg+xml", LOGO_SVG);
}

// Stream a byte range of the static page in watchdog-friendly chunks so a slow
// client can't trip the stall detector mid-send.
static void streamPage(const char* p, size_t n) {
  const size_t CHUNK = 1024;
  for (size_t off = 0; off < n; off += CHUNK) {
    size_t c = (n - off < CHUNK) ? (n - off) : CHUNK;
    server.sendContent_P(p + off, c);
    wdgWebMs = millis();
  }
}

// GET /lang/<code>  -- one UI translation dictionary (v3.5)
//
// The dashboard's English is the text already in PAGE_HTML, so English costs nothing
// and needs no request. Every other language is a gzipped JSON dict generated into
// web_ui.h by tools/build_ui.py, and the browser fetches only the one it needs.
//
// Content-Encoding: gzip is correct HERE (and wrong for /api/companion/settings): these
// bytes are a *transfer encoding* of JSON that the browser transparently inflates before
// the page's fetch().json() ever sees it. The companion blob is the opposite -- there the
// gzip IS the payload, which is why that endpoint must not claim the header.
//
// One route is registered per language in webInit(), so an unknown code simply 404s.
static void handleLang(size_t idx) {
  const UiLang& L = UI_LANGS[idx];
  wdgWebMs = millis();
  server.client().setConnectionTimeout(3000);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Dictionaries live in the firmware image, so they change only on reflash -- the same
  // ETag as the page busts them at exactly the right moment.
  static const char* LANG_ETAG = "\"" __DATE__ "-" __TIME__ "\"";
  server.sendHeader("ETag", LANG_ETAG);
  server.sendHeader("Cache-Control", "no-cache");
  if (server.header("If-None-Match") == LANG_ETAG) { server.send(304, "application/json", ""); return; }
  server.sendHeader("Content-Encoding", "gzip");
  server.setContentLength(L.len);
  server.send(200, "application/json", "");
  server.sendContent_P((PGM_P)L.gz, L.len);
  wdgWebMs = millis();
}

// GET /
static void handleRoot() {
  wdgWebMs = millis();                 // streaming response can take a while
  // Cap per-write blocking so a stalled browser cannot wedge taskWeb. This must be
  // setConnectionTimeout(): NetworkClient keeps its own `_timeout` (which is what seeds
  // SO_SNDTIMEO) and does NOT override Stream::setTimeout(), so the old setTimeout(3000)
  // set the *read* timeout and capped nothing at all -- the comment above was a lie.
  server.client().setConnectionTimeout(3000);   // 3s per socket write
  // The page is baked into the firmware, so its bytes change only when the firmware
  // is rebuilt -- and every rebuild changes __TIME__. Serve it with that as an ETag
  // and honour If-None-Match: navigating away and back then costs a tiny 304 instead
  // of re-downloading the whole ~53 KB page, while a reflash still busts the cache and
  // delivers the new UI. (The old no-store forced a full re-download on every visit.)
  static const char* PAGE_ETAG = "\"" __DATE__ "-" __TIME__ "\"";
  server.sendHeader("ETag", PAGE_ETAG);
  server.sendHeader("Cache-Control", "no-cache");   // revalidate, but cheaply (304)
  if (server.header("If-None-Match") == PAGE_ETAG) { server.send(304, "text/html", ""); return; }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  // Stream the static page (web_ui.h), substituting the single {FWVER} token
  // with the firmware version so the page stays tied to the FW_VERSION macro.
  const char*  page  = PAGE_HTML;
  const size_t total = sizeof(PAGE_HTML) - 1;
  const char*  mark  = strstr(page, "{FWVER}");
  if (mark) {
    streamPage(page, (size_t)(mark - page));
    server.sendContent(FW_VERSION);
    streamPage(mark + 7, total - (size_t)(mark + 7 - page));  // 7 = strlen("{FWVER}")
  } else {
    streamPage(page, total);
  }
  server.sendContent("");             // terminate the chunked response
}

// GET /api/rs485/messages
static void handleApiMessages() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", ringDrain());
}

// POST /api/rs485/send
static void handleApiSend() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* d = doc["data"] | "";
  bool raw = doc["raw"] | false;   // optional: send verbatim, bypassing sanitization
  uint8_t outBuf[TX_MAX_BYTES];
  size_t  outLen = min(strlen(d), (size_t)TX_MAX_BYTES);
  memcpy(outBuf, d, outLen);
  if (!outLen) { sendJsonError(400, "Empty data"); return; }
  { char cd[MSG_MAX_BYTES]; snprintf(cd, sizeof(cd), "send %s", d);
    ringPushCommand('R', cd); }   // one 'REST' row, then the TX frame below
  rs485Send(outBuf, outLen, raw);
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%zu,\"raw\":%s}", outLen, raw ? "true" : "false");
  server.send(200, "application/json", resp);
}

// POST /api/rs485/batch  (v3.0) -- send many frames in one request.
// Body: {"frames":["m00-A\n","m01-B\n",...], "step_ms":15}. Each frame is sent
// normalized (like /api/rs485/send); an optional step_ms paces the cascade
// device-side. Lets the companion draw a whole animated page in ONE HTTP call
// instead of one request per module. Caps keep the request bounded and the web
// watchdog fed.
static void handleApiSendBatch() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  JsonArray frames = doc["frames"].as<JsonArray>();
  if (frames.isNull()) { sendJsonError(400, "'frames' array required"); return; }
  int step = doc["step_ms"] | 0;
  if (step < 0)  step = 0;
  if (step > 30) step = 30;          // keep per-frame pacing small
  // One 'REST' row marking the batch, just above the TX frames it produces.
  { char cd[48]; snprintf(cd, sizeof(cd), "batch %u frames, step=%dms",
      (unsigned)frames.size(), step); ringPushCommand('R', cd); }
  int sent = 0;
  uint32_t due = millis();           // frame i is due at now + i*step
  for (JsonVariant v : frames) {
    if (sent >= 512) break;          // bound the batch
    const char* f = v.as<const char*>();
    if (!f || !*f) continue;
    uint8_t outBuf[TX_MAX_BYTES];
    size_t outLen = min(strlen(f), (size_t)TX_MAX_BYTES);
    memcpy(outBuf, f, outLen);
    // Pace by SCHEDULING, never by delay(): this handler runs on taskWeb, and blocking it
    // freezes the one-connection HTTP server (and piles up concurrent sockets). taskRS485
    // sends each frame when due. step==0, an over-long frame, or a full queue -> send now.
    if (step > 0 && rs485SendScheduled(outBuf, outLen, due)) {
      due += (uint32_t)step;
    } else {
      rs485Send(outBuf, outLen, false);
    }
    sent++;
    wdgWebMs = millis();             // this loop is now fast, but stay watchdog-safe
  }
  char resp[48];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"sent\":%d}", sent);
  server.send(200, "application/json", resp);
}

// GET /api/flap/modules
// Streamed with chunked transfer + a small per-module stack buffer instead of
// building one large heap String. This avoids the alloc/free of a multi-KB
// String on every poll (the UI polls this every few seconds), which was a
// meaningful contributor to long-run heap fragmentation. The sfMutex is taken
// only briefly to snapshot each entry -- never held across the (potentially
// blocking) sendContent network write, which could otherwise stall taskRS485.
static void handleApiModules() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // setConnectionTimeout(), NOT setTimeout(): NetworkClient declares its own `_timeout`
  // (which is what seeds SO_SNDTIMEO) and does NOT override Stream::setTimeout(), so
  // setTimeout() sets the *read* timeout and caps nothing. See the note in the send
  // loop below: on a large wall the sum of these writes is what trips the watchdog.
  server.client().setConnectionTimeout(1500);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");

  // Emit modules sorted by ID so the grid is always ordered (newly provisioned
  // modules slot into place instead of appearing at the end). Build a sorted
  // index order under the lock first; unprovisioned entries (id==255) naturally
  // sort to the end. Then snapshot+send each entry, re-checking under the lock.
  static uint8_t order[MAX_MODULES];
  int count = 0;
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  count = sfModuleCount;
  for (int i = 0; i < count; i++) order[i] = (uint8_t)i;
  // Insertion sort the index array by the modules' IDs (stable, small N).
  for (int a = 1; a < count; a++) {
    uint8_t key = order[a];
    uint8_t keyId = sfModules[key].id;
    int b = a - 1;
    while (b >= 0 && sfModules[order[b]].id > keyId) { order[b + 1] = order[b]; b--; }
    order[b + 1] = key;
  }
  xSemaphoreGive(sfMutex);

  // Coalesce modules into MSS-sized chunks instead of one socket write per module.
  //
  // This was one sendContent() per module, and on a large wall that is a watchdog
  // reboot waiting to happen: each write can block for SO_SNDTIMEO (3 s, the
  // NetworkClient default) if the browser stalls or half-closes, and NOTHING in the
  // loop fed wdgWebMs. 41 modules x 3 s = 123 s, past loop()'s 120 s web-stall
  // threshold -> "STALL: Web=0" -> reboot. Below ~40 modules the sum stays under the
  // threshold, which is the only reason this never fired on a small bus.
  //
  // Three things fix it: bound each write (setConnectionTimeout below), feed the
  // watchdog per flush, and stop early if the peer has gone away.
  char   batch[1400];   // ~1436-byte lwIP MSS: one chunk maps to one TCP segment
  size_t bl      = 0;
  int    emitted = 0;
  for (int k = 0; k < count; k++) {
    int idx = order[k];
    // Snapshot this entry under the lock, then release before formatting/sending.
    SFModule m;
    bool valid = false;
    xSemaphoreTake(sfMutex, portMAX_DELAY);
    if (idx < sfModuleCount) { m = sfModules[idx]; valid = true; }
    xSemaphoreGive(sfMutex);
    if (!valid) continue;   // list shrank (prune/deprovision) mid-iteration

    // Tracked flap char is one Windows-1252 byte; emit it as JSON-safe UTF-8.
    char flapBuf[6] = {0};
    if (m.flapChar) flapToJsonUtf8(&m.flapChar, 1, flapBuf, sizeof(flapBuf));
    char obj[288];
    int on = snprintf(obj, sizeof(obj),
      "%s{\"id\":%d,\"sn\":\"%s\",\"provisioned\":%s,\"acked\":%s,\"flapIndex\":%d,"
      "\"flapChar\":\"%s\",\"fwVersion\":\"%s\",\"lastSeen\":%lu,\"lastSeenEpoch\":%lu,"
      "\"dupSuspect\":%s}",
      emitted ? "," : "", (int)m.id, m.serialNum, m.provisioned ? "true" : "false",
      m.acked ? "true" : "false",
      m.flapIndex, flapBuf, m.fwVersion, m.lastSeen, m.lastSeenEpoch,
      m.dupSuspect ? "true" : "false");
    if (on < 0) on = 0;
    if (on > (int)sizeof(obj) - 1) on = (int)sizeof(obj) - 1;   // snprintf truncated

    if (bl + (size_t)on >= sizeof(batch)) {   // flush before it overflows
      batch[bl] = 0;                          // sendContent() strlen()s its argument
      server.sendContent(batch);
      bl = 0;
      wdgWebMs = millis();                    // the SUM of the writes is what trips it
      // A client that vanished mid-response cannot be finished; every remaining write
      // would just burn its full send timeout. Stop rather than hold taskWeb hostage.
      if (!server.client().connected()) return;
    }
    memcpy(batch + bl, obj, (size_t)on); bl += (size_t)on;
    emitted++;
  }
  if (bl) { batch[bl] = 0; server.sendContent(batch); wdgWebMs = millis(); }
  server.sendContent("]");
  server.sendContent("");   // terminate the chunked response
}

// GET /api/display/state -- the data behind the visual "display wall". Returns
// the configured grid dimensions plus the character each cell is showing. Cells
// are addressed by module ID mapped left-to-right, top-to-bottom (cell index =
// row*cols + col == module id), matching how text is distributed across modules.
// A cell shows: the tracked character if known, "?" if the module exists but its
// char is unknown (e.g. after a home or index-set), or null if no module has
// that id. Kept small so the UI can poll it cheaply.
static void handleApiDisplayState() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  int rows = cfg.gridRows < 1 ? 1 : cfg.gridRows;
  int cols = cfg.gridCols < 1 ? 1 : cfg.gridCols;
  int cells = rows * cols;
  // cellChar: 0 = no module at this id, 1 = module present but char unknown,
  // otherwise the printable character. Filled under the mutex, JSON built after.
  static char cellChar[64 * 64];   // matches the 64x64 grid cap enforced in settings
  if (cells > (int)sizeof(cellChar)) cells = sizeof(cellChar);
  memset(cellChar, 0, cells);
  // Primary source: the last flap byte transmitted to each grid cell, so the wall
  // mirrors EVERYTHING the gateway sent -- provisioned or not, and independent of
  // the module registry (which only tracks provisioned ids).
  for (int i = 0; i < cells && i < (int)sizeof(gWallChars); i++) {
    uint8_t wc = (uint8_t)gWallChars[i];
    if (wc && isFlapByte(wc)) cellChar[i] = (char)wc;
  }
  // A provisioned module that hasn't been sent a character yet still reads as
  // present ("?") rather than empty.
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < sfModuleCount; i++) {
    const SFModule& m = sfModules[i];
    if (m.provisioned && m.id < cells && cellChar[m.id] == 0) cellChar[m.id] = 1;
  }
  xSemaphoreGive(sfMutex);

  // Stream the response (chunked) from the static cellChar snapshot rather than
  // building a multi-KB heap String for a frequently-polled endpoint. The mutex
  // was already released above, so nothing is held across these network writes.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  char head[48];
  snprintf(head, sizeof(head), "{\"rows\":%d,\"cols\":%d,\"cells\":[", rows, cols);
  server.sendContent(head);
  // Emit cells in batches to keep the number of tiny network writes down.
  char batch[256]; size_t bl = 0;
  for (int i = 0; i < cells; i++) {
    char cellBuf[12]; int cn;
    char c = cellChar[i];
    if (c == 0)       cn = snprintf(cellBuf, sizeof(cellBuf), "%snull", i ? "," : "");
    else if (c == 1)  cn = snprintf(cellBuf, sizeof(cellBuf), "%s\"?\"", i ? "," : "");
    else {
      // Windows-1252 byte -> JSON-safe UTF-8 (handles euro/accented glyphs).
      char u[6]; flapToJsonUtf8(&c, 1, u, sizeof(u));
      cn = snprintf(cellBuf, sizeof(cellBuf), "%s\"%s\"", i ? "," : "", u);
    }
    if (cn < 0) cn = 0;
    if (bl + (size_t)cn >= sizeof(batch)) { server.sendContent(batch); bl = 0; }
    memcpy(batch + bl, cellBuf, cn); bl += cn;
  }
  if (bl) { batch[bl] = 0; server.sendContent(batch); }
  server.sendContent("]}");
  server.sendContent("");   // terminate the chunked response
}

// POST /api/flap/char   {"id":5,"char":"A"}   id=-1 for broadcast
static void handleApiChar() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  const char* ch = doc["char"] | "";
  if (!ch[0]) { sendJsonError(400, "Missing char"); return; }
  // `ch` is UTF-8: a euro/accented glyph is multi-byte. Transcode to a single
  // Windows-1252 byte and display the first character (see charset.h).
  char enc[8];
  utf8ToFlap(ch, enc, sizeof(enc));
  if (!enc[0]) { sendJsonError(400, "Unsupported character"); return; }
  sfSendChar(id, enc[0]);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/index  {"id":5,"index":3}
static void handleApiIndex() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id  = doc["id"]    | -1;
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= 64) { sendJsonError(400, "Invalid index (0-63)"); return; }
  DBG("[API] show index %d on module %d\n", idx, id);
  sfSendIndex(id, idx);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/text   {"text":"HELLO","start":0}
static void handleApiText() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* text = doc["text"] | "";
  int start = doc["start"] | 0;
  if (!text[0]) { sendJsonError(400, "Empty text"); return; }
  sfSendText(start, text, false);
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"chars\":%zu}", strlen(text));
  server.send(200, "application/json", resp);
}

// POST /api/flap/home   {"id":5}  or  {"id":-1}
static void handleApiHome() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  DBG("[API] home module %d\n", id);
  sfHome(id);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/calibrate  {"id":5}
// Starts an asynchronous calibration. Sends m<id>c and returns immediately so
// the single-threaded web server stays responsive while the reel physically
// measures a revolution (~6.5s, up to 15s). The module replies m<id>:<steps>,
// captured in sfParseResponse; the UI polls /api/flap/calibrate/status for the
// result. The module saves the measured value to its own EEPROM as part of
// calibration. A broadcast (id<0) is fire-and-forget.
static void handleApiCalibrate() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  DBG("[API] calibrate module %d\n", id);

  // Broadcast: no single reply to wait for.
  if (id < 0) {
    sfCalibrate(id);
    server.send(200, "application/json", "{\"ok\":true,\"broadcast\":true}");
    return;
  }

  // Reject a second start while one is already running for a different module;
  // re-starting the same module is allowed (re-arms the capture).
  if (gCalib.jobActive && gCalib.jobId != id) {
    char busy[96];
    snprintf(busy, sizeof(busy),
      "{\"ok\":false,\"error\":\"calibration already running for module %d\"}",
      gCalib.jobId);
    server.send(409, "application/json", busy);
    return;
  }

  // Arm the capture slot and the job, then send the calibrate command.
  gCalib.steps       = 0;
  gCalib.ts   = 0;
  gCalib.waitId      = id;
  gCalib.jobActive   = true;
  gCalib.jobId       = id;
  gCalib.jobSteps    = -1;
  gCalib.jobDeadline = millis() + 15000;
  sfCalibrate(id);

  char out[64];
  snprintf(out, sizeof(out), "{\"ok\":true,\"started\":true,\"id\":%d}", id);
  server.send(200, "application/json", out);
}

// POST /api/flap/diag {id}
// First step of the self-diagnostics run (firmware v26+). Captures the instant
// 'Q' stats snapshot synchronously (returned inline as `q`) and starts the 'T'
// Hall self-test, which drives the motor ~2 revolutions. The UI polls
// /api/flap/diag/status for the Hall result, then calls /api/flap/diag/mech to
// run the mechanical test. T and M share one async job (one motor test at a time).
static void handleApiDiag() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  if (id < 0 || id > 254) { sendJsonError(400, "valid id required"); return; }
  DBG("[API] diagnostics module %d (Q + Hall)\n", id);

  // Reject if a motor test is already running for a different module.
  if (gDiag.jobActive && gDiag.jobId != id) {
    char busy[96];
    snprintf(busy, sizeof(busy),
      "{\"ok\":false,\"error\":\"diagnostics already running for module %d\"}",
      gDiag.jobId);
    server.send(409, "application/json", busy);
    return;
  }

  // 1) Instant stats snapshot ('Q') -- captured synchronously (no motor).
  bool qok = sfSendAndCaptureQ(id, 1500);

  // 2) Hall self-test ('T') -- motor-driven (~2 revolutions). Arm the capture +
  //    shared job, fire the command, and return immediately.
  gDiag.t.ts         = 0;
  gDiag.t.code       = -1;
  gDiag.t.waitId     = id;
  gDiag.jobActive   = true;
  gDiag.jobId       = id;
  gDiag.jobKind     = 'T';
  gDiag.jobDeadline = millis() + 35000UL;
  char tframe[16];
  snprintf(tframe, sizeof(tframe), "m%dT\n", id);
  rs485SendStr(tframe);

  char out[200];
  if (qok) {
    snprintf(out, sizeof(out),
      "{\"ok\":true,\"started\":true,\"id\":%d,"
      "\"q\":{\"resetCause\":%d,\"bootCount\":%d,\"vcc\":%d,\"eepromOk\":%d,\"curIndex\":%d}}",
      id, gDiag.q.reset, gDiag.q.boot, gDiag.q.vcc, gDiag.q.ee, gDiag.q.cur);
  } else {
    snprintf(out, sizeof(out), "{\"ok\":true,\"started\":true,\"id\":%d,\"q\":null}", id);
  }
  server.send(200, "application/json", out);
}

// POST /api/flap/diag/mech {id}
// Second motor test of the run: the mechanical self-test ('M'), which spins the
// motor ~6 revolutions. Called by the UI after the Hall test completes. Result
// is polled from /api/flap/diag/status.
static void handleApiDiagMech() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  if (id < 0 || id > 254) { sendJsonError(400, "valid id required"); return; }

  // Optional rotation count (firmware v29+). Absent -> bare 'm<id>M' (module
  // default of 5). Present -> 'm<id>M<n>' with n clamped to [5,20]; the module
  // re-clamps too. The motor time scales with n, so the job deadline does too.
  bool haveRevs = doc["revs"].is<int>();
  int  revs     = haveRevs ? (int)doc["revs"] : 5;
  if (revs < 5)  revs = 5;
  if (revs > 20) revs = 20;
  DBG("[API] diagnostics module %d (mechanical, %s%d revs)\n",
      id, haveRevs ? "" : "default ", revs);

  if (gDiag.jobActive && gDiag.jobId != id) {
    char busy[96];
    snprintf(busy, sizeof(busy),
      "{\"ok\":false,\"error\":\"diagnostics already running for module %d\"}",
      gDiag.jobId);
    server.send(409, "application/json", busy);
    return;
  }

  gDiag.m.ts         = 0;
  gDiag.m.code       = -1;
  gDiag.m.revs[0]    = 0;
  gDiag.m.waitId     = id;
  gDiag.jobActive   = true;
  gDiag.jobId       = id;
  gDiag.jobKind     = 'M';
  // ~20s/rev worst-case bound plus headroom (5 revs -> 130s, 20 revs -> 430s).
  gDiag.jobDeadline = millis() + 30000UL + (unsigned long)revs * 20000UL;
  char mframe[20];
  if (haveRevs) snprintf(mframe, sizeof(mframe), "m%dM%d\n", id, revs);
  else          snprintf(mframe, sizeof(mframe), "m%dM\n", id);
  rs485SendStr(mframe);

  char out[64];
  snprintf(out, sizeof(out), "{\"ok\":true,\"started\":true,\"id\":%d,\"revs\":%d}", id, revs);
  server.send(200, "application/json", out);
}

// GET /api/flap/diag/status
// Poll target for the active async motor test (Hall 'T' or mechanical 'M').
//   {"ok":true,"state":"idle"}
//   {"ok":true,"state":"pending","kind":"hall"|"mech","id":N}
//   {"ok":true,"state":"done","kind":"hall","id":N,"code":C,"rising":..,"active":..,"falling":..}
//   {"ok":true,"state":"done","kind":"mech","id":N,"code":C,"min":..,"max":..,
//        "spreadTenths":..,"gateActive":..,"gateSpan":..,"magWidth":..,"revs":"r1,r2,..."}
//   {"ok":true,"state":"timeout","kind":...,"id":N}
static void handleApiDiagStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char out[460];
  if (gDiag.jobActive) {
    char kind = gDiag.jobKind;
    bool ready = (kind == 'T') ? (gDiag.t.ts != 0) : (gDiag.m.ts != 0);
    if (ready) {
      gDiag.jobActive = false;
      if (kind == 'T') {
        gDiag.t.waitId = -1;
        snprintf(out, sizeof(out),
          "{\"ok\":true,\"state\":\"done\",\"kind\":\"hall\",\"id\":%d,"
          "\"code\":%d,\"rising\":%d,\"active\":%d,\"falling\":%d}",
          gDiag.jobId, gDiag.t.code, gDiag.t.rising, gDiag.t.active, gDiag.t.falling);
      } else {
        gDiag.m.waitId = -1;
        snprintf(out, sizeof(out),
          "{\"ok\":true,\"state\":\"done\",\"kind\":\"mech\",\"id\":%d,"
          "\"code\":%d,\"min\":%d,\"max\":%d,\"spreadTenths\":%d,"
          "\"gateActive\":%d,\"gateSpan\":%d,\"magWidth\":%d,\"revs\":\"%s\"}",
          gDiag.jobId, gDiag.m.code, gDiag.m.minVal, gDiag.m.maxVal, gDiag.m.spread,
          gDiag.m.gateActive, gDiag.m.gateSpan, gDiag.m.magWidth, gDiag.m.revs);
      }
      server.send(200, "application/json", out);
      return;
    }
    if ((long)(millis() - gDiag.jobDeadline) >= 0) {
      gDiag.jobActive = false;
      gDiag.t.waitId = -1;
      gDiag.m.waitId = -1;
      snprintf(out, sizeof(out), "{\"ok\":true,\"state\":\"timeout\",\"kind\":\"%s\",\"id\":%d}",
               (kind == 'T') ? "hall" : "mech", gDiag.jobId);
      server.send(200, "application/json", out);
      return;
    }
    snprintf(out, sizeof(out), "{\"ok\":true,\"state\":\"pending\",\"kind\":\"%s\",\"id\":%d}",
             (kind == 'T') ? "hall" : "mech", gDiag.jobId);
    server.send(200, "application/json", out);
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"state\":\"idle\"}");
}

// GET /api/flap/calibrate/status
// Poll target for the async calibration job. Reports one of:
//   {"ok":true,"state":"idle"}                          no job has run
//   {"ok":true,"state":"pending","id":N}                still measuring
//   {"ok":true,"state":"done","id":N,"stepsPerRev":S}   result ready
//   {"ok":true,"state":"timeout","id":N}                no reply within window
// The "done"/"timeout" result latches until the next start (or until read), so
// a poll that arrives right after completion still sees it.
static void handleApiCalibrateStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char out[96];

  if (gCalib.jobActive) {
    // Capture arrives via sfParseResponse setting gCalib.ts.
    if (gCalib.ts != 0) {
      gCalib.jobSteps  = gCalib.steps;
      gCalib.jobActive = false;
      gCalib.waitId    = -1;
      snprintf(out, sizeof(out),
        "{\"ok\":true,\"state\":\"done\",\"id\":%d,\"stepsPerRev\":%d}",
        gCalib.jobId, gCalib.jobSteps);
      server.send(200, "application/json", out);
      return;
    }
    if ((long)(millis() - gCalib.jobDeadline) >= 0) {
      gCalib.jobActive = false;
      gCalib.waitId    = -1;
      snprintf(out, sizeof(out),
        "{\"ok\":true,\"state\":\"timeout\",\"id\":%d}", gCalib.jobId);
      server.send(200, "application/json", out);
      return;
    }
    snprintf(out, sizeof(out),
      "{\"ok\":true,\"state\":\"pending\",\"id\":%d}", gCalib.jobId);
    server.send(200, "application/json", out);
    return;
  }

  // No active job. If a result was latched from the last run, report it once.
  if (gCalib.jobSteps >= 0) {
    snprintf(out, sizeof(out),
      "{\"ok\":true,\"state\":\"done\",\"id\":%d,\"stepsPerRev\":%d}",
      gCalib.jobId, gCalib.jobSteps);
    gCalib.jobSteps = -1;   // consume the latched result
    server.send(200, "application/json", out);
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"state\":\"idle\"}");
}

// POST /api/flap/version  {"id":5}
static void handleApiVersion() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  DBG("[API] version query module %d\n", id);
  if (id < 0 || id > 254) { sendJsonError(400, "id required (0-254)"); return; }

  // Send a direct version query and wait for a fresh reply. The window scales
  // mildly with id (broadcast-stagger headroom); a direct query usually answers
  // in ~35-70ms now that the newline collision is fixed (see sfQueryVersion).
  char          fwVer[8]     = "";
  char          sn[21]       = "";
  unsigned long repLastSeen  = 0;
  unsigned long waitMs   = 500UL + (unsigned long)(id > 25 ? 25 : id) * 100UL;
  bool gotReply = sfSendVersionAndWait(id, waitMs, fwVer, sizeof(fwVer),
                                       sn, sizeof(sn), &repLastSeen);

  if (gotReply) {
    char out[128];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"id\":%d,\"ver\":\"%s\",\"sn\":\"%s\",\"stale\":false,\"lastSeen\":%lu}",
             id, fwVer, sn, repLastSeen);
    DBG("[API] version response: id=%d ver=%s sn=%s\n", id, fwVer, sn);
    server.send(200, "application/json", out);
  } else {
    // Timed out -- check if we already have a cached version from before
    char cachedVer[8] = "";
    char cachedSn[21] = "";
    int           cachedId      = -1;
    unsigned long cachedLastSeen = 0;
    xSemaphoreTake(sfMutex, portMAX_DELAY);
    SFModule* mc = sfFindById((uint8_t)id);
    if (mc && mc->fwVersion[0]) {
      strlcpy(cachedVer, mc->fwVersion, sizeof(cachedVer));
      strlcpy(cachedSn,  mc->serialNum, sizeof(cachedSn));
      cachedId      = mc->id;
      cachedLastSeen = mc->lastSeen;
    } else if (mc) {
      // Known module, no cached version yet, and this query timed out. Do NOT
      // stamp any sentinel: a direct version query is reliable now that the
      // newline-collision is fixed, so a timeout here is a transient miss (bus
      // busy, momentary contention), not evidence the module lacks the command.
      // Return what we have (id/sn) and let the next poll re-query.
      strlcpy(cachedSn,  mc->serialNum, sizeof(cachedSn));
      cachedId      = mc->id;
      cachedLastSeen = mc->lastSeen;
    }
    xSemaphoreGive(sfMutex);
    if (cachedId >= 0) {
      // Return stale cached data
      char out[160];
      snprintf(out, sizeof(out),
               "{\"ok\":true,\"id\":%d,\"ver\":\"%s\",\"sn\":\"%s\",\"stale\":true,\"lastSeen\":%lu}",
               cachedId, cachedVer, cachedSn, cachedLastSeen);
      DBG("[API] version timeout for module %d -- returning stale data: ver=%s\n", id, cachedVer);
      server.send(200, "application/json", out);
    } else {
      DBG("[API] version query timeout for module %d (no cached data)\n", id);
      server.send(200, "application/json",
                  "{\"ok\":false,\"error\":\"no response from module\"}");
    }
  }
}

// POST /api/flap/provision  {"sn":"AABBCC...","id":5}
static void handleApiProvision() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn = doc["sn"] | "";
  int newId = doc["id"] | -1;
  if (!sn[0] || newId < 0) { sendJsonError(400, "sn and id required"); return; }
  sfProvision(sn, newId);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/deprovision  {"id":5} or {"id":-1} for all
static void handleApiDeprovision() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -999;
  if (id == -999) { sendJsonError(400, "id required"); return; }
  DBG("[API] deprovision module %d\n", id);
  sfDeprovision(id);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/homebysn  {"sn":"AABBCC..."}
static void handleApiHomeBySN() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn = doc["sn"] | "";
  if (!sn[0]) { sendJsonError(400, "sn required"); return; }
  DBG("[API] home by SN %s\n", sn);
  sfHomeBySN(sn);
  server.send(200, "application/json", "{\"ok\":true}");
}

/* ----------------------------------------------------------
   GET /api/capabilities -- what this wall can actually show
   ----------------------------------------------------------
   One call, made once when a client connects, that answers "what characters can this display
   show?" without the client having to know what kind of gateway it is talking to. The Matrix
   Portal gateway answers the same question at the same URL with the same shape; there the wall
   is drawn and every cell shares one reel, so its answer is always uniform. Here the wall is
   real: every module owns its reel, firmware v31 lets each be told a DIFFERENT one, and so the
   honest answer can be several sets at once.

     union    every character SOME module can show. "Can this wall show a Z anywhere?"
     common   every character EVERY module can show. "Can I lay this text across arbitrary
              cells?" -- and on a mixed wall these genuinely differ. If module 1 has A-Z and
              module 2 has 0-9, the union is A-Z0-9 and the common set is EMPTY: the wall
              cannot show "HI42" wherever it likes, and only `common` says so.
     sets     each DISTINCT reel once, with the ids that use it. A uniform wall is one entry
              and one range -- a few hundred bytes however big the wall -- and a mixed wall
              costs one entry per genuinely different reel, which is the information rather
              than a repetition of it.

   TWO TRANSLATIONS, because the wire is not the repertoire.
     * r o y g b p w are the seven COLOUR flaps, not letters. They are reported under "colors",
       by name, and kept out of the character union -- a client that read them as letters would
       believe a classic reel can show a lowercase w.
     * 'q' is not the letter q. The classic reel has no lowercase, so its char map borrowed that
       byte for the DOUBLE-QUOTE flap. It is reported as '"', which is what the flap shows.
   Both translations are exactly what sfSendChar() does when it resolves a frame, so what this
   endpoint promises is what the wall will actually do.                                        */
static const char* const CAP_COLOUR_NAMES[7] = {
  "red", "orange", "yellow", "green", "blue", "purple", "white"   // in FLAP_COLOUR_CODES order
};

// A BUFFERED writer for the response below.
//
// server.sendContent() is one HTTP chunk and one TCP write. Streaming a repertoire a CHARACTER
// at a time -- the obvious way to write it, and what this did at first -- sends 57 characters as
// 57 chunks of one byte, each waiting on its own round-trip. On the Matrix Portal gateway, whose
// reel is four times longer, the same mistake took FIVE SECONDS to deliver 1.6 KB while
// /api/status delivered 465 bytes in twenty milliseconds. Time-to-first-byte was 11 ms
// throughout: none of it was the computing. It was the writing.
//
// So: accumulate, and flush a kilobyte at a time.
static char   capBuf[1024];
static size_t capLen = 0;

// The per-request scratch, in PSRAM. See the note in handleApiCapabilities().
struct CapScratch {
  char          reel[MAX_MODULES][SF_MAX_FLAPS + 1];   // one reel per module id (~16.6 KB)
  uint8_t       ids[MAX_MODULES];
  FlapSetSource src[MAX_MODULES];
  bool          done[MAX_MODULES];
  uint8_t       share[MAX_MODULES];
};
static CapScratch* capScratch = NULL;

static void capFlush() {
  if (!capLen) return;
  capBuf[capLen] = 0;
  server.sendContent(capBuf);
  capLen = 0;
  wdgWebMs = millis();
}
static void capPut(const char* str) {
  size_t n = strlen(str);
  if (capLen + n >= sizeof(capBuf) - 1) capFlush();
  if (n >= sizeof(capBuf) - 1) { server.sendContent(str); return; }   // never truncate a caller
  memcpy(capBuf + capLen, str, n);
  capLen += n;
}

// A byte map as a JSON string body, in reel order (ASCII, then the high bytes).
static void capPutMap(const bool* map) {
  for (int b = 0x20; b < 256; b++) {
    if (!map[b]) continue;
    char one[12];
    if (b == '"' || b == '\\') { one[0] = '\\'; one[1] = (char)b; one[2] = 0; }
    else {
      char utf8[8] = "";
      size_t n = flapByteToUtf8((uint8_t)b, utf8);
      utf8[n] = 0;
      snprintf(one, sizeof(one), "%s", utf8);
    }
    capPut(one);
  }
}

static void handleApiCapabilities() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.client().setConnectionTimeout(1500);

  // Snapshot the wall under the lock -- one reel per module, plus where it came from -- and do
  // every byte of the work below OUTSIDE it. taskRS485 must not wait on a socket write.
  //
  // The snapshot is ~19 KB (a 65-byte reel per module id), and it lives in PSRAM. It was static
  // internal RAM at first, and that was not a style preference: it took free heap from 145 KB to
  // 96 KB, and the next web OTA upload ran the heap down to EIGHT HUNDRED BYTES and reset the
  // board mid-flash. An endpoint nobody calls twice a day must not stand between this gateway and
  // its own firmware updates. Allocated on first use, never freed -- the monitor ring, the MQTT
  // queue and the registry itself are in PSRAM for the same reason.
  if (!capScratch) {
    capScratch = (CapScratch*) psramAlloc("capabilities scratch", sizeof(CapScratch));
    if (!capScratch) { sendJsonError(503, "out of memory"); return; }
  }
  char          (*reel)[SF_MAX_FLAPS + 1] = capScratch->reel;
  uint8_t*       ids                      = capScratch->ids;
  FlapSetSource* src                      = capScratch->src;
  int n = 0;
  if (sfMutex && xSemaphoreTake(sfMutex, portMAX_DELAY) == pdTRUE) {
    for (int i = 0; i < sfModuleCount && n < MAX_MODULES; i++) {
      const SFModule& m = sfModules[i];
      if (!m.provisioned) continue;
      ids[n] = m.id;
      src[n] = sfFlapSetOf(m, reel[n]);
      n++;
    }
    xSemaphoreGive(sfMutex);
  }

  bool uni[256] = {false}, com[256] = {false};
  bool anyKnown = false, uniform = true;
  const char* firstReel = NULL;
  for (int i = 0; i < n; i++) {
    if (src[i] == FLAPSET_UNKNOWN) { uniform = false; continue; }   // no reel: nothing to fold
    capFoldReel(reel[i], uni, com, !anyKnown);
    if (!anyKnown) firstReel = reel[i];
    else if (strcmp(firstReel, reel[i]) != 0) uniform = false;
    anyKnown = true;
  }
  if (!anyKnown) { memset(com, 0, sizeof(com)); uniform = false; }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  capLen = 0;
  char head[256];
  snprintf(head, sizeof(head),
           "{\"product\":\"%s\",\"fw\":\"%s\",\"api\":\"%s\","
           "\"grid\":{\"rows\":%d,\"cols\":%d},\"modules\":%d,\"maxFlaps\":%d,",
           PRODUCT_NAME, FW_VERSION, API_VERSION, cfg.gridRows, cfg.gridCols, n, SF_MAX_FLAPS);
  capPut(head);

  // The colour flaps the wall actually HAS. A custom 40-flap reel with no colour codes in it
  // reports none, and that is not a technicality -- asking such a module for red would hang the
  // display on a flap that does not exist.
  capPut("\"colors\":[");
  { bool firstC = true;
    for (int c = 0; FLAP_COLOUR_CODES[c]; c++) {
      bool have = false;
      for (int i = 0; i < n && !have; i++)
        if (src[i] != FLAPSET_UNKNOWN && strchr(reel[i], FLAP_COLOUR_CODES[c])) have = true;
      if (!have) continue;
      char one[16];
      snprintf(one, sizeof(one), "%s\"%s\"", firstC ? "" : ",", CAP_COLOUR_NAMES[c]);
      capPut(one);
      firstC = false;
    } }
  capPut("],");

  char mid[48];
  snprintf(mid, sizeof(mid), "\"charset\":{\"uniform\":%s,\"union\":\"", uniform ? "true" : "false");
  capPut(mid);
  capPutMap(uni);
  capPut("\",\"common\":\"");
  capPutMap(com);
  capPut("\",\"assumed\":[");
  { bool f1 = true;
    for (int i = 0; i < n; i++) {
      if (src[i] != FLAPSET_ASSUMED) continue;
      char one[8]; snprintf(one, sizeof(one), "%s%u", f1 ? "" : ",", (unsigned)ids[i]);
      capPut(one); f1 = false;
    } }
  capPut("],\"unknown\":[");
  { bool f2 = true;
    for (int i = 0; i < n; i++) {
      if (src[i] != FLAPSET_UNKNOWN) continue;
      char one[8]; snprintf(one, sizeof(one), "%s%u", f2 ? "" : ",", (unsigned)ids[i]);
      capPut(one); f2 = false;
    } }
  capPut("]},\"sets\":[");

  // Each DISTINCT reel once, with the ids that carry it as a compact range list ("0-44,50").
  // This is what keeps the response small: 45 modules with one reel between them is one entry,
  // not 45 copies of the same 64 characters.
  wdgWebMs = millis();
  bool*    done  = capScratch->done;         // PSRAM, with the rest of the scratch
  uint8_t* share = capScratch->share;        // the ids carrying the reel being emitted
  memset(done, 0, MAX_MODULES * sizeof(bool));
  bool firstSet = true;
  for (int i = 0; i < n; i++) {
    if (done[i] || src[i] == FLAPSET_UNKNOWN) continue;

    // Everyone with this exact reel, in id order (the snapshot is already in id order).
    int k = 0;
    for (int j = i; j < n; j++) {
      if (done[j] || src[j] == FLAPSET_UNKNOWN) continue;
      if (strcmp(reel[j], reel[i]) != 0)          continue;
      done[j] = true;
      share[k++] = ids[j];
    }

    // Where this GROUP's reel came from. A group is modules that carry the SAME characters, and
    // they need not have come by them the same way: on a real wall, a v31 module that reports the
    // default reel lands in the same group as the pre-v31 modules whose default is only assumed.
    // Calling that group "assumed" understates what module 13 actually told us, and calling it
    // "reported" would claim the others told us something they cannot say. It is "mixed", and
    // charset.assumed remains the per-module truth.
    bool anyRep = false, anyAsm = false;
    for (int j = 0; j < k; j++) {
      for (int q = 0; q < n; q++) {
        if (ids[q] != share[j]) continue;
        if (src[q] == FLAPSET_REPORTED) anyRep = true;
        if (src[q] == FLAPSET_ASSUMED)  anyAsm = true;
      }
    }
    const char* how = (anyRep && anyAsm) ? "mixed" : (anyRep ? "reported" : "assumed");

    char one[SF_MAX_FLAPS + 96];
    snprintf(one, sizeof(one), "%s{\"flaps\":%d,\"source\":\"%s\",\"chars\":\"",
             firstSet ? "" : ",", (int)strlen(reel[i]), how);
    capPut(one);
    // The reel VERBATIM -- the byte order a module addresses by INDEX, colour codes and all.
    // The union above is a repertoire; this is the reel itself, because a client setting a flap
    // by index needs the positions, not the alphabet.
    { char esc[SF_MAX_FLAPS * 3 + 1];
      flapToJsonUtf8(reel[i], strlen(reel[i]), esc, sizeof(esc), 0);
      capPut(esc); }
    capPut("\",\"modules\":\"");
    { // capRangeList compresses runs, so it needs share[] in ASCENDING id order -- but sfModules
      // (and so ids[]/share[]) is in registration order, which after churn or a staggered boot
      // can be scattered. Sort first, or the "ranges" come out wrong. Insertion sort: k is small.
      for (int a = 1; a < k; a++) {
        uint8_t v = share[a]; int b = a - 1;
        while (b >= 0 && share[b] > v) { share[b + 1] = share[b]; b--; }
        share[b + 1] = v;
      }
      // Sized for the worst case so it can never truncate and silently drop ids: even an all-
      // singletons group is at most MAX_MODULES entries of "255," (4 bytes). A 128-byte buffer
      // dropped real ids once a group's compressed list passed ~127 bytes.
      static char ranges[MAX_MODULES * 4 + 1];
      capRangeList(share, k, ranges, sizeof(ranges));
      capPut(ranges); }
    capPut("\"}");
    firstSet = false;
    wdgWebMs = millis();
  }
  capPut("],");

  // How the wall MOVES. "mechanical" with the worst-case settle time: a module's full
  // revolution takes up to ~4 s, and a frame can send any flap anywhere. A client uses this
  // to decide what update rates are honest (a ticking seconds field is not) — and it is a
  // fact about MOTION, stated directly, so nobody has to infer it from which endpoints exist.
  // The Matrix Portal gateway answers the same key with kind "drawn" and its flip duration.
  capPut("\"motion\":{\"kind\":\"mechanical\",\"settleMs\":4000},");

  // What the wall can DO, not just show, so a client reads this instead of sniffing the
  // firmware version and guessing.
  capPut("\"features\":[\"colors\",\"index\",\"batch\",\"quiet\",\"maintenance\","
         "\"ha\",\"ota\",\"flapconfig\"]}");
  capFlush();                 // the body -- everything above is still sitting in capBuf
  server.sendContent("");     // the terminating chunk
}

/* POST /api/display/cells -- set a row of cells in ONE call, the same contract the Matrix
 * Portal gateway answers, so a client (the companion) can drive both walls through a single
 * display endpoint instead of this wall's batch/char path and the Matrix's cells path. It is
 * the "show this" half whose "what can you show" half is GET /api/capabilities.
 *
 * Body: { "start": 0, "step_ms": 15, "cells": [ {ch|color|blank|skip}, ... ] }
 *   start     first module id the cells land on (default 0). id = start + position.
 *   step_ms   0..30, paces the cascade -- SCHEDULED on taskRS485, never a delay() here (a
 *             blocking wait would freeze the one-connection HTTP server; see the batch API).
 *   cells     one per module, left to right, each exactly one of:
 *               {"ch":"A"}        a character. Lowercase folds to uppercase, '"' reaches the
 *                                 reel's double-quote flap, accents pass through; the MODULE
 *                                 resolves the byte against its own reel.
 *               {"color":"red"}   a colour flap, NAMED: red orange yellow green blue purple white
 *               {"blank":true}    home the module
 *               {"skip":true}     leave the module alone
 *
 * LENIENT, and that is the one real difference from the Matrix's strict form. This wall's
 * modules each carry their OWN 64-flap reel and can differ, so "can this be shown" is per
 * module and may not even be known yet -- a cell that cannot be shown is SKIPPED, not a 400
 * that discards the whole row. The response reports sent vs skipped so a caller that wants
 * precision can consult /api/capabilities first. Structural errors (bad JSON, no cells) still
 * 400.
 *
 * Sent BY CHARACTER (m<id>-<char>), not by index like the Matrix: index N names a different
 * glyph on a module with a different reel, whereas the byte lets each module map it against its
 * own reel. Same JSON in, hardware-appropriate frame out.
 */
static void handleApiDisplayCells() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  JsonArray cells = doc["cells"].as<JsonArray>();
  if (cells.isNull()) { sendJsonError(400, "'cells' array required"); return; }
  int start = doc["start"] | 0;
  if (start < 0 || start > 254) { sendJsonError(400, "'start' must be 0..254"); return; }
  int step = doc["step_ms"] | 0;
  if (step < 0)  step = 0;
  if (step > 30) step = 30;

  { char cd[48]; snprintf(cd, sizeof(cd), "cells from id %d, step=%dms", start, step);
    ringPushCommand('R', cd); }

  int n = 0, sent = 0, skipped = 0;
  uint32_t due = millis();
  for (JsonObjectConst c : cells) {
    int id = start + n;
    if (id > 254) break;            // ran off the addressable range; stop counting here
    n++;

    char frame[24];
    int  flen = 0;

    if (c["skip"].is<bool>() && c["skip"].as<bool>()) {
      skipped++; continue;                                   // leave the module as it is
    } else if (c["blank"].is<bool>() && c["blank"].as<bool>()) {
      flen = snprintf(frame, sizeof(frame), "m%dh\n", id);   // home
    } else if (c["color"].is<const char*>()) {
      const char* name = c["color"].as<const char*>();
      int ci = -1;
      for (int k = 0; k < 7; k++)
        if (name && strcasecmp(name, CAP_COLOUR_NAMES[k]) == 0) { ci = k; break; }
      if (ci < 0) { skipped++; continue; }                   // unknown colour name: lenient skip
      flen = snprintf(frame, sizeof(frame), "m%d-%c\n", id, FLAP_COLOUR_CODES[ci]);
    } else if (c["ch"].is<const char*>()) {
      const char* ch = c["ch"].as<const char*>();
      char enc[8];
      utf8ToFlap(ch ? ch : "", enc, sizeof(enc));            // UTF-8 -> CP1252 byte(s)
      uint8_t b = enc[0] ? sfResolveFlapByte((uint8_t)enc[0]) : 0;
      if (!b) { skipped++; continue; }                       // no showable glyph: lenient skip
      flen = snprintf(frame, sizeof(frame), "m%d-%c\n", id, b);
    } else {
      skipped++; continue;                                   // malformed cell: lenient skip
    }

    // Pace by SCHEDULING (like the batch API). step==0 or a full queue -> send now.
    if (step > 0 && rs485SendScheduled((const uint8_t*)frame, (size_t)flen, due)) {
      due += (uint32_t)step;
    } else {
      rs485Send((const uint8_t*)frame, (size_t)flen, false);
    }
    sent++;
    wdgWebMs = millis();
  }

  char resp[80];
  snprintf(resp, sizeof(resp),
           "{\"ok\":true,\"cells\":%d,\"sent\":%d,\"skipped\":%d}", n, sent, skipped);
  server.send(200, "application/json", resp);
}

// GET /api/status
static void handleApiStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Use snprintf to avoid JsonDocument heap allocation (called every 3s by browser)
  char rtcBuf[24]; rtcFormatTime(rtcBuf, sizeof(rtcBuf));
  IPAddress lip = WiFi.localIP(), aip = WiFi.softAPIP();
  // Per-task minimum-ever free stack (bytes). A value trending toward 0 is an
  // early warning of the stack-canary crash class.
  unsigned stk485 = hTaskRS485 ? uxTaskGetStackHighWaterMark(hTaskRS485) : 0;
  unsigned stkWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
  unsigned stkNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
  unsigned stkOta = hTaskOTA   ? uxTaskGetStackHighWaterMark(hTaskOTA)   : 0;
  unsigned stkRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
  // v3.0: seconds since the companion last checked in (-1 = never / deregistered)
  long compAge = gCompanionSeenMs ? (long)((millis() - gCompanionSeenMs) / 1000UL) : -1;
  char out[720];
  snprintf(out, sizeof(out),
    "{\"uptime\":%lu,\"rx\":%lu,\"tx\":%lu,\"baud\":%lu,"
    "\"wifi\":%s,\"ip\":\"%d.%d.%d.%d\",\"apip\":\"%d.%d.%d.%d\","
    "\"heap\":%u,\"minheap\":%u,\"mqtt\":%s,\"modules\":%d,"
    "\"stk\":{\"rs485\":%u,\"web\":%u,\"net\":%u,\"ota\":%u,\"rtc\":%u},"
    "\"time\":\"%s\",\"ntpSynced\":%s,\"maint\":%s,\"quiet\":%s,"
    "\"companion\":{\"status\":\"%s\",\"age\":%ld}}",
    millis()/1000, rxCount, txCount, cfg.rs485Baud,
    (WiFi.status()==WL_CONNECTED)?"true":"false",
    lip[0],lip[1],lip[2],lip[3],
    aip[0],aip[1],aip[2],aip[3],
    ESP.getFreeHeap(), ESP.getMinFreeHeap(),
    mqtt.connected()?"true":"false",
    sfModuleCount,
    stk485, stkWeb, stkNet, stkOta, stkRtc,
    rtcBuf,
    ntpSynced?"true":"false",
    gMaintenanceMode?"true":"false",
    gQuietTime?"true":"false",
    gCompanionStatus, compAge);
  server.send(200, "application/json", out);
}

// GET /api/config
static void handleApiConfigGet() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  JsonDocument doc;
  doc["version"]  = FW_VERSION;   // v3.1: lets the companion feature-gate on the firmware
  doc["wSSID"]    = cfg.wifiSSID;
  doc["mqHost"]   = cfg.mqttHost;
  doc["mqPort"]   = cfg.mqttPort;
  doc["mqUser"]   = cfg.mqttUser;
  doc["mqPfx"]    = cfg.mqttPrefix;
  doc["baud"]     = cfg.rs485Baud;
  doc["dataBits"] = cfg.rs485DataBits;
  doc["parity"]   = cfg.rs485Parity;
  doc["stopBits"] = cfg.rs485StopBits;
  doc["posixTZ"]    = cfg.posixTZ;
  doc["ntpServer"]  = cfg.ntpServer;
  doc["gridRows"]   = cfg.gridRows;
  doc["gridCols"]   = cfg.gridCols;
  doc["serialDebug"]   = cfg.serialDebug;
  doc["haEnabled"]     = cfg.haEnabled;
  doc["otaPasswordSet"] = (strlen(cfg.otaPassword) > 0);
  char out[768];   // headroom for "version" + JSON-escaped SSID/TZ strings
  serializeJson(doc, out, sizeof(out));
  server.send(200, "application/json", out);
}

// POST /api/config/wifi
static void handleApiConfigWifi() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  strlcpy(cfg.wifiSSID, doc["ssid"] | "", sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPass, doc["pass"] | "", sizeof(cfg.wifiPass));
  saveConfig();
  DBG("[CFG] WiFi SSID set to '%s'\n", cfg.wifiSSID);
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  WiFi.disconnect();
}

// POST /api/config/mqtt
static void handleApiConfigMqtt() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  strlcpy(cfg.mqttHost,   doc["host"]   | "", sizeof(cfg.mqttHost));
  cfg.mqttPort          = doc["port"]   | DEFAULT_MQTT_PORT;
  strlcpy(cfg.mqttUser,   doc["user"]   | "", sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,   doc["pass"]   | "", sizeof(cfg.mqttPass));
  strlcpy(cfg.mqttPrefix, doc["prefix"] | DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));
  saveConfig();
  DBG("[CFG] MQTT broker set to %s:%d  prefix=%s\n", cfg.mqttHost, cfg.mqttPort, cfg.mqttPrefix);
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  mqtt.disconnect();
  // PubSubClient caches the broker it was last told to dial. mqttInit() is the only
  // other caller and runs once at boot, so without this the new broker is saved and
  // logged but never actually dialled: the reconnect keeps using the boot-time target
  // (or none at all, if none was configured then) and fails forever with rc=-2.
  if (strlen(cfg.mqttHost)) mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqttFailCount = 0;   // a fresh broker gets a fresh strike budget
}

// POST /api/config/rs485
static void handleApiConfigRS485() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  unsigned long newBaud = doc["baud"]     | cfg.rs485Baud;
  cfg.rs485DataBits     = doc["dataBits"] | cfg.rs485DataBits;
  cfg.rs485Parity       = doc["parity"]   | cfg.rs485Parity;
  cfg.rs485StopBits     = doc["stopBits"] | cfg.rs485StopBits;
  bool baudChanged      = (newBaud != cfg.rs485Baud);
  cfg.rs485Baud         = newBaud;
  saveConfig();
  if (baudChanged) { rs485.end(); rs485Begin(); }
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/config/settings  -- save all settings in one call
static void handleApiConfigSettings() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  // WiFi
  if (doc["ssid"].is<const char*>()) strlcpy(cfg.wifiSSID, doc["ssid"] | "", sizeof(cfg.wifiSSID));
  if (doc["pass"].is<const char*>()) strlcpy(cfg.wifiPass, doc["pass"] | "", sizeof(cfg.wifiPass));
  // MQTT
  if (doc["mqHost"].is<const char*>())   strlcpy(cfg.mqttHost,   doc["mqHost"]   | "", sizeof(cfg.mqttHost));
  if (doc["mqPort"].is<int>())           cfg.mqttPort = doc["mqPort"];
  if (doc["mqUser"].is<const char*>())   strlcpy(cfg.mqttUser,   doc["mqUser"]   | "", sizeof(cfg.mqttUser));
  if (doc["mqPass"].is<const char*>())   strlcpy(cfg.mqttPass,   doc["mqPass"]   | "", sizeof(cfg.mqttPass));
  if (doc["mqPfx"].is<const char*>())    strlcpy(cfg.mqttPrefix, doc["mqPfx"]    | DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));
  // RS485
  unsigned long newBaud = doc["baud"] | cfg.rs485Baud;
  cfg.rs485DataBits  = doc["dataBits"] | cfg.rs485DataBits;
  cfg.rs485Parity    = doc["parity"]   | cfg.rs485Parity;
  cfg.rs485StopBits  = doc["stopBits"] | cfg.rs485StopBits;
  // Timezone
  // OTA password update
  if (doc["otaPassword"].is<const char*>()) {
    strlcpy(cfg.otaPassword, doc["otaPassword"] | "", sizeof(cfg.otaPassword));
    saveConfig();
    if (strlen(cfg.otaPassword) > 0) {
      ArduinoOTA.setPassword(cfg.otaPassword);
    }
    printf("[CFG] OTA password updated\n");
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  // Serial debug toggle
  if (doc["serialDebug"].is<bool>()) {
    cfg.serialDebug = doc["serialDebug"].as<bool>();
    gSerialDebug    = cfg.serialDebug;
    saveConfig();
    printf("[CFG] Serial debug %s\n", cfg.serialDebug ? "enabled" : "disabled");
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  // Home Assistant integration toggle
  if (doc["haEnabled"].is<bool>()) {
    bool was = cfg.haEnabled;
    cfg.haEnabled = doc["haEnabled"].as<bool>();
    saveConfig();
    printf("[CFG] Home Assistant integration %s\n", cfg.haEnabled ? "enabled" : "disabled");
    if (mqtt.connected()) {
      if (cfg.haEnabled && !was) { haPublishDiscovery(true); mqttPublishStateTopics(); }
      else if (!cfg.haEnabled && was) { haPublishDiscovery(false); }  // remove entities
    }
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (doc["posixTZ"].is<const char*>()) {
    strlcpy(cfg.posixTZ, doc["posixTZ"] | "UTC0", sizeof(cfg.posixTZ));
    strlcpy(gPosixTZ, cfg.posixTZ, sizeof(gPosixTZ));
    setenv("TZ", gPosixTZ, 1);
    tzset();
    ntpSynced = false;
    DBG("[CFG] Timezone set to %s\n", cfg.posixTZ);
  }
  if (doc["ntpServer"].is<const char*>()) {
    strlcpy(cfg.ntpServer, doc["ntpServer"] | DEFAULT_NTP_SERVER, sizeof(cfg.ntpServer));
    if (!cfg.ntpServer[0]) strlcpy(cfg.ntpServer, DEFAULT_NTP_SERVER, sizeof(cfg.ntpServer));
    ntpSynced = false;   // re-sync against the new server on next network tick
    DBG("[CFG] NTP server set to %s\n", cfg.ntpServer);
  }
  if (doc["gridRows"].is<int>() || doc["gridCols"].is<int>()) {
    int gr = doc["gridRows"] | cfg.gridRows;
    int gc = doc["gridCols"] | cfg.gridCols;
    if (gr < 1)   gr = 1;
    if (gr > 64)  gr = 64;   // sane upper bounds for the visual wall
    if (gc < 1)   gc = 1;
    if (gc > 64)  gc = 64;
    cfg.gridRows = (uint8_t)gr;
    cfg.gridCols = (uint8_t)gc;
    DBG("[CFG] Display grid set to %dx%d (rows x cols)\n", gr, gc);
  }
  bool baudChanged = (newBaud != cfg.rs485Baud);
  cfg.rs485Baud = newBaud;
  saveConfig();
  if (baudChanged) { rs485.end(); rs485Begin(); }
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  // Only disconnect/reconnect if WiFi or MQTT credentials were in the payload
  bool hasWifi = doc["ssid"].is<const char*>() || doc["pass"].is<const char*>();
  bool hasMqtt = doc["mqHost"].is<const char*>() || doc["mqPort"].is<int>();
  if (hasMqtt) mqtt.disconnect();
  if (hasWifi) WiFi.disconnect();
}

static void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  // PUT is used by /api/companion/settings (v3.1); the rest of the API is GET/POST.
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,PUT,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}


// ?? New command handlers ??????????????????????????????????????????

// POST /api/flap/homeoffset
static void handleApiHomeOffset() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id    = doc["id"]    | -99;
  int steps = doc["steps"] | -9999;
  if (id == -99 || steps == -9999) { sendJsonError(400, "id and steps required"); return; }
  DBG("[API] home offset module %d -> %d steps\n", id, steps);
  sfHomeOffset(id, steps);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/totalsteps
static void handleApiTotalSteps() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id    = doc["id"]    | -99;
  int steps = doc["steps"] | -1;
  if (id == -99 || steps < 0) { sendJsonError(400, "id and steps required"); return; }
  DBG("[API] total steps module %d -> %d\n", id, steps);
  sfSetTotalSteps(id, steps);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/nudge
static void handleApiNudge() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id    = doc["id"]    | -99;
  int steps = doc["steps"] | -9999;
  if (id == -99 || steps == -9999) { sendJsonError(400, "id and steps required"); return; }
  DBG("[API] nudge module %d by %d steps\n", id, steps);
  sfNudge(id, steps);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/goto
static void handleApiGoto() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id   = doc["id"]   | -99;
  int step = doc["step"] | -1;
  if (id == -99 || step < 0) { sendJsonError(400, "id and step required"); return; }
  DBG("[API] goto module %d step %d\n", id, step);
  sfGoto(id, step);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/writepos
static void handleApiWritePos() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id  = doc["id"]  | -99;
  int idx = doc["idx"] | -1;
  int pos = doc["pos"] | -1;
  if (id == -99 || idx < 0 || pos < 0) { sendJsonError(400, "id, idx and pos required"); return; }
  DBG("[API] write pos module %d idx=%d pos=%d\n", id, idx, pos);
  sfWritePos(id, idx, pos);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/autohome
static void handleApiAutoHome() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id     = doc["id"]     | -99;
  int enable = doc["enable"] | -1;
  if (id == -99 || enable < 0) { sendJsonError(400, "id and enable required"); return; }
  DBG("[API] auto-home module %d -> %s\n", id, enable ? "on" : "off");
  sfAutoHome(id, enable);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/erase
static void handleApiErase() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -99;
  if (id == -99) { sendJsonError(400, "id required"); return; }
  DBG("[API] erase map module %d\n", id);
  sfErase(id);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/factoryreset
static void handleApiFactoryReset() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -99;
  if (id == -99) { sendJsonError(400, "id required"); return; }
  DBG("[API] factory reset module %d\n", id);
  sfFactoryReset(id);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/dumpbysn
static void handleApiDumpBySN() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn = doc["sn"] | "";
  if (!sn[0]) { sendJsonError(400, "sn required"); return; }
  DBG("[API] dump by SN %s\n", sn);
  sfDumpBySN(sn);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/factoryresetbysn
static void handleApiFactoryResetBySN() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn = doc["sn"] | "";
  if (!sn[0]) { sendJsonError(400, "sn required"); return; }
  DBG("[API] factory reset by SN %s\n", sn);
  sfFactoryResetBySN(sn);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/restorebysn
static void handleApiRestoreBySN() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn         = doc["sn"]         | "";
  int         homeOffset = doc["homeOffset"]  | -9999;
  int         totalSteps = doc["totalSteps"]  | -1;
  const char* map        = doc["map"]         | "";
  int         flapCount  = doc["flapCount"]   | 0;   // 0 = leave flap set unchanged
  const char* charSet    = doc["charSet"]     | "";
  if (!sn[0] || homeOffset == -9999 || totalSteps < 0) {
    sendJsonError(400, "sn, homeOffset, totalSteps required"); return;
  }
  // Optional flap-set tail (firmware v31+ round-trips the 'A' dump's
  // :<flapCount>:<flapChars>). Validate before it touches a frame.
  bool hasCount = (flapCount != 0);
  bool hasChars = (charSet[0] != 0);
  if (hasCount && (flapCount < 1 || flapCount > SF_MAX_FLAPS)) {
    sendJsonError(400, "flapCount out of range (1-64)"); return;
  }
  char chars[SF_MAX_FLAPS + 1] = "";
  if (hasChars) {
    const char* err = sfValidateCharSet(charSet, chars, sizeof(chars));
    if (err) { sendJsonError(400, err); return; }
  }
  // Build mXW<sn>:<ho>:<ts>:<map>[:<flapCount>[:<chars>]]\n. The map can be large;
  // snprintf into a bounded static buffer (off taskWeb's stack) and reject
  // anything that would overflow a single frame -- a truncated restore command
  // would corrupt the module's EEPROM, so it's safer to refuse than to send a
  // partial map. The firmware's mXW tail parser matches this layout: a count-only
  // tail is ":<count>", a chars-only tail "::<chars>", and both ":<count>:<chars>".
  // Assemble into a single string, then frame it. Each piece checks that the
  // running length stays strictly inside the buffer (snprintf returns the length
  // it WOULD have written, which can exceed the buffer), so an overflow is caught
  // and the restore is refused rather than truncated.
  static char cmd[TX_MAX_BYTES + 1];
  char tail[8 + SF_MAX_FLAPS + 2] = "";   // ":<count>:<chars>" worst case
  if (hasCount || hasChars) {
    int t = snprintf(tail, sizeof(tail), ":");
    if (hasCount) t += snprintf(tail + t, sizeof(tail) - t, "%d", flapCount);
    if (hasChars) t += snprintf(tail + t, sizeof(tail) - t, ":%s", chars);
  }
  int n = snprintf(cmd, sizeof(cmd), "mXW%s:%d:%d:%s%s\n", sn, homeOffset, totalSteps, map, tail);
  if (n < 0 || (size_t)n >= sizeof(cmd)) {
    sendJsonError(400, "restore payload too large for one frame"); return;
  }
  DBG("[API] restore by SN %s\n", sn);
  rs485SendStr(cmd);
  server.send(200, "application/json", "{\"ok\":true}");
}

// Validate and transcode a flap character set. `charSet` is UTF-8 (as received
// over JSON); it is converted to the single-byte flap encoding (Windows-1252)
// the bus protocol and module firmware use -- so euro signs and accented letters
// each become one flap byte (see charset.h). On success the encoded bytes are
// written to `out` (NUL-terminated) and NULL is returned; otherwise a
// human-readable error message is returned.
static const char* sfValidateCharSet(const char* charSet, char* out, size_t outLen) {
  char tmp[SF_MAX_FLAPS * 4 + 4];          // hold the transcode before length-check
  bool allMapped = true;
  size_t n = utf8ToFlap(charSet, tmp, sizeof(tmp), &allMapped);
  if (!allMapped)        return "charSet has characters not in Windows-1252";
  if (n == 0)            return "charSet has no displayable characters";
  if (n > SF_MAX_FLAPS)  return "charSet too long (max 64 characters)";
  if (n + 1 > outLen)    return "charSet too long";
  memcpy(out, tmp, n + 1);
  return nullptr;
}

// POST /api/flap/flapconfig  -- configure a module's flap set ('N', firmware v31+)
//   {"id":5,"flapCount":40,"charSet":" ABC..."}      direct (id=-1 broadcasts m*N)
//   {"sn":"AABB...","flapCount":40,"charSet":"..."}  by serial number
// flapCount (1-64) and charSet are INDEPENDENT and optional; at least one is
// required. An omitted/empty side is left unchanged on the module. No reply -- the
// module applies 'N' silently; read it back with /api/flap/all (firmware v31+).
static void handleApiFlapConfig() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn        = doc["sn"]        | "";
  int         id        = doc["id"]        | -99;
  int         flapCount = doc["flapCount"] | 0;     // 0 = leave unchanged
  const char* charSet   = doc["charSet"]   | "";
  bool hasCount = (flapCount != 0);
  bool hasChars = (charSet[0] != 0);
  if (!hasCount && !hasChars) { sendJsonError(400, "flapCount or charSet required"); return; }
  if (hasCount && (flapCount < 1 || flapCount > SF_MAX_FLAPS)) {
    sendJsonError(400, "flapCount out of range (1-64)"); return;
  }
  char chars[SF_MAX_FLAPS + 1] = "";
  if (hasChars) {
    const char* err = sfValidateCharSet(charSet, chars, sizeof(chars));
    if (err) { sendJsonError(400, err); return; }
  }
  int   reqCount = hasCount ? flapCount : 0;
  const char* reqChars = hasChars ? chars : nullptr;
  if (sn[0]) {
    DBG("[API] flap config SN %s count=%d chars='%s'\n", sn, flapCount, chars);
    sfSetFlapConfigBySN(sn, reqCount, reqChars);
  } else {
    if (id < -1 || id > 254) { sendJsonError(400, "id required (-1 broadcast, 0-254)"); return; }
    DBG("[API] flap config module %d count=%d chars='%s'\n", id, flapCount, chars);
    sfSetFlapConfig(id, reqCount, reqChars);
  }
  server.send(200, "application/json", "{\"ok\":true}");
}


// POST /api/flap/dump
static void handleApiDump() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -99;
  if (id < 0 || id > 254) { sendJsonError(400, "id required (0-254)"); return; }
  DBG("[API] dump module %d\n", id);

  char sn[21] = "";
  char fwVer[8] = "";
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* m = sfFindById((uint8_t)id);
  if (m) {
    strlcpy(sn,    m->serialNum, sizeof(sn));
    strlcpy(fwVer, m->fwVersion, sizeof(fwVer));
  }
  xSemaphoreGive(sfMutex);

  // Parse firmware version number (strip leading 'v' if present)
  const char* verStr = (fwVer[0] == 'v' || fwVer[0] == 'V') ? fwVer + 1 : fwVer;
  int fwVerNum = atoi(verStr);

  // Read fresh: prefer the serial-number dump mXD<sn> for fw>=15 with a known
  // SN; fall back to m<id>d if that gets no reply. A full 64-flap dump is ~565
  // bytes (~590ms to transmit at 9600 baud) plus the module's EEPROM-read time,
  // so the wait must be well over 500ms; 1200ms lets mXD fully respond before any
  // fallback, avoiding a half-duplex collision with a late reply.
  char rawDump[TX_MAX_BYTES] = "";
  bool gotReply = false;
  if (fwVerNum >= 15 && sn[0]) {
    DBG("[API] dump module %d via SN %s (fw=%d)\n", id, sn, fwVerNum);
    char f[40]; snprintf(f, sizeof(f), "mXD%s\n", sn);
    gotReply = sfSendAndCaptureDump(id, f, 1200, rawDump, sizeof(rawDump));
  }
  if (!gotReply) {
    DBG("[API] dump module %d via ID (fw=%d)\n", id, fwVerNum);
    char f[16]; snprintf(f, sizeof(f), "m%dd\n", id);
    gotReply = sfSendAndCaptureDump(id, f, 1200, rawDump, sizeof(rawDump));
  }
  gDump.waitId = -1;  // disarm capture

  if (gotReply) {
    // JSON-escape the dump, then format the reply (static buffers: off taskWeb's
    // stack, and the synchronous server serves one request at a time). sn is
    // validated alphanumeric, so it needs no escaping.
    static char escDump[TX_MAX_BYTES * 2];
    size_t ei = 0;
    for (const char* p2 = rawDump; *p2 && ei < sizeof(escDump) - 2; p2++) {
      if (*p2 == '"' || *p2 == '\\') escDump[ei++] = '\\';
      escDump[ei++] = *p2;
    }
    escDump[ei] = 0;
    static char out[TX_MAX_BYTES * 2 + 96];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"id\":%d,\"sn\":\"%s\",\"dump\":\"%s\",\"stale\":false}",
             id, sn, escDump);
    server.send(200, "application/json", out);
  } else {
    server.send(200, "application/json",
      "{\"ok\":false,\"error\":\"no response from module\"}");
  }
}

// POST /api/flap/all  {"id":N}
// Refresh a module's COMPLETE state -- firmware version, serial, and EEPROM dump.
// For a module known to be v25+ this is a SINGLE bus transaction using the
// combined 'A' command, instead of a version query followed by a dump. For older
// firmware (or if 'A' times out) it falls back to the classic version-then-dump
// sequence. Returns the same dump string the /api/flap/dump endpoint does, plus
// the refreshed ver/sn, so the Info dialog can render from one response.
static void handleApiAll() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -99;
  if (id < 0 || id > 254) { sendJsonError(400, "id required (0-254)"); return; }
  DBG("[API] all (version+EEPROM) module %d\n", id);

  char sn[21] = "", fwVer[8] = "";
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* m = sfFindById((uint8_t)id);
  if (m) { strlcpy(sn, m->serialNum, sizeof(sn)); strlcpy(fwVer, m->fwVersion, sizeof(fwVer)); }
  xSemaphoreGive(sfMutex);
  const char* verStr = (fwVer[0] == 'v' || fwVer[0] == 'V') ? fwVer + 1 : fwVer;
  int fwVerNum = atoi(verStr);

  static char rawDump[TX_MAX_BYTES];   // static: keeps taskWeb's stack clear of a 768B frame
  rawDump[0] = 0;
  bool gotReply = false;
  const char* mode = "A";

  if (fwVerNum >= 25) {
    // Known v25+: one combined 'A' transaction. If it times out the module is
    // offline -- a v+d fallback would almost certainly also time out, just ~2s
    // slower -- so we return stale rather than fall back. The next version read
    // (Identify, stale-probe, explicit query) self-corrects the cache if the
    // module was in fact downgraded or swapped for an older one.
    char f[64];
    if (sn[0]) { DBG("[API] all via mXA %s\n", sn); snprintf(f, sizeof(f), "mXA%s\n", sn); }
    else       { DBG("[API] all via m%dA\n", id);   snprintf(f, sizeof(f), "m%dA\n", id); }
    gotReply = sfSendAndCaptureDump(id, f, 1300, rawDump, sizeof(rawDump));  // 'A' ~570ms + assembly
  } else {
    // Unknown or older firmware: version query, then dump (two transactions).
    // Wait for the version reply to clear the bus before the dump goes out.
    mode = "vd";
    sfSendVersionAndWait(id, 700, fwVer, sizeof(fwVer), sn, sizeof(sn), NULL);
    verStr   = (fwVer[0] == 'v' || fwVer[0] == 'V') ? fwVer + 1 : fwVer;
    fwVerNum = atoi(verStr);
    char f[40];
    if (fwVerNum >= 15 && sn[0]) snprintf(f, sizeof(f), "mXD%s\n", sn);
    else                         snprintf(f, sizeof(f), "m%dd\n", id);
    gotReply = sfSendAndCaptureDump(id, f, 1300, rawDump, sizeof(rawDump));
  }
  gDump.waitId = -1;  // disarm capture
  // Snapshot the 'A'-only extras the parse left behind (n/a == -99 for the v+d
  // path or a stale read). Safe to read now: the slot is disarmed, so no later
  // reply can overwrite them before we format.
  int aAutoHome   = gDump.autoHome;
  int aCurIndex   = gDump.curIndex;
  int aReportedId = gDump.reportedId;
  // Configurable flap set from the v31+ 'A' tail (-99 / "" when not provided).
  // gDump.flapChars holds raw Windows-1252 bytes; convert to JSON-safe UTF-8 so
  // euro/accented glyphs survive in the JSON response (up to 3 bytes each).
  int aFlapCount  = gDump.flapCount;
  static char aFlapChars[SF_MAX_FLAPS * 3 + 4];   // static: off taskWeb stack
  flapToJsonUtf8((const char*)gDump.flapChars, strlen((const char*)gDump.flapChars),
                   aFlapChars, sizeof(aFlapChars));

  // Read the freshest version/serial the reply left in the registry.
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* mf = sfFindById((uint8_t)id);
  if (mf) { strlcpy(sn, mf->serialNum, sizeof(sn)); strlcpy(fwVer, mf->fwVersion, sizeof(fwVer)); }
  xSemaphoreGive(sfMutex);

  // JSON-escape the dump, then format the reply (static buffers: off taskWeb's
  // stack; the synchronous server serves one request at a time). sn is validated
  // alphanumeric and fwVer is a version token, so neither needs escaping.
  static char escDump[TX_MAX_BYTES * 2];
  size_t ei = 0;
  for (const char* p2 = rawDump; *p2 && ei < sizeof(escDump) - 2; p2++) {
    if (*p2 == '"' || *p2 == '\\') escDump[ei++] = '\\';
    escDump[ei++] = *p2;
  }
  escDump[ei] = 0;
  static char out[TX_MAX_BYTES * 2 + 256];
  snprintf(out, sizeof(out),
           "{\"ok\":true,\"id\":%d,\"ver\":\"%s\",\"sn\":\"%s\",\"dump\":\"%s\","
           "\"autoHome\":%d,\"curIndex\":%d,\"reportedId\":%d,"
           "\"flapCount\":%d,\"flapChars\":\"%s\",\"stale\":%s,\"mode\":\"%s\"}",
           id, fwVer, sn, escDump, aAutoHome, aCurIndex, aReportedId,
           aFlapCount, aFlapChars, gotReply ? "false" : "true", mode);
  server.send(200, "application/json", out);
}

// POST /api/flap/identify
static void handleApiIdentify() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  DBG("[API] identify all -- clearing registry and broadcasting m*v\n");
  // Wipe both the in-memory list and the persisted copy, then re-discover.
  sfModulesClear();
  rs485SendStr("m*v\n");
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/mqtt/test {host?,port?,user?,pass?} -- tries the given (or saved)
// broker settings WITHOUT touching the live connection, so settings can be
// verified before saving. Two phases: TCP reachability (3s cap), then a real
// MQTT CONNECT/CONNACK using a throwaway client. Runs on taskWeb (8KB stack;
// the temporary client objects are small and its packet buffer is heap).
static void handleApiMqttTest() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char host[64]; int port = cfg.mqttPort;
  char user[48], pass[64];
  strlcpy(host, cfg.mqttHost, sizeof(host));
  strlcpy(user, cfg.mqttUser, sizeof(user));
  strlcpy(pass, cfg.mqttPass, sizeof(pass));
  if (server.hasArg("plain") && server.arg("plain").length() > 1) {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
      if (doc["host"].is<const char*>()) strlcpy(host, doc["host"] | "", sizeof(host));
      if (doc["port"].is<int>())         port = doc["port"] | cfg.mqttPort;
      if (doc["user"].is<const char*>()) strlcpy(user, doc["user"] | "", sizeof(user));
      if (doc["pass"].is<const char*>()) strlcpy(pass, doc["pass"] | "", sizeof(pass));
    }
  }
  if (!host[0]) { sendJsonError(400, "no broker host configured"); return; }
  DBG("[MQTT] testing %s:%d\n", host, port);

  wdgWebMs = millis();
  WiFiClient testNet;
  // Phase 1: TCP reachability with an explicit 3s cap.
  if (!testNet.connect(host, (uint16_t)port, 3000)) {
    server.send(200, "application/json",
      "{\"ok\":false,\"tcp\":false,\"mqtt\":false,"
      "\"error\":\"TCP connect failed (host/port unreachable)\"}");
    return;
  }
  wdgWebMs = millis();
  // Phase 2: real MQTT CONNECT on the already-open socket. PubSubClient skips
  // its own TCP connect when the client is connected, so this only exchanges
  // CONNECT/CONNACK. CONNACK from a live broker arrives in milliseconds.
  PubSubClient testMq(testNet);
  testMq.setBufferSize(128);   // CONNECT/CONNACK only -- keep the heap use tiny
  bool mqOk;
  if (user[0]) mqOk = testMq.connect("sfgw-test", user, pass);
  else         mqOk = testMq.connect("sfgw-test");
  int state = testMq.state();
  testMq.disconnect();
  testNet.stop();
  wdgWebMs = millis();

  const char* why = "";
  switch (state) {                       // PubSubClient state codes
    case  0: why = "connected";                       break;
    case  1: why = "bad protocol version";            break;
    case  2: why = "client id rejected";              break;
    case  3: why = "broker unavailable";              break;
    case  4: why = "bad username or password";        break;
    case  5: why = "not authorized";                  break;
    case -2: why = "network failed during handshake"; break;
    case -4: why = "broker did not respond (timeout)";break;
    default: why = "connection failed";               break;
  }
  char out[160];
  snprintf(out, sizeof(out),
    "{\"ok\":%s,\"tcp\":true,\"mqtt\":%s,\"state\":%d,\"detail\":\"%s\"}",
    mqOk ? "true" : "false", mqOk ? "true" : "false", state, why);
  server.send(200, "application/json", out);
}

// GET/POST /api/maintenance
static void handleApiMaintenance() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // GET returns current state; POST {"on":true|false} sets it.
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Bad JSON"); return;
    }
    if (!doc["on"].is<bool>()) { sendJsonError(400, "'on' (bool) required"); return; }
    gMaintenanceMode = doc["on"].as<bool>();
    printf("[MAINT] Maintenance mode %s\n", gMaintenanceMode ? "ENABLED" : "disabled");
    mqttPublishStateTopics();
  }
  char out[40];
  snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s}",
           gMaintenanceMode ? "true" : "false");
  server.send(200, "application/json", out);
}

// GET returns Quiet Time state; POST {"on":true|false} sets it.
static void handleApiQuiet() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Bad JSON"); return;
    }
    if (!doc["on"].is<bool>()) { sendJsonError(400, "'on' (bool) required"); return; }
    bool on = doc["on"].as<bool>();
    // The schedule wins inside its window: refuse a manual OFF here too (see the
    // MQTT handler for the rationale). Disable the schedule to override.
    if (!on && quietSchedInWindow()) {
      printf("[QUIET] REST quiet OFF ignored -- schedule active (in window)\n");
    } else {
      sfSetQuietTime(on);
    }
    mqttPublishStateTopics();
  }
  char out[40];
  snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s}", gQuietTime ? "true" : "false");
  server.send(200, "application/json", out);
}

// GET/POST /api/quiet/schedule  -- daily Quiet-Time schedule (v3.0).
// The schedule is evaluated once a second in taskRTC; when the current local
// time enters/leaves the window, Quiet Time is toggled automatically.
static void handleApiQuietSchedule() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Bad JSON"); return;
    }
    if (doc["enabled"].is<bool>())        cfg.quietSchedEnabled = doc["enabled"].as<bool>();
    if (doc["start"].is<const char*>())   strlcpy(cfg.quietStart, doc["start"].as<const char*>(), sizeof(cfg.quietStart));
    if (doc["end"].is<const char*>())     strlcpy(cfg.quietEnd,   doc["end"].as<const char*>(),   sizeof(cfg.quietEnd));
    if (doc["days"].is<int>())            cfg.quietDays = (uint8_t)(doc["days"].as<int>() & 0x7F);
    if (doc["offset"].is<int>()) {        // browser's UTC offset (minutes east of UTC)
      int o = doc["offset"].as<int>();
      if (o < -720) o = -720;             // clamp to the valid TZ range (UTC-12:00 .. UTC+14:00)
      if (o >  840) o =  840;
      cfg.quietTzOffsetMin = (int16_t)o;
    }
    saveConfig();
    DBG("[CFG] Quiet schedule %s %s-%s days=0x%02X tzoff=%dmin\n",
        cfg.quietSchedEnabled ? "on" : "off", cfg.quietStart, cfg.quietEnd,
        cfg.quietDays, (int)cfg.quietTzOffsetMin);
  }
  JsonDocument out;
  out["enabled"] = cfg.quietSchedEnabled;
  out["start"]   = cfg.quietStart;
  out["end"]     = cfg.quietEnd;
  out["days"]    = cfg.quietDays;
  out["offset"]  = cfg.quietTzOffsetMin;   // browser's UTC offset, echoed back for the client
  char buf[128];
  serializeJson(out, buf, sizeof(buf));
  server.send(200, "application/json", buf);
}

/* The dashboard's own tabs, as deep links (the hash the nav's router accepts --
   NOT the pane id: "calibration", not "calib"). Advertised to the companion at
   registration so its nav can link exactly the tabs this firmware has, instead of
   a list hard-coded over there that goes stale whenever this one changes. Keep in
   step with the <nav> in web_ui.h and the M map beside it. -- v3.4 */
static const char* const GW_TAB_ID[]  = {"modules", "display", "provision", "calibration",
                                         "monitor", "settings", "status"};
static const char* const GW_TAB_LBL[] = {"Modules", "Display", "Provision", "Calibration",
                                         "Monitor", "Settings", "Status"};
static const size_t GW_TAB_N = sizeof(GW_TAB_ID) / sizeof(GW_TAB_ID[0]);

// Store the tab list a companion advertised, re-serialised into gCompanionTabs.
// Anything malformed, oversized, or over the caps leaves the buffer EMPTY rather
// than half-filled: the dashboard then falls back to its built-in companion tabs,
// which is the same behaviour as an older companion that advertises nothing.
static void storeCompanionTabs(JsonArrayConst tabs) {
  gCompanionTabs[0] = '\0';
  if (tabs.isNull() || tabs.size() == 0 || tabs.size() > COMPANION_TABS_MAX_N) return;

  JsonDocument out;
  JsonArray arr = out.to<JsonArray>();
  for (JsonObjectConst t : tabs) {
    const char* id  = t["id"].is<const char*>()    ? t["id"].as<const char*>()    : nullptr;
    const char* lbl = t["label"].is<const char*>() ? t["label"].as<const char*>() : nullptr;
    if (!id || !lbl || !id[0] || !lbl[0]) return;
    if (strlen(id) > COMPANION_TAB_ID_MAX || strlen(lbl) > COMPANION_TAB_LBL_MAX) return;
    // The id lands in a URL hash and the label in the nav, so keep both to plain
    // printable ASCII -- no quotes, no control characters, nothing to escape.
    for (const char* p = id; *p; p++)
      if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return;
    for (const char* p = lbl; *p; p++)
      if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e || *p == '"' || *p == '\\') return;
    JsonObject e = arr.add<JsonObject>();
    e["id"]    = id;
    e["label"] = lbl;
  }
  if (measureJson(out) >= sizeof(gCompanionTabs)) return;   // would not fit: advertise nothing
  serializeJson(out, gCompanionTabs, sizeof(gCompanionTabs));
}

// GET/POST /api/companion  -- the companion app registers its URL here (v3.0)
// and heartbeats its running status. The URL is persisted (only rewritten to
// NVS when it changes, to avoid flash wear from heartbeats); the status is
// runtime-only. An empty url deregisters.
//
// v3.4: the POST may carry `tabs` -- the deep links the companion's own UI has --
// and the response always carries `gwTabs`, this firmware's. Either side may say
// nothing (an older peer), in which case the other falls back to its built-in list.
static void handleApiCompanion() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Bad JSON"); return;
    }
    if (doc["url"].is<const char*>()) {
      const char* url = doc["url"].as<const char*>();
      if (strcmp(url, cfg.companionUrl) != 0) {
        // Apply to RAM now -- the companion tabs must light up immediately -- but do
        // NOT saveConfig() here. Two companions registering against one gateway flip
        // this value on every heartbeat, and an unconditional save made that an NVS
        // write every ~30 s for as long as both were up. taskNetwork persists it once
        // the value has held still (COMPANION_SAVE_DEBOUNCE_MS); a contested URL never
        // reaches flash, which is what we want.
        strlcpy(cfg.companionUrl, url, sizeof(cfg.companionUrl));
        gCompanionUrlDirty   = true;
        gCompanionUrlDirtyMs = millis();   // restart the clock on EVERY change
        DBG("[CFG] Companion URL set to %s\n", cfg.companionUrl);
      }
      if (url[0] == '\0') {                                                      // deregister
        gCompanionStatus[0] = '\0'; gCompanionTabs[0] = '\0'; gCompanionSeenMs = 0;
      } else gCompanionSeenMs = millis();
    }
    // A companion that advertises its tabs re-sends them on every heartbeat, so
    // this is a plain overwrite. One that never mentions `tabs` leaves whatever we
    // hold alone -- a heartbeat carrying only a status must not wipe the list.
    if (doc["tabs"].is<JsonArrayConst>()) storeCompanionTabs(doc["tabs"].as<JsonArrayConst>());
    if (doc["status"].is<const char*>()) {
      // Copy + sanitise so the string is always JSON-safe when echoed back.
      const char* st = doc["status"].as<const char*>();
      size_t n = 0;
      for (size_t i = 0; st[i] && n < sizeof(gCompanionStatus) - 1; i++) {
        char c = st[i];
        if (c == '"' || c == '\\') c = '\'';
        if ((unsigned char)c < 0x20) c = ' ';
        gCompanionStatus[n++] = c;
      }
      gCompanionStatus[n] = '\0';
      gCompanionSeenMs = millis();
    }
  }
  JsonDocument out;
  out["url"]    = cfg.companionUrl;
  out["status"] = gCompanionStatus;
  // The companion's tabs, for the dashboard's nav. Already valid JSON (we wrote
  // it with serializeJson), so splice it in verbatim rather than re-parsing it.
  // Empty array = this companion never advertised any; the dashboard then uses
  // its built-in list.
  out["tabs"]   = serialized(gCompanionTabs[0] ? gCompanionTabs : "[]");
  // ...and ours, for the companion's nav.
  JsonArray gw = out["gwTabs"].to<JsonArray>();
  for (size_t i = 0; i < GW_TAB_N; i++) {
    JsonObject e = gw.add<JsonObject>();
    e["id"]    = GW_TAB_ID[i];
    e["label"] = GW_TAB_LBL[i];
  }
  // Sized for the worst case: a full-size companion list (384) + our gwTabs (~300)
  // + the URL (128) + the status (80) + keys. A String would heap-allocate on every
  // heartbeat, and the dashboard polls this endpoint every 4 s.
  char buf[COMPANION_TABS_MAX + 768];
  serializeJson(out, buf, sizeof(buf));
  server.send(200, "application/json", buf);
}

/* ----------------------------------------------------------
   Companion settings blob  (v3.1)

   A stateless companion keeps its settings/playlists/triggers here instead of on
   its own disk. The payload is gzip(minified JSON) whose schema belongs entirely
   to the companion -- the gateway stores the bytes verbatim and never parses them.

   The body is binary, which rules out server.arg("plain"): the WebServer copies a
   non-form body through String(char*), so it would stop at the first NUL byte --
   and a gzip header carries one at offset 3. Instead the PUT registers an upload
   callback, which makes the WebServer take its "raw" path and stream the body to
   us in HTTP_RAW_BUFLEN chunks. handleApiCompanionSettingsRaw writes those chunks
   straight to a temp file; handleApiCompanionSettingsPut then sends the response.
---------------------------------------------------------- */
static File   compFile;          // temp file, open across the RAW_WRITE chunks
static size_t compRecvd = 0;     // bytes written so far this request
static int    compErr   = 0;     // 0 = ok, else the HTTP status to report

// Give up on the transfer: close + delete the temp file, remember the status.
// The WebServer keeps draining the socket either way (we cannot stop its read
// loop), so every later chunk is simply dropped and the response still lands.
static void compAbort(int status) {
  if (compFile) compFile.close();
  if (sfFsReady) FFat.remove(COMPANION_TMP);
  compErr = status;
}

// PUT /api/companion/settings -- raw body callback (one call per chunk)
static void handleApiCompanionSettingsRaw() {
  HTTPRaw& raw = server.raw();
  wdgWebMs = millis();          // a slow client must not trip the web watchdog

  switch (raw.status) {
    case RAW_START: {
      compRecvd = 0;
      compErr   = 0;
      size_t len = (size_t)server.clientContentLength();
      // Decide before opening anything, so a bad request never touches flash.
      if (!sfFsReady)                  { compErr = 503; break; }  // no filesystem mounted
      if (len == 0)                    { compErr = 400; break; }  // nothing to store
      if (len > COMPANION_MAX_BYTES)   { compErr = 413; break; }
      FFat.remove(COMPANION_TMP);                                 // clear a stale temp file
      compFile = FFat.open(COMPANION_TMP, "w");
      if (!compFile) compErr = 507;
      break;
    }

    case RAW_WRITE:
      if (compErr || !compFile) break;                         // already failed -- drain
      // Content-Length was checked up front, but a body may overrun it; re-check
      // so a lying header still cannot fill the flash.
      if (compRecvd + raw.currentSize > COMPANION_MAX_BYTES) { compAbort(413); break; }
      if (compFile.write(raw.buf, raw.currentSize) != raw.currentSize) { compAbort(507); break; }
      compRecvd += raw.currentSize;
      break;

    case RAW_END:
      if (compErr) { compAbort(compErr); break; }              // reuse the cleanup path
      compFile.close();
      // A truncated body (fewer bytes than promised) must not overwrite good settings.
      if (compRecvd != (size_t)server.clientContentLength()) { compAbort(400); break; }
      // Publish atomically: the old blob survives intact until the rename lands.
      FFat.remove(COMPANION_FILE);
      if (!FFat.rename(COMPANION_TMP, COMPANION_FILE)) { compAbort(507); break; }
      DBG("[CFG] Companion settings stored (%u bytes)\n", (unsigned)compRecvd);
      break;

    case RAW_ABORTED:
      compAbort(400);
      break;
  }
}

// PUT /api/companion/settings -- response, after the raw body has been consumed
static void handleApiCompanionSettingsPut() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  int err = compErr;
  compErr = 0;                  // never leak this request's verdict into the next
  switch (err) {
    case 0:   break;
    case 400: sendJsonError(400, "Empty or truncated body"); return;
    case 413: sendJsonError(413, "Settings blob too large"); return;
    case 503: sendJsonError(503, "No filesystem");         return;
    default:  sendJsonError(507, "Write failed");          return;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"bytes\":%u}", (unsigned)compRecvd);
  server.send(200, "application/json", buf);
}

// GET /api/companion/settings -- hand the stored blob back byte-for-byte
static void handleApiCompanionSettingsGet() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  if (!sfFsReady || !FFat.exists(COMPANION_FILE)) { sendJsonError(404, "No settings stored"); return; }
  File f = FFat.open(COMPANION_FILE, "r");
  if (!f) { sendJsonError(404, "No settings stored"); return; }
  size_t n = f.size();
  if (n == 0) { f.close(); sendJsonError(404, "No settings stored"); return; }

  wdgWebMs = millis();
  server.client().setConnectionTimeout(3000);   // cap per-write blocking (see handleRoot:
                                               // setTimeout() here was a no-op)
  server.setContentLength(n);
  // Deliberately NOT "Content-Encoding: gzip": these bytes are the payload, not a
  // transfer encoding of it. Declaring the encoding would make HTTP clients gunzip
  // the body transparently, and the companion -- which decompresses itself -- would
  // then be handed plain JSON it tries to decompress again.
  server.send(200, "application/gzip", "");
  uint8_t buf[512];
  while (size_t got = f.read(buf, sizeof(buf))) {
    server.sendContent((const char*)buf, got);
    wdgWebMs = millis();
  }
  f.close();
}

void webInit() {
  static const char* COLLECT_HDRS[] = { "If-None-Match" };
  server.collectHeaders(COLLECT_HDRS, 1);   // so handleRoot can honour conditional GETs
  server.on("/",                     HTTP_GET,     handleRoot);
  server.on("/favicon.svg",          HTTP_GET,     handleFavicon);
  server.on("/logo.svg",             HTTP_GET,     handleLogo);
  // v3.5: one route per UI language, "/lang/<code>". Registering them individually
  // (rather than parsing a path parameter) keeps the URI matcher plain and makes an
  // unknown code fall through to the normal 404. English is never registered -- it is
  // the text already in the page.
  for (size_t i = 0; i < UI_LANG_COUNT; i++) {
    server.on((String("/lang/") + UI_LANGS[i].code).c_str(), HTTP_GET, [i]() { handleLang(i); });
  }
  server.on("/ota",                  HTTP_GET,     handleOTAPage);
  server.on("/api/ota/upload",       HTTP_POST,    sendOTAUploadResult, handleOTAUpload);
  server.on("/api/rs485/messages",   HTTP_GET,     handleApiMessages);
  server.on("/api/rs485/send",       HTTP_POST,    handleApiSend);
  server.on("/api/rs485/send",       HTTP_OPTIONS, handleOptions);
  server.on("/api/rs485/batch",      HTTP_POST,    handleApiSendBatch);
  server.on("/api/rs485/batch",      HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/modules",     HTTP_GET,     handleApiModules);
  server.on("/api/display/state",    HTTP_GET,     handleApiDisplayState);
  server.on("/api/display/cells",    HTTP_POST,    handleApiDisplayCells);
  server.on("/api/display/cells",    HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/identify",    HTTP_POST,    handleApiIdentify);
  server.on("/api/flap/identify",    HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/char",        HTTP_POST,    handleApiChar);
  server.on("/api/flap/char",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/index",       HTTP_POST,    handleApiIndex);
  server.on("/api/flap/index",       HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/text",        HTTP_POST,    handleApiText);
  server.on("/api/flap/text",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/home",        HTTP_POST,    handleApiHome);
  server.on("/api/flap/home",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/calibrate",   HTTP_POST,    handleApiCalibrate);
  server.on("/api/flap/calibrate",   HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/calibrate/status", HTTP_GET, handleApiCalibrateStatus);
  server.on("/api/flap/diag",        HTTP_POST,    handleApiDiag);
  server.on("/api/flap/diag",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/diag/mech",   HTTP_POST,    handleApiDiagMech);
  server.on("/api/flap/diag/mech",   HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/diag/status", HTTP_GET,     handleApiDiagStatus);
  server.on("/api/flap/version",     HTTP_POST,    handleApiVersion);
  server.on("/api/flap/version",     HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/provision",   HTTP_POST,    handleApiProvision);
  server.on("/api/flap/deprovision", HTTP_POST,    handleApiDeprovision);
  server.on("/api/flap/deprovision", HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/homebysn",        HTTP_POST,    handleApiHomeBySN);
  server.on("/api/flap/homebysn",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/provision",       HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/homeoffset",      HTTP_POST,    handleApiHomeOffset);
  server.on("/api/flap/homeoffset",      HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/totalsteps",      HTTP_POST,    handleApiTotalSteps);
  server.on("/api/flap/totalsteps",      HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/nudge",           HTTP_POST,    handleApiNudge);
  server.on("/api/flap/nudge",           HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/goto",            HTTP_POST,    handleApiGoto);
  server.on("/api/flap/goto",            HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/writepos",        HTTP_POST,    handleApiWritePos);
  server.on("/api/flap/writepos",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/autohome",        HTTP_POST,    handleApiAutoHome);
  server.on("/api/flap/autohome",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/erase",           HTTP_POST,    handleApiErase);
  server.on("/api/flap/erase",           HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/factoryreset",    HTTP_POST,    handleApiFactoryReset);
  server.on("/api/flap/factoryreset",    HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/dump",              HTTP_POST,    handleApiDump);
  server.on("/api/flap/dump",              HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/all",               HTTP_POST,    handleApiAll);
  server.on("/api/flap/all",               HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/dumpbysn",          HTTP_POST,    handleApiDumpBySN);
  server.on("/api/flap/dumpbysn",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/factoryresetbysn",HTTP_POST,    handleApiFactoryResetBySN);
  server.on("/api/flap/factoryresetbysn",HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/restorebysn",     HTTP_POST,    handleApiRestoreBySN);
  server.on("/api/flap/restorebysn",     HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/flapconfig",      HTTP_POST,    handleApiFlapConfig);
  server.on("/api/flap/flapconfig",      HTTP_OPTIONS, handleOptions);
  server.on("/api/capabilities",     HTTP_GET,     handleApiCapabilities);
  server.on("/api/capabilities",     HTTP_OPTIONS, handleOptions);
  server.on("/api/status",           HTTP_GET,     handleApiStatus);
  server.on("/api/mqtt/test",        HTTP_POST,    handleApiMqttTest);
  server.on("/api/mqtt/test",        HTTP_OPTIONS, handleOptions);
  server.on("/api/maintenance",      HTTP_GET,     handleApiMaintenance);
  server.on("/api/maintenance",      HTTP_POST,    handleApiMaintenance);
  server.on("/api/maintenance",      HTTP_OPTIONS, handleOptions);
  server.on("/api/quiet",            HTTP_GET,     handleApiQuiet);
  server.on("/api/quiet",            HTTP_POST,    handleApiQuiet);
  server.on("/api/quiet",            HTTP_OPTIONS, handleOptions);
  server.on("/api/quiet/schedule",   HTTP_GET,     handleApiQuietSchedule);
  server.on("/api/quiet/schedule",   HTTP_POST,    handleApiQuietSchedule);
  server.on("/api/quiet/schedule",   HTTP_OPTIONS, handleOptions);
  server.on("/api/companion",        HTTP_GET,     handleApiCompanion);
  server.on("/api/companion",        HTTP_POST,    handleApiCompanion);
  server.on("/api/companion",        HTTP_OPTIONS, handleOptions);
  // v3.1 blob store. Passing the 4th (upload) callback is what puts the PUT on the
  // WebServer's raw-body path, so the binary gzip arrives intact.
  server.on("/api/companion/settings", HTTP_GET,     handleApiCompanionSettingsGet);
  server.on("/api/companion/settings", HTTP_PUT,     handleApiCompanionSettingsPut,
                                                     handleApiCompanionSettingsRaw);
  server.on("/api/companion/settings", HTTP_OPTIONS, handleOptions);
  server.on("/api/config",           HTTP_GET,     handleApiConfigGet);
  server.on("/api/config/wifi",      HTTP_POST,    handleApiConfigWifi);
  server.on("/api/config/wifi",      HTTP_OPTIONS, handleOptions);
  server.on("/api/config/mqtt",      HTTP_POST,    handleApiConfigMqtt);
  server.on("/api/config/mqtt",      HTTP_OPTIONS, handleOptions);
  server.on("/api/config/rs485",     HTTP_POST,    handleApiConfigRS485);
  server.on("/api/config/rs485",     HTTP_OPTIONS, handleOptions);
  server.on("/api/config/settings",  HTTP_POST,    handleApiConfigSettings);
  server.on("/api/config/settings",  HTTP_OPTIONS, handleOptions);
  server.begin();
  printf("[Web] HTTP server %s (port 80)\n",
         server.listening() ? "started" : "FAILED to bind -- taskWeb will retry");
}

// server.begin() is called exactly once, at boot, and its result was never checked; a silent bind
// failure there refuses every request for the whole uptime because nothing re-calls begin(). This
// is that retry. listening() is the ground truth (NetworkServer::_listening), so on a healthy
// server this is a no-op -- it re-establishes the listener only when it is genuinely down, which
// also recovers the case where begin() ran (at setup) before the network stack was ready. Called
// every 20s from taskWeb. Returns true when the listener is live.
bool webEnsureListening() {
  if (server.listening()) return true;
  printf("[Web] port 80 listener is down -- re-establishing\n");
  server.begin();
  const bool up = server.listening();
  printf("[Web] listener %s\n", up ? "restored" : "still down (will retry)");
  return up;
}
