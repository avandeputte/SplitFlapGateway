#include "gateway.h"



// rs485.cpp -- RS-485 half-duplex bus layer.
// Owns the UART (rs485Begin), the single send choke point (rs485Send: strips/
// re-frames every outbound command, enforces the bus-quiet collision guard and
// Quiet Time, and logs to the monitor ring), and the PSRAM-backed diagnostic
// ring buffer the web Bus Monitor reads. Frame-classification helpers here are
// file-private. rs485Send is serialized by txMutex across tasks.
// ---- file-private forward declarations ----
static bool sfFrameIsDisplayMotion(const uint8_t* data, size_t len);
static bool sfIsDirectVersionQuery(const uint8_t* data, size_t len);
static char sfFrameCmd(const uint8_t* data, size_t len, int* outAddr);
static size_t sfKnownCommandLen(const uint8_t* data, size_t len);
static uint32_t buildSerialConfig();
static void sfQuietCapturePending(const uint8_t* data, size_t len);
static void sfTrackFromFrame(const uint8_t* data, size_t len);
static void* psramAlloc(const char* name, size_t bytes);

// Allocate the monitor ring (PSRAM preferred). Call once from setup() before
// any task that pushes to the ring is started.
// Allocate a large buffer in PSRAM (preferred) or internal RAM (fallback),
// zeroed. Logs where it landed. Returns NULL only if both allocations fail.
static void* psramAlloc(const char* name, size_t bytes) {
  void* p = NULL;
  if (psramFound()) p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (p) {
    printf("[MEM] %s in PSRAM (%u bytes)\n", name, (unsigned)bytes);
  } else {
    p = malloc(bytes);   // fallback: internal RAM
    printf("[MEM] %s in internal RAM (%u bytes)%s\n", name, (unsigned)bytes,
           psramFound() ? " -- PSRAM alloc failed" : " -- no PSRAM");
  }
  if (p) memset(p, 0, bytes);
  return p;
}

// Allocate the large runtime buffers in PSRAM to free internal RAM (which the
// WiFi/TCP stack needs during a web-OTA upload). Call once from setup() before
// any task or registry init touches these buffers. ~58 KB moved off internal
// RAM in total (monitor ring + MQTT queue + module registry).
void psramAllocInit() {
  msgRing   = (RS485Msg*) psramAlloc("monitor ring", sizeof(RS485Msg) * MSG_RING_SIZE);
  mqttQueue = (MqttQItem*) psramAlloc("MQTT queue",   sizeof(MqttQItem) * MQTT_Q_SIZE);
  sfModules = (SFModule*) psramAlloc("module registry", sizeof(SFModule) * MAX_MODULES);
}

void ringPush(const RS485Msg& m) {
  if (!msgMutex || !msgRing) return;
  xSemaphoreTake(msgMutex, portMAX_DELAY);
  msgRing[msgHead] = m;
  msgHead = (msgHead + 1) % MSG_RING_SIZE;
  xSemaphoreGive(msgMutex);
}

