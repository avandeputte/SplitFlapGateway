#include "gateway.h"

// modfw.cpp -- see modfw.h for the shape of a run and why the bus is held exclusively.

ModFwState gModFw;
static TaskHandle_t hTaskModFw = NULL;
static uint8_t*  gImage    = NULL;      // the 15,360-byte image, in PSRAM
static uint16_t  gImageCrc = 0;

// The one status reply slot. Only the update task asks for status, and it asks one module at a
// time, so a single slot is enough -- the same arrangement the dump capture uses.
static struct {
  volatile int           waitId = -1;
  volatile unsigned long ts     = 0;
  uint8_t version, reason, flags;
  uint16_t imageCrc;
  uint8_t  pages[32];
} gBootSt;

void modfwInit() {
  if (!gModFw.list) gModFw.list = (ModFwTarget*) psramAlloc("module update targets",
                                                            sizeof(ModFwTarget) * MAX_MODULES);
}

/* ---- frames ---------------------------------------------------------------------------
   A bootloader frame is '#', the frame's bytes in upper-case hex, then '\n'. The CRC covers
   every byte before it. Modules running their firmware ignore these lines entirely -- their
   parser only acts on 'm' and '~' -- so this traffic is invisible to a module that is not in
   update mode. */
static void bootFrame(const uint8_t* data, size_t n) {
  static char line[MODFW_PAGE * 2 + 32];
  if (n + 2 > (sizeof(line) - 3) / 2) return;
  const uint16_t crc = sfCrc16(data, n);
  size_t w = 0;
  line[w++] = '#';
  static const char* kHex = "0123456789ABCDEF";
  for (size_t i = 0; i < n; i++) { line[w++] = kHex[data[i] >> 4]; line[w++] = kHex[data[i] & 15]; }
  line[w++] = kHex[(crc >> 12) & 15]; line[w++] = kHex[(crc >> 8) & 15];
  line[w++] = kHex[(crc >> 4) & 15];  line[w++] = kHex[crc & 15];
  line[w++] = '\n';
  rs485Send((const uint8_t*)line, w, true);     // raw: this framing is not the firmware's
}

void modfwNoteReply(const uint8_t* data, size_t len) {
  // '#' + 82 hex + '\n' for a 41-byte status frame; anything else is not for us.
  if (len < 3 || data[0] != '#') return;
  size_t hexLen = len - 1;
  while (hexLen && (data[hexLen] == '\n' || data[hexLen] == '\r')) hexLen--;
  if (hexLen % 2) return;
  const size_t n = hexLen / 2;
  if (n != 41) return;
  uint8_t d[41];
  for (size_t i = 0; i < n; i++) {
    char hi = (char)data[1 + i * 2], lo = (char)data[2 + i * 2];
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    int a = nib(hi), b = nib(lo);
    if (a < 0 || b < 0) return;
    d[i] = (uint8_t)((a << 4) | b);
  }
  if (sfCrc16(d, n) != 0) return;               // the CRC trails the frame, so the whole thing checks to 0
  if (d[1] != 's') return;
  if (gBootSt.waitId >= 0 && d[0] != (uint8_t)gBootSt.waitId) return;
  gBootSt.version  = d[2];
  gBootSt.reason   = d[3];
  gBootSt.flags    = d[4];
  gBootSt.imageCrc = (uint16_t)((d[5] << 8) | d[6]);
  memcpy(gBootSt.pages, d + 7, 32);
  gBootSt.ts = millis();
}

// Ask one module for its bootloader status. Up to `tries` attempts, each waiting out the
// documented 250 ms. False means nothing answered, which is also what a module without the
// bootloader does.
static bool bootStatus(uint8_t id, int tries = 3) {
  for (int t = 0; t < tries; t++) {
    gBootSt.ts = 0;
    gBootSt.waitId = id;
    const uint8_t f[2] = { id, 'S' };
    bootFrame(f, 2);
    const unsigned long deadline = millis() + MODFW_STATUS_WAIT_MS;
    while ((long)(millis() - deadline) < 0) {
      wdgWebMs = millis();
      vTaskDelay(pdMS_TO_TICKS(10));
      if (gBootSt.ts) { gBootSt.waitId = -1; return true; }
    }
  }
  gBootSt.waitId = -1;
  return false;
}

