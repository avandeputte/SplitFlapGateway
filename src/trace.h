// trace.h -- the raw bus traffic of a soak test, kept so somebody else can read it. (v3.17)
//
// The soak's report says what happened; a trace says what was on the wire. For a wall you
// cannot touch -- someone else's, or your own reproduced locally -- that difference is the
// whole investigation: which frame came back short, what the bytes were, how long the gap was
// before the retry. The last run's trace is always kept and always downloadable, clean or not,
// because a clean trace from a working wall is the baseline you compare a broken one against.
//
// Two rules shape the implementation:
//
//   * Nothing is written to flash while the test runs. Flash writes stall the cache and add
//     milliseconds of jitter, and the test is measuring milliseconds. The trace accumulates in
//     PSRAM and is written out once, after the bus is released.
//   * Memory is bounded and the NEWEST data wins. The ring evicts whole lines from the front
//     and counts what it dropped, so the file is always chronological and always says what is
//     missing.
//
// Frames are stored WHOLE. Cutting long replies was measured against a real run and saved only
// 29% -- most frames on this bus are short -- while costing the one thing a trace is for: the
// bytes of the reply that went wrong. At full fidelity a 64-module half-hour soak is ~3.6 MB,
// which the ring holds. The format line buffer lives in PSRAM too, since a whole frame escaped
// for text is 4 KB and internal RAM is the scarce memory here.

#ifndef SFGW_TRACE_H
#define SFGW_TRACE_H

#include "common.h"

#define TRACE_BYTES        (4UL * 1024UL * 1024UL)   // PSRAM ring: a 64-module 30-min run is ~3.6 MB
#define TRACE_PAYLOAD_MAX  RING_FRAME_BYTES          // whole frames: the longest the bus can carry
#define TRACE_FILE         "/soaktrace.txt"

void traceInit();                       // once, from setup()
// Begin a capture. Everything until traceEnd() is recorded. `what` goes in the header.
void traceBegin(const char* what);
bool traceActive();
uint32_t traceFrames();      // frames captured in the current (or last) run
// One frame, as seen on the wire. dir 'T' sent, 'R' received, 'C' a command from REST/MQTT.
void traceFrame(char dir, const uint8_t* data, size_t len, size_t fullLen);
// Anything worth a line of its own: a pass boundary, a discarded frame, an incident.
void traceNote(const char* fmt, ...);
// End the capture and write it to flash, replacing the previous trace. Returns bytes written.
size_t traceEnd(const char* summary);
// What is on flash right now: size in bytes, and the header line, for the UI and the API.
bool traceFileInfo(size_t* bytes, char* firstLine, size_t lineLen);
// Stream the stored trace to a caller (the download endpoint).
void traceEmitFile(void (*emit)(const char* chunk));

#endif // SFGW_TRACE_H
