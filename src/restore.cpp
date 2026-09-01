#include "gateway.h"
#include <esp_heap_caps.h>

// restore.cpp -- restore-on-boot (v3.12). See restore.h for the contract.
//
// Shape of a run (taskRestore, its own FreeRTOS task, priority 1):
//
//   WAITING    the bus is already locked (restoreBusy() is true from the moment the
//              state leaves IDLE), and we wait out the post-boot delay so the modules
//              have finished their own power-up and auto-home before we talk to them.
//   WRITING    fire-and-forget, paced: for every entry, one mXW<sn>:... frame (the
//              same frame POST /api/flap/restorebysn builds) then RESTORE_STEP_MS of
//              silence for the module's EEPROM write; then, if the backup's id differs
//              from the id the registry has for that serial, an mXI<sn>:<id>. A
//              re-provisioned module restarts and is deaf for a few seconds, which is
//              why ids are written AFTER the calibration, and why the verify phase
//              waits for such a module to come back before asking it anything.
//   VERIFYING  every entry is read back (mXA by serial on v25+, mXD on v15+, m<id>d on
//              older firmware) and compared field by field: home offset, steps per
//              revolution, the flap map, and -- when both sides have one -- the flap
//              set. A mismatch or a silent module gets ONE repair round (the write is
//              re-sent and the read-back repeated). After that the verdict stands.
//   HOMING     m*h, so every reel re-homes against its restored calibration.
//   DONE       counters are final; the lock releases.
//
// It cannot hang: every wait is bounded, a module that never answers is "missing",
// and a cancel request is honoured between steps. The lock therefore always
// releases, which is the property that makes "block everything" acceptable.
//
// The backup is parsed with ArduinoJson into PSRAM (a 64-module backup is ~45 KB;
// the internal heap is what WiFi lives on). The parsed document stays alive for the
// whole run so the two phases iterate the same entries.

RestoreProgress gRestore;
RestoreFileInfo gRestoreFile;

static TaskHandle_t  hTaskRestore   = NULL;
static volatile bool gRestoreCancel = false;

// ---- ArduinoJson allocator that prefers PSRAM ------------------------------------
struct PsramJsonAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override {
    void* p = psramFound() ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : NULL;
    return p ? p : malloc(n);
  }
  void deallocate(void* p) override { free(p); }
  void* reallocate(void* p, size_t n) override {
    void* q = psramFound() ? heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM) : NULL;
    return q ? q : realloc(p, n);
  }
};
static PsramJsonAllocator gJsonAlloc;

// ---- per-entry result codes --------------------------------------------------------
enum : uint8_t { RES_PENDING = 0, RES_OK, RES_MISMATCH, RES_MISSING, RES_SKIPPED };

// One backup entry, decoded. Strings point into the JsonDocument (alive for the run).
struct RestoreEntry {
  const char* sn;
  int         id;           // -1 = not in the backup
  int         homeOffset;
  int         totalSteps;
  const char* map;          // "i=p,i=p,..." (may be "")
  int         flapCount;    // 0 = none in the backup
  char        flapBytes[SF_MAX_FLAPS + 1];   // backup flapChars transcoded to bus bytes ("" = none)
};

const char* restoreStateName() {
  switch (gRestore.state) {
    case RESTORE_WAITING:   return "waiting";
    case RESTORE_WRITING:   return "writing";
    case RESTORE_VERIFYING: return "verifying";
    case RESTORE_HOMING:    return "homing";
    case RESTORE_DONE:      return "done";
    case RESTORE_FAILED:    return "failed";
    case RESTORE_CANCELLED: return "cancelled";
    default:                return "idle";
  }
}

// The map field of a dump is "i=p,i=p"; anything else in it would corrupt the mXW frame.
static bool restoreMapIsClean(const char* map) {
  for (const char* p = map; *p; p++)
    if (!(isdigit((unsigned char)*p) || *p == '=' || *p == ',')) return false;
  return true;
}