bool modfwProbe(int id, uint8_t* version, uint8_t* reason, uint8_t* flags,
                uint16_t* imageCrc, int* pagesHeld) {
  if (id < 0 || id > 254) return false;
  if (!bootStatus((uint8_t)id, 2)) return false;
  if (version)  *version  = gBootSt.version;
  if (reason)   *reason   = gBootSt.reason;
  if (flags)    *flags    = gBootSt.flags;
  if (imageCrc) *imageCrc = gBootSt.imageCrc;
  if (pagesHeld) {
    int held = 0;
    for (int p = MODFW_FIRST_PAGE; p <= MODFW_LAST_PAGE; p++)
      if (gBootSt.pages[p / 8] & (1 << (p % 8))) held++;
    *pagesHeld = held;
  }
  return true;
}

/* ---- the image ------------------------------------------------------------------------ */
bool modfwValidateFile(const char* path, uint16_t* crcOut, char* err, size_t errLen) {
  if (!sfFsReady)         { strlcpy(err, "no filesystem", errLen); return false; }
  if (!FFat.exists(path)) { strlcpy(err, "no image stored", errLen); return false; }
  File f = FFat.open(path, "r");
  if (!f) { strlcpy(err, "cannot open the image", errLen); return false; }
  const size_t n = f.size();
  if (n != MODFW_IMAGE_BYTES) {
    f.close();
    snprintf(err, errLen, "%u bytes, expected %u (firmware-update.bin)",
             (unsigned)n, (unsigned)MODFW_IMAGE_BYTES);
    return false;
  }
  // CRC the file in chunks: the image carries its own CRC in its last two bytes.
  uint16_t crc = 0xFFFF;
  static uint8_t buf[512];
  size_t left = MODFW_IMAGE_BYTES - 2;
  while (left) {
    size_t take = left > sizeof(buf) ? sizeof(buf) : left;
    if (f.read(buf, take) != (int)take) { f.close(); strlcpy(err, "cannot read the image", errLen); return false; }
    for (size_t i = 0; i < take; i++) {               // same CRC as everywhere else
      crc ^= (uint16_t)buf[i] << 8;
      for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    left -= take;
    wdgWebMs = millis();
  }
  uint8_t tail[2] = {0, 0};
  f.read(tail, 2);
  f.close();
  const uint16_t want = (uint16_t)((tail[0] << 8) | tail[1]);
  if (want != crc || want == 0xFFFF) {
    snprintf(err, errLen, "the image's CRC does not check out (%04X vs %04X)", crc, want);
    return false;
  }
  if (crcOut) *crcOut = want;
  return true;
}

bool modfwImageInfo(size_t* bytes, uint16_t* crc, bool* valid) {
  if (!sfFsReady || !FFat.exists(MODFW_FILE)) return false;
  File f = FFat.open(MODFW_FILE, "r");
  if (!f) return false;
  if (bytes) *bytes = f.size();
  f.close();
  char err[72];
  uint16_t c = 0;
  const bool ok = modfwValidateFile(MODFW_FILE, &c, err, sizeof(err));
  if (crc)   *crc = c;
  if (valid) *valid = ok;
  return true;
}

static bool loadImage(char* err, size_t errLen) {
  if (!gImage) gImage = (uint8_t*) psramAlloc("module firmware image", MODFW_IMAGE_BYTES);
  if (!gImage) { strlcpy(err, "out of memory for the image", errLen); return false; }
  if (!modfwValidateFile(MODFW_FILE, &gImageCrc, err, errLen)) return false;
  File f = FFat.open(MODFW_FILE, "r");
  if (!f) { strlcpy(err, "cannot open the image", errLen); return false; }
  size_t got = f.read(gImage, MODFW_IMAGE_BYTES);
  f.close();
  if (got != MODFW_IMAGE_BYTES) { strlcpy(err, "short read of the image", errLen); return false; }
  return true;
}

/* ---- the run -------------------------------------------------------------------------- */
static void sendPage(uint8_t to, int page) {
  uint8_t f[5 + MODFW_PAGE];
  f[0] = to; f[1] = 'P';
  f[2] = (uint8_t)(gImageCrc >> 8); f[3] = (uint8_t)(gImageCrc & 0xFF);
  f[4] = (uint8_t)page;
  memcpy(f + 5, gImage + (size_t)(page - MODFW_FIRST_PAGE) * MODFW_PAGE, MODFW_PAGE);
  bootFrame(f, sizeof(f));
}

static bool modfwWait(unsigned long ms) {        // cancel-aware, like the soak's
  const unsigned long end = millis() + ms;
  while ((long)(millis() - end) < 0) {
    if (gModFw.cancel) return false;
    vTaskDelay(pdMS_TO_TICKS(ms > 20 ? 10 : 2));
  }
  return !gModFw.cancel;
}

static void finish(uint8_t phase, const char* why) {
  if (why && why[0]) strlcpy(gModFw.err, why, sizeof(gModFw.err));
  gModFw.elapsedMs = millis() - gModFw.startedMs;
  gModFw.phase = phase;
  busRelease("modupdate");
  printf("[MODFW] %s after %lus (%d of %d module(s) updated)%s%s\n",
         phase == MODFW_DONE ? "finished" : (phase == MODFW_CANCELLED ? "cancelled" : "failed"),
         gModFw.elapsedMs / 1000UL, (int)gModFw.ok, (int)gModFw.targets,
         why && why[0] ? ": " : "", why ? why : "");
  traceEnd(gModFw.err[0] ? gModFw.err : "module firmware update finished");
  hTaskModFw = NULL;
  vTaskDelete(NULL);
}

static void taskModFw(void* pv) {
  busHold("modupdate", BUS_EXCLUSIVE, 180000UL);
  traceBegin("module firmware update");

  // 1. into the bootloader. The enter command must be checksummed: a plain one is ignored, so
  //    noise on the wire can never put a wall into update mode.
  gModFw.phase = MODFW_ENTERING;
  traceNote("# entering update mode");
  for (int i = 0; i < gModFw.targets; i++) {
    char cmd[12];
    snprintf(cmd, sizeof(cmd), "m%dU", (int)gModFw.list[i].id);
    rs485SendChecked(cmd);
    if (!modfwWait(MODFW_ENTER_WAIT_MS)) finish(MODFW_CANCELLED, "cancelled");
  }

  // 2. confirm each one is waiting in its bootloader.
  gModFw.phase = MODFW_CONFIRMING;
  int ready = 0;
  for (int i = 0; i < gModFw.targets; i++) {
    busHold("modupdate", BUS_EXCLUSIVE, 180000UL);
    ModFwTarget& t = gModFw.list[i];
    t.inBootloader = bootStatus(t.id);
    if (t.inBootloader) { ready++; traceNote("# module %d: bootloader v%d, reason %d", t.id, gBootSt.version, gBootSt.reason); }
    else                  traceNote("! module %d did not answer in its bootloader", t.id);
    if (gModFw.cancel) finish(MODFW_CANCELLED, "cancelled");
  }
  if (!ready) finish(MODFW_FAILED, "no module answered in its bootloader");

  // 3. the pages, then ask who is still missing what, and resend. Addressed to every module at
  //    once: one pass updates the whole wall in the time it takes one module.
  for (int pass = 1; pass <= MODFW_MAX_PASSES; pass++) {
    gModFw.pass = pass;
    gModFw.phase = MODFW_SENDING;
    /* What to send this pass: the union of what each module still misses, taken from ITS own
       last status. The first pass is everything; later passes are only the gaps, which is why
       a wall with one flaky module costs one slow pass rather than a whole repeat. */
    static bool need[MODFW_LAST_PAGE + 1];
    memset(need, 0, sizeof(need));
    int wanted = 0;
    for (int i = 0; i < gModFw.targets; i++) {
      const ModFwTarget& t = gModFw.list[i];
      if (!t.inBootloader || t.complete) continue;
      for (int p = MODFW_FIRST_PAGE; p <= MODFW_LAST_PAGE; p++) {
        if (need[p]) continue;
        if (pass == 1 || !(t.pages[p / 8] & (1 << (p % 8)))) need[p] = true;
      }
    }
    for (int p = MODFW_FIRST_PAGE; p <= MODFW_LAST_PAGE; p++) if (need[p]) wanted++;
    if (!wanted) break;
    gModFw.pagesThisPass = wanted;
    gModFw.pagesSent = 0;
    traceNote("# pass %d: %d page(s) to every module", pass, wanted);
    for (int p = MODFW_FIRST_PAGE; p <= MODFW_LAST_PAGE; p++) {
      if (!need[p]) continue;
      sendPage(MODFW_TO_ALL, p);
      gModFw.pagesSent = gModFw.pagesSent + 1;
      if ((gModFw.pagesSent % 16) == 0) busHold("modupdate", BUS_EXCLUSIVE, 180000UL);
      if (!modfwWait(MODFW_PAGE_GAP_MS)) finish(MODFW_CANCELLED, "cancelled");
    }

    // 4. who has everything?
    gModFw.phase = MODFW_VERIFYING;
    int incomplete = 0;
    for (int i = 0; i < gModFw.targets; i++) {
      ModFwTarget& t = gModFw.list[i];
      if (!t.inBootloader) continue;
      busHold("modupdate", BUS_EXCLUSIVE, 180000UL);
      if (!bootStatus(t.id)) { t.pagesLeft = MODFW_PAGES; memset(t.pages, 0, sizeof(t.pages)); incomplete++; continue; }
      int left = 0;
      if (gBootSt.imageCrc != gImageCrc) {                        // a different image: all of it again
        left = MODFW_PAGES;
        memset(t.pages, 0, sizeof(t.pages));
      } else {
        memcpy(t.pages, gBootSt.pages, sizeof(t.pages));          // what THIS module holds
        for (int p = MODFW_FIRST_PAGE; p <= MODFW_LAST_PAGE; p++)
          if (!(t.pages[p / 8] & (1 << (p % 8)))) left++;
      }
      t.pagesLeft = (uint16_t)left;
      t.complete  = (left == 0);
      if (left) incomplete++;
      traceNote("# module %d: %d page(s) still missing", t.id, left);
      if (gModFw.cancel) finish(MODFW_CANCELLED, "cancelled");
    }
    if (!incomplete) break;
    if (pass == MODFW_MAX_PASSES) finish(MODFW_FAILED, "some modules never took the whole image");
  }

  // 5. go.
  gModFw.phase = MODFW_GOING;
  traceNote("# go");
  { const uint8_t f[2] = { MODFW_TO_ALL, 'G' };
    bootFrame(f, 2); }
  if (!modfwWait(MODFW_GO_WAIT_MS)) finish(MODFW_CANCELLED, "cancelled");

  /* 6. Who came back?
     A module that started the image restarts into it and HOMES its reel, which takes several
     seconds during which it answers nothing. Asking once, straight after the go, calls a
     perfectly good update a failure -- measured: the first ask at 2.3 s found silence and the
     module answered v36 four seconds later. So give each one up to MODFW_SETTLE_MS, asking
     again as it comes back, and only then conclude. A module still answering its bootloader
     did not start the image, and that is a real failure, so check that first. */
  gModFw.phase = MODFW_CHECKING;
  if (!modfwWait(MODFW_GO_WAIT_MS)) finish(MODFW_CANCELLED, "cancelled");
  int good = 0;
  for (int i = 0; i < gModFw.targets; i++) {
    ModFwTarget& t = gModFw.list[i];
    if (!t.inBootloader) continue;
    const unsigned long deadline = millis() + MODFW_SETTLE_MS;
    bool answered = false, stuck = false;
    while (!answered && (long)(millis() - deadline) < 0 && !gModFw.cancel) {
      busHold("modupdate", BUS_EXCLUSIVE, 180000UL);
      char fw[8] = "";
      if (sfSendVersionAndWait(t.id, 900, fw, sizeof(fw), NULL, 0, NULL)) {
        const char* v = (fw[0] == 'v' || fw[0] == 'V') ? fw + 1 : fw;
        t.fwAfter = (uint8_t)atoi(v);
        answered = true;
        good++;
        traceNote("# module %d now runs v%d", t.id, t.fwAfter);
        break;
      }
      if (bootStatus(t.id, 1)) { stuck = true; break; }   // still in the bootloader: a real failure
      modfwWait(700);                                     // still homing, most likely
    }
    if (stuck)         traceNote("! module %d is still in its bootloader (flags %02X)", t.id, gBootSt.flags);
    else if (!answered) traceNote("! module %d did not answer within %d ms of the go", t.id, MODFW_SETTLE_MS);
  }
  gModFw.ok = good;
  finish(good == gModFw.targets ? MODFW_DONE : MODFW_FAILED,
         good == gModFw.targets ? "" : "not every module came back on the new firmware");
}

bool modfwStart(const int* ids, int n, char* err, size_t errLen) {
  if (gModFw.phase == MODFW_ENTERING || gModFw.phase == MODFW_CONFIRMING ||
      gModFw.phase == MODFW_SENDING  || gModFw.phase == MODFW_VERIFYING ||
      gModFw.phase == MODFW_GOING    || gModFw.phase == MODFW_CHECKING || hTaskModFw) {
    strlcpy(err, "an update is already running", errLen); return false;
  }
  if (restoreBusy() || gSoak.active || gProbe.active || gDiag.jobActive || gCalib.jobActive) {
    strlcpy(err, "the bus is busy with something else", errLen); return false;
  }
  modfwInit();
  if (!gModFw.list) { strlcpy(err, "out of memory", errLen); return false; }
  if (!loadImage(err, errLen)) return false;

  // Only modules that report the bootloader's firmware. Anything older would ignore the enter
  // command and never answer a status request, and there is no point sending it 15 KB.
  int count = 0;
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < sfModuleCount && count < MAX_MODULES; i++) {
    const SFModule& m = sfModules[i];
    if (!m.provisioned || m.id == 255) continue;
    const char* v = (m.fwVersion[0] == 'v' || m.fwVersion[0] == 'V') ? m.fwVersion + 1 : m.fwVersion;
    const int fw = atoi(v);
    bool wanted = (n == 0);
    for (int k = 0; k < n && !wanted; k++) if (ids[k] == (int)m.id) wanted = true;
    if (!wanted) continue;
    if (fw < MODULE_FW_BOOTLOADER) continue;
    ModFwTarget& t = gModFw.list[count++];
    t.id = m.id; t.fwBefore = (uint8_t)fw; t.fwAfter = 0;
    t.pagesLeft = MODFW_PAGES; t.inBootloader = false; t.complete = false;
    memset(t.pages, 0, sizeof(t.pages));
  }
  if (sfMutex) xSemaphoreGive(sfMutex);
  if (!count) {
    snprintf(err, errLen, "no module runs firmware v%d or later, so none has a bootloader",
             MODULE_FW_BOOTLOADER);
    return false;
  }
  gModFw.targets = count; gModFw.pass = 0; gModFw.pagesSent = 0; gModFw.pagesThisPass = 0;
  gModFw.ok = 0; gModFw.cancel = false; gModFw.err[0] = 0;
  gModFw.startedMs = millis(); gModFw.elapsedMs = 0;
  gModFw.phase = MODFW_ENTERING;
  if (xTaskCreatePinnedToCore(taskModFw, "ModFw", 6144, NULL, 1, &hTaskModFw, 1) != pdPASS) {
    hTaskModFw = NULL; gModFw.phase = MODFW_FAILED;
    strlcpy(err, "could not start the update task", errLen);
    return false;
  }
  printf("[MODFW] updating %d module(s) to the stored image (CRC %04X)\n", count, gImageCrc);
  return true;
}

