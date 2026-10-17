// rtc.h -- PCF85063 real-time clock + NTP: public time API.

#ifndef SFGW_RTC_H
#define SFGW_RTC_H

#include "common.h"

struct RtcTime {
  uint16_t year;
  uint8_t  month, day, hour, minute, second;
  bool     valid;
};

// ---- owned globals (defined in globals.cpp) ----
extern volatile RtcTime rtcNow;
extern char gPosixTZ[64];

void rtcHwInit();
// Apply cfg.posixTZ/gPosixTZ to the C library. The ONE place that writes TZ --
// and it must be re-applied after every configTime() call (see rtc.cpp).
void rtcApplyTZ();
void rtcRead();
bool rtcNTPSync();
void rtcFormatTime(char* out, size_t outLen);
unsigned long rtcEpochNow();

#endif // SFGW_RTC_H
