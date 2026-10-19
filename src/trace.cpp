#include "gateway.h"
#include <stdarg.h>

// trace.cpp -- see trace.h for the contract and the two rules.

static char*    gBuf   = NULL;
static size_t   gHead  = 0;        // next write offset
static size_t   gTail  = 0;        // oldest byte still held
static size_t   gUsed  = 0;
static volatile bool gOn = false;
static uint32_t gLines = 0, gDropped = 0, gFrames = 0;
static unsigned long gStartMs = 0;
static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;

void traceInit() {
  if (!gBuf) gBuf = (char*) psramAlloc("soak trace", TRACE_BYTES);
}
bool traceActive() { return gOn; }

// Drop whole lines from the front until `need` bytes are free. Line granularity keeps the
// file parseable: a half line at the top would just be noise to whoever reads it.
static void makeRoom(size_t need) {
  while (TRACE_BYTES - gUsed < need && gUsed) {
    size_t n = 0;
    while (n < gUsed && gBuf[(gTail + n) % TRACE_BYTES] != '\n') n++;
    n++;                                   // the newline itself
    if (n > gUsed) n = gUsed;
    gTail = (gTail + n) % TRACE_BYTES;
    gUsed -= n;
    gDropped++;
  }
}
static void putRaw(const char* s, size_t n) {
  if (!gBuf || !n) return;
  if (n >= TRACE_BYTES) n = TRACE_BYTES - 1;
  portENTER_CRITICAL(&gMux);
  makeRoom(n);
  for (size_t i = 0; i < n; i++) gBuf[(gHead + i) % TRACE_BYTES] = s[i];
  gHead = (gHead + n) % TRACE_BYTES;
  gUsed += n;
  gLines++;
  portEXIT_CRITICAL(&gMux);
}

void traceNote(const char* fmt, ...) {
  if (!gOn || !gBuf) return;
  char line[256];
  int n = snprintf(line, sizeof(line), "%8lu ", millis() - gStartMs);
  va_list ap; va_start(ap, fmt);
  n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n > sizeof(line) - 2) n = sizeof(line) - 2;
  line[n++] = '\n'; line[n] = 0;
  putRaw(line, (size_t)n);
}

void traceFrame(char dir, const uint8_t* data, size_t len, size_t fullLen) {
  if (!gOn || !gBuf) return;
  static char line[TRACE_PAYLOAD_MAX * 4 + 96];
  int n = snprintf(line, sizeof(line), "%8lu %c ", millis() - gStartMs, dir);
  size_t keep = len > TRACE_PAYLOAD_MAX ? TRACE_PAYLOAD_MAX : len;
  for (size_t i = 0; i < keep && (size_t)n < sizeof(line) - 6; i++) {
    uint8_t c = data[i];
    if (c == '\n')      { line[n++] = '\\'; line[n++] = 'n'; }
    else if (c == '\r') { line[n++] = '\\'; line[n++] = 'r'; }
    else if (c == '\\') { line[n++] = '\\'; line[n++] = '\\'; }
    else if (c >= 32 && c < 127) line[n++] = (char)c;
    else n += snprintf(line + n, sizeof(line) - n, "\\x%02X", c);
  }
  if (fullLen > keep)   // say what was left out, and how long the frame really was
    n += snprintf(line + n, sizeof(line) - n, "  [+%u B, %u total]",
                  (unsigned)(fullLen - keep), (unsigned)fullLen);
  if ((size_t)n > sizeof(line) - 2) n = sizeof(line) - 2;
  line[n++] = '\n'; line[n] = 0;
  gFrames++;
  putRaw(line, (size_t)n);
}

