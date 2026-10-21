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
  uint8_t       probeTries;  // stale-probe attempts made so far (drop at MODULE_PROBE_MAX_TRIES). Reset on any reply (sfTouch).
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
  // The module's own reel -- what it can actually show. Read from the v31+ tail of an 'A'
  // reply, cached here and PERSISTED, because asking the whole wall costs ~90 s of bus time
  // (see the flap-set note in common.h). GET /api/capabilities is built from these.
  //   flapCount == -99  the set is NOT KNOWN. Either the module has never been asked, or it is
  //                     older than v31 and cannot answer -- capabilities tells those apart by
  //                     fwVersion, and reports the old ones as "assumed" rather than guessing
  //                     silently.
  int16_t       flapCount;                    // active flaps (1..64), -99 = not known
  char          flapChars[SF_MAX_FLAPS + 1];  // the ordered set, "" = not known
  // Background fill: the gateway asks ONE unknown module at a time, slowly, so the trickle
  // never competes with the display for the bus. Runtime only -- a set once learned is
  // persisted, so this does nothing at all on a settled wall.
  unsigned long flapDueMs;   // millis() to (re)ask this module (0 = not scheduled)
  uint8_t       flapTries;   // attempts so far; give up at FLAPSET_MAX_TRIES
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
  // The module's reel. Persisted for one reason: re-reading it costs ~90 s of bus time across a
  // 45-module wall, and it changes only when someone deliberately changes it ('N').
  int16_t       flapCount;                    // -99 = not known
  char          flapChars[SF_MAX_FLAPS + 1];  // "" = not known
};

struct ModulesFileHeader {
  unsigned long magic;   // MODULES_MAGIC
  int           count;   // number of PersistedModule records following
};

