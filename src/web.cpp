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
static void handleApiRestoreBySN();
static void handleApiSend();
static void handleApiSendBatch();
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
  server.send(200, "image/svg+xml", FAVICON_SVG);
}

// Web UI wordmark (header logo): the app name on a split-flap board -- the same
// two-flap tiles, seam and pivots as the favicon, one cell per character. Served
// at /logo.svg and used in <h1> in place of the title text.
const char LOGO_SVG[] =
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 447.6 44' role='img' aria-label='Split-Flap Gateway'><defs><linearGradient id='sfTop' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#3c424c'/><stop offset='1' stop-color='#2d323b'/></linearGradient><linearGradient id='sfBot' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#272b32'/><stop offset='1' stop-color='#181b20'/></linearGradient></defs><rect x='3' y='2' width='250' height='40' rx='6' fill='#000' opacity='0.30'/><clipPath id='clip1'><rect x='3' y='0' width='250' height='40' rx='6'/></clipPath><g clip-path='url(#clip1)'><rect x='3' y='0' width='250' height='20' fill='url(#sfTop)'/><rect x='3' y='20' width='250' height='20' fill='url(#sfBot)'/><rect x='27.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='28.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='52.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='53.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='77.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='78.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='102.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='103.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='127.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='128.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='152.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='153.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='177.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='178.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='202.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='203.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='227.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='228.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><text x='15.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>S</text><text x='40.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>P</text><text x='65.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>L</text><text x='90.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>I</text><text x='115.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>T</text><text x='140.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>-</text><text x='165.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>F</text><text x='190.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>L</text><text x='215.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>A</text><text x='240.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>P</text><rect x='3' y='18.9' width='250' height='2.2' fill='#0c0d10'/><rect x='3' y='21.1' width='250' height='0.8' fill='#565c68' opacity='0.7'/></g><rect x='1.2' y='17.5' width='3.6' height='5' rx='1.4' fill='#0c0d10'/><rect x='251.2' y='17.5' width='3.6' height='5' rx='1.4' fill='#0c0d10'/><rect x='3' y='0' width='250' height='40' rx='6' fill='none' stroke='#0a0b0d' stroke-width='0.8'/><rect x='269' y='2' width='175' height='40' rx='6' fill='#000' opacity='0.30'/><clipPath id='clip2'><rect x='269' y='0' width='175' height='40' rx='6'/></clipPath><g clip-path='url(#clip2)'><rect x='269' y='0' width='175' height='20' fill='url(#sfTop)'/><rect x='269' y='20' width='175' height='20' fill='url(#sfBot)'/><rect x='293.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='294.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='318.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='319.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='343.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='344.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='368.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='369.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='393.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='394.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='418.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='419.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><text x='281.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>G</text><text x='306.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>A</text><text x='331.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>T</text><text x='356.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>E</text><text x='381.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>W</text><text x='406.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>A</text><text x='431.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>Y</text><rect x='269' y='18.9' width='175' height='2.2' fill='#0c0d10'/><rect x='269' y='21.1' width='175' height='0.8' fill='#565c68' opacity='0.7'/></g><rect x='267.2' y='17.5' width='3.6' height='5' rx='1.4' fill='#0c0d10'/><rect x='442.2' y='17.5' width='3.6' height='5' rx='1.4' fill='#0c0d10'/><rect x='269' y='0' width='175' height='40' rx='6' fill='none' stroke='#0a0b0d' stroke-width='0.8'/></svg>";
// GET /logo.svg
static void handleLogo() {
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

// GET /
static void handleRoot() {
  wdgWebMs = millis();                 // streaming response can take a while
  // Cap per-write blocking so a stalled browser cannot wedge taskWeb.
  server.client().setTimeout(3000);    // 3s per socket operation
  // The page is embedded in the firmware, so it changes with every FW update.
  // Without this, browsers cache the old HTML/JS and keep serving stale UI after
  // a flash -- tell them never to cache it so a reload always gets the new page.
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
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
  unsigned long budget = 0;          // total artificial delay (ms), capped
  for (JsonVariant v : frames) {
    if (sent >= 512) break;          // bound the batch
    const char* f = v.as<const char*>();
    if (!f || !*f) continue;
    uint8_t outBuf[TX_MAX_BYTES];
    size_t outLen = min(strlen(f), (size_t)TX_MAX_BYTES);
    memcpy(outBuf, f, outLen);
    rs485Send(outBuf, outLen, false);
    sent++;
    wdgWebMs = millis();             // feed the web watchdog during a long batch
    if (step > 0 && budget < 8000UL) { delay(step); budget += (unsigned long)step; }
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

  int emitted = 0;
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
    snprintf(obj, sizeof(obj),
      "%s{\"id\":%d,\"sn\":\"%s\",\"provisioned\":%s,\"acked\":%s,\"flapIndex\":%d,"
      "\"flapChar\":\"%s\",\"fwVersion\":\"%s\",\"lastSeen\":%lu,\"lastSeenEpoch\":%lu,"
      "\"dupSuspect\":%s}",
      emitted ? "," : "", (int)m.id, m.serialNum, m.provisioned ? "true" : "false",
      m.acked ? "true" : "false",
      m.flapIndex, flapBuf, m.fwVersion, m.lastSeen, m.lastSeenEpoch,
      m.dupSuspect ? "true" : "false");
    server.sendContent(obj);
    emitted++;
  }
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
  char out[640];
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
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
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

// GET/POST /api/companion  -- the companion app registers its URL here (v3.0)
// and heartbeats its running status. The URL is persisted (only rewritten to
// NVS when it changes, to avoid flash wear from heartbeats); the status is
// runtime-only. An empty url deregisters.
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
      if (strcmp(url, cfg.companionUrl) != 0) {      // persist only on change
        strlcpy(cfg.companionUrl, url, sizeof(cfg.companionUrl));
        saveConfig();
        DBG("[CFG] Companion URL set to %s\n", cfg.companionUrl);
      }
      if (url[0] == '\0') { gCompanionStatus[0] = '\0'; gCompanionSeenMs = 0; }  // deregister
      else gCompanionSeenMs = millis();
    }
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
  char buf[256];
  serializeJson(out, buf, sizeof(buf));
  server.send(200, "application/json", buf);
}

void webInit() {
  server.on("/",                     HTTP_GET,     handleRoot);
  server.on("/favicon.svg",          HTTP_GET,     handleFavicon);
  server.on("/logo.svg",             HTTP_GET,     handleLogo);
  server.on("/ota",                  HTTP_GET,     handleOTAPage);
  server.on("/api/ota/upload",       HTTP_POST,    sendOTAUploadResult, handleOTAUpload);
  server.on("/api/rs485/messages",   HTTP_GET,     handleApiMessages);
  server.on("/api/rs485/send",       HTTP_POST,    handleApiSend);
  server.on("/api/rs485/send",       HTTP_OPTIONS, handleOptions);
  server.on("/api/rs485/batch",      HTTP_POST,    handleApiSendBatch);
  server.on("/api/rs485/batch",      HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/modules",     HTTP_GET,     handleApiModules);
  server.on("/api/display/state",    HTTP_GET,     handleApiDisplayState);
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
  printf("[Web] HTTP server started\n");
}
