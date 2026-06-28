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
void rtcRead();
bool rtcNTPSync();
void rtcFormatTime(char* out, size_t outLen);
unsigned long rtcEpochNow();

#endif // SFGW_RTC_H
