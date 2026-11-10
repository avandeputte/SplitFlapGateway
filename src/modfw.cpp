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
  uint8_t  board;          // from a version-2 status frame; 0 = the bootloader did not say
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
  // '#' + hex + '\n'. A status frame is 41 bytes from bootloader version 1, and 42 from
  // version 2 -- the v5 board's bootloader, which appends the board it is running on. Reject
  // the frame on its length alone and a v5 module looks exactly like a module that never
  // answered, which is how this first showed up: the enter command went out, the module
  // replied, and the update failed with "no module answered in its bootloader".
  if (len < 3 || data[0] != '#') return;
  size_t hexLen = len - 1;
  while (hexLen && (data[hexLen] == '\n' || data[hexLen] == '\r')) hexLen--;
  if (hexLen % 2) return;
  const size_t n = hexLen / 2;
  if (n != 41 && n != 42) return;
  uint8_t d[42];
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
  gBootSt.board    = (n == 42) ? d[39] : 0;   // only the v5 bootloader says, and it says 5
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
      wdgWebFeed();
      vTaskDelay(pdMS_TO_TICKS(10));
      if (gBootSt.ts) { gBootSt.waitId = -1; return true; }
    }
  }
  gBootSt.waitId = -1;
  return false;
}

bool modfwProbe(int id, uint8_t* version, uint8_t* reason, uint8_t* flags,
                uint16_t* imageCrc, int* pagesHeld, uint8_t* board) {
  if (id < 0 || id > 254) return false;
  if (!bootStatus((uint8_t)id, 2)) return false;
  if (version)  *version  = gBootSt.version;
  if (reason)   *reason   = gBootSt.reason;
  if (flags)    *flags    = gBootSt.flags;
  if (imageCrc) *imageCrc = gBootSt.imageCrc;
  if (board)    *board    = gBootSt.board;
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
    wdgWebFeed();
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

int modfwImageVersion(const char* path) {
  if (!sfFsReady || !FFat.exists(path)) return 0;
  File f = FFat.open(path, "r");
  if (!f) return 0;
  static uint8_t buf[512];
  int found = 0, version = 0;
  int digits = 0, value = 0;          // a run of digits, carried across chunk boundaries
  bool prevDigit = false;
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    for (int i = 0; i < n; i++) {
      const uint8_t c = buf[i];
      if (c >= '0' && c <= '9') {
        if (digits < 4) { value = value * 10 + (c - '0'); digits++; }
        else digits = 5;              // too long to be a version
        prevDigit = true;
        continue;
      }
      if (prevDigit && c == 0 && digits >= 2 && digits <= 3 && value >= MODULE_FW_BOOTLOADER && value <= 255) {
        if (!found || value != version) { found++; version = value; }
      }
      digits = 0; value = 0; prevDigit = false;
    }
    wdgWebFeed();
  }
  f.close();
  return (found == 1) ? version : 0;  // more than one candidate: say nothing rather than guess
}

// Cached, because a status poll every two seconds should not re-read and re-CRC 15 KB of flash
// while an update's timing is being measured in milliseconds.
static struct { bool known = false, exists = false, valid = false;
                size_t bytes = 0; uint16_t crc = 0; int version = 0; int board = 0; } gImg;

// The board the stored image is for, from the tag at MODFW_BOARD_AT. An image from before the
// tag existed reads 0xFF there and is a v4 image by construction: the v5 board's first firmware
// is v36, which is also the first to carry the tag.
static int imageBoard(const char* path) {
  if (!sfFsReady || !FFat.exists(path)) return 0;
  File f = FFat.open(path, "r");
  if (!f) return 0;
  int b = 0;
  if (f.size() == MODFW_IMAGE_BYTES && f.seek(MODFW_BOARD_AT)) {
    const int c = f.read();
    b = (c >= 1 && c <= 9) ? c : MODFW_BOARD_DEFAULT;     // 0xFF (untagged) -> v4
  }
  f.close();
  return b;
}

void modfwImageForget() { gImg.known = false; }

