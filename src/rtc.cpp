#include "gateway.h"



// rtc.cpp -- PCF85063 I2C real-time clock and NTP synchronisation.
// Keeps wall-clock time across reboots (used for module last-seen pruning and
// for timestamping bus frames). rtcRead() is polled by taskRTC once a second;
// rtcNTPSync() runs once WiFi connects. All times are stored/handled as UTC.
// ---- file-private forward declarations ----
static bool rtcI2CRead(uint8_t reg, uint8_t* buf, uint8_t len);
static bool rtcI2CWrite(uint8_t reg, const uint8_t* buf, uint8_t len);
static int rtcBcdToDec(uint8_t v);
static uint8_t rtcDecToBcd(int v);
static void rtcWriteUnix(time_t t);

static uint8_t rtcDecToBcd(int v)     { return (uint8_t)((v/10*16)+(v%10)); }
static int     rtcBcdToDec(uint8_t v) { return (v/16*10)+(v%16); }

static bool rtcI2CWrite(uint8_t reg, const uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) Wire.write(buf[i]);
  return Wire.endTransmission(true) == 0;
}
static bool rtcI2CRead(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  Wire.requestFrom((uint8_t)PCF85063_ADDR, len);
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

void rtcHwInit() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  uint8_t ctrl = 0x01;
  rtcI2CWrite(PCF85063_CTRL1, &ctrl, 1);
  DBG("[RTC] PCF85063 init OK\n");
}

void rtcRead() {
  uint8_t buf[7] = {0};
  if (!rtcI2CRead(PCF85063_SEC_REG, buf, 7)) return;
  rtcNow.second = rtcBcdToDec(buf[0] & 0x7F);
  rtcNow.minute = rtcBcdToDec(buf[1] & 0x7F);
  rtcNow.hour   = rtcBcdToDec(buf[2] & 0x3F);
  rtcNow.day    = rtcBcdToDec(buf[3] & 0x3F);
  rtcNow.month  = rtcBcdToDec(buf[5] & 0x1F);
  rtcNow.year   = rtcBcdToDec(buf[6]) + RTC_YEAR_OFFSET;
}

static void rtcWriteUnix(time_t t) {
  struct tm* tm = gmtime(&t);
  uint8_t buf[7] = {
    rtcDecToBcd(tm->tm_sec),
    rtcDecToBcd(tm->tm_min),
    rtcDecToBcd(tm->tm_hour),
    rtcDecToBcd(tm->tm_mday),
    rtcDecToBcd(tm->tm_wday),
    rtcDecToBcd(tm->tm_mon + 1),
    rtcDecToBcd(tm->tm_year - 100)
  };
  rtcI2CWrite(PCF85063_SEC_REG, buf, 7);
}

// rtcNTPSync: always syncs in UTC. The timezone offset is applied only
// We never pass a tz offset to configTime -- that avoids mktime/gmtime
// double-offset bugs entirely.
bool rtcNTPSync() {
  const char* ntpSrv = cfg.ntpServer[0] ? cfg.ntpServer : DEFAULT_NTP_SERVER;
  DBG("[NTP] Syncing (UTC) via %s...\n", ntpSrv);
  configTime(0, 0, ntpSrv);  // always fetch UTC
  struct tm info;
  unsigned long start = millis();
  while (!getLocalTime(&info, 200)) {
    if (millis() - start > NTP_TIMEOUT_MS) {
      DBG("[NTP] Timed out\n");
      return false;
    }
  }
  // info is UTC because we passed 0 to configTime.
  // Convert to time_t and write to RTC chip.
  // Use timegm-equivalent: set TZ to UTC, call mktime, restore.
  // On ESP32 the system TZ is always UTC so mktime IS timegm here.
  time_t utc = mktime(&info);
  rtcWriteUnix(utc);
  rtcNow.valid = true;
  char tbuf[32];
  strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &info);
  return true;
}

void rtcFormatTime(char* out, size_t outLen) {
  // Snapshot volatile fields
  uint16_t yr = rtcNow.year;   uint8_t mo = rtcNow.month;
  uint8_t  dy = rtcNow.day;    uint8_t hr = rtcNow.hour;
  uint8_t  mn = rtcNow.minute; uint8_t sc = rtcNow.second;
  bool     vld = rtcNow.valid;
  if (!vld) {
    snprintf(out, outLen, "%02u:%02u:%02u", hr, mn, sc);
    return;
  }
  if (!timeMutex) {
    snprintf(out, outLen, "%02u:%02u:%02u", hr, mn, sc);
    return;
  }
  // IMPORTANT: TZ is set ONCE in loadConfig()/handleApiConfigSettings()
  // (boot + on change). Calling setenv() here on every invocation leaks heap
  // on ESP32 newlib, so we convert stored-UTC to local time without touching TZ.
  // Only proceed under the mutex if we actually acquired it -- giving a mutex
  // we don't own corrupts its state and can permanently wedge other tasks.
  if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    // Could not get the lock in time -- fall back to a TZ-free HH:MM:SS so the
    // caller still gets something and we never give an unowned mutex.
    snprintf(out, outLen, "%02u:%02u:%02u", hr, mn, sc);
    return;
  }
  // Compute the UTC epoch manually (days-since-epoch algorithm).
  // This avoids timegm() (not in ESP32 newlib) and mktime()+setenv (leaks).
  static const int cumDays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
  long y = yr;
  long days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
  days += cumDays[(mo - 1) % 12];
  // Add leap day if past Feb in a leap year
  bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  if (leap && mo > 2) days += 1;
  days += (dy - 1);
  time_t utcEpoch = (time_t)days * 86400L + (long)hr * 3600L + (long)mn * 60L + sc;
  // localtime_r applies the already-set TZ environment (set once at boot)
  struct tm lt;
  localtime_r(&utcEpoch, &lt);
  snprintf(out, outLen, "%04d-%02d-%02d %02d:%02d:%02d",
           lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
           lt.tm_hour, lt.tm_min, lt.tm_sec);
  xSemaphoreGive(timeMutex);
}

// Return current UTC wall-clock epoch from the RTC, or 0 if RTC not valid.
// Used for module last-seen tracking that must survive reboots (millis() resets).
unsigned long rtcEpochNow() {
  if (!rtcNow.valid) return 0;
  uint16_t yr = rtcNow.year;   uint8_t mo = rtcNow.month;
  uint8_t  dy = rtcNow.day;    uint8_t hr = rtcNow.hour;
  uint8_t  mn = rtcNow.minute; uint8_t sc = rtcNow.second;
  static const int cumDays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
  long y = yr;
  long days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
  days += cumDays[(mo - 1) % 12];
  bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  if (leap && mo > 2) days += 1;
  days += (dy - 1);
  return (unsigned long)((long)days * 86400L + (long)hr * 3600L + (long)mn * 60L + sc);
}