// Split "<homeOffset>:<totalSteps>[:<map>]". Returns false if it is not that shape.
static bool restoreParseDump(const char* dump, int* ho, int* ts, const char** map) {
  if (!dump || !dump[0]) return false;
  char* end = NULL;
  long a = strtol(dump, &end, 10);
  if (end == dump || *end != ':') return false;
  const char* p = end + 1;
  long b = strtol(p, &end, 10);
  if (end == p || (*end != ':' && *end != 0)) return false;
  if (b < 0) return false;
  *ho  = (int)a;
  *ts  = (int)b;
  *map = (*end == ':') ? end + 1 : "";
  return restoreMapIsClean(*map);
}

// Decode one backup entry. Returns false if it cannot be restored from.
static bool restoreParseEntry(JsonObjectConst o, RestoreEntry& e) {
  memset(&e, 0, sizeof(e));
  e.sn = o["sn"] | "";
  if (!sfIsValidSN(e.sn)) return false;
  if (!restoreParseDump(o["dump"] | "", &e.homeOffset, &e.totalSteps, &e.map)) return false;
  e.id = o["id"] | -1;
  if (e.id < 0 || e.id > 254) e.id = -1;
  // Optional v31+ flap set. The backup holds it as UTF-8 JSON; the bus wants
  // Windows-1252 bytes. An unusable set is dropped (the calibration still restores).
  int fc = o["flapCount"] | 0;
  if (fc >= 1 && fc <= SF_MAX_FLAPS) e.flapCount = fc;
  const char* chars = o["flapChars"] | "";
  if (chars[0]) {
    char tmp[SF_MAX_FLAPS * 4 + 4];
    bool all = true;
    size_t n = utf8ToFlap(chars, tmp, sizeof(tmp), &all);
    if (all && n >= 1 && n <= SF_MAX_FLAPS) memcpy(e.flapBytes, tmp, n + 1);
  }
  // A flap set is only trusted when it is self-consistent: the character set must be
  // exactly flapCount long. A backup made from a read-back that lost its tail carries e.g.
  // flapCount 64 with 3 characters, and restoring THAT would write the truncated set into
  // the module (seen in the field). Such an entry restores the calibration but leaves the
  // module's flap set alone.
  if (e.flapCount && e.flapBytes[0] && (int)strlen(e.flapBytes) != e.flapCount) {
    DBG("[RESTORE] %s: flap set in backup is inconsistent (%d flaps, %u chars) -- not restoring it\n",
        e.sn, e.flapCount, (unsigned)strlen(e.flapBytes));
    e.flapCount = 0;
    e.flapBytes[0] = 0;
  } else if (e.flapCount && !e.flapBytes[0]) {
    e.flapCount = 0;   // a count without its characters is the same truncation
  }
  return true;
}

// Read `path` into a PSRAM buffer and parse it into `doc`. NULL on success, else a reason.
static const char* restoreLoadDoc(JsonDocument& doc, const char* path, size_t* bytesOut) {
  if (!sfFsReady)          return "no filesystem";
  if (!FFat.exists(path))  return "no backup stored";
  File f = FFat.open(path, "r");
  if (!f)                  return "cannot open backup";
  size_t n = f.size();
  if (n == 0)              { f.close(); return "backup file is empty"; }
  if (n > RESTORE_MAX_BYTES) { f.close(); return "backup file too large"; }
  char* buf = (char*)(psramFound() ? heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM) : NULL);
  if (!buf) buf = (char*)malloc(n + 1);
  if (!buf)                { f.close(); return "out of memory"; }
  size_t got = f.read((uint8_t*)buf, n);
  f.close();
  buf[got] = 0;
  DeserializationError err = deserializeJson(doc, (const char*)buf);
  free(buf);
  if (bytesOut) *bytesOut = got;
  if (err)                 return "backup is not valid JSON";
  if (!doc["modules"].is<JsonArrayConst>()) return "backup has no modules array";
  return NULL;
}

// Summarise a parsed backup into `info`.
static void restoreSummarise(JsonDocument& doc, size_t bytes, RestoreFileInfo* info) {
  info->exists  = true;
  info->bytes   = bytes;
  info->version = doc["version"] | 0;
  strlcpy(info->created, doc["created"] | "", sizeof(info->created));
  info->entries = 0;
  info->modules = 0;
  JsonArrayConst mods = doc["modules"].as<JsonArrayConst>();
  for (JsonObjectConst o : mods) {
    info->entries++;
    RestoreEntry e;
    if (restoreParseEntry(o, e)) info->modules++;
  }
}

