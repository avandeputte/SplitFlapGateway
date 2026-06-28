// modules.h -- split-flap module registry, structs, and protocol API.

#ifndef SFGW_MODULES_H
#define SFGW_MODULES_H

#include "common.h"

/* ----------------------------------------------------------
   Module registry  (tracks known modules on the bus)
   See MAX_MODULES in common.h.
---------------------------------------------------------- */
struct SFModule {
  uint8_t  id;               // 0-254; 255 = slot empty
  char     serialNum[21];    // hex serial from advertisement (0-terminated)
  bool     provisioned;      // false = advertising (id==255 from adv)
  int      flapIndex;        // last known flap index (-1 = unknown)
  char     flapChar;         // last known displayed char (0 = unknown)
  char     fwVersion[8];     // firmware version string
  unsigned long lastSeen;    // millis() of last activity (resets on reboot)
  unsigned long lastSeenEpoch; // RTC wall-clock epoch of last activity (survives reboot)
  // Quiet Time: the display the host last requested while quiet (not yet shown).
  // pendChar holds a requested character, or pendIndex a requested flap index;
  // hasPend marks one is waiting. On Quiet Time -> off these drive the resync.
  char     pendChar;         // requested char while quiet (0 = none / index-based)
  int      pendIndex;        // requested flap index while quiet (-1 = none)
  bool     hasPend;          // a deferred display request is waiting
  // Stale-probe: before dropping a module that hasn't been seen in
  // MODULE_STALE_SECS, the gateway sends it a version query and waits. probeMs
  // is the millis() deadline by which it must reply; 0 = not being probed. A
  // reply (any frame -> sfTouch) clears it; if the deadline passes, it's dropped.
  unsigned long probeMs;     // probe-response deadline (0 = not probing)
  // Provisioning-confirmed: set true when this module produces a provisioning
  // ack (mXack). A legacy (v7) module has no serial and never acks provisioning,
  // so an acked module is, by definition, NOT legacy -- regardless of whether
  // its firmware version has been read back yet. Persisted.
  bool     acked;            // true once the module acknowledged provisioning
  // Deferred post-provision version query. A version request sent inline in the
  // ack handler fires before the module is ready to answer on its new ID (no
  // reply observed on the bus). Instead we set a deadline a moment in the future
  // and let taskRS485 issue the query once it passes. 0 = none pending.
  unsigned long verDueMs;    // millis() to issue the post-provision version query (0 = none)
  uint8_t       verTries;    // post-provision version-query attempts made so far
  // Duplicate-ID heuristic (runtime only, NOT persisted -- re-evaluated each boot).
  // Counts corrupt-SN version/all rejects for this ID within a rolling window;
  // dupSuspect latches when the count crosses the threshold and clears on the
  // next clean version/all parse for this ID.
  uint8_t       dupRejectCount;  // corrupt version/all rejects in the current window
  unsigned long dupRejectTs;     // millis() of the most recent corrupt reject (0 = none)
  bool          dupSuspect;      // latched: likely duplicate ID on the bus
};

// ------------------------------------------------------------------
// ------------------------------------------------------------------
// Sticky module persistence (FATFS file "/modules.dat")
// Persists known modules across reboots; prunes entries older than
// MODULE_STALE_SECS based on RTC wall-clock epoch. Only durable fields
// are stored (id, serial, provisioned, fwVersion, lastSeenEpoch) -- the
// transient display state is NOT persisted.
//
// Stored in the FATFS partition (already present in the default
// "16M Flash (3MB APP/9.9MB FATFS)" scheme) -- no custom partition needed.
// File format: a 4-byte magic+count header followed by N PersistedModule
// records written as raw bytes. (MODULES_FILE / MODULES_MAGIC are defined in
// common.h.)
// ------------------------------------------------------------------
struct PersistedModule {
  uint8_t       id;
  char          serialNum[21];
  bool          provisioned;
  bool          acked;          // provisioning-confirmed (never legacy)
  char          fwVersion[8];
  unsigned long lastSeenEpoch;
};

struct ModulesFileHeader {
  unsigned long magic;   // MODULES_MAGIC
  int           count;   // number of PersistedModule records following
};

// ---- Transient module request/response capture (single-slot mailboxes) ----
// The web task arms a wait-id, sends a bus frame, then polls a ready timestamp
// the RS485 parser sets when the matching reply lands. One slot each: the
// synchronous web server serves a single request at a time.
struct DumpCapture {                 // EEPROM 'd' dump / combined 'A' reply
  volatile int           waitId   = -1;            // module id being waited on
  char                   data[TX_MAX_BYTES] = "";  // raw dump after 'd:'
  volatile unsigned long ts       = 0;             // millis() when captured (0=none)
  // Fields ONLY an 'A' (combined) reply carries; -99 = not provided.
  volatile int           autoHome   = -99;         // 0/1, or -99 n/a
  volatile int           curIndex   = -99;         // flap index; -1 unknown, -2 released, -99 n/a
  volatile int           reportedId = -99;         // module's self-reported id, -99 n/a
  // Configurable flap set, appended to the 'A' reply by firmware v31+ ('N'
  // command). flapCount = -99 when the reply carried no flap-config tail (older
  // firmware); flapChars is the ordered character set, empty when not provided.
  volatile int           flapCount  = -99;         // active flap count (1..64), -99 n/a
  char                   flapChars[SF_MAX_FLAPS + 1] = "";  // ordered char set ('' n/a)
};