// ---- Transient module request/response capture (single-slot mailboxes) ----
// The web task arms a wait-id, sends a bus frame, then polls a ready timestamp
// the RS485 parser sets when the matching reply lands. One slot each: the
// synchronous web server serves a single request at a time.
// Restore-on-boot (restore.cpp) reads modules back BY SERIAL and cannot know which id
// the reply will carry, so it arms waitId with this wildcard; the parser then records
// which module actually answered in gotId / gotSN. Safe because every other user of
// the slot (the REST dump handlers) is refused while a restore runs.
#define SF_WAIT_ANY (-2)
struct DumpCapture {                 // EEPROM 'd' dump / combined 'A' reply
  volatile int           waitId   = -1;            // module id being waited on, or SF_WAIT_ANY
  volatile int           gotId    = -1;            // id the captured reply came from
  char                   gotSN[21] = "";           // serial an 'A' reply named ("" for a 'd' reply)
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

/* The v33 module firmware appends three counters to the Q snapshot:
     m<id>Q:<resetCause>:<bootCount>:<vcc>:<eepromOk>:<curIndex>:<rxOverflows>:<bcastSent>:<bcastLate>
   They are the module's own account of what the bus did to it: how often its receive ring
   overflowed (which, per the firmware authors, only happens while it is busy), how many
   staggered broadcast replies it actually sent, and how many went out late. Comparing
   bcastSent against what the gateway heard separates "the reply was lost on the wire" from
   "the module never sent one". -1 = not reported, which is every firmware before v33. */
struct DiagState {                   // self-diagnostics (module fw v26+/v28+)
  // 'Q' snapshot -- instant, captured synchronously (no motor)
  struct { volatile int waitId = -1; volatile unsigned long ts = 0;
           volatile int reset = 0, boot = 0, vcc = 0, ee = 0, cur = -1;
           volatile int rxOv = -1, bcSent = -1, bcLate = -1, crcErr = -1; } q;
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

/* Bus probe (v3.14) -- "does this module answer SHORT replies but not LONG ones?"
 *
 * The question a silent module cannot answer for you. Each step asks for a reply of a
 * different size -- 'v' (~30 B), 'Q' (~40 B), 'd' (offset/steps/map) and 'A' (everything,
 * and the only reply that carries the flap set, so the longest a module ever sends). Each is
 * repeated a few times and the reply SIZE is recorded.
 *
 * Reading the result:
 *   all four clean                  the module and the bus are fine
 *   short pass, 'd'/'A' fail        reply-length dependent -- a long frame is being corrupted
 *                                   or overrun, not a dead module. Check bus termination, and
 *                                   the [RX] discarded counter on the Status tab.
 *   nothing answers                 the module really is silent: power, wiring, or wrong ID
 */
#define PROBE_STEPS 4
struct ProbeStep {
  char cmd;                  // 'v' 'Q' 'd' 'A'
  volatile int tries;
  volatile int ok;
  volatile int bytes;        // longest reply seen for this step (0 = never answered)
};
struct ProbeState {
  volatile bool          active   = false;
  volatile bool          done     = false;   // a finished run is waiting to be read
  volatile int           id       = -1;
  volatile int           rounds   = 0;
  volatile int           step     = 0;       // steps completed, for progress
  volatile unsigned long startedMs = 0;
  volatile unsigned long elapsedMs = 0;
  volatile unsigned long dropsBefore = 0;    // rxDropped when the run started
  volatile unsigned long dropsDuring = 0;
  ProbeStep              s[PROBE_STEPS];
};

/* Bus soak test (v3.15) -- the probe, for the whole wall, for a long time, silently.
 *
 * "A couple of modules play up now and then" is the report that a single probe cannot catch.
 * The soak walks every provisioned module, pass after pass, for the number of minutes you
 * ask, and each pass exercises the bus a different way:
 *   'V'  broadcast roll call (m*v<lo>-<hi>): every module answers in its timed slot; a reply
 *        that is missing, arrives damaged, or arrives TWICE for one id is scored
 *   'v' 'Q' 'd' 'A'  one module at a time, replies of increasing size (the probe's four)
 *   'B'  broadcast combined dump (m*A<lo>-<hi>): the longest replies, back to back
 *   'W'  write round trip: the module's home offset, then one calibrated map entry, is
 *        nudged by one half-step, read back, PUT BACK, and read back again. Both writes
 *        are a bare EEPROM store on the module -- the reel is not told to go anywhere.
 *        A module whose original value cannot be confirmed restored is flagged (altered)
 *        and its original values are kept, so it can be put right by hand.
 * There is deliberately NO "fill the whole map" leg. One was tried (2026-10-17): on module
 * firmware v32 a module whose 64 map entries are all set can no longer send its 'd' or 'A'
 * reply at all -- it answers 'v' and 'Q' but the dump never comes -- and stays that way
 * until the map is erased. That is the field report "module stopped answering mXA, erasing
 * EEPROM fixed it", reproduced on eight modules at once. The soak detects that signature
 * from its ordinary passes instead (short replies 100%, long replies 0%), and the fix is
 * the module firmware's to make. Nothing the soak sends moves a flap. It holds the bus (bus.h) so content cannot interleave, and
 * it stops on its own at the deadline or on cancel -- never in the middle of a write round
 * trip, which always runs to its restore step.
 *
 * Reading the result: a module that fails only 'd', 'A' and 'B' is losing LONG frames (the
 * bus, not the module); one that fails everything, or intermittently at every size, is a
 * power or contact problem; misses only on 'V'/'B' are replies colliding (timing, or two
 * modules on one id); a wall where every module is clean for 30 minutes is a wall you can
 * trust.
 */
#define SOAK_STEPS 7                 // order of SoakRow.tries/ok: v Q d A W V B
struct SoakRow {
  uint8_t  id;
  uint8_t  tries[SOAK_STEPS];
  uint8_t  ok[SOAK_STEPS];
  uint16_t maxLen;           // longest reply seen from this module
  uint8_t  streak;           // consecutive failures right now (worst-case flicker detector)
  uint8_t  worstStreak;
  uint8_t  altered;          // bit0 home offset, bit1 map entry: written and NOT confirmed put back
  uint8_t  mangled;          // broadcast replies from this id that arrived but did not parse whole
  uint8_t  dup;              // broadcast windows in which this id answered more than once
  uint8_t  wWhy;             // why the write leg failed, OR of: 1 write did not read back, 2 restore
  uint8_t  late;             // long reads that only answered when asked a second time
  uint8_t  fw;               // the module's firmware version as a number (0 = unknown)
  uint16_t drops;            // frames the gateway discarded while reading THIS module
  int8_t   origIdx;          // the map entry the write leg uses (-1 none) ...
  uint16_t origHo, origPos;  // ... and the values it found there before the first write
};
struct SoakState {
  volatile bool          active    = false;
  volatile bool          done      = false;
  volatile bool          cancel    = false;
  volatile int           modules   = 0;      // rows in use
  volatile int           passes    = 0;
  volatile int           queries   = 0;
  volatile int           failures  = 0;
  volatile unsigned long startedMs = 0;
  volatile unsigned long deadlineMs= 0;
  volatile unsigned long elapsedMs = 0;
  volatile unsigned long dropsBefore = 0, dropsDuring = 0, rejectsBefore = 0, rejectsDuring = 0;
  volatile int           writes    = 0;      // write round trips run
  volatile int           restoreFail = 0;    // values that could not be confirmed put back
  volatile int           dups      = 0;      // broadcast replies beyond the first from one id
  volatile int           stray     = 0;      // broadcast replies from ids not in the table
  volatile int           idLo      = 0, idHi = 0;   // broadcast range (provisioned ids)
  // Safety snapshot: every module's calibration, read and stored before the test writes
  // anything, so a run that cannot put a value back can be undone (SOAK_SNAPSHOT_FILE).
  volatile bool          snapRunning = false;
  volatile bool          snapOkFlag  = false;   // a usable snapshot exists for THIS run
  volatile int           snapDone  = 0, snapTotal = 0, snapOk = 0;
  char                   snapErr[48] = "";
  SoakRow*               rows      = NULL;   // MAX_MODULES, in PSRAM, allocated on first start
};
extern SoakState gSoak;
// Called by the reply parser (taskRS485) for every 'v' and 'A' reply; counted only while
// the soak has a broadcast window open. `whole` = the reply parsed completely.
void sfSoakNoteReply(uint8_t id, char cmd, bool whole);
// Start a soak of `minutes` (1-60) over every provisioned module. False if something else
// owns the bus (probe, self-test, calibration) or the task will not start.
bool sfSoakStart(int minutes);
// Minimum free stack the soak/probe tasks have ever had, in bytes; 0 when not running.
unsigned sfSoakStackFree();
unsigned sfProbeStackFree();
void sfSoakCancel();

// ---- owned globals (defined in globals.cpp) ----
extern ProbeState gProbe;
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
// True if `sn` has the shape of a module serial (4-20 alphanumerics).
bool sfIsValidSN(const char* sn);
// Look a module up by serial (takes sfMutex). Fills the non-NULL outs; false if unknown.
bool sfLookupBySN(const char* sn, uint8_t* idOut, char* fwOut, size_t fwLen);
// Build the by-serial EEPROM restore frame mXW<sn>:<ho>:<ts>:<map>[:<count>[:<chars>]]\n.
// flapCount<1 and flapBytes NULL/"" leave the module's flap set unchanged; flapBytes are
// bus bytes (already transcoded, see sfValidateCharSet). Returns the frame length, or 0
// if it would not fit in outLen -- a truncated restore would corrupt the module's EEPROM,
// so callers must refuse rather than send a partial map.
size_t sfBuildRestoreFrame(const char* sn, int homeOffset, int totalSteps, const char* map,
                           int flapCount, const char* flapBytes, char* out, size_t outLen);
// Validate a UTF-8 flap character set and transcode it to the single-byte bus encoding
// (Windows-1252) into `out` (NUL-terminated). NULL on success, else an error message.
const char* sfValidateCharSet(const char* charSet, char* out, size_t outLen);
void sfFsInit(bool forceFormat = false);
// The flap set a module is BELIEVED to have, and how sure the gateway is of it.
enum FlapSetSource { FLAPSET_REPORTED, FLAPSET_ASSUMED, FLAPSET_UNKNOWN };
// Fills `out` (SF_MAX_FLAPS+1 bytes) with the module's set and says where it came from.
// Call with sfMutex HELD.
FlapSetSource sfFlapSetOf(const SFModule& m, char* out);
// Forget a module's cached set (-1 = all) and re-read it. Only an 'N' invalidates one.
void sfInvalidateFlapSet(int addr);
void sfModulesSave();
void sfModulesLoad();
void sfModulesClear();
void sfModulesPruneStale();
void sfTrackChar(int addr, char c);
void sfSendChar(int addr, char c);
// The wire byte for a CP1252 byte (uppercase fold except colour codes; '"' -> 'q'), or 0 if it
// is not a displayable flap glyph. Reel-independent: the fallback for a module the gateway has
// no reel for. Prefer sfResolveFor() on any path that knows which module it is addressing.
uint8_t sfResolveFlapByte(uint8_t b);
// Copy module `id`'s flap set into `out` (SF_MAX_FLAPS+1 bytes). True if the gateway knows it
// (reported, or assumed for a pre-v31 module); false leaves `out` empty. Takes sfMutex itself,
// so never call it while holding that mutex.
bool sfReelOf(int id, char* out);
// The wire byte for one requested byte ON THAT MODULE: a symbol code (flapsym.h) becomes the
// reel's letter, its stand-in or a blank; '"' reaches the reel's double-quote flap whichever
// byte it is printed as; everything else is sfResolveFlapByte(). 0 = nothing showable.
uint8_t sfResolveFor(int id, uint8_t b);
// Send one ALREADY-RESOLVED wire byte (no folding, no reel lookup). For replaying a captured
// frame, and for callers that resolved the byte themselves against a snapshot of the reels.
void sfSendCharRaw(int addr, uint8_t b);
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
/* Put one bare terminator on the bus. A module whose receive ring overflowed drops bytes
   until it sees one, so without this the next command addressed to it is what gets eaten and
   the module looks silent when it is only resyncing. Costs one byte. */
void sfBusResync();
/* True when this module's firmware verifies checksummed frames (v33+). The registry's version
   is the source; an unknown version is treated as old, so nothing changes until a module has
   actually told the gateway what it runs. */
bool sfModuleChecksums(int id);
/* Send one frame to a module, checksummed when that module verifies them and plain when it
   does not. Use this for anything whose corruption costs something: reads, writes, restores.
   Display content stays on the plain path -- five extra bytes on a six-byte command is 80%
   more traffic for a frame the next update overwrites anyway. */
void sfSendToModule(int id, const char* frame);
bool sfSendAndCaptureDump(int id, const char* frame, unsigned long timeoutMs, char* out, size_t outLen,
                          volatile bool* abort = NULL);
/* The same, but asks twice: a module that ate the first command while resyncing answers the
   second. `late` (optional) is set when only the retry worked -- worth recording, it means
   something on the bus is overflowing module receivers.
   `alt` (optional) is a DIFFERENT frame for the second ask. That is how the serial-number
   forms earn their keep: ask by id first, which is 6 bytes, and only if that goes unanswered
   ask by serial number, which is 24 and cannot be answered by the wrong module. Addressing
   everything by serial put 20 needless bytes on the bus for every read -- 78% of all command
   bytes in a soak -- and every module on the wall has to receive them. */
bool sfReadDump(int id, const char* frame, unsigned long timeoutMs,
                char* out, size_t outLen, bool* late = NULL, volatile bool* abort = NULL,
                const char* alt = NULL);
bool sfSendAndCaptureQ(int id, unsigned long timeoutMs);
bool sfSendVersionAndWait(int id, unsigned long timeoutMs, char* fwOut, size_t fwLen, char* snOut, size_t snLen, unsigned long* lastSeenOut);
void sfDeprovision(int addr);
void sfProvision(const char* sn, int newId);
void sfHomeBySN(const char* sn);
void sfSendText(int startAddr, const char* text, bool blankUnused);
void sfSetQuietTime(bool on);
void sfParseResponse(const uint8_t* data, size_t len);
// Start a bus probe on `id`. False if one is already running or the task will not start.
bool sfProbeStart(int id, int rounds);

#endif // SFGW_MODULES_H