bool restoreValidateFile(const char* path, RestoreFileInfo* info, char* err, size_t errLen) {
  JsonDocument doc(&gJsonAlloc);
  size_t bytes = 0;
  const char* why = restoreLoadDoc(doc, path, &bytes);
  if (why) { strlcpy(err, why, errLen); return false; }
  RestoreFileInfo tmp;
  restoreSummarise(doc, bytes, &tmp);
  if (tmp.modules == 0) { strlcpy(err, "backup has no usable modules", errLen); return false; }
  if (info) *info = tmp;
  return true;
}

bool restoreRefreshFileInfo() {
  RestoreFileInfo info;   // defaults: exists=false
  char err[64];
  if (restoreValidateFile(RESTORE_FILE, &info, err, sizeof(err))) {
    gRestoreFile = info;
  } else {
    gRestoreFile = RestoreFileInfo();
    if (sfFsReady && FFat.exists(RESTORE_FILE)) {
      // The file is there but unusable -- report its presence so the UI can offer delete.
      gRestoreFile.exists = true;
      File f = FFat.open(RESTORE_FILE, "r");
      if (f) { gRestoreFile.bytes = f.size(); f.close(); }
      printf("[RESTORE] stored backup is unusable: %s\n", err);
    }
  }
  return gRestoreFile.exists;
}

bool restoreDeleteFile() {
  if (restoreBusy()) return false;
  if (sfFsReady) { FFat.remove(RESTORE_FILE); FFat.remove(RESTORE_TMP); }
  gRestoreFile = RestoreFileInfo();
  printf("[RESTORE] stored backup deleted\n");
  return true;
}

// ---- bus helpers ------------------------------------------------------------------

// Arm the shared dump-capture slot for ANY module, send `frame`, wait for a 'd' or
// 'A' reply. Exclusive use of the slot is guaranteed by the lock: the REST handlers
// that also use it are all refused while restoreBusy().
static bool restoreCaptureDump(const char* frame, unsigned long timeoutMs) {
  gDump.data[0]   = 0;
  gDump.ts        = 0;
  gDump.autoHome  = -99;
  gDump.curIndex  = -99;
  gDump.reportedId = -99;
  gDump.flapCount = -99;
  gDump.flapChars[0] = 0;
  gDump.gotId     = -1;
  gDump.gotSN[0]  = 0;
  gDump.waitId    = SF_WAIT_ANY;
  rs485SendStr(frame);
  unsigned long deadline = millis() + timeoutMs;
  while ((long)(millis() - deadline) < 0) {
    vTaskDelay(pdMS_TO_TICKS(10));
    if (gDump.ts != 0) { gDump.waitId = -1; return true; }
  }
  gDump.waitId = -1;
  return false;
}

// Expand "i=p,..." into a per-index table (-1 = unset). Later entries win, like the module.
static void restoreMapToTable(const char* map, int32_t out[SF_MAX_FLAPS]) {
  for (int i = 0; i < SF_MAX_FLAPS; i++) out[i] = -1;
  const char* p = map;
  while (p && *p) {
    char* end = NULL;
    long idx = strtol(p, &end, 10);
    if (end == p || *end != '=') break;
    const char* v = end + 1;
    long pos = strtol(v, &end, 10);
    if (end == v) break;
    if (idx >= 0 && idx < SF_MAX_FLAPS) out[idx] = (int32_t)pos;
    if (*end == ',') p = end + 1; else break;
  }
}