String ringDrain() {
  if (!msgMutex || !msgRing) return "[]";
  xSemaphoreTake(msgMutex, portMAX_DELAY);
  int head = msgHead;
  xSemaphoreGive(msgMutex);

  String out;
  // Reserve for the actual pending count (~330 bytes/message worst case:
  // 256 escaped chars + JSON overhead). A fixed 512 caused repeated reallocs
  // after a burst filled the ring (up to 64 messages = ~21KB).
  int pending = (head - msgPollCursor + MSG_RING_SIZE) % MSG_RING_SIZE;
  out.reserve((size_t)pending * 330 + 16);
  out = "[";
  bool first = true;
  int i = msgPollCursor;
  while (i != head) {
    const RS485Msg& m = msgRing[i];
    if (!first) out += ',';
    first = false;
    // Stack buffer -- no heap allocation per message
    char ascii[MSG_MAX_BYTES * 2 + 1]; size_t ai = 0;
    for (size_t j = 0; j < m.len && ai < sizeof(ascii)-2; j++) {
      uint8_t b = m.data[j];
      if      (b == '\n' || b == '\r') { /* skip newlines */ }
      else if (b == '"'  ) { ascii[ai++] = '\\'; ascii[ai++] = '"';}  // escape quote
      else if (b == '\\' ) { ascii[ai++] = '\\'; ascii[ai++] = '\\';}// escape backslash
      else if (b >= 32 && b <= 126) { ascii[ai++] = (char)b; }
      else { ascii[ai++] = '.'; }
    }
    ascii[ai] = 0;
    out += "{\"ts\":";      out += m.timestamp;
    out += ",\"ep\":";      out += m.epoch;
    out += ",\"wt\":\"";    out += m.wallTime;  out += '"';
    out += ",\"dir\":\"";   out += m.dir;       out += '"';
    out += ",\"command\":\""; out += ascii;       out += '"';
    out += ",\"len\":";     out += m.len;
    if (m.sanitized) out += ",\"san\":1";
    out += '}';
    i = (i + 1) % MSG_RING_SIZE;
  }
  out += ']';
  msgPollCursor = head;
  return out;
}
static uint32_t buildSerialConfig() {
  struct Entry { uint8_t d, p, s; uint32_t c; };
  static const Entry table[] = {
    {8,0,1,SERIAL_8N1},{8,0,2,SERIAL_8N2},
    {8,1,1,SERIAL_8E1},{8,1,2,SERIAL_8E2},
    {8,2,1,SERIAL_8O1},{8,2,2,SERIAL_8O2},
    {7,0,1,SERIAL_7N1},{7,0,2,SERIAL_7N2},
    {7,1,1,SERIAL_7E1},{7,1,2,SERIAL_7E2},
    {7,2,1,SERIAL_7O1},{7,2,2,SERIAL_7O2},
    {6,0,1,SERIAL_6N1},{5,0,1,SERIAL_5N1},
  };
  for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
    if (table[i].d == cfg.rs485DataBits &&
        table[i].p == cfg.rs485Parity   &&
        table[i].s == cfg.rs485StopBits) return table[i].c;
  }
  return SERIAL_8N1;
}

void rs485Begin() {
  // Enlarge the UART RX buffer BEFORE begin(). The default is 256 bytes, but a
  // full EEPROM dump response is ~565 bytes and streams in over ~590ms at 9600
  // baud. If taskRS485 is briefly preempted by other core-0 work while the frame
  // arrives, a 256-byte buffer (plus the 128-byte HW FIFO) can overflow and the
  // driver silently DROPS bytes -- producing mid-frame corruption. A 1024-byte
  // buffer holds a full frame with margin. Must be set before begin() to apply.
  rs485.setRxBufferSize(1024);
  rs485.begin(cfg.rs485Baud, buildSerialConfig(), RS485_RX_PIN, RS485_TX_PIN);
  rs485.setPins(-1, -1, -1, RS485_EN_PIN);
  rs485.setMode(UART_MODE_RS485_HALF_DUPLEX);
  DBG("[RS485] baud=%lu\n", cfg.rs485Baud);
}