void traceBegin(const char* what) {
  if (!gBuf) return;
  portENTER_CRITICAL(&gMux);
  gHead = gTail = gUsed = 0; gLines = gDropped = gFrames = 0;
  portEXIT_CRITICAL(&gMux);
  gStartMs = millis();
  gOn = true;
  char when[32] = "clock not set";
  if (rtcNow.valid)
    snprintf(when, sizeof(when), "%04u-%02u-%02uT%02u:%02u:%02uZ",
             rtcNow.year, rtcNow.month, rtcNow.day, rtcNow.hour, rtcNow.minute, rtcNow.second);
  traceNote("# SplitFlap Gateway %s -- %s", FW_VERSION, what);
  traceNote("# started %s, bus %lu baud, %d module(s) known", when, cfg.rs485Baud, sfModuleCount);
  traceNote("# columns: ms-since-start, direction (T sent, R received, ! event), frame");
  traceNote("# long healthy replies are cut to %d bytes; the real length is in brackets",
            TRACE_PAYLOAD_MAX);
  // The inventory makes the file self-contained for someone who has never seen this wall.
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < sfModuleCount; i++) {
    const SFModule& m = sfModules[i];
    if (!m.provisioned || m.id == 255) continue;
    traceNote("# module %3d  fw %-4s  sn %s", (int)m.id, m.fwVersion, m.serialNum);
  }
  if (sfMutex) xSemaphoreGive(sfMutex);
}

size_t traceEnd(const char* summary) {
  if (!gBuf) return 0;
  if (summary && summary[0]) traceNote("# %s", summary);
  if (gDropped) traceNote("# %lu earlier line(s) were dropped: the trace holds the most recent %u KB",
                          (unsigned long)gDropped, (unsigned)(TRACE_BYTES / 1024));
  gOn = false;
  if (!sfFsReady) return 0;
  // Only now, with the bus released, does anything touch flash.
  FFat.remove(TRACE_FILE);
  File f = FFat.open(TRACE_FILE, "w");
  if (!f) { printf("[TRACE] cannot open %s\n", TRACE_FILE); return 0; }
  size_t written = 0;
  const unsigned long t0 = millis();
  size_t i = 0;
  static char chunk[2048];
  while (i < gUsed) {
    size_t n = gUsed - i; if (n > sizeof(chunk)) n = sizeof(chunk);
    for (size_t k = 0; k < n; k++) chunk[k] = gBuf[(gTail + i + k) % TRACE_BYTES];
    written += f.write((const uint8_t*)chunk, n);
    i += n;
    wdgWebMs = millis();
    // Yield every 32 KB rather than every chunk: a tick is 10 ms, and a 2 MB trace would
    // otherwise spend ten seconds asleep. The bus is already released, so this is only
    // politeness toward the web and network tasks.
    if ((i & 0x7FFF) < sizeof(chunk)) vTaskDelay(1);
  }
  f.close();
  printf("[TRACE] %u frames, %u bytes written to %s in %lu ms (%lu line(s) dropped)\n",
         (unsigned)gFrames, (unsigned)written, TRACE_FILE, millis() - t0, (unsigned long)gDropped);
  return written;
}

bool traceFileInfo(size_t* bytes, char* firstLine, size_t lineLen) {
  if (!sfFsReady || !FFat.exists(TRACE_FILE)) return false;
  File f = FFat.open(TRACE_FILE, "r");
  if (!f) return false;
  if (bytes) *bytes = f.size();
  if (firstLine && lineLen) {
    size_t n = 0;
    while (f.available() && n < lineLen - 1) { char c = f.read(); if (c == '\n') break; firstLine[n++] = c; }
    firstLine[n] = 0;
  }
  f.close();
  return true;
}

void traceEmitFile(void (*emit)(const char* chunk)) {
  if (!sfFsReady || !FFat.exists(TRACE_FILE)) { emit("no trace stored\n"); return; }
  File f = FFat.open(TRACE_FILE, "r");
  if (!f) { emit("cannot read the stored trace\n"); return; }
  static char chunk[1460];
  while (f.available()) {
    size_t n = f.readBytes(chunk, sizeof(chunk) - 1);
    chunk[n] = 0;
    emit(chunk);
    wdgWebMs = millis();
  }
  f.close();
}
