// perf.h -- who is asking the web server for what, and what it costs.
//
// The gateway serves ONE connection at a time (see ARCHITECTURE.md), so "the UI is sluggish"
// is always one of three things: too many requests, a request that is expensive, or a client
// that connected and then said nothing. From outside they are indistinguishable -- every
// request is simply slow. This counts all three from inside, per endpoint and per client, so
// the answer is a table rather than a theory. GET /api/perf reads it; ?reset=1 starts a fresh
// window. The cost is one strcmp walk over a small table per request.
#ifndef SFGW_PERF_H
#define SFGW_PERF_H

#include <stdint.h>
#include <stddef.h>

// One handled request. `newLowHeap` is the internal heap's all-time low-water mark if THIS
// request pushed it lower, else 0 -- so a heap dip has an endpoint's name on it.
void perfNoteRequest(const char* uri, uint32_t ip, uint32_t ms, uint32_t newLowHeap);

// The long-running tasks, for perfLoopTick(). Each calls it once per pass of its main loop;
// the longest gap between two passes is how long that task was stuck somewhere -- a blocking
// handler on Web, a flash write on Network, a held mutex on RS485.
enum { PERF_T_RS485 = 0, PERF_T_WEB, PERF_T_NET, PERF_T_RTC, PERF_T_COUNT };
void perfLoopTick(int task);
// Milliseconds since the web server last completed a request (since boot, if it never has).
// Proof of life for the TCP stack: see the MQTT reconnect logic in taskNetwork.
uint32_t perfSinceLastRequestMs();
void perfNoteWait(uint32_t ms);          // time held by a connected client that sent nothing
void perfReset();
// Streams the table as JSON through `emit` (chunks are NUL-terminated).
void perfEmit(void (*emit)(const char* chunk));

#endif // SFGW_PERF_H
