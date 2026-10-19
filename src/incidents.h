// incidents.h -- a short history of everything the bus got wrong. (v3.16)
//
// Counters say how many frames were discarded; they never say which module had the bus at the
// time, what was being asked, or whether it happened in one burst or spread over an hour. That
// is the difference between "frames discarded: 6" and "module 24 was asked for its dump six
// times and every reply was discarded at the receive ceiling" -- and the second one is an
// answer. Chasing a 64-module wall through two firmwares is what made the case for keeping the
// detail rather than the tally.
//
// The ring lives in PSRAM (16 KB of 8 MB) and survives everything except a reboot. It is
// written from taskRS485, taskSoak and taskWeb, so pushes take a spinlock -- they are a handful
// of stores, and the alternative is a torn record in exactly the situation being debugged.

#ifndef SFGW_INCIDENTS_H
#define SFGW_INCIDENTS_H

#include "common.h"

#define INCIDENT_SLOTS 1024

struct BusIncident {
  uint32_t ms;        // millis() at capture
  uint32_t epoch;     // UTC epoch, 0 when the clock was not set yet
  uint8_t  id;        // module it belongs to; 255 = could not be attributed
  char     kind;      // see below
  char     cmd;       // the command in flight ('v','Q','d','A','W'), 0 if not applicable
  uint16_t bytes;     // size involved, when it is known
};

// kind:
//   'D'  a frame was discarded -- too long for the accumulator, or it never terminated
//   'R'  a frame arrived complete and would not parse (corrupt, or two replies on top)
//   'L'  a long read answered only when it was asked a second time
//   'M'  a long read was never answered, after both asks
//   'X'  two modules appear to share one id

void incidentsInit();                                            // once, from setup()
void incidentLog(uint8_t id, char kind, char cmd, uint16_t bytes);
unsigned incidentsTotal();                                       // since boot, including evicted
void incidentsClear();
// Newest first, at most `max` of them, as {"ok":true,"total":N,"shown":M,"items":[...]}.
// Streamed through `emit` in TCP-sized bites: a 120-entry answer is ~9 KB, and a static
// buffer that size would come out of internal RAM, which is the one memory this board is
// actually short of.
void incidentsEmit(void (*emit)(const char* chunk), int max);

#endif // SFGW_INCIDENTS_H
