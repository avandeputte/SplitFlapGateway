#include "gateway.h"



// modules.cpp -- split-flap module registry and protocol.
// Three concerns: (1) the in-RAM registry of known modules (sfUpsert/sfFindById
// ... under sfMutex) and its FATFS persistence; (2) command builders that emit
// the ASCII bus protocol via rs485Send (sfSendChar, sfHome, sfProvision, ...);
// (3) sfParseResponse, which decodes every inbound frame (version/dump/calib/
// diagnostics/advertisement/ack) and updates the registry or capture mailboxes.
// ---- file-private forward declarations ----
static SFModule* sfFindBySN(const char* sn);
static SFModule* sfUpsert(uint8_t id, const char* sn);
static bool sfApplyVersionFields(uint8_t id, const char* fwCopy, const char* sn);
static bool sfDedupeBySNLocked(const char* sn);
static bool sfNoteCorruptVersionReply(uint8_t id);
static bool sfValidSN(const char* sn);
static inline void sfTouch(SFModule* m);
static void sfRestoreBySN(const char* payload);
static void sfSetAutoHome(int addr, bool enable);
static void sfSetId(int currentAddr, int newId);
static void sfTrackCharLocked(int addr, char c);

/* ----------------------------------------------------------
   Split-flap protocol helpers
---------------------------------------------------------- */

// Find or create a module registry entry by ID
SFModule* sfFindById(uint8_t id) {
  for (int i = 0; i < sfModuleCount; i++)
    if (sfModules[i].id == id) return &sfModules[i];
  return NULL;
}

// Find module by serial number
static SFModule* sfFindBySN(const char* sn) {
  for (int i = 0; i < sfModuleCount; i++)
    if (strcmp(sfModules[i].serialNum, sn) == 0) return &sfModules[i];
  return NULL;
}

// A serial number is a module's true identity, but its id changes on
// (de)provision. A stale in-flight frame carrying an old id can briefly create a
// second registry entry for a serial we already track (seen as duplicate cards
// in the grid after a deprovision). This collapses any extra entries sharing a
// serial down to the FIRST one, keeping the registry's "one serial = one module"
// invariant. Caller MUST already hold sfMutex. Returns true if anything changed.
static bool sfDedupeBySNLocked(const char* sn) {
  if (!sn || !sn[0]) return false;
  int first = -1;
  bool changed = false;
  for (int i = 0; i < sfModuleCount; ) {
    if (strcmp(sfModules[i].serialNum, sn) == 0) {
      if (first < 0) { first = i; i++; continue; }
      // Duplicate: drop it (shift the tail down, stable order).
      for (int j = i; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
      sfModuleCount--;
      memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
      changed = true;
    } else i++;
  }
  return changed;
}

// Add or update a module entry
static SFModule* sfUpsert(uint8_t id, const char* sn) {
  SFModule* m = (id != 255) ? sfFindById(id) : sfFindBySN(sn);
  bool isNew = false;
  if (!m) {
    if (sfModuleCount >= MAX_MODULES) return NULL;
    m = &sfModules[sfModuleCount++];
    memset(m, 0, sizeof(SFModule));
    m->id = id;
    m->flapIndex = -1;
    m->flapChar  = 0;
    isNew = true;
  }
  if (sn && sn[0]) strlcpy(m->serialNum, sn, sizeof(m->serialNum));
  m->lastSeen = millis();
  m->probeMs  = 0;   // module is transmitting -> cancel any pending stale probe
  unsigned long ep = rtcEpochNow();
  if (ep) m->lastSeenEpoch = ep;
  if (isNew) sfModulesDirty = true;  // new module -> persist
  return m;
}
// Mount the FATFS partition. Format on first use if needed.
void sfFsInit() {
  // Try to mount WITHOUT auto-format first (fast path on every normal boot).
  if (FFat.begin(false)) {
    sfFsReady = true;
    DBG("[MOD] FATFS mounted (%lu KB free)\n",
        (unsigned long)(FFat.freeBytes() / 1024));
    return;
  }
  // First boot after flashing: the partition is unformatted. Formatting a
  // ~10MB partition is a long blocking flash operation -- log it clearly so
  // the delay is expected, and so the watchdog boot grace period covers it.
  printf("[MOD] FATFS not formatted -- formatting now (one-time, may take a while)...\n");
  if (FFat.begin(true)) {       // true = format if mount fails
    sfFsReady = true;
    printf("[MOD] FATFS formatted and mounted (%lu KB free)\n",
           (unsigned long)(FFat.freeBytes() / 1024));
  } else {
    sfFsReady = false;
    printf("[MOD] FATFS mount/format failed -- module persistence disabled\n");
  }
}

// Save the current registry to the FATFS file.
void sfModulesSave() {
  if (!sfFsReady) return;
  // Build a compact array of durable records under sfMutex.
  static PersistedModule recs[MAX_MODULES];  // static: avoid large stack frame
  int n = 0;
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < sfModuleCount && n < MAX_MODULES; i++) {
    const SFModule& m = sfModules[i];
    recs[n].id            = m.id;
    strlcpy(recs[n].serialNum, m.serialNum, sizeof(recs[n].serialNum));
    recs[n].provisioned   = m.provisioned;
    recs[n].acked         = m.acked;
    strlcpy(recs[n].fwVersion, m.fwVersion, sizeof(recs[n].fwVersion));
    recs[n].lastSeenEpoch = m.lastSeenEpoch;
    n++;
  }
  if (sfMutex) xSemaphoreGive(sfMutex);

  // Write to a temp file then rename, so a crash mid-write can't corrupt
  // the existing good copy.
  File f = FFat.open(MODULES_FILE ".tmp", "w");
  if (!f) { DBG("[MOD] open for write failed\n"); return; }
  ModulesFileHeader hdr = { MODULES_MAGIC, n };
  f.write((const uint8_t*)&hdr, sizeof(hdr));
  if (n > 0) f.write((const uint8_t*)recs, n * sizeof(PersistedModule));
  f.close();
  FFat.remove(MODULES_FILE);
  FFat.rename(MODULES_FILE ".tmp", MODULES_FILE);
  DBG("[MOD] Saved %d modules to FATFS\n", n);
}