struct CalibState {                  // calibration: sync steps/rev + async job
  volatile int           waitId   = -1;            // id handleApiCalibrate waits on
  volatile int           steps    = 0;             // captured steps/rev
  volatile unsigned long ts       = 0;             // millis() when captured (0=none)
  volatile bool          jobActive   = false;      // async measure job in flight
  volatile int           jobId       = -1;         // module being calibrated
  volatile int           jobSteps    = -1;         // measured result (-1 until done)
  volatile unsigned long jobDeadline = 0;          // millis() timeout
};

struct DiagState {                   // self-diagnostics (module fw v26+/v28+)
  // 'Q' snapshot -- instant, captured synchronously (no motor)
  struct { volatile int waitId = -1; volatile unsigned long ts = 0;
           volatile int reset = 0, boot = 0, vcc = 0, ee = 0, cur = -1; } q;
  // 'T' Hall test -- motor ~2 revolutions
  struct { volatile int waitId = -1; volatile unsigned long ts = 0;
           volatile int code = -1, rising = 0, active = 0, falling = 0; } t;
  // 'M' mechanical -- motor ~6 revolutions (20-100s)
  struct { volatile int waitId = -1; volatile unsigned long ts = 0;
           volatile int code = -1, minVal = 0, maxVal = 0, spread = 0,
                        gateActive = 0, gateSpan = 0, magWidth = 0;
           char revs[256] = {0}; } m;              // raw "r1,r2,..." per-rev list
  // Shared async motor-test job ('T' then 'M'); Q is synchronous.
  volatile bool          jobActive   = false;
  volatile int           jobId       = -1;
  volatile char          jobKind     = 0;          // 'T' or 'M'
  volatile unsigned long jobDeadline = 0;
};

// ---- owned globals (defined in globals.cpp) ----
extern SFModule* sfModules;
extern SemaphoreHandle_t sfMutex;
extern StaticSemaphore_t sfMutexBuf;
extern int sfModuleCount;
extern DumpCapture gDump;
extern CalibState  gCalib;
extern DiagState   gDiag;
extern volatile bool sfModulesDirty;
extern volatile unsigned long sfModulesDirtyMs;
extern bool sfFsReady;

SFModule* sfFindById(uint8_t id);
void sfFsInit();
void sfModulesSave();
void sfModulesLoad();
void sfModulesClear();
void sfModulesPruneStale();
void sfTrackChar(int addr, char c);
void sfSendChar(int addr, char c);
void sfSendIndex(int addr, int idx);
void sfHomeOffset(int addr, int steps);
void sfSetTotalSteps(int addr, int steps);
void sfNudge(int addr, int steps);
void sfGoto(int addr, int step);
void sfWritePos(int addr, int idx, int pos);
void sfAutoHome(int addr, int enable);
void sfErase(int addr);
void sfFactoryReset(int addr);
// Configure a module's flap set ('N' command, firmware v31+). count and chars
// are INDEPENDENT and optional: pass count<1 to leave the count unchanged, and
// chars=NULL/"" to leave the character set unchanged. addr<0 broadcasts (m*N).
void sfSetFlapConfig(int addr, int count, const char* chars);
// Same, addressed by serial number (mXN<sn>:<count>:<chars>).
void sfSetFlapConfigBySN(const char* sn, int count, const char* chars);
void sfDumpBySN(const char* sn);
void sfFactoryResetBySN(const char* sn);
void sfHome(int addr);
void sfCalibrate(int addr);
void sfQueryVersion(int addr);
bool sfSendAndCaptureDump(int id, const char* frame, unsigned long timeoutMs, char* out, size_t outLen);
bool sfSendAndCaptureQ(int id, unsigned long timeoutMs);
bool sfSendVersionAndWait(int id, unsigned long timeoutMs, char* fwOut, size_t fwLen, char* snOut, size_t snLen, unsigned long* lastSeenOut);
void sfDeprovision(int addr);
void sfProvision(const char* sn, int newId);
void sfHomeBySN(const char* sn);
void sfSendText(int startAddr, const char* text, bool blankUnused);
void sfSetQuietTime(bool on);
void sfParseResponse(const uint8_t* data, size_t len);

#endif // SFGW_MODULES_H
