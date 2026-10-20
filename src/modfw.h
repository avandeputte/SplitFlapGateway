// modfw.h -- updating the MODULES' firmware over the bus. (v4.0.0)
//
// From module firmware v35 every module carries a 1 KB bootloader that can replace its own
// firmware with an image sent over RS-485. A whole wall updates in about a minute without
// anyone touching a module, and without a UPDI programmer. The protocol is specified in the
// module firmware's BUS-UPDATE.md; `tools/busflash.py` there is the reference sender, and this
// is the same procedure with the same timing, run from the gateway.
//
// Shape of a run:
//   1. check every target reports firmware v35 or later -- older modules have no bootloader
//      and would ignore the whole exercise
//   2. send the enter-update command, which MUST be checksummed: a plain one is ignored, so
//      line noise can never take a wall off the air
//   3. ask each target for its bootloader status, to confirm it is waiting
//   4. send the image's 240 pages to every module at once (address FF)
//   5. ask each target which pages it actually wrote and read back, and resend what is missing
//   6. send go; a module with the complete image checks it and restarts into it
//   7. confirm the new firmware version
//
// The bootloader never starts a partial image, never writes EEPROM (so identity and
// calibration survive), and a module that loses power mid-update simply waits for the image.
// The gateway holds the bus EXCLUSIVELY throughout: this is the one operation where another
// frame on the wire can cost someone a wall of modules.

#ifndef SFGW_MODFW_H
#define SFGW_MODFW_H

#include "common.h"

#define MODFW_PAGE            64
#define MODFW_FIRST_PAGE      16
#define MODFW_LAST_PAGE       255
#define MODFW_PAGES           (MODFW_LAST_PAGE + 1 - MODFW_FIRST_PAGE)   // 240
#define MODFW_IMAGE_BYTES     (MODFW_PAGES * MODFW_PAGE)                 // 15,360
#define MODFW_FILE            "/modfw.bin"
#define MODFW_TMP             "/modfw.tmp"
#define MODULE_FW_BOOTLOADER  35       // first module firmware that carries the bootloader
#define MODFW_TO_ALL          0xFF

// Quiet after a frame's last byte, from BUS-UPDATE.md's timing table.
#define MODFW_PAGE_GAP_MS     20       // the module erases, writes and reads back a page
#define MODFW_STATUS_WAIT_MS  250      // its status reply starts ~3 ms later and takes ~90 ms
#define MODFW_GO_WAIT_MS      500      // two image checks and a restart
#define MODFW_ENTER_WAIT_MS   200      // the restart into the bootloader
#define MODFW_MAX_PASSES      5        // resend rounds before giving up
/* How long to let a module come back after the go. It restarts, re-checks the whole image,
   waits out the staggered start-up that keeps a wall from drawing its inrush at once, and
   homes its reel -- and it answers nothing through any of that. Measured on module 27 of a
   real wall: 28.7 s from the go to its first answered version query. 45 s leaves room for a
   higher ID, whose start-up stagger is longer, without making a stuck module wait long. */
#define MODFW_SETTLE_MS       45000

enum ModFwPhase : uint8_t {
  MODFW_IDLE = 0, MODFW_ENTERING, MODFW_CONFIRMING, MODFW_SENDING, MODFW_VERIFYING,
  MODFW_GOING, MODFW_CHECKING, MODFW_DONE, MODFW_FAILED, MODFW_CANCELLED
};

struct ModFwTarget {
  uint8_t id;
  uint8_t fwBefore;        // what it ran when the run started
  uint8_t fwAfter;         // what it reports afterwards (0 = did not answer)
  uint16_t pagesLeft;      // pages it still needs, as of the last status
  bool     inBootloader;   // it answered a status request
  bool     complete;       // it had every page before the go
  uint8_t  pages[32];      // which pages IT has, from its own last status: bit n%8 of byte n/8
};

struct ModFwState {
  volatile uint8_t  phase    = MODFW_IDLE;
  volatile bool     cancel   = false;
  volatile int      targets  = 0;
  volatile int      pass     = 0;        // resend round, 1-based
  volatile int      pagesSent= 0;        // in the current pass
  volatile int      pagesThisPass = 0;
  volatile int      ok       = 0;        // modules now running the new firmware
  volatile unsigned long startedMs = 0, elapsedMs = 0;
  char              err[72]  = "";
  ModFwTarget*      list     = NULL;     // PSRAM, MAX_MODULES entries
};
extern ModFwState gModFw;

void modfwInit();                                   // once, from setup()
// What is stored: size, the image's own CRC, and whether it checks out.
bool modfwImageInfo(size_t* bytes, uint16_t* crc, bool* valid);
// Validate a candidate image file (size and CRC). `err` gets a short reason on failure.
bool modfwValidateFile(const char* path, uint16_t* crcOut, char* err, size_t errLen);
// Start a run over `ids` (or every eligible module when n == 0). False, with a reason, if the
// image is missing, nothing is eligible, or the bus is busy with something else.
bool modfwStart(const int* ids, int n, char* err, size_t errLen);
void modfwCancel();
void modfwStatusJson(char* out, size_t outLen);
// One module's bootloader status, for the dashboard's "is this module ready" check. Sends an
// S frame and waits; false when nothing answers (which is what a module without the
// bootloader does).
bool modfwProbe(int id, uint8_t* version, uint8_t* reason, uint8_t* flags,
                uint16_t* imageCrc, int* pagesHeld);
// Receive hook: a bootloader frame ('#...') arrived. Called from the RS-485 task.
void modfwNoteReply(const uint8_t* data, size_t len);

#endif // SFGW_MODFW_H