// Load persisted modules from the FATFS file at boot, pruning stale entries.
void sfModulesLoad() {
  if (!sfFsReady) return;
  if (!FFat.exists(MODULES_FILE)) { DBG("[MOD] no saved module file\n"); return; }
  File f = FFat.open(MODULES_FILE, "r");
  if (!f) { DBG("[MOD] open for read failed\n"); return; }

  ModulesFileHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.magic != MODULES_MAGIC || hdr.count <= 0 || hdr.count > MAX_MODULES) {
    DBG("[MOD] bad/empty module file -- skipping\n");
    f.close();
    return;
  }
  static PersistedModule recs[MAX_MODULES];
  size_t want = (size_t)hdr.count * sizeof(PersistedModule);
  size_t got  = f.read((uint8_t*)recs, want);
  f.close();
  if (got != want) {
    DBG("[MOD] file size mismatch (%u != %u) -- skipping\n",
        (unsigned)got, (unsigned)want);
    return;
  }

  unsigned long nowEp = rtcEpochNow();  // 0 if RTC not yet valid
  int loaded = 0, pruned = 0;
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < hdr.count && sfModuleCount < MAX_MODULES; i++) {
    // Prune entries older than MODULE_STALE_SECS (only when we have a
    // valid clock AND a recorded epoch to compare against).
    if (nowEp && recs[i].lastSeenEpoch &&
        nowEp > recs[i].lastSeenEpoch &&
        (nowEp - recs[i].lastSeenEpoch) > MODULE_STALE_SECS) {
      pruned++;
      continue;
    }
    // Skip records whose SN fails validation: a bus collision before the
    // validation fix could have persisted a garbage SN (e.g. a glued frame
    // tail). Dropping the record here HEALS the registry -- the module
    // re-registers with its correct SN on its next version response.
    {
      char snChk[21];
      strlcpy(snChk, recs[i].serialNum, sizeof(snChk));
      if (snChk[0] && !sfValidSN(snChk)) {
        DBG("[MOD] dropping persisted record id=%d with corrupt SN\n", recs[i].id);
        pruned++;
        continue;
      }
    }
    SFModule* m = &sfModules[sfModuleCount++];
    memset(m, 0, sizeof(SFModule));
    m->id            = recs[i].id;
    strlcpy(m->serialNum, recs[i].serialNum, sizeof(m->serialNum));
    m->provisioned   = recs[i].provisioned;
    m->acked         = recs[i].acked;
    strlcpy(m->fwVersion, recs[i].fwVersion, sizeof(m->fwVersion));
    m->lastSeenEpoch = recs[i].lastSeenEpoch;
    m->flapIndex     = -1;
    m->flapChar      = 0;
    m->lastSeen      = 0;   // not seen yet this boot
    loaded++;
  }
  if (sfMutex) xSemaphoreGive(sfMutex);
  DBG("[MOD] Loaded %d modules from FATFS (%d pruned as stale)\n", loaded, pruned);
}

// Wipe both the in-memory registry and the persisted file.
void sfModulesClear() {
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  sfModuleCount = 0;
  memset(sfModules, 0, sizeof(SFModule) * MAX_MODULES);
  if (sfMutex) xSemaphoreGive(sfMutex);
  if (sfFsReady) FFat.remove(MODULES_FILE);
  sfModulesDirty = false;
  DBG("[MOD] Registry cleared (memory + FATFS)\n");
}