// Inspect an outbound frame and update per-module display tracking. This runs
// for EVERY transmitted frame (called from rs485Send), so a well-formed display
// command sent through a raw path -- the Bus Monitor "Send Frame" box, the
// /api/rs485/send endpoint, or the MQTT splitflap/send topic -- updates tracking
// exactly like the high-level helpers do. Recognized forms:
//   m<id>-<char>  / m*-<char>   show character (broadcast with '*')
//   m<id>+<idx>   / m*+<idx>    show flap index (char becomes unknown)
//   m<id>h        / m*h         home (char becomes unknown)
// Anything else (provisioning mX..., version/dump/tuning commands, responses)
// is ignored. addr -1 means broadcast. Takes sfMutex internally; never call
// while already holding it.
static void sfTrackFromFrame(const uint8_t* data, size_t len) {
  // Minimum "m" + addr + cmd = 3 chars (e.g. "m*h"). Must start with 'm'.
  if (len < 3 || data[0] != 'm') return;
  // The provisioning/by-serial frames all use a literal 'X' as the address
  // token (mXadv, mXack, mXI, mXH, mXD, mXW, mXF). Those never change a known
  // module's displayed character, so skip them outright.
  if (data[1] == 'X') return;

  size_t i = 1;
  int addr;
  if (data[i] == '*') {            // broadcast
    addr = -1;
    i++;
  } else if (data[i] >= '0' && data[i] <= '9') {
    long v = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
      v = v * 10 + (data[i] - '0');
      i++;
      if (v > 254) return;         // out of valid id range -> not a display cmd
    }
    addr = (int)v;
  } else {
    return;                        // not an address we recognize
  }
  if (i >= len) return;
  char cmd = (char)data[i];

  if (cmd == '-') {                // show character: next byte is the char
    if (i + 1 >= len) return;
    char c = (char)data[i + 1];
    if (c < 0x20 || c > 0x7E) return;
    sfTrackChar(addr, c);
  } else if (cmd == '+') {         // show index: record index, char unknown
    long idx = 0;
    size_t j = i + 1;
    if (j >= len || data[j] < '0' || data[j] > '9') return;  // need a number
    while (j < len && data[j] >= '0' && data[j] <= '9') {
      idx = idx * 10 + (data[j] - '0');
      j++;
      if (idx > 63) { idx = -1; break; }   // out of flap range -> unknown
    }
    if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (addr < 0) {
        for (int k = 0; k < sfModuleCount; k++) {
          if (sfModules[k].provisioned) {
            sfModules[k].flapIndex = (int)idx;
            sfModules[k].flapChar  = 0;
          }
        }
      } else {
        SFModule* m = sfFindById((uint8_t)addr);
        if (m) { m->flapIndex = (int)idx; m->flapChar = 0; }
      }
      xSemaphoreGive(sfMutex);
    }
  } else if (cmd == 'h' &&         // home: char becomes unknown.
             (i + 1 >= len || data[i + 1] == '\n' || data[i + 1] == '\r')) {
    // Guard against false matches: 'h' must be the whole command, not a prefix
    // of something else. (There is no other 'h...' display command, but this
    // keeps the matcher strict.)
    sfTrackChar(addr, 0);
  }
}

// Parse the address + command of an outbound frame for Quiet Time. Returns the
// command char (or 0 if not a normal addressed display frame) and sets *outAddr
// to the module id, or -1 for broadcast ('*'). Mirrors sfTrackFromFrame's
// address parsing. Used only to classify display-motion frames.
static char sfFrameCmd(const uint8_t* data, size_t len, int* outAddr) {
  *outAddr = -2;
  if (len < 3 || data[0] != 'm') return 0;
  if (data[1] == 'X') return 0;          // by-serial provisioning frame
  size_t i = 1;
  int addr;
  if (data[i] == '*') { addr = -1; i++; }
  else if (data[i] >= '0' && data[i] <= '9') {
    long v = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
      v = v * 10 + (data[i] - '0'); i++;
      if (v > 254) return 0;
    }
    addr = (int)v;
  } else return 0;
  if (i >= len) return 0;
  *outAddr = addr;
  return (char)data[i];
}

// True if the frame is normal display motion that Quiet Time should suppress:
// show character ('-'), show index ('+'), or home ('h'). Deliberate calibration
// moves (calibrate 'c', goto 'g', nudge 's') are intentionally NOT suppressed,
// since they only originate from an operator actively calibrating.
static bool sfFrameIsDisplayMotion(const uint8_t* data, size_t len) {
  int addr;
  char cmd = sfFrameCmd(data, len, &addr);
  if (cmd == 0) return false;
  if (cmd == '-' || cmd == '+') return true;
  if (cmd == 'h') {
    // 'h' must be the whole command (mXh / m*h), not a prefix of something else.
    size_t i = 1;
    if (data[i] == '*') i++;
    else { while (i < len && data[i] >= '0' && data[i] <= '9') i++; }
    size_t after = i + 1;   // byte after the 'h'
    if (after >= len || data[after] == '\n' || data[after] == '\r') return true;
  }
  return false;
}