void modfwCancel() {
  if (gModFw.phase != MODFW_IDLE && gModFw.phase < MODFW_DONE) gModFw.cancel = true;
}

static const char* phaseName() {
  switch (gModFw.phase) {
    case MODFW_ENTERING:   return "entering";
    case MODFW_CONFIRMING: return "confirming";
    case MODFW_SENDING:    return "sending";
    case MODFW_VERIFYING:  return "verifying";
    case MODFW_GOING:      return "going";
    case MODFW_CHECKING:   return "checking";
    case MODFW_DONE:       return "done";
    case MODFW_FAILED:     return "failed";
    case MODFW_CANCELLED:  return "cancelled";
    default:               return "idle";
  }
}

void modfwStatusJson(char* out, size_t outLen) {
  size_t bytes = 0; uint16_t crc = 0; bool valid = false;
  const bool have = modfwImageInfo(&bytes, &crc, &valid);
  int n = snprintf(out, outLen,
    "{\"ok\":true,\"state\":\"%s\",\"targets\":%d,\"pass\":%d,\"pagesSent\":%d,\"pagesThisPass\":%d,"
    "\"updated\":%d,\"elapsed\":%lu,\"error\":\"%s\","
    "\"image\":{\"stored\":%s,\"bytes\":%u,\"crc\":\"%04X\",\"valid\":%s,\"pages\":%d},\"modules\":[",
    phaseName(), (int)gModFw.targets, (int)gModFw.pass, (int)gModFw.pagesSent,
    (int)gModFw.pagesThisPass, (int)gModFw.ok,
    (unsigned long)(gModFw.phase >= MODFW_DONE ? gModFw.elapsedMs
                                               : (gModFw.startedMs ? millis() - gModFw.startedMs : 0)),
    gModFw.err, have ? "true" : "false", (unsigned)bytes, crc, valid ? "true" : "false",
    MODFW_PAGES);
  if (gModFw.list) {
    for (int i = 0; i < gModFw.targets && n > 0 && (size_t)n < outLen; i++) {
      const ModFwTarget& t = gModFw.list[i];
      n += snprintf(out + n, outLen - n,
        "%s{\"id\":%d,\"from\":%d,\"to\":%d,\"inBootloader\":%s,\"pagesLeft\":%d}",
        i ? "," : "", (int)t.id, (int)t.fwBefore, (int)t.fwAfter,
        t.inBootloader ? "true" : "false", (int)t.pagesLeft);
    }
  }
  if (n > 0 && (size_t)n < outLen) snprintf(out + n, outLen - n, "]}");
}