// Two-phase stale handling, called periodically:
//   Phase 1: a module unseen for MODULE_STALE_SECS that isn't being probed is
//            sent a version query (m<id>v) and given MODULE_PROBE_GRACE_MS to
//            reply. A reply (sfTouch) clears the probe and keeps the module.
//   Phase 2: a probed module whose grace window has elapsed without a reply is
//            actually dropped. This avoids evicting a module that has merely
//            been quiet (modules only speak when addressed).
// Version queries are sent OUTSIDE sfMutex (rs485Send re-takes the lock via
// frame tracking, so probing under it would deadlock): IDs to probe are
// collected under the lock, then queried after release.
void sfModulesPruneStale() {
  unsigned long nowEp = rtcEpochNow();
  if (!nowEp) return;  // no valid clock yet
  unsigned long nowMs = millis();
  bool changed = false;
  static uint8_t toProbe[MAX_MODULES];
  int probeN = 0;

  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < sfModuleCount; ) {
    SFModule& m = sfModules[i];
    bool stale = (m.lastSeenEpoch && nowEp > m.lastSeenEpoch &&
                  (nowEp - m.lastSeenEpoch) > MODULE_STALE_SECS);
    if (stale && m.probeMs == 0 && probeN < MODULE_PROBE_BATCH) {
      // Phase 1: start a probe -- give it a chance to answer before dropping.
      // Bounded batch per cycle; further stale modules are probed on a later
      // cycle so the probe burst stays short and collision-free.
      m.probeMs = nowMs + MODULE_PROBE_GRACE_MS;
      toProbe[probeN++] = m.id;
      i++;
    } else if (stale && m.probeMs != 0 && nowMs >= m.probeMs) {
      // Phase 2: probed and the grace window elapsed with no reply -> drop it.
      for (int j = i; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
      sfModuleCount--;
      memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
      changed = true;
    } else {
      i++;   // fresh, or probe still pending -> leave it
    }
  }
  if (sfMutex) xSemaphoreGive(sfMutex);

  // Send the probes now that the lock is released. SPACE THEM OUT: a module
  // answers a bare 'v' query synchronously and instantly, so firing them
  // back-to-back makes each reply collide with the next query on the half-duplex
  // bus -- the replies are lost and live modules get dropped despite being
  // "probed". A short gap lets each reply land and be parsed (sfTouch clears the
  // probe and refreshes lastSeen) before the next query goes out.
  for (int i = 0; i < probeN; i++) {
    DBG("[MOD] stale module %d -- probing before drop\n", toProbe[i]);
    sfQueryVersion(toProbe[i]);
    for (unsigned long w = 0; w < MODULE_PROBE_SPACING_MS; w += 10) {
      wdgNetMs = millis();                  // keep the net-task watchdog fed
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  if (changed) sfModulesDirty = true;
}

/* ----------------------------------------------------------
   Send split-flap commands
   All generate the ASCII bus protocol and call rs485SendStr()
---------------------------------------------------------- */

// Display a character on one module.  addr=-1 = broadcast.
// The gateway does NOT translate the character to a flap index -- the module
// firmware does that itself. We only ensure it is a printable ASCII byte and
// uppercase it (the flap set is uppercase), then send m<id>-<char> verbatim.
// Record the character a module is now displaying. addr<0 means a broadcast
// was sent, so every known module shows the same character -- update them all.
// Pass c=0 to mark the displayed character as unknown (e.g. after a home, when
// the module has left its previous flap but we can't name the new one without
// the module's flap table). flapIndex is cleared because the gateway tracks the
// character, not the index, on the char path. Caller must already hold sfMutex.
static void sfTrackCharLocked(int addr, char c) {
  if (addr < 0) {
    for (int i = 0; i < sfModuleCount; i++) {
      if (sfModules[i].provisioned) {
        sfModules[i].flapChar  = c;
        sfModules[i].flapIndex = -1;
      }
    }
  } else {
    SFModule* m = sfFindById((uint8_t)addr);
    if (m) { m->flapChar = c; m->flapIndex = -1; }
  }
}

// Mutex-wrapping convenience for callers that are not already holding sfMutex.
void sfTrackChar(int addr, char c) {
  if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    sfTrackCharLocked(addr, c);
    xSemaphoreGive(sfMutex);
  }
}

// Send one flap character. `c` is a single Windows-1252 byte: ASCII 0x20-0x7E, or
// a high byte (euro/accents/smart punctuation) -- see charset.h. ASCII letters are
// normalised to uppercase (to match the default reel order); high bytes are sent
// verbatim so accented case is preserved. Bytes that aren't a valid flap glyph
// (controls, undefined slots, the 0x00/0xFF firmware sentinels) are dropped.
void sfSendChar(int addr, char c) {
  uint8_t b = (uint8_t)c;
  if (b >= 'a' && b <= 'z') b = (uint8_t)(b - 'a' + 'A');     // uppercase ASCII only
  if (!isFlapByte(b)) return;                                 // not a valid flap glyph
  char buf[24];
  if (addr < 0)
    snprintf(buf, sizeof(buf), "m*-%c\n", b);
  else
    snprintf(buf, sizeof(buf), "m%d-%c\n", addr, b);
  rs485SendStr(buf);
  // Display tracking is handled centrally in rs485Send via sfTrackFromFrame,
  // so every path (including raw frame sends) is covered uniformly.
}

// Display by flap index.  addr=-1 = broadcast.
void sfSendIndex(int addr, int idx) {
  char buf[24];
  if (addr < 0)
    snprintf(buf, sizeof(buf), "m*+%d\n", idx);
  else
    snprintf(buf, sizeof(buf), "m%d+%d\n", addr, idx);
  rs485SendStr(buf);
  // Display tracking handled centrally in rs485Send via sfTrackFromFrame.
}

void sfHomeOffset(int addr, int steps) {
  char buf[32];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*o%d\n", steps);
  else           snprintf(buf, sizeof(buf), "m%do%d\n", addr, steps);
  rs485SendStr(buf);
}
void sfSetTotalSteps(int addr, int steps) {
  char buf[32];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*t%d\n", steps);
  else           snprintf(buf, sizeof(buf), "m%dt%d\n", addr, steps);
  rs485SendStr(buf);
}
void sfNudge(int addr, int steps) {
  char buf[32];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*s%d\n", steps);
  else           snprintf(buf, sizeof(buf), "m%ds%d\n", addr, steps);
  rs485SendStr(buf);
}
void sfGoto(int addr, int step) {
  char buf[32];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*g%d\n", step);
  else           snprintf(buf, sizeof(buf), "m%dg%d\n", addr, step);
  rs485SendStr(buf);
}
void sfWritePos(int addr, int idx, int pos) {
  char buf[32];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*w%d:%d\n", idx, pos);
  else           snprintf(buf, sizeof(buf), "m%dw%d:%d\n", addr, idx, pos);
  rs485SendStr(buf);
}
void sfAutoHome(int addr, int enable) {
  char buf[24];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*a%d\n", enable ? 1 : 0);
  else           snprintf(buf, sizeof(buf), "m%da%d\n", addr, enable ? 1 : 0);
  rs485SendStr(buf);
}
void sfErase(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*e\n");
  else           snprintf(buf, sizeof(buf), "m%de\n", addr);
  rs485SendStr(buf);
}
void sfFactoryReset(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*F\n");
  else           snprintf(buf, sizeof(buf), "m%dF\n", addr);
  rs485SendStr(buf);
}
// Configure the runtime flap set ('N' command, firmware v31+).
//   m<id>N<count>:<chars>   (direct)        m*N<count>:<chars>   (broadcast)
// Both parts are optional and independent: a count<1 omits the count digits, and
// an empty/NULL chars omits the ":<chars>" tail, so the firmware leaves that side
// unchanged. The char set is sent verbatim (it may contain ':') -- the caller is
// responsible for stripping CR/LF, which would otherwise terminate the frame.
// No reply: the module applies 'N' silently (read it back via 'A').
void sfSetFlapConfig(int addr, int count, const char* chars) {
  bool hasCount = (count >= 1 && count <= SF_MAX_FLAPS);
  bool hasChars = (chars && chars[0]);
  if (!hasCount && !hasChars) return;            // nothing to set
  char buf[16 + SF_MAX_FLAPS + 1];               // "m255N64:" + up to 64 chars + '\n'
  int n = (addr < 0) ? snprintf(buf, sizeof(buf), "m*N")
                     : snprintf(buf, sizeof(buf), "m%dN", addr);
  if (hasCount) n += snprintf(buf + n, sizeof(buf) - n, "%d", count);
  if (hasChars) n += snprintf(buf + n, sizeof(buf) - n, ":%s", chars);
  snprintf(buf + n, sizeof(buf) - n, "\n");
  rs485SendStr(buf);
}

// Configure the flap set by serial number (mXN<sn>:<count>:<chars>). The two
// colons are always emitted so the firmware's field parser (sn / count / chars)
// stays aligned; an omitted count leaves that field empty (unchanged), and an
// omitted char set leaves the trailing field empty (unchanged).
void sfSetFlapConfigBySN(const char* sn, int count, const char* chars) {
  bool hasCount = (count >= 1 && count <= SF_MAX_FLAPS);
  bool hasChars = (chars && chars[0]);
  if (!hasCount && !hasChars) return;
  char buf[32 + SF_MAX_FLAPS + 1];
  int n = snprintf(buf, sizeof(buf), "mXN%s:", sn);
  if (hasCount) n += snprintf(buf + n, sizeof(buf) - n, "%d", count);
  n += snprintf(buf + n, sizeof(buf) - n, ":");
  if (hasChars) n += snprintf(buf + n, sizeof(buf) - n, "%s", chars);
  snprintf(buf + n, sizeof(buf) - n, "\n");
  rs485SendStr(buf);
}

// Provisioning variants (address mX by serial number)
void sfDumpBySN(const char* sn) {
  char buf[64];
  snprintf(buf, sizeof(buf), "mXD%s\n", sn);
  rs485SendStr(buf);
}
void sfFactoryResetBySN(const char* sn) {
  char buf[64];
  snprintf(buf, sizeof(buf), "mXF%s\n", sn);
  rs485SendStr(buf);
}
static void sfRestoreBySN(const char* payload) {
  // payload is the full mXW... command string (caller builds it)
  rs485SendStr(payload);
}

// Home one module or all (addr=-1)
void sfHome(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*h\n");
  else          snprintf(buf, sizeof(buf), "m%dh\n", addr);
  rs485SendStr(buf);
  // Display tracking handled centrally in rs485Send via sfTrackFromFrame
  // (home -> displayed character becomes unknown).
}

// Calibrate one module or all
void sfCalibrate(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*c\n");
  else          snprintf(buf, sizeof(buf), "m%dc\n", addr);
  rs485SendStr(buf);
}

// Query firmware version of a DIRECT (single-id, never broadcast) module.
//
// NOTE: the omitted trailing '\n' is NOT a module protocol requirement. The
// module accepts BOTH "m<id>v" and "m<id>v\n" and answers either one -- it acts
// on the 'v' byte itself. We drop the newline to work around a GATEWAY-SIDE
// limitation in our half-duplex bus turnaround, as follows:
//
// A module answers a direct version query SYNCHRONOUSLY, the instant it parses
// the 'v' byte -- with zero assembly delay (unlike a dump, which builds its
// EEPROM string before raising DE, so its reply is naturally late enough to be
// safe). Our problem is on the gateway: the ESP32's hardware-managed DE keeps us
// driving the line until the WHOLE frame has clocked out, and our UART receiver
// is off while we transmit, so we can't release the bus and flip to RX fast
// enough to catch that immediate reply. A trailing '\n' keeps us transmitting
// for one extra byte-time (~1 ms at 9600 baud) AFTER the module has already
// started replying -- the reply's leading bytes land while we're still driving
// the line (and deaf), so they're lost and no [RX] frame is ever assembled. That
// was the long-standing "version query gets no reply, but a dump right after
// works" symptom -- a gateway turnaround race, not a module quirk.
//
// Dropping the newline shortens our transmit window so we've already released
// the bus and are listening by the time the module answers (verified on
// hardware: a newline-less send always replies; the byte-identical "m<id>v\n"
// never did). The BROADCAST "m*v\n" path is separate and keeps its newline: a
// wildcard query collects an optional ID range and the module fires a staggered,
// DEFERRED reply on the '\n' (or a 50 ms idle timeout), so there is no
// turnaround race there. sfQueryVersion is only ever called with a concrete id.
//
// (Belt and suspenders: rs485Send() now normalizes framing for EVERY sender, so
// even a hand-typed "m<id>v\n" from the bus monitor or a REST/MQTT raw send is
// shipped bare. This helper still emits the bare form directly to document intent.)
void sfQueryVersion(int addr) {
  char buf[16];
  snprintf(buf, sizeof(buf), "m%dv", addr);   // no '\n' -- gateway turnaround workaround, see note
  rs485SendStr(buf);
}

// Arm the shared dump-capture slot for `id`, send `frame`, then wait up to
// `timeoutMs` for the module's reply -- a 'd' or 'A' response that sfParseResponse
// stores in gDump.data. Returns true and copies the captured dump into `out`
// if a reply arrived. Runs on taskWeb; touches wdgWebMs so a long wait is
// watchdog-safe. The capture slot is single-use, which is safe because the
// synchronous web server serves one request at a time.
bool sfSendAndCaptureDump(int id, const char* frame, unsigned long timeoutMs,
                                 char* out, size_t outLen) {
  gDump.data[0] = 0;
  gDump.ts  = 0;
  gDump.autoHome = gDump.curIndex = gDump.reportedId = -99;  // A-only; cleared per attempt
  gDump.flapCount = -99; gDump.flapChars[0] = 0;             // v31+ 'A' tail; cleared per attempt
  gDump.waitId     = id;
  rs485SendStr(frame);
  unsigned long deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    wdgWebMs = millis();
    vTaskDelay(pdMS_TO_TICKS(10));
    if (gDump.ts != 0) { strlcpy(out, gDump.data, outLen); return true; }
  }
  return false;
}

