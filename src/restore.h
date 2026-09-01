// restore.h -- restore-on-boot: replay a stored calibration backup to every module.
//
// A backup file (the JSON the Settings tab's "Backup Calibration" produces) can be
// uploaded to the gateway and kept in its FATFS partition. When the setting is on,
// every boot replays it: each module's EEPROM calibration is written by serial
// number, its id is reassigned from the backup, the result is read back and
// compared, and finally the whole wall is homed. While that runs the gateway
// refuses every other bus command (REST answers 503, MQTT commands are dropped),
// so nothing can interleave with the restore. It always finishes on its own: a
// module that never answers is counted as missing, not waited for forever.
//
// The same run can be started by hand (POST /api/restore/run) and cancelled
// (POST /api/restore/cancel); a cancel stops after the current module.

#ifndef SFGW_RESTORE_H
#define SFGW_RESTORE_H

#include "common.h"

enum RestoreState : uint8_t {
  RESTORE_IDLE = 0,     // nothing running, nothing has run since boot
  RESTORE_WAITING,      // armed: counting down the post-boot delay (bus already locked)
  RESTORE_WRITING,      // sending mXW (+ mXI) to each module, paced by RESTORE_STEP_MS
  RESTORE_VERIFYING,    // reading each module's EEPROM back and comparing
  RESTORE_HOMING,       // broadcasting m*h so every reel picks up its new calibration
  RESTORE_DONE,         // finished (see the counters for how it went)
  RESTORE_FAILED,       // could not start: no file, unreadable file, no filesystem
  RESTORE_CANCELLED     // stopped by POST /api/restore/cancel
};

// Live progress of the current (or last) run. Written by taskRestore, read by the
// REST handlers; the scalar fields are volatile because they cross tasks.
struct RestoreProgress {
  volatile uint8_t       state     = RESTORE_IDLE;
  volatile int           total     = 0;    // entries in the backup being replayed
  volatile int           done      = 0;    // entries completed in the CURRENT phase
  volatile int           okCount   = 0;    // verified: EEPROM matches the backup
  volatile int           mismatch  = 0;    // answered, but the EEPROM differs after the repair round
  volatile int           missing   = 0;    // never answered the verify query
  volatile int           skipped   = 0;    // backup entry was unusable (no serial, bad dump)
  volatile unsigned long startAtMs = 0;    // millis() the write phase may begin (boot delay)
  volatile unsigned long startedMs = 0;    // millis() the run actually started
  volatile unsigned long finishedMs= 0;    // millis() it ended (0 while running)
  char                   trigger[8] = "";  // "boot" or "manual"
  char                   lastError[64] = "";
};

// What is stored on the flash right now (refreshed at boot and after an upload/delete).
struct RestoreFileInfo {
  bool   exists  = false;
  size_t bytes   = 0;
  int    modules = 0;          // usable entries (valid serial + parseable dump)
  int    entries = 0;          // entries in the file, usable or not
  int    version = 0;          // the backup's "version" field
  char   created[32] = "";     // the backup's "created" field, verbatim
};

extern RestoreProgress gRestore;
extern RestoreFileInfo gRestoreFile;

// True while the gateway must refuse other bus commands.
static inline bool restoreBusy() {
  uint8_t s = gRestore.state;
  return s == RESTORE_WAITING || s == RESTORE_WRITING ||
         s == RESTORE_VERIFYING || s == RESTORE_HOMING;
}
const char* restoreStateName();

// Boot: read the stored file's summary and, if the setting is on and the file is
// usable, lock the bus at once and schedule the run for cfg.restoreDelaySec after boot.
// Call once from setup(), after sfFsInit() and rs485Begin().
void restoreInit();
// Re-read the stored file and refresh gRestoreFile. Returns gRestoreFile.exists.
bool restoreRefreshFileInfo();
// Validate a candidate backup at `path` (the upload temp file). Fills `info` and, on
// failure, a short reason into `err`. Returns true if the file can be restored from.
bool restoreValidateFile(const char* path, RestoreFileInfo* info, char* err, size_t errLen);
// Start a run `delayMs` from now. Returns false (and sets `err`) if one is already
// running, there is no usable file, or the task could not be created.
bool restoreStart(unsigned long delayMs, const char* trigger, char* err, size_t errLen);
// Ask a running restore to stop after the current step. Returns false if none is running.
bool restoreCancel();
// Delete the stored backup. Returns false if a run is in progress.
bool restoreDeleteFile();
// JSON for the REST API. full=true: the /api/restore body (settings + file + progress);
// full=false: the compact object embedded in /api/status.
void restoreStatusJson(char* out, size_t outLen, bool full);

#endif // SFGW_RESTORE_H