// Send the entry's calibration (and id, when it differs). `readyAt` receives the
// millis() by which the module may be asked to read it back.
//
// The map is deliberately NOT sent inside the mXW frame (see RESTORE_MXW_SETTLE_MS in
// common.h): the mXW carries offset, steps and the flap set and clears the map, and the
// entries follow one per short m<id>w frame, paced. That needs the module's id, which is
// the backup's (after re-provisioning) or the registry's; only if neither is known does
// the map go inline, which is what the module can least cope with.
static void restoreWriteOne(const RestoreEntry& e, unsigned long* readyAt) {
  static char frame[TX_MAX_BYTES + 1];   // static: off the task stack
  uint8_t curId = 255;
  bool known = sfLookupBySN(e.sn, &curId, NULL, 0);
  int  wId   = (e.id >= 0) ? e.id : (known ? (int)curId : -1);
  bool perEntry = (wId >= 0 && e.map[0]);

  size_t n = sfBuildRestoreFrame(e.sn, e.homeOffset, e.totalSteps, perEntry ? "" : e.map,
                                 e.flapCount, e.flapBytes, frame, sizeof(frame));
  if (!n) {
    // Cannot happen for a backup the gateway itself produced; log rather than truncate.
    printf("[RESTORE] %s: restore frame too long -- skipped\n", e.sn);
    *readyAt = millis();
    return;
  }
  if (!perEntry && e.map[0])
    printf("[RESTORE] %s: no id known -- map sent inline (the module may drop entries)\n", e.sn);
  rs485SendStr(frame);
  vTaskDelay(pdMS_TO_TICKS(RESTORE_MXW_SETTLE_MS));   // map erase + flap-set write on the module

  if (e.id >= 0 && (!known || curId != (uint8_t)e.id)) {
    DBG("[RESTORE] %s: id %d -> %d\n", e.sn, known ? (int)curId : -1, e.id);
    sfProvision(e.sn, e.id);
    // The module writes its id and restarts with a staggered start-up (~150 ms x id).
    // Nothing can be sent to it until then, so the entries below wait it out too.
    unsigned long back = millis() + MODULE_POSTPROV_VER_MS + 150UL * (unsigned long)e.id;
    while ((long)(millis() - back) < 0 && !gRestoreCancel) vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (perEntry) {
    int sent = 0;
    const char* p = e.map;
    while (*p && !gRestoreCancel) {
      char* end = NULL;
      long idx = strtol(p, &end, 10);
      if (end == p || *end != '=') break;
      const char* v = end + 1;
      long pos = strtol(v, &end, 10);
      if (end == v) break;
      if (idx >= 0 && idx < SF_MAX_FLAPS) {
        sfWritePos(wId, (int)idx, (int)pos);
        sent++;
        vTaskDelay(pdMS_TO_TICKS(RESTORE_ENTRY_GAP_MS));
      }
      if (*end == ',') p = end + 1; else break;
    }
    DBG("[RESTORE] %s: %d map entries written to id %d\n", e.sn, sent, wId);
  }
  *readyAt = millis() + RESTORE_SETTLE_MS;
}

// Read the entry's EEPROM back and compare. RES_OK / RES_MISMATCH / RES_MISSING.
static uint8_t restoreVerifyOne(const RestoreEntry& e) {
  uint8_t curId = 255;
  char fw[8] = "";
  bool known = sfLookupBySN(e.sn, &curId, fw, sizeof(fw));
  const char* v = (fw[0] == 'v' || fw[0] == 'V') ? fw + 1 : fw;
  int fwNum = known ? atoi(v) : 0;

  char frame[48];
  bool gotReply = false;
  for (int attempt = 0; attempt < RESTORE_VERIFY_TRIES && !gotReply; attempt++) {
    // Unknown firmware: try the rich 'A' first, then the older by-serial dump.
    if (fwNum >= 25 || (fwNum == 0 && attempt == 0)) snprintf(frame, sizeof(frame), "mXA%s\n", e.sn);
    else if (fwNum >= 15 || fwNum == 0)              snprintf(frame, sizeof(frame), "mXD%s\n", e.sn);
    else if (known && curId != 255)                  snprintf(frame, sizeof(frame), "m%dd\n", curId);
    else                                             snprintf(frame, sizeof(frame), "mXD%s\n", e.sn);
    gotReply = restoreCaptureDump(frame, RESTORE_VERIFY_TIMEOUT_MS);
    if (!gotReply) vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!gotReply) return RES_MISSING;

  // An 'A' reply names its serial: make sure it is the module we asked.
  if (gDump.gotSN[0] && strcasecmp(gDump.gotSN, e.sn) != 0) {
    DBG("[RESTORE] %s: reply from %s instead\n", e.sn, gDump.gotSN);
    return RES_MISSING;
  }
  int ho = 0, ts = 0; const char* map = "";
  if (!restoreParseDump(gDump.data, &ho, &ts, &map)) return RES_MISMATCH;
  if (ho != e.homeOffset || ts != e.totalSteps) {
    DBG("[RESTORE] %s: ho/ts %d/%d != backup %d/%d\n", e.sn, ho, ts, e.homeOffset, e.totalSteps);
    return RES_MISMATCH;
  }
  int32_t want[SF_MAX_FLAPS], have[SF_MAX_FLAPS];
  restoreMapToTable(e.map, want);
  restoreMapToTable(map,   have);
  for (int i = 0; i < SF_MAX_FLAPS; i++) {
    if (want[i] != have[i]) {
      DBG("[RESTORE] %s: flap %d is %ld, backup says %ld\n", e.sn, i, (long)have[i], (long)want[i]);
      return RES_MISMATCH;
    }
  }
  // Flap set: only when both the backup and the reply carry one (v31+ 'A'), and only
  // when the reply's is self-consistent -- a read-back that lost its tail is not evidence
  // that the module's set differs (the parser already drops those; belt and braces).
  if (gDump.flapCount > 0 && (int)strlen((const char*)gDump.flapChars) == gDump.flapCount) {
    if (e.flapCount && gDump.flapCount != e.flapCount) return RES_MISMATCH;
    if (e.flapBytes[0] && strcmp((const char*)gDump.flapChars, e.flapBytes) != 0) return RES_MISMATCH;
  }
  return RES_OK;
}

static void restoreFinish(uint8_t state) {
  gDump.waitId       = -1;
  gRestore.finishedMs = millis();
  gRestore.state      = state;
  printf("[RESTORE] %s: %d ok, %d mismatch, %d missing, %d skipped of %d (%lus)\n",
         restoreStateName(), gRestore.okCount, gRestore.mismatch, gRestore.missing,
         gRestore.skipped, gRestore.total,
         (gRestore.finishedMs - gRestore.startedMs) / 1000UL);
}

// ---- the task ----------------------------------------------------------------------
static void taskRestore(void* pv) {
  // 1. The post-boot delay. The lock is already held (state == WAITING).
  while ((long)(millis() - gRestore.startAtMs) < 0 && !gRestoreCancel)
    vTaskDelay(pdMS_TO_TICKS(100));
  if (gRestoreCancel) { restoreFinish(RESTORE_CANCELLED); hTaskRestore = NULL; vTaskDelete(NULL); }

  gRestore.startedMs = millis();
  printf("[RESTORE] starting (%s) from %s\n", gRestore.trigger, RESTORE_FILE);

  // 2. Load the backup.
  JsonDocument doc(&gJsonAlloc);
  const char* why = restoreLoadDoc(doc, RESTORE_FILE, NULL);
  if (why) {
    strlcpy(gRestore.lastError, why, sizeof(gRestore.lastError));
    printf("[RESTORE] cannot start: %s\n", why);
    restoreFinish(RESTORE_FAILED);
    hTaskRestore = NULL; vTaskDelete(NULL);
  }
  JsonArrayConst mods = doc["modules"].as<JsonArrayConst>();
  int total = (int)mods.size();
  if (total > MAX_MODULES) total = MAX_MODULES;
  gRestore.total = total;

  static uint8_t       res[MAX_MODULES];
  static unsigned long readyAt[MAX_MODULES];
  memset(res, 0, sizeof(res));
  memset(readyAt, 0, sizeof(readyAt));

  // 3. Write phase: fire-and-forget, paced.
  gRestore.state = RESTORE_WRITING;
  gRestore.done  = 0;
  {
    int i = 0;
    for (JsonObjectConst o : mods) {
      if (i >= total) break;
      if (gRestoreCancel) break;
      RestoreEntry e;
      if (!restoreParseEntry(o, e)) {
        res[i] = RES_SKIPPED;
        gRestore.skipped = gRestore.skipped + 1;
        DBG("[RESTORE] entry %d skipped (no serial or bad dump)\n", i);
      } else {
        DBG("[RESTORE] write %d/%d %s\n", i + 1, total, e.sn);
        restoreWriteOne(e, &readyAt[i]);
      }
      i++;
      gRestore.done = i;
    }
  }
  if (gRestoreCancel) { restoreFinish(RESTORE_CANCELLED); hTaskRestore = NULL; vTaskDelete(NULL); }

  // 4. Verify phase, with one repair round for anything that did not match.
  gRestore.state = RESTORE_VERIFYING;
  for (int round = 0; round <= RESTORE_REPAIR_ROUNDS && !gRestoreCancel; round++) {
    gRestore.done = 0;
    int i = 0;
    for (JsonObjectConst o : mods) {
      if (i >= total) break;
      if (gRestoreCancel) break;
      int idx = i++;
      gRestore.done = i;
      if (res[idx] == RES_SKIPPED || res[idx] == RES_OK) continue;
      RestoreEntry e;
      if (!restoreParseEntry(o, e)) { res[idx] = RES_SKIPPED; continue; }
      // A re-provisioned module is deaf while it restarts; give it its time.
      while ((long)(millis() - readyAt[idx]) < 0 && !gRestoreCancel) vTaskDelay(pdMS_TO_TICKS(50));
      res[idx] = restoreVerifyOne(e);
      if (res[idx] == RES_OK) {
        DBG("[RESTORE] verify %d/%d %s ok\n", i, total, e.sn);
      } else if (round < RESTORE_REPAIR_ROUNDS) {
        printf("[RESTORE] verify %d/%d %s %s -- re-sending\n", i, total, e.sn,
               res[idx] == RES_MISSING ? "no reply" : "mismatch");
        restoreWriteOne(e, &readyAt[idx]);
      } else {
        printf("[RESTORE] verify %d/%d %s %s\n", i, total, e.sn,
               res[idx] == RES_MISSING ? "no reply" : "mismatch");
      }
    }
  }
  {
    int ok = 0, mm = 0, ms = 0, sk = 0;
    for (int i = 0; i < total; i++) {
      switch (res[i]) {
        case RES_OK:       ok++; break;
        case RES_MISMATCH: mm++; break;
        case RES_SKIPPED:  sk++; break;
        default:           ms++; break;   // MISSING, or PENDING after a cancel
      }
    }
    gRestore.okCount = ok; gRestore.mismatch = mm; gRestore.missing = ms; gRestore.skipped = sk;
  }
  if (gRestoreCancel) { restoreFinish(RESTORE_CANCELLED); hTaskRestore = NULL; vTaskDelete(NULL); }

  // 5. Home the wall so every reel re-homes against its restored calibration.
  gRestore.state = RESTORE_HOMING;
  sfHome(-1);
  vTaskDelay(pdMS_TO_TICKS(200));

  // 6. Done -- the lock releases here.
  restoreFinish(RESTORE_DONE);
  hTaskRestore = NULL;
  vTaskDelete(NULL);
}

bool restoreStart(unsigned long delayMs, const char* trigger, char* err, size_t errLen) {
  if (restoreBusy() || hTaskRestore) { strlcpy(err, "restore already running", errLen); return false; }
  if (!gRestoreFile.exists || gRestoreFile.modules == 0) {
    strlcpy(err, gRestoreFile.exists ? "stored backup is unusable" : "no backup stored", errLen);
    return false;
  }
  gRestoreCancel = false;
  gRestore.total = 0;      gRestore.done = 0;
  gRestore.okCount = 0;    gRestore.mismatch = 0;
  gRestore.missing = 0;    gRestore.skipped = 0;
  gRestore.startedMs = 0;  gRestore.finishedMs = 0;
  gRestore.lastError[0] = 0;
  strlcpy(gRestore.trigger, trigger, sizeof(gRestore.trigger));
  gRestore.startAtMs = millis() + delayMs;
  gRestore.state     = RESTORE_WAITING;   // the lock is held from this moment
  // Priority 1 (like the network task); core 1 so it never competes with the RS-485
  // receive task on core 0. 8 KB: the JSON lives in PSRAM, the frames are static.
  if (xTaskCreatePinnedToCore(taskRestore, "Restore", 8192, NULL, 1, &hTaskRestore, 1) != pdPASS) {
    hTaskRestore = NULL;
    gRestore.state = RESTORE_FAILED;
    strlcpy(gRestore.lastError, "could not start restore task", sizeof(gRestore.lastError));
    strlcpy(err, gRestore.lastError, errLen);
    return false;
  }
  printf("[RESTORE] scheduled (%s) in %lu ms: %d module(s), bus locked\n",
         trigger, delayMs, gRestoreFile.modules);
  return true;
}

bool restoreCancel() {
  if (!restoreBusy()) return false;
  gRestoreCancel = true;
  printf("[RESTORE] cancel requested\n");
  return true;
}

void restoreInit() {
  restoreRefreshFileInfo();
  if (!cfg.restoreOnBoot) return;
  char err[64];
  if (!restoreStart((unsigned long)cfg.restoreDelaySec * 1000UL, "boot", err, sizeof(err))) {
    // Enabled but nothing usable to restore from: say so, and do NOT lock the bus.
    printf("[RESTORE] restore-on-boot is enabled but skipped: %s\n", err);
    gRestore.state = RESTORE_FAILED;
    strlcpy(gRestore.lastError, err, sizeof(gRestore.lastError));
  }
}

// ---- JSON --------------------------------------------------------------------------
void restoreStatusJson(char* out, size_t outLen, bool full) {
  unsigned long now = millis();
  long startsIn = 0;
  if (gRestore.state == RESTORE_WAITING) {
    long d = (long)(gRestore.startAtMs - now);
    startsIn = d > 0 ? (d + 999) / 1000 : 0;
  }
  if (!full) {
    snprintf(out, outLen,
      "{\"state\":\"%s\",\"done\":%d,\"total\":%d,\"startsIn\":%ld}",
      restoreStateName(), (int)gRestore.done, (int)gRestore.total, startsIn);
    return;
  }
  long elapsed = 0;
  if (gRestore.startedMs) {
    unsigned long end = gRestore.finishedMs ? gRestore.finishedMs : now;
    elapsed = (long)((end - gRestore.startedMs) / 1000UL);
  }
  long finishedAgo = gRestore.finishedMs ? (long)((now - gRestore.finishedMs) / 1000UL) : -1;
  // `created` is copied verbatim from the backup; escape it in case it is not the
  // ISO timestamp the dashboard writes.
  char created[64]; size_t ci = 0;
  for (const char* p = gRestoreFile.created; *p && ci < sizeof(created) - 3; p++) {
    if (*p == '"' || *p == '\\') created[ci++] = '\\';
    if ((unsigned char)*p < 0x20) continue;
    created[ci++] = *p;
  }
  created[ci] = 0;
  snprintf(out, outLen,
    "{\"ok\":true,\"enabled\":%s,\"delay\":%u,"
    "\"file\":{\"exists\":%s,\"bytes\":%u,\"modules\":%d,\"entries\":%d,\"version\":%d,\"created\":\"%s\"},"
    "\"state\":\"%s\",\"busy\":%s,\"trigger\":\"%s\",\"total\":%d,\"done\":%d,"
    "\"okCount\":%d,\"mismatch\":%d,\"missing\":%d,\"skipped\":%d,"
    "\"startsIn\":%ld,\"elapsed\":%ld,\"finishedAgo\":%ld,\"lastError\":\"%s\"}",
    cfg.restoreOnBoot ? "true" : "false", (unsigned)cfg.restoreDelaySec,
    gRestoreFile.exists ? "true" : "false", (unsigned)gRestoreFile.bytes,
    gRestoreFile.modules, gRestoreFile.entries, gRestoreFile.version, created,
    restoreStateName(), restoreBusy() ? "true" : "false", gRestore.trigger,
    (int)gRestore.total, (int)gRestore.done,
    (int)gRestore.okCount, (int)gRestore.mismatch, (int)gRestore.missing, (int)gRestore.skipped,
    startsIn, elapsed, finishedAgo, gRestore.lastError);
}