// Send a 'Q' diagnostics snapshot to `id` and wait up to `timeoutMs` for the
// instant reply (no motor movement). Result lands in the gDiag.q.* fields via
// sfParseResponse. Returns true on a reply. Runs on taskWeb; watchdog-safe.
bool sfSendAndCaptureQ(int id, unsigned long timeoutMs) {
  gDiag.q.ts     = 0;
  gDiag.q.waitId = id;
  char frame[16];
  snprintf(frame, sizeof(frame), "m%dQ\n", id);
  rs485SendStr(frame);
  unsigned long deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    wdgWebMs = millis();
    vTaskDelay(pdMS_TO_TICKS(10));
    if (gDiag.q.ts != 0) { gDiag.q.waitId = -1; return true; }
  }
  gDiag.q.waitId = -1;
  return false;
}

// Send a direct version query to `id` and wait up to `timeoutMs` for the reply
// to land in the registry (lastSeen advances AND fwVersion populated). On a
// reply, fills any non-NULL out params and returns true. Runs on taskWeb.
bool sfSendVersionAndWait(int id, unsigned long timeoutMs,
                                 char* fwOut, size_t fwLen,
                                 char* snOut, size_t snLen,
                                 unsigned long* lastSeenOut) {
  unsigned long seenBefore = 0;
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* mb = sfFindById((uint8_t)id);
  if (mb) seenBefore = mb->lastSeen;
  xSemaphoreGive(sfMutex);
  sfQueryVersion(id);   // bare 'v' -- collision-safe
  unsigned long deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    wdgWebMs = millis();
    vTaskDelay(pdMS_TO_TICKS(10));
    bool got = false;
    xSemaphoreTake(sfMutex, portMAX_DELAY);
    SFModule* mx = sfFindById((uint8_t)id);
    if (mx && mx->lastSeen != seenBefore && mx->fwVersion[0]) {
      if (fwOut) strlcpy(fwOut, mx->fwVersion, fwLen);
      if (snOut) strlcpy(snOut, mx->serialNum, snLen);
      if (lastSeenOut) *lastSeenOut = mx->lastSeen;
      got = true;
    }
    xSemaphoreGive(sfMutex);
    if (got) return true;
  }
  return false;
}

// Set module ID (address it by current ID, give it a new one)
static void sfSetId(int currentAddr, int newId) {
  char buf[24];
  snprintf(buf, sizeof(buf), "m%di%d\n", currentAddr, newId);
  rs485SendStr(buf);
  SFModule* m = sfFindById((uint8_t)currentAddr);
  if (m) m->id = (uint8_t)newId;
}