// Remember the display the host requested while Quiet Time is on, so the reels
// can resync when it turns off. Only show-char/show-index frames carry display
// intent worth replaying; home is suppressed but not queued.
static void sfQuietCapturePending(const uint8_t* data, size_t len) {
  int addr;
  char cmd = sfFrameCmd(data, len, &addr);
  size_t i = 1; if (data[i]=='*') i++; else { while (i<len && data[i]>='0' && data[i]<='9') i++; }
  if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (cmd == '-') {
    if (i + 1 < len) {
      char c = (char)data[i + 1];
      if (addr < 0) {
        for (int k = 0; k < sfModuleCount; k++)
          if (sfModules[k].provisioned) { sfModules[k].pendChar=c; sfModules[k].pendIndex=-1; sfModules[k].hasPend=true; }
      } else {
        SFModule* m = sfFindById((uint8_t)addr);
        if (m) { m->pendChar=c; m->pendIndex=-1; m->hasPend=true; }
      }
    }
  } else if (cmd == '+') {
    long idx = 0; size_t j = i + 1; bool got=false;
    while (j < len && data[j] >= '0' && data[j] <= '9') { idx = idx*10 + (data[j]-'0'); j++; got=true; if (idx>63){idx=-1;break;} }
    if (got) {
      if (addr < 0) {
        for (int k = 0; k < sfModuleCount; k++)
          if (sfModules[k].provisioned) { sfModules[k].pendChar=0; sfModules[k].pendIndex=(int)idx; sfModules[k].hasPend=true; }
      } else {
        SFModule* m = sfFindById((uint8_t)addr);
        if (m) { m->pendChar=0; m->pendIndex=(int)idx; m->hasPend=true; }
      }
    }
  }
  xSemaphoreGive(sfMutex);
}

// True iff `data[0..len)` (caller strips trailing CR/LF first) is a DIRECT,
// numeric-id firmware-version query "m<id>v" with no payload after the 'v'.
// This is the one frame the gateway must transmit WITHOUT a newline terminator
// (see sfQueryVersion() for the half-duplex turnaround reason). Broadcast "m*v",
// by-serial "mX.." frames, and anything with bytes after the 'v' are NOT matched
// and so keep their terminator.
static bool sfIsDirectVersionQuery(const uint8_t* data, size_t len) {
  int addr;
  if (sfFrameCmd(data, len, &addr) != 'v') return false;   // command must be 'v'
  if (addr < 0) return false;                              // numeric id only (reject m*v)
  size_t i = 1;                                            // locate the command char:
  while (i < len && data[i] >= '0' && data[i] <= '9') i++; //   skip 'm', then the digits
  return (i + 1 == len);                                   // 'v' must be the final byte
}

