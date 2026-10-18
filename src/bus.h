// bus.h -- who owns the RS-485 bus right now, and how much. (v3.15)
//
// Maintenance work -- provisioning, calibrating, self-tests, the probe and soak tests, a
// backup sweep, a restore -- must not have display content painted over it. Until now that
// took remembering to flip Maintenance Mode on (and it only ever gated MQTT: a companion
// driving the wall over REST sailed straight through). This replaces remembering.
//
// Every such operation takes a HOLD on the bus, automatically, at one of two strengths:
//
//   BUS_BUSY       external CONTENT is suppressed -- MQTT display commands are dropped and the
//                  REST display endpoints answer 503 to any caller that is not this gateway's
//                  own dashboard (it tags its requests; see busIsOperator). The operator keeps
//                  working. Used by provisioning, calibration, diagnostics, probes, backups,
//                  and the manual Maintenance switch.
//
//                  A BUSY hold pauses CONTENT -- characters, text, raw frames. It does NOT
//                  pause homing, which is itself a maintenance action and the one an operator
//                  reaches for most while calibrating (issue #7: homing from the Calibration
//                  tab answered 503 because the tab's own hold was in force). Only the
//                  machine-run tests below, where one extra frame invalidates the result,
//                  pause homing as well.
//
//   BUS_EXCLUSIVE  NOTHING else reaches the bus, dashboard included. Used by the boot restore,
//                  where an interleaved frame corrupts an EEPROM write.
//
// A hold carries a reason (shown in the dashboard banner) and a LEASE: it expires on its own.
// Operations that know their length set a generous lease; long-running tasks renew it as they
// go; the dashboard renews it while a backup or a calibration session is in progress. So a
// crashed browser tab or an abandoned job can never leave the wall locked. Only the manual
// Maintenance switch holds without expiry, because that is the whole point of it.

#ifndef SFGW_BUS_H
#define SFGW_BUS_H

#include "common.h"

enum BusLevel : uint8_t { BUS_FREE = 0, BUS_BUSY = 1, BUS_EXCLUSIVE = 2 };

#define BUS_HOLD_SLOTS   6
#define BUS_REASON_MAX   20

struct BusHold {
  char          reason[BUS_REASON_MAX];   // "" = slot free
  uint8_t       level;
  unsigned long sinceMs;
  unsigned long untilMs;                  // 0 = no expiry (manual maintenance only)
  uint32_t      ownerIp;                  // client that took it over REST (0 = the firmware)
};

// Take or renew a hold. The same reason renews in place (level may be raised, never lowered
// by a renew). leaseMs 0 = no expiry. Returns false only if every slot is taken.
// `ownerIp` records the client that asked for it, so that client is recognised as the
// operator afterwards even if its request tag does not survive the path to us (a proxy that
// drops unknown headers, an older page, a second operator UI). 0 = taken by the firmware.
bool busHold(const char* reason, BusLevel level, unsigned long leaseMs, uint32_t ownerIp = 0);
// True if `ip` holds one of the holds in force. Never true for 0.
bool busOwnedByIp(uint32_t ip);
// True while a machine-run test owns the bus -- the probe, the soak, the module self-tests, a
// restore. These measure the bus itself, so ANY extra frame (even the operator's) spoils them,
// and they refuse homing as well as content.
bool busTestRunning();
// Release a hold only if the FIRMWARE took it (never a client's lease).
void busReleaseFirmware(const char* reason);
// Called about once a second: drop any firmware hold whose job is no longer running, so a
// hold can never outlive its reason (issue: a cancelled soak left the wall paused until the
// lease ran out or the gateway was power-cycled). Client leases are not touched -- they
// expire on their own.
void busSweep();
// Release the hold with this reason. Safe to call when there is none.
void busRelease(const char* reason);
// The strongest hold in force right now (expired leases are reaped here).
BusLevel busLevel();
static inline bool busBusy()      { return busLevel() >= BUS_BUSY; }
static inline bool busExclusive() { return busLevel() == BUS_EXCLUSIVE; }
// The reason of the strongest hold, or "" when free.
const char* busReason();
// Seconds left on the strongest hold; -1 = no expiry; 0 = free.
long busRemainingSec();
// JSON object for /api/status and /api/bus: {"level":..,"reason":"..","remaining":..,"holds":[..]}
void busStatusJson(char* out, size_t outLen);

#endif // SFGW_BUS_H