// Provisioning: assign ID to unprovisioned module by serial number
// Deprovision: send reset command to one module (by ID) or all
void sfDeprovision(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*R\n");
  else          snprintf(buf, sizeof(buf), "m%dR\n", addr);
  rs485SendStr(buf);
  // Remove from local registry. Shift the tail down (stable order) rather than
  // swapping the last entry into the gap, so the Modules grid keeps a consistent
  // order after a deprovision -- matching sfModulesPruneStale's removal.
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  if (addr < 0) {
    sfModuleCount = 0;
    memset(sfModules, 0, sizeof(SFModule) * MAX_MODULES);
  } else {
    for (int i = 0; i < sfModuleCount; i++) {
      if (sfModules[i].id == (uint8_t)addr) {
        for (int j = i; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
        sfModuleCount--;
        memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
        break;
      }
    }
  }
  xSemaphoreGive(sfMutex);
  sfModulesDirty = true;   // persist the removal
}

void sfProvision(const char* sn, int newId) {
  char buf[48];
  snprintf(buf, sizeof(buf), "mXI%s:%d\n", sn, newId);
  rs485SendStr(buf);
}

// Home a module by serial number
void sfHomeBySN(const char* sn) {
  char buf[48];
  snprintf(buf, sizeof(buf), "mXH%s\n", sn);
  rs485SendStr(buf);
}

// Set auto-home flag
static void sfSetAutoHome(int addr, bool enable) {
  char buf[20];
  snprintf(buf, sizeof(buf), "m%da%d\n", addr, enable ? 1 : 0);
  rs485SendStr(buf);
}

// Factory reset a module (preserves ID)
// Send a text string across a sequence of module IDs starting at startAddr.
// Each character is sent to startAddr, startAddr+1, ... up to strlen(text).
void sfSendText(int startAddr, const char* text, bool blankUnused) {
  // `text` arrives as UTF-8 (from the web UI / MQTT / JSON). Transcode it to the
  // single-byte flap encoding (Windows-1252) first, so one displayed glyph --
  // including a euro sign or an accented letter, which are multi-byte in UTF-8 --
  // maps to exactly one flap module. Unrepresentable code points are dropped.
  char enc[SF_MAX_FLAPS * 4 + 1];
  size_t len = utf8ToFlap(text, enc, sizeof(enc));
  for (size_t i = 0; i < len; i++) {
    // sfSendChar uppercases ASCII and rejects non-printable bytes itself; the
    // module firmware maps the character byte to a flap index.
    sfSendChar((int)(startAddr + i), enc[i]);
    delay(10); // inter-message gap to avoid bus collision
  }
  // Optionally blank any previously-set modules beyond the text length
  // (caller passes blankUnused=true when overwriting a display row)
  (void)blankUnused; // extensible for future use
}

// Set Quiet Time on/off. On the falling edge (on -> off) the reels are resynced
// to the last display each module was asked to show while quiet, so the physical
// display catches up to the most recent request. Safe to call from any task; the
// resync sends through rs485Send, which now transmits normally (quiet is off).
void sfSetQuietTime(bool on) {
  bool was = gQuietTime;
  gQuietTime = on;
  if (was && !on) {
    // Snapshot pending requests under the lock, clear them, then send unlocked
    // (rs485Send must not be called while holding sfMutex).
    struct Pend { int id; char ch; int idx; };
    static Pend list[MAX_MODULES];
    int n = 0;
    if (xSemaphoreTake(sfMutex, portMAX_DELAY) == pdTRUE) {
      for (int i = 0; i < sfModuleCount && n < MAX_MODULES; i++) {
        if (sfModules[i].hasPend) {
          list[n].id  = sfModules[i].id;
          list[n].ch  = sfModules[i].pendChar;
          list[n].idx = sfModules[i].pendIndex;
          n++;
          sfModules[i].hasPend = false;
        }
      }
      xSemaphoreGive(sfMutex);
    }
    for (int i = 0; i < n; i++) {
      if (list[i].ch) sfSendChar(list[i].id, list[i].ch);
      else if (list[i].idx >= 0) sfSendIndex(list[i].id, list[i].idx);
    }
    if (n) printf("[QUIET] off -- resynced %d module(s) to last requested display\n", n);
  }
  printf("[QUIET] Quiet Time %s\n", on ? "ENABLED" : "disabled");
}
/* ----------------------------------------------------------
   Parse responses from modules (called from the RS485 receive task)
   Format examples:
     m38v:12\n           version response
     m38:4096\n          calibration result (steps per rev)
     m38d:2832:4096:\n   EEPROM dump
     mXadv:AABBCCDD...\n advertisement from unprovisioned module
     mXack:AABBCC...:5\n provisioning acknowledgement
---------------------------------------------------------- */

// Update a module's activity timestamps (millis + RTC epoch for persistence).
static inline void sfTouch(SFModule* m) {
  if (!m) return;
  m->lastSeen = millis();
  m->probeMs  = 0;   // any activity means it's alive -> cancel any pending probe
  unsigned long ep = rtcEpochNow();
  if (ep) m->lastSeenEpoch = ep;
}

// A serial number is 4..20 alphanumeric characters (in practice 20 hex chars
// from the module's chip ID). Bus collisions destroy frame terminators and can
// glue responses together, producing SN tokens containing ':' or raw garbage
// bytes; storing one of those poisons the registry (and FATFS), after which
// every SN-addressed command (mXD<sn>, mXW<sn>, ...) silently fails. Validate
// before EVERY store and on load so corruption can never enter or persist.
static bool sfValidSN(const char* sn) {
  if (!sn || !sn[0]) return false;
  size_t n = strlen(sn);
  if (n < 4 || n > 20) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)sn[i];
    if (!isalnum(c)) return false;
  }
  return true;
}