bool modfwImageInfo(size_t* bytes, uint16_t* crc, bool* valid, int* version, int* board) {
  if (!gImg.known) {
    gImg.known = true;
    gImg.exists = (sfFsReady && FFat.exists(MODFW_FILE));
    gImg.bytes = 0; gImg.crc = 0; gImg.valid = false; gImg.version = 0; gImg.board = 0;
    if (gImg.exists) {
      File f = FFat.open(MODFW_FILE, "r");
      if (f) { gImg.bytes = f.size(); f.close(); }
      char err[72];
      gImg.valid = modfwValidateFile(MODFW_FILE, &gImg.crc, err, sizeof(err));
      if (gImg.valid) {
        gImg.version = modfwImageVersion(MODFW_FILE);
        gImg.board   = imageBoard(MODFW_FILE);
      }
    }
  }
  if (!gImg.exists) return false;
  if (bytes)   *bytes   = gImg.bytes;
  if (crc)     *crc     = gImg.crc;
  if (valid)   *valid   = gImg.valid;
  if (version) *version = gImg.version;
  if (board)   *board   = gImg.board;
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
    if (t.inBootloader) {
      // The bootloader's own account of which board it is on (version 2 and later; an older
      // one does not say, and is a v4). This is the authority -- the registry's answer came
      // from a version reply that may be hours old -- so a module that contradicts the image
      // here is dropped before a single page goes to it.
      const uint8_t blBoard = gBootSt.board ? gBootSt.board : (uint8_t)MODFW_BOARD_DEFAULT;
      if (gModFw.imageBoard && blBoard != gModFw.imageBoard) {
        t.inBootloader = false;
        t.board = blBoard;
        traceNote("! module %d is a board v%d and this image is for v%d -- left alone",
                  t.id, blBoard, gModFw.imageBoard);
        continue;
      }
      t.board = blBoard;
      ready++;
      traceNote("# module %d: bootloader v%d, board v%d, reason %d",
                t.id, gBootSt.version, blBoard, gBootSt.reason);
    }
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

bool modfwRunning() {
  return hTaskModFw ||
         gModFw.phase == MODFW_ENTERING || gModFw.phase == MODFW_CONFIRMING ||
         gModFw.phase == MODFW_SENDING  || gModFw.phase == MODFW_VERIFYING ||
         gModFw.phase == MODFW_GOING    || gModFw.phase == MODFW_CHECKING;
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

  // Which board this image is for. An image only runs on its own board: on the wrong one the
  // firmware sees the strap, leaves the bus pins alone and goes back to its bootloader, where it
  // waits for a right image. Recoverable over the bus, but it takes the module off the wall for
  // the length of another update, so the gateway does not do it on purpose.
  const int imgBoard = imageBoard(MODFW_FILE);

  // Only modules that report the bootloader's firmware. Anything older would ignore the enter
  // command and never answer a status request, and there is no point sending it 15 KB.
  int count = 0, wrongBoard = 0, firstWrongId = -1, firstWrongBoard = 0;
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
    // What board this module is. A module that REPORTS one is taken at its word; one that does
    // not is a v4, and that is a construction rather than a guess: every image a v5 board can
    // run says so, because the v5 board's first firmware is the one that added the field. The
    // failure this protects is asymmetric -- treating a silent module as a v4 costs at worst a
    // skipped update, visible in the status, while treating it as a v5 sends an image its board
    // cannot run and drops it into its bootloader until the right one arrives.
    const int board = m.board ? m.board : MODFW_BOARD_DEFAULT;
    if (imgBoard && board != imgBoard) {
      if (firstWrongId < 0) { firstWrongId = m.id; firstWrongBoard = board; }
      wrongBoard++;
      continue;
    }
    ModFwTarget& t = gModFw.list[count++];
    t.id = m.id; t.board = (uint8_t)board; t.fwBefore = (uint8_t)fw; t.fwAfter = 0;
    t.pagesLeft = MODFW_PAGES; t.inBootloader = false; t.complete = false;
    memset(t.pages, 0, sizeof(t.pages));
  }
  if (sfMutex) xSemaphoreGive(sfMutex);
  if (!count && wrongBoard) {
    snprintf(err, errLen, "this image is for board v%d; %s v%d. Upload the v%d image",
             imgBoard, wrongBoard == 1 ? "that module is a" : "those modules are a",
             firstWrongBoard, firstWrongBoard);
    return false;
  }
  if (!count) {
    snprintf(err, errLen, "no module runs firmware v%d or later, so none has a bootloader",
             MODULE_FW_BOOTLOADER);
    return false;
  }
  if (wrongBoard)
    printf("[MODFW] skipping %d module(s) built on another board (image is for v%d)\n",
           wrongBoard, imgBoard);
  gModFw.skippedBoard = (uint8_t)(wrongBoard > 255 ? 255 : wrongBoard);
  gModFw.imageBoard   = (uint8_t)imgBoard;
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

void modfwStatusEmit(void (*emit)(const char* chunk)) {
  size_t bytes = 0; uint16_t crc = 0; bool valid = false; int version = 0, board = 0;
  const bool have = modfwImageInfo(&bytes, &crc, &valid, &version, &board);
  char out[420];
  int n = snprintf(out, sizeof(out),
    "{\"ok\":true,\"state\":\"%s\",\"targets\":%d,\"pass\":%d,\"pagesSent\":%d,\"pagesThisPass\":%d,"
    "\"updated\":%d,\"elapsed\":%lu,\"error\":\"%s\",\"skippedBoard\":%u,"
    "\"image\":{\"stored\":%s,\"bytes\":%u,\"crc\":\"%04X\",\"valid\":%s,\"version\":%d,\"board\":%d,"
    "\"pages\":%d},\"modules\":[",
    phaseName(), (int)gModFw.targets, (int)gModFw.pass, (int)gModFw.pagesSent,
    (int)gModFw.pagesThisPass, (int)gModFw.ok,
    (unsigned long)(gModFw.phase >= MODFW_DONE ? gModFw.elapsedMs
                                               : (gModFw.startedMs ? millis() - gModFw.startedMs : 0)),
    gModFw.err, (unsigned)gModFw.skippedBoard, have ? "true" : "false", (unsigned)bytes, crc,
    valid ? "true" : "false", version, board, MODFW_PAGES);
  (void)n;
  emit(out);
  if (gModFw.list) {
    for (int i = 0; i < gModFw.targets; i++) {
      const ModFwTarget& t = gModFw.list[i];
      char row[140];
      snprintf(row, sizeof(row),
        "%s{\"id\":%d,\"board\":%d,\"from\":%d,\"to\":%d,\"inBootloader\":%s,\"pagesLeft\":%d}",
        i ? "," : "", (int)t.id, (int)t.board, (int)t.fwBefore, (int)t.fwAfter,
        t.inBootloader ? "true" : "false", (int)t.pagesLeft);
      emit(row);
    }
  }
  emit("]}");
}