// Given a frame `data[0..len)` (caller strips trailing CR/LF first), return the
// length of the longest prefix that forms a COMPLETE, well-formed known command.
// Bytes beyond that are extraneous and the caller may trim them -- so "m4vDSassa"
// collapses to "m4v" instead of leaning on the module to ignore the junk. This is
// grammar ENFORCEMENT, not guessing: each command's payload shape is fixed by the
// (frozen) protocol. Frames we can't model confidently -- by-serial "mX.." frames
// and any unrecognized command char -- return the full length UNCHANGED, so a
// long restore map or a future command is never truncated. The raw-send bypass
// skips this entirely.
static size_t sfKnownCommandLen(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'm') return len;        // not an m-frame: leave as-is
  if (data[1] == 'X') return len;                   // by-serial frame: pass through untouched
  size_t i = 1;
  bool wildcard = false;
  if (data[i] == '*') {                             // wildcard address
    wildcard = true;
    i++;
  } else if (data[i] >= '0' && data[i] <= '9') {    // numeric id (0..254)
    long v = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') { v = v*10 + (data[i]-'0'); i++; }
    if (v > 254) return len;                         // invalid id: don't touch
  } else {
    return len;                                      // malformed address: leave as-is
  }
  if (i >= len) return len;                           // no command char yet: leave as-is
  char cmd = (char)data[i];
  i++;                                                // consume the command char
  switch (cmd) {
    // Zero-payload commands: complete the instant the command char is read.
    case 'h': case 'c': case 'd':
    case 'e': case 'R': case 'F':
      return i;                                       // trim anything after
    // Version query and combined all-fields dump ('A', v25+): zero-payload when
    // addressed by a numeric id, but a wildcard broadcast may carry an optional
    // "<lo>-<hi>" range (m*v0-49 / m*A0-49) -- pass those through untrimmed.
    case 'v': case 'A':
      return wildcard ? len : i;
    // Show one character: keep exactly one payload byte.
    case '-':
      if (i < len) i++;
      return i;
    // Numeric-payload commands: keep the leading run of digits.
    case '+': case 'o': case 't': case 's':
    case 'g': case 'a': case 'i': {
      size_t d = i;
      while (d < len && data[d] >= '0' && data[d] <= '9') d++;
      return (d == i) ? len : d;                      // no digits where expected: leave as-is
    }
    // Write calibrated position "<index>:<pos>" -- two numeric fields.
    case 'w': {
      size_t d = i;
      while (d < len && data[d] >= '0' && data[d] <= '9') d++;        // index
      if (d == i || d >= len || data[d] != ':') return len;          // malformed: leave as-is
      d++;                                                            // ':'
      size_t p = d;
      while (d < len && data[d] >= '0' && data[d] <= '9') d++;        // position
      return (d == p) ? len : d;                                      // no position digits: leave
    }
    default:
      return len;                                     // unknown command: pass through
  }
}

