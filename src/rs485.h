// rs485.h -- RS-485 bus: monitor ring, low-level UART, and the send API.

#ifndef SFGW_RS485_H
#define SFGW_RS485_H

#include "common.h"

/* ----------------------------------------------------------
   Message ring buffer
---------------------------------------------------------- */
struct RS485Msg {
  unsigned long timestamp;
  char          dir;            // 'T' transmitted to bus, 'R' received from a module, 'C' inbound REST/MQTT command marker
  char          origin;         // 'C' rows only: 'R' REST or 'M' MQTT (0 for TX/RX)
  bool          sanitized;      // TX only: true if the gateway trimmed trailing junk past a complete command
  uint8_t       data[MSG_MAX_BYTES];
  size_t        len;
  char          wallTime[24];   // gateway-TZ string (MQTT / serial debug)
  unsigned long epoch;          // UTC epoch at capture (0 if RTC not valid);
                                // the web UI renders this in the BROWSER's
                                // local timezone -- no gateway TZ config needed
};

// ---- owned globals (defined in globals.cpp) ----
extern SemaphoreHandle_t txMutex;
extern StaticSemaphore_t txMutexBuf;
extern RS485Msg* msgRing;
extern volatile int msgHead;
extern volatile int msgPollCursor;
extern StaticSemaphore_t msgMutexBuf;
extern SemaphoreHandle_t msgMutex;
extern HardwareSerial rs485;
extern volatile unsigned long rxCount;
extern volatile unsigned long txCount;
extern volatile unsigned long sfParseRejects;

void psramAllocInit();
// A large buffer in PSRAM (internal RAM as a fallback), zeroed. Internal RAM is what the
// WiFi/TCP stack lives on during a web-OTA upload -- anything big that is not on a hot path
// belongs here instead.
void* psramAlloc(const char* name, size_t bytes);
void ringPush(const RS485Msg& m);
void ringPushCommand(char origin, const char* desc);  // log an inbound REST/MQTT command ('C' row)
String ringDrain();
void rs485Begin();
void rs485Send(const uint8_t* data, size_t len, bool raw = false);
void rs485SendStr(const char* s);

// ---- scheduled (paced) outbound frames ----
// /api/rs485/batch paces a cascade so modules receive frames staggered (the companion's
// animation styles rely on it). It used to do that with delay() between sends -- INSIDE
// the web handler, on taskWeb. On the one-connection-at-a-time WebServer that froze the
// whole HTTP server for the batch's duration (up to 8 s), during which concurrent
// connections piled up in lwIP's accept queue holding TCP window buffers. On the LED-panel
// port this drove min-free internal heap toward exhaustion; here the heap is roomier but
// the freeze itself (an 8 s unresponsive web UI on a big batch) is reason enough to fix.
//
// So pacing moves OFF taskWeb: the handler stamps each frame with a due time and enqueues
// it here, then returns immediately. taskRS485 -- which already wakes every 5 ms -- sends
// each frame when its due time arrives. The web server never blocks; the cascade is
// unchanged (the 5 ms tick quantises step_ms, which is 5..30 ms anyway).
#define TXQ_SIZE       128     // scheduled frames in flight (~3 full pages)
#define TXQ_FRAME_MAX  48      // display/index/home frames fit; longer ones send inline
struct TxQItem { uint32_t dueMs; uint16_t len; uint8_t data[TXQ_FRAME_MAX]; };
extern TxQItem* txQueue;                 // PSRAM ring (SPSC: taskWeb in, taskRS485 out)
extern volatile int txQHead, txQTail;
extern SemaphoreHandle_t txQMutex;
extern StaticSemaphore_t txQMutexBuf;
// Enqueue one frame for delivery at dueMs (millis timebase). Returns false if it will not
// fit (too long, or queue full) -- caller should then send it inline via rs485Send.
bool rs485SendScheduled(const uint8_t* data, size_t len, uint32_t dueMs);
// Send every queued frame whose due time has arrived. Called by taskRS485 each tick.
void rs485PollScheduled(uint32_t now);

#endif // SFGW_RS485_H
