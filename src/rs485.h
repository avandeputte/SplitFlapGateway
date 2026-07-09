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
void ringPush(const RS485Msg& m);
void ringPushCommand(char origin, const char* desc);  // log an inbound REST/MQTT command ('C' row)
String ringDrain();
void rs485Begin();
void rs485Send(const uint8_t* data, size_t len, bool raw = false);
void rs485SendStr(const char* s);

#endif // SFGW_RS485_H