void rs485Send(const uint8_t* data, size_t len, bool raw) {
  if (!len || len > TX_MAX_BYTES) return;

  // --- Wire framing + sanitization (single choke point for every send path) --
  // Normal path: the gateway owns wire correctness so callers never have to:
  //   1) strip any trailing CR/LF the caller supplied,
  //   2) trim anything past a complete, well-formed known command, so a stray
  //      "m4vDSassa" becomes "m4v" rather than relying on the module to ignore
  //      the junk (see sfKnownCommandLen), then
  //   3) re-add exactly one '\n' terminator -- EXCEPT a direct numeric-id version
  //      query "m<id>v", which must ship bare to dodge the half-duplex turnaround
  //      collision (see sfQueryVersion). So "m1v", "m1v\n", "m1v\r\n", and even
  //      "m1vJUNK" all leave as bare "m1v", while "m5-A" and "m9o2832" leave
  //      correctly newline-terminated (which also spares payload commands the
  //      module's 50 ms idle-timeout wait).
  // Raw path (raw==true -- the Bus Monitor "Raw" toggle, or {"raw":true} on the
  // REST/MQTT send): transmit the caller's exact bytes verbatim, with no trim and
  // no terminator change -- a deliberate debugging escape hatch. Bus-collision
  // guarding, Quiet Time, tracking, logging, and the monitor ring still apply.
  size_t bare;
  bool   appendNL;
  bool   sanitized = false;     // true if sfKnownCommandLen trimmed trailing junk
  if (raw) {
    bare     = len;     // verbatim -- no stripping, no sanitizing
    appendNL = false;   // no terminator added
  } else {
    bare = len;
    while (bare > 0 && (data[bare-1] == '\n' || data[bare-1] == '\r')) bare--;
    if (!bare) return;                                  // nothing but terminators
    size_t preSan = bare;
    bare      = sfKnownCommandLen(data, bare);          // trim trailing junk
    sanitized = (bare < preSan);                        // bytes past a complete command were dropped
    appendNL  = !sfIsDirectVersionQuery(data, bare);    // version query => ship bare
  }
  if (!bare) return;

  // Quiet Time: swallow normal display-motion frames so the flaps stay still.
  // The request is acknowledged (we return as if sent) and the desired display
  // is remembered for resync; nothing reaches the bus and tracking is unchanged.
  if (gQuietTime && sfFrameIsDisplayMotion(data, bare)) {
    sfQuietCapturePending(data, bare);
    DBG("[QUIET] suppressed display frame (%u bytes)\n", (unsigned)bare);
    return;
  }
  // Collision avoidance on the half-duplex bus: if modules are mid-response
  // (e.g. the staggered reply train after a broadcast m*v), transmitting now
  // would fight their drivers, corrupting bytes and destroying the newline
  // terminators (observed as glued/garbled frames and poisoned serial numbers).
  // Hold off until the bus has been quiet for TX_BUS_GUARD_MS, bounded by
  // TX_BUS_WAIT_CAP_MS so we always make progress.
  //
  // From here through the monitor-ring push is the bus-touching critical section.
  // txMutex serializes it across taskWeb / taskNetwork / taskRS485 so two senders
  // can't interleave bytes on the UART. There are no early returns inside, so the
  // mutex is always released. (Held across sfTrackFromFrame/ringPush, which take
  // sfMutex/msgMutex -- order is txMutex -> {sfMutex,msgMutex}, never inverted.)
  if (txMutex) xSemaphoreTake(txMutex, portMAX_DELAY);
  {
    unsigned long waitStart = millis();
    while (millis() - gLastRxMs < TX_BUS_GUARD_MS &&
           millis() - waitStart < TX_BUS_WAIT_CAP_MS) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  rs485.write(data, bare);                                  // the bare command...
  if (appendNL) { const uint8_t nl = '\n'; rs485.write(&nl, 1); }  // ...+ terminator unless version query
  rs485.flush();
  txCount++;
  // Update per-module display tracking from this frame. Doing it here -- the
  // single point every outbound frame passes through -- means raw sends are
  // tracked exactly like the high-level helpers, with no per-path duplication.
  sfTrackFromFrame(data, bare);
  gDisplayDirty = true;   // HA display sensor refresh (network task, rate-limited)
  // Log the transmitted frame (the command without a trailing terminator, for
  // readability -- the raw path may carry one). The monitor ring below keeps the
  // exact on-wire bytes. Cap the debug buffer at MSG_MAX_BYTES; long frames truncate.
  { char dbg[MSG_MAX_BYTES];
    size_t dlen = bare;
    while (dlen > 0 && (data[dlen-1] == '\n' || data[dlen-1] == '\r')) dlen--;
    if (dlen > sizeof(dbg) - 1) dlen = sizeof(dbg) - 1;
    memcpy(dbg, data, dlen); dbg[dlen] = '\0';
    DBG("[TX] %s%s\n", dbg, sanitized ? "  (sanitized)" : ""); }
  RS485Msg m;
  m.timestamp = millis();
  m.dir = 'T';
  m.sanitized = sanitized;
  // Reconstruct the on-wire frame for the monitor ring (bare command plus the
  // terminator we actually sent), bounded to MSG_MAX_BYTES. This keeps TX ring
  // entries consistent with RX entries, which carry their own '\n'.
  size_t ringLen = (bare > MSG_MAX_BYTES) ? MSG_MAX_BYTES : bare;
  memcpy(m.data, data, ringLen);
  if (appendNL && ringLen < MSG_MAX_BYTES) m.data[ringLen++] = '\n';
  m.len = ringLen;
  rtcFormatTime(m.wallTime, sizeof(m.wallTime));
  m.epoch = rtcEpochNow();   // UTC epoch; web UI renders in browser-local time
  ringPush(m);
  mqttPublishMsg(m);
  if (txMutex) xSemaphoreGive(txMutex);
}

// Send a null-terminated ASCII string on RS485
void rs485SendStr(const char* s) {
  rs485Send((const uint8_t*)s, strlen(s));
}