// Apply the version-bearing fields from a 'v' or 'A' response to the registry.
// Validates the serial (a glued/garbled frame yields a corrupt SN whose storage
// would poison the registry and FATFS); returns false if so, so the caller can
// reject the whole frame. `fwCopy` is the already-normalized firmware string
// ("?" when empty); `sn` may be empty (legacy version-only reply), in which case
// only the firmware is updated. The entry is re-found by id under sfMutex (the
// upsert pointer can be invalidated by concurrent compaction), and any other
// entry holding the same serial is collapsed so a serial maps to one record.
static bool sfApplyVersionFields(uint8_t id, const char* fwCopy, const char* sn) {
  if (sn && sn[0] && !sfValidSN(sn)) return false;
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* mm = sfFindById(id);
  if (mm) {
    if (strcmp(mm->fwVersion, fwCopy) != 0) sfModulesDirty = true;  // persist new fw
    strlcpy(mm->fwVersion, fwCopy, sizeof(mm->fwVersion));
    // A clean version/all reply parsed for this ID -> the duplicate-ID suspicion
    // (if any) is resolved; reset the heuristic.
    mm->dupRejectCount = 0;
    mm->dupRejectTs    = 0;
    mm->dupSuspect     = false;
    if (sn && sn[0]) {
      strlcpy(mm->serialNum, sn, sizeof(mm->serialNum));
      for (int k = 0; k < sfModuleCount; k++) {
        if (&sfModules[k] != mm && strcmp(sfModules[k].serialNum, sn) == 0) {
          for (int j = k; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
          sfModuleCount--;
          memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
          sfModulesDirty = true;
          break;
        }
      }
    }
  }
  xSemaphoreGive(sfMutex);
  return true;
}

// Record a corrupt-SN version/all reject for `id` and advance the duplicate-ID
// heuristic. Returns true EXACTLY ONCE -- the moment the suspicion first latches
// -- so the caller emits a single warning rather than spamming. Counts within a
// rolling window so isolated transient collisions decay instead of accumulating;
// only persistent same-ID corruption (the duplicate-ID signature) crosses the
// threshold. Runs under sfMutex internally.
static bool sfNoteCorruptVersionReply(uint8_t id) {
  bool newlyLatched = false;
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* m = sfFindById(id);
  if (m) {
    unsigned long now = millis();
    if (m->dupRejectTs && (now - m->dupRejectTs) <= DUP_ID_REJECT_WINDOW_MS) {
      if (m->dupRejectCount < 255) m->dupRejectCount++;
    } else {
      m->dupRejectCount = 1;   // first reject, or the window lapsed -> restart
    }
    m->dupRejectTs = now;
    if (!m->dupSuspect && m->dupRejectCount >= DUP_ID_REJECT_THRESHOLD) {
      m->dupSuspect = true;
      newlyLatched = true;
    }
  }
  xSemaphoreGive(sfMutex);
  return newlyLatched;
}

void sfParseResponse(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'm') return;

  // Convert to null-terminated string for easier parsing.
  // Sized for long inbound frames (a full dump response is ~590 bytes).
  // NOTE: static (not stack) -- sfParseResponse is called only from taskRS485
  // (single caller, no reentrancy), and a 768-byte stack buffer here would
  // overflow that task's 6KB stack. Keeping it in .bss avoids the overflow.
  static char buf[TX_MAX_BYTES + 1];
  size_t copyLen = (len < TX_MAX_BYTES) ? len : TX_MAX_BYTES;
  memcpy(buf, data, copyLen);
  buf[copyLen] = 0;
  // Strip trailing \r\n
  for (int i = (int)copyLen - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r'); i--)
    buf[i] = 0;

  // -- Provisioning advertisement: mXadv:<serialNumber>
  if (strncmp(buf, "mXadv:", 6) == 0) {
    const char* sn = buf + 6;
    if (!sfValidSN(sn)) {
      DBG("[SF] rejecting adv with corrupt SN: %s\n", sn);
      sfParseRejects++;
      return;
    }
    xSemaphoreTake(sfMutex, portMAX_DELAY);
    SFModule* m = sfFindBySN(sn);
    if (!m) {
      m = sfUpsert(255, sn);
      if (m) m->provisioned = false;
      DBG("[SF] Unprovisioned adv: %s\n", sn);
    } else if (m->provisioned || m->id != 255) {
      // The module is advertising (so it has NO assigned id) but we still hold a
      // provisioned/old-id entry for this serial -- e.g. it was just
      // deprovisioned, or a stale in-flight frame re-created an id entry. The
      // serial is the true identity, so reclaim this same entry in place rather
      // than leaving a duplicate: mark it unprovisioned and clear the stale id.
      m->provisioned = false;
      m->id = 255;
      sfModulesDirty = true;
      DBG("[SF] Re-advertised known SN %s -- reset to unprovisioned\n", sn);
    }
    if (m) sfTouch(m);
    if (sfDedupeBySNLocked(sn)) sfModulesDirty = true;
    // Purge any "ghost" entries: a provisioned record with an EMPTY serial is
    // invalid (every provisioned module has a serial) -- it's the residue of a
    // stale in-flight frame that re-created an id entry for a module that has
    // since been deprovisioned and is now advertising. Drop them so they don't
    // show as blank/duplicate cards in the grid.
    for (int k = 0; k < sfModuleCount; ) {
      if (sfModules[k].provisioned && sfModules[k].serialNum[0] == '\0') {
        for (int j = k; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
        sfModuleCount--;
        memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
        sfModulesDirty = true;
      } else k++;
    }
    xSemaphoreGive(sfMutex);
    mqttPublishSFEvent("adv", sn);
    return;
  }

  // -- Provisioning ack. The module replies mXack<sn>:<id>. Some firmware
  // revisions insert a colon after the token (mXack:<sn>:<id>); accept BOTH.
  // Match the 5-char "mXack" token, then skip an optional ':' before the serial.
  // (A strict "mXack:" match missed the colon-less form the modules actually
  // send -- so the ack handler, and the post-provision version query inside it,
  // never ran, leaving a blank entry with no serial number or firmware version.)
  if (strncmp(buf, "mXack", 5) == 0) {
    const char* rest = buf + 5;
    if (*rest == ':') rest++;            // tolerate optional colon after "mXack"
    char tmp[48];
    strlcpy(tmp, rest, sizeof(tmp));
    char* colon = strrchr(tmp, ':');     // separates <sn> from <id>
    if (colon) {
      *colon = 0;
      const char* sn = tmp;
      int newId = atoi(colon + 1);
      if (!sfValidSN(sn)) {
        DBG("[SF] rejecting ack with corrupt SN: %s\n", sn);
      sfParseRejects++;
        return;
      }
      xSemaphoreTake(sfMutex, portMAX_DELAY);
      SFModule* m = sfFindBySN(sn);
      if (!m) m = sfUpsert((uint8_t)newId, sn);
      if (m) {
        m->id = (uint8_t)newId;
        m->provisioned = true;
        m->acked = true;                         // acked provisioning -> never legacy
        m->verDueMs = millis() + MODULE_POSTPROV_VER_MS;  // schedule deferred version query
        m->verTries = 0;
        sfTouch(m);
        sfModulesDirty = true;
      }
      xSemaphoreGive(sfMutex);
      char payload[64];
      snprintf(payload, sizeof(payload), "{\"sn\":\"%s\",\"id\":%d}", sn, newId);
      mqttPublishSFEvent("ack", payload);
      // The post-provision version query is issued by taskRS485 once verDueMs
      // passes (see the deferred-query sweep) -- NOT inline here. Sending it
      // immediately fires before the module is ready to answer on its new ID, so
      // no reply comes back. The short delay lets it settle first.
    }
    return;
  }

  // -- Normal module response: m<id><cmd>:<data>
  // Parse the address first
  const char* p = buf + 1; // skip leading 'm'
  char idStr[8] = {0};
  int  idLen = 0;
  while (*p && isdigit((unsigned char)*p) && idLen < 7) idStr[idLen++] = *p++;
  if (idLen == 0) return;
  uint8_t id = (uint8_t)atoi(idStr);

  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* m = sfUpsert(id, NULL);
  if (m) { m->provisioned = true; sfTouch(m); }
  xSemaphoreGive(sfMutex);

  if (!m) return;
  char cmd = *p++;

  // Version response: m<id>v:<version>:<moduleId>:<serialNumber>
  // Legacy format m<id>v:<version> also accepted.
  if (cmd == 'v' && *p == ':') {
    char verBuf[64];
    strlcpy(verBuf, p + 1, sizeof(verBuf));
    // Strip trailing whitespace/newlines
    for (int k = (int)strlen(verBuf)-1; k >= 0 && (verBuf[k] == '\n' || verBuf[k] == '\r' || verBuf[k] == ' '); k--)
      verBuf[k] = '\0';
    // Split into up to 3 fields on ':'
    char* field[3] = {nullptr, nullptr, nullptr};
    field[0] = verBuf;
    int fi = 1;
    for (char* cp = verBuf; *cp && fi < 3; cp++) {
      if (*cp == ':') { *cp = '\0'; field[fi++] = cp + 1; }
    }
    int reportedId = (field[1] && field[1][0]) ? atoi(field[1]) : -1;
    char fwCopy[8] = "?";
    if (field[0] && field[0][0]) strlcpy(fwCopy, field[0], sizeof(fwCopy));
    // Validate + write fwVersion/serial under the lock (re-finds the entry by id,
    // collapses any duplicate-serial record). A corrupt SN rejects the frame.
    if (!sfApplyVersionFields((uint8_t)id, fwCopy, field[2])) {
      DBG("[SF] rejecting corrupt version response for module %d (sn:%s)\n",
          id, field[2] ? field[2] : "");
      sfParseRejects++;
      if (sfNoteCorruptVersionReply((uint8_t)id)) {
        DBG("[SF] *** possible DUPLICATE ID %d on the bus: %d corrupt version replies "
            "(framing intact, serial garbled) -- two modules may share this ID ***\n",
            id, DUP_ID_REJECT_THRESHOLD);
        char wpl[72];
        snprintf(wpl, sizeof(wpl), "{\"id\":%d,\"reason\":\"duplicate_id_suspected\"}", id);
        mqttPublishSFEvent("warning", wpl);
      }
      return;
    }
    DBG("[SF] Module %d fw:%s reportedId:%d sn:%s\n",
                  id, fwCopy, reportedId, field[2] ? field[2] : "");
    char payload[96];
    snprintf(payload, sizeof(payload),
      "{\"id\":%d,\"ver\":\"%s\",\"reportedId\":%d,\"sn\":\"%s\"}",
      id, fwCopy, reportedId, field[2] ? field[2] : "");
    mqttPublishSFEvent("version", payload);
  }
  // Calibration result: m<id>:<steps>
  else if (cmd == ':') {
    int steps = atoi(p);
    DBG("[SF] Module %d calibrated: %d steps/rev\n", id, steps);
    if (gCalib.waitId == (int)id) {
      gCalib.steps     = steps;
      gCalib.ts = millis();
    }
    char payload[48];
    snprintf(payload, sizeof(payload), "{\"id\":%d,\"stepsPerRev\":%d}", id, steps);
    mqttPublishSFEvent("calibrated", payload);
  }
  // EEPROM dump: m<id>d:<homeOffset>:<totalSteps>:<map>
  else if (cmd == 'd' && *p == ':') {
    DBG("[SF] Module %d dump: %s\n", id, p + 1);
    // Capture into the shared single-slot buffer IF a dump request is
    // waiting for this module id (set by handleApiDump). No per-module cache.
    // Buffers sized for a full dump response (~590 bytes for a 64-flap map).
    // static (not stack): only taskRS485 reaches here, and 768-byte stack
    // buffers would overflow its 6KB stack.
    static char clean[TX_MAX_BYTES];
    strlcpy(clean, p + 1, sizeof(clean));
    size_t dl = strlen(clean);
    while (dl > 0 && (clean[dl-1] == '\n' || clean[dl-1] == '\r')) clean[--dl] = 0;
    if (gDump.waitId == (int)id) {
      strlcpy(gDump.data, clean, sizeof(gDump.data));
      gDump.ts = millis();
    }
    // Size the MQTT-event payload to the queue slot. The REST dump path
    // (handleApiDump) carries the full untruncated dump; this MQTT event is an
    // optional notification, so matching the queue size avoids a truncation gap.
    static char payload[MQTT_BUF_SIZE];
    snprintf(payload, sizeof(payload), "{\"id\":%d,\"dump\":\"%s\"}", id, clean);
    mqttPublishSFEvent("dump", payload);
  }
  // Combined all-fields dump (firmware v25+): a single reply carrying everything
  // the 'v' and 'd' responses do, plus autoHome and the live current index:
  //   m<id>A:<version>:<moduleId>:<serialNumber>:<homeOffset>:<totalSteps>:<autoHome>:<curIndex>:<map>
  // We update fwVersion + serialNum exactly like the version response, and
  // reconstruct the "<homeOffset>:<totalSteps>:<map>" dump portion into the same
  // capture slot the 'd' path uses -- so a single 'A' satisfies both a version
  // refresh and a dump read in one bus transaction (see handleApiAll).
  else if (cmd == 'A' && *p == ':') {
    static char aBuf[TX_MAX_BYTES];     // static: taskRS485's 6KB stack can't hold this
    strlcpy(aBuf, p + 1, sizeof(aBuf));
    for (int k = (int)strlen(aBuf)-1; k >= 0 && (aBuf[k]=='\n'||aBuf[k]=='\r'||aBuf[k]==' '); k--) aBuf[k] = 0;
    // Extract the optional v31+ flap-config tail BEFORE the destructive ':' split
    // below clobbers its separators. The map field is colon-free, so the 8th colon
    // (if present) begins <flapCount> and the 9th begins <flapChars> (which may
    // itself contain ':', so take it verbatim to end-of-string). A pre-v31 'A'
    // reply has only 7 colons and no tail, leaving these at -99/"".
    int  aFlapCount = -99;
    char aFlapChars[SF_MAX_FLAPS + 1] = "";
    {
      int colons = 0; const char* c8 = nullptr; const char* c9 = nullptr;
      for (const char* cp = aBuf; *cp; cp++) {
        if (*cp == ':') { if (++colons == 8) c8 = cp; else if (colons == 9) { c9 = cp; break; } }
      }
      if (c8) {
        aFlapCount = atoi(c8 + 1);
        if (c9) strlcpy(aFlapChars, c9 + 1, sizeof(aFlapChars));
      }
    }
    // Split into the 7 scalar fields plus the trailing map (which has no ':').
    // f: 0 ver, 1 modId, 2 sn, 3 homeOffset, 4 totalSteps, 5 autoHome, 6 curIndex, 7 map
    // The cap is well above the 8 fields we use: because the map is colon-free it
    // still lands cleanly in f[7], and any field a future firmware appends after
    // it falls into f[8+] and is harmlessly ignored -- rather than being glued
    // onto the map (which would corrupt the dump/backup string).
    char* f[16] = {0}; f[0] = aBuf; int fi = 1;
    for (char* cp = aBuf; *cp && fi < 16; cp++) { if (*cp == ':') { *cp = 0; f[fi++] = cp + 1; } }
    char fwCopy[8] = "?";
    if (f[0] && f[0][0]) strlcpy(fwCopy, f[0], sizeof(fwCopy));
    int reportedId = (f[1] && f[1][0]) ? atoi(f[1]) : -1;
    // Validate + write fwVersion/serial under the lock (shared with the 'v' path).
    if (!sfApplyVersionFields((uint8_t)id, fwCopy, f[2])) {
      DBG("[SF] rejecting corrupt all-fields response for module %d (sn:%s)\n", id, f[2] ? f[2] : "");
      sfParseRejects++;
      if (sfNoteCorruptVersionReply((uint8_t)id)) {
        DBG("[SF] *** possible DUPLICATE ID %d on the bus: %d corrupt version replies "
            "(framing intact, serial garbled) -- two modules may share this ID ***\n",
            id, DUP_ID_REJECT_THRESHOLD);
        char wpl[72];
        snprintf(wpl, sizeof(wpl), "{\"id\":%d,\"reason\":\"duplicate_id_suspected\"}", id);
        mqttPublishSFEvent("warning", wpl);
      }
      return;
    }
    // Reconstruct the dump portion into the shared capture slot if a request is
    // waiting (handleApiAll arms it), so the existing wait machinery works. The
    // 'A'-only extras (autoHome, curIndex, self-reported id) ride along in
    // dedicated globals -- they can't go in the dump string without breaking
    // parseDump, which expects exactly ho:ts:map. All set BEFORE the ready flag.
    if (gDump.waitId == (int)id) {
      snprintf(gDump.data, sizeof(gDump.data), "%s:%s:%s",
               f[3] ? f[3] : "", f[4] ? f[4] : "", f[7] ? f[7] : "");
      gDump.autoHome   = (f[5] && f[5][0]) ? atoi(f[5]) : -99;
      gDump.curIndex   = (f[6] && f[6][0]) ? atoi(f[6]) : -99;
      gDump.reportedId = reportedId;   // module's self-reported id (f[1])
      gDump.flapCount  = aFlapCount;   // v31+ flap-config tail (-99 if not present)
      strlcpy((char*)gDump.flapChars, aFlapChars, sizeof(gDump.flapChars));
      gDump.ts = millis();
    }
    DBG("[SF] Module %d ALL fw:%s reportedId:%d sn:%s ho:%s ts:%s\n",
        id, fwCopy, reportedId, f[2] ? f[2] : "", f[3] ? f[3] : "", f[4] ? f[4] : "");
    // Publish both a version and a dump event, since 'A' carries both.
    char vpl[96];
    snprintf(vpl, sizeof(vpl), "{\"id\":%d,\"ver\":\"%s\",\"reportedId\":%d,\"sn\":\"%s\"}",
             id, fwCopy, reportedId, f[2] ? f[2] : "");
    mqttPublishSFEvent("version", vpl);
    static char dpl[MQTT_BUF_SIZE];
    snprintf(dpl, sizeof(dpl), "{\"id\":%d,\"dump\":\"%s:%s:%s\"}",
             id, f[3] ? f[3] : "", f[4] ? f[4] : "", f[7] ? f[7] : "");
    mqttPublishSFEvent("dump", dpl);
  }
  // Diagnostics snapshot (firmware v26+): m<id>Q:<resetCause>:<bootCount>:<vcc_mV>:<eepromOk>:<curIndex>
  else if (cmd == 'Q' && *p == ':') {
    char qb[48];
    strlcpy(qb, p + 1, sizeof(qb));
    for (int k = (int)strlen(qb)-1; k >= 0 && (qb[k]=='\n'||qb[k]=='\r'||qb[k]==' '); k--) qb[k] = 0;
    char* qf[5] = {0}; qf[0] = qb; int qi = 1;
    for (char* cp = qb; *cp && qi < 5; cp++) { if (*cp == ':') { *cp = 0; qf[qi++] = cp + 1; } }
    int rc   = (qf[0] && qf[0][0]) ? atoi(qf[0]) : 0;
    int boot = (qf[1] && qf[1][0]) ? atoi(qf[1]) : 0;
    int vcc  = (qf[2] && qf[2][0]) ? atoi(qf[2]) : 0;
    int ee   = (qf[3] && qf[3][0]) ? atoi(qf[3]) : 0;
    int cur  = (qf[4] && qf[4][0]) ? atoi(qf[4]) : -1;
    DBG("[SF] Module %d diag Q: rc=%d boot=%d vcc=%d ee=%d cur=%d\n", id, rc, boot, vcc, ee, cur);
    if (gDiag.q.waitId == (int)id) {
      gDiag.q.reset = rc; gDiag.q.boot = boot; gDiag.q.vcc = vcc; gDiag.q.ee = ee; gDiag.q.cur = cur;
      gDiag.q.ts = millis();
    }
    char payload[96];
    snprintf(payload, sizeof(payload),
      "{\"id\":%d,\"resetCause\":%d,\"bootCount\":%d,\"vcc\":%d,\"eepromOk\":%d,\"curIndex\":%d}",
      id, rc, boot, vcc, ee, cur);
    mqttPublishSFEvent("diag", payload);
  }
  // Mechanical self-test (v26+; v28 appends gate/magnet/per-rev fields):
  // m<id>M:<code>:<min>:<max>:<spread>:<gateActive>:<gateSpan>:<avgMagnetWidth>:<r1>,...,<rN>
  else if (cmd == 'M' && *p == ':') {
    char mb[256];
    strlcpy(mb, p + 1, sizeof(mb));
    for (int k = (int)strlen(mb)-1; k >= 0 && (mb[k]=='\n'||mb[k]=='\r'||mb[k]==' '); k--) mb[k] = 0;
    char* mf[8] = {0}; mf[0] = mb; int mi = 1;
    for (char* cp = mb; *cp && mi < 8; cp++) { if (*cp == ':') { *cp = 0; mf[mi++] = cp + 1; } }
    int code = (mf[0] && mf[0][0]) ? atoi(mf[0]) : -1;
    int mn   = (mf[1] && mf[1][0]) ? atoi(mf[1]) : 0;
    int mx   = (mf[2] && mf[2][0]) ? atoi(mf[2]) : 0;
    int spr  = (mf[3] && mf[3][0]) ? atoi(mf[3]) : 0;
    int ga   = (mf[4] && mf[4][0]) ? atoi(mf[4]) : 0;
    int gs   = (mf[5] && mf[5][0]) ? atoi(mf[5]) : 0;
    int mw   = (mf[6] && mf[6][0]) ? atoi(mf[6]) : 0;
    const char* revs = (mf[7] && mf[7][0]) ? mf[7] : "";
    DBG("[SF] Module %d diag M: code=%d min=%d max=%d spread=%d gateA=%d gateS=%d magW=%d revs=%s\n",
        id, code, mn, mx, spr, ga, gs, mw, revs);
    if (gDiag.m.waitId == (int)id) {
      gDiag.m.code = code; gDiag.m.minVal = mn; gDiag.m.maxVal = mx; gDiag.m.spread = spr;
      gDiag.m.gateActive = ga; gDiag.m.gateSpan = gs; gDiag.m.magWidth = mw;
      strlcpy(gDiag.m.revs, revs, sizeof(gDiag.m.revs));
      gDiag.m.ts = millis();
    }
    char payload[96];
    snprintf(payload, sizeof(payload),
      "{\"id\":%d,\"code\":%d,\"min\":%d,\"max\":%d,\"spreadTenths\":%d}",
      id, code, mn, mx, spr);
    mqttPublishSFEvent("diag", payload);
  }
  // Hall sensor self-test (v26+; v28 appends fallingEdges):
  // m<id>T:<code>:<risingEdges>:<activeSamples>:<fallingEdges>
  else if (cmd == 'T' && *p == ':') {
    char tb[48];
    strlcpy(tb, p + 1, sizeof(tb));
    for (int k = (int)strlen(tb)-1; k >= 0 && (tb[k]=='\n'||tb[k]=='\r'||tb[k]==' '); k--) tb[k] = 0;
    char* tf[4] = {0}; tf[0] = tb; int ti = 1;
    for (char* cp = tb; *cp && ti < 4; cp++) { if (*cp == ':') { *cp = 0; tf[ti++] = cp + 1; } }
    int code = (tf[0] && tf[0][0]) ? atoi(tf[0]) : -1;
    int rise = (tf[1] && tf[1][0]) ? atoi(tf[1]) : 0;
    int act  = (tf[2] && tf[2][0]) ? atoi(tf[2]) : 0;
    int fall = (tf[3] && tf[3][0]) ? atoi(tf[3]) : 0;
    DBG("[SF] Module %d diag T: code=%d rising=%d active=%d falling=%d\n", id, code, rise, act, fall);
    if (gDiag.t.waitId == (int)id) {
      gDiag.t.code = code; gDiag.t.rising = rise; gDiag.t.active = act; gDiag.t.falling = fall;
      gDiag.t.ts = millis();
    }
    char payload[96];
    snprintf(payload, sizeof(payload),
      "{\"id\":%d,\"hallCode\":%d,\"rising\":%d,\"active\":%d,\"falling\":%d}",
      id, code, rise, act, fall);
    mqttPublishSFEvent("diag", payload);
  }
}
