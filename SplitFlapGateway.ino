/*
 * Split-Flap Gateway
 * Single-file Arduino sketch for Waveshare ESP32-S3-RS485-CAN
 *
 * Split-flap protocol (v12 firmware, backward-compatible to v6):
 *   Bus format:   m<ADDR><CMD>[data]\n   at 9600 baud 8N1
 *   Address:      decimal (zero-padded 2-digit v6 style, or variable-length v7+)
 *                 broadcast: m** (v6) or m* (v7+)
 *                 provisioning: mX...
 *   Char set:     " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw"
 *   Key commands: m<id>-<char>    display character
 *                 m<id>+<n>       display by flap index
 *                 m<id>h          home
 *                 m<id>c          calibrate
 *                 m<id>i<n>       set ID
 *                 m<id>v          get firmware version -> m<id>v:<ver>\n
 *                 m<id>d          dump EEPROM config   -> m<id>d:<ho>:<ts>:<map>\n
 *                 m<id>R          reset provisioning
 *                 m<id>F          factory reset
 *                 mXadv:<sn>      advertisement from unprovisioned module
 *                 mXI<sn>:<id>    assign ID by serial number
 *                 mXH<sn>         home by serial number
 *
 * Features:
 *   WiFi STA + AP fallback for config
 *   MQTT publish/subscribe with configurable broker and credentials
 *   RS485 half-duplex UART1 (TX=17, RX=18, DE/RE=21)
 *   Web UI: split-flap control panel, live bus monitor, config pages
 *   REST API (see reference at bottom of file)
 *   MQTT topics: <prefix>/rx  <prefix>/tx  <prefix>/status
 *                <prefix>/send  <prefix>/flap/set  <prefix>/flap/home
 *                <prefix>/flap/adv  <prefix>/flap/ack
 *
 * Required libraries (Arduino Library Manager):
 *   PubSubClient  by Nick O'Leary
 *   ArduinoJson   by Benoit Blanchon (v6 or v7)
 *   WiFi, WebServer, Preferences  (built-in ESP32 Arduino core >= 3.0)
 *
 * Board: ESP32-S3 Dev Module
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <FFat.h>
#include <ESPmDNS.h>

/* ============================================================================
 *  BOARD / BUILD CONFIGURATION
 *  ----------------------------------------------------------------------------
 *  Everything you need to retarget this firmware to a different board, pinout,
 *  or default setup lives in this single block. Change values here -- nothing
 *  below this section hardcodes pins, buffer sizes, or factory defaults.
 *
 *  Default config is for the Waveshare ESP32-S3-RS485-CAN board.
 * ==========================================================================*/

/* ---- RS-485 transceiver pins ---- */
#define RS485_TX_PIN   17
#define RS485_RX_PIN   18
#define RS485_EN_PIN   21          // DE/RE direction control (high = transmit)

/* ---- I2C + PCF85063 RTC (SDA=39, SCL=38, addr 0x51) ----
   NTP syncs the RTC on first WiFi connection. */
#define I2C_SDA_PIN       39
#define I2C_SCL_PIN       38
#define PCF85063_ADDR     0x51
#define PCF85063_SEC_REG  0x04
#define PCF85063_CTRL1    0x00
#define RTC_YEAR_OFFSET   2000     // PCF85063 reg 6 is 0-99 = 2000-2099

/* ---- Firmware identity ---- */
#define FW_VERSION           "1.8"            // gateway firmware version (UI + boot log)

/* ---- Network / service defaults (overridable at runtime via Settings) ---- */
#define DEFAULT_AP_SSID      "Split-Flap-GW"  // SoftAP SSID when no WiFi configured
#define DEFAULT_AP_PASS      "12345678"       // SoftAP password (>= 8 chars)
#define DEFAULT_BAUD         9600UL           // RS-485 bus baud rate
#define DEFAULT_MQTT_PORT    1883
#define DEFAULT_MQTT_PREFIX  "splitflap"
#define DEFAULT_NTP_SERVER   "pool.ntp.org"   // overridable via Settings
#define NTP_TIMEOUT_MS       8000UL

/* ---- Bus timing ----
   Half-duplex collision avoidance: before transmitting, wait until the bus has
   been quiet for TX_BUS_GUARD_MS (so we never stomp on an in-flight module
   response train), capped at TX_BUS_WAIT_CAP_MS so a noisy bus can't block
   transmit forever. 12ms ~= 12 byte-times at 9600 baud. */
#define TX_BUS_GUARD_MS      12
#define TX_BUS_WAIT_CAP_MS   400

/* ---- Buffer / queue sizes ----
   Outbound frames may be longer than the 256-byte monitor-ring entry size --
   a full 64-flap restore command (mXW<sn>:<offset>:<steps>:<map>) can reach
   ~620 bytes. TX_MAX_BYTES bounds what rs485Send will transmit; the monitor
   ring still stores only the first MSG_MAX_BYTES for display. */
#define MSG_RING_SIZE        64    // monitor ring: number of frames retained
#define MSG_MAX_BYTES        256   // monitor ring: bytes stored per frame
#define TX_MAX_BYTES         768   // max bytes rs485Send will transmit in one frame
#define MQTT_BUF_SIZE        768   // holds a full restore command via MQTT
#define MQTT_Q_SIZE          32    // outbound MQTT publish queue depth

/* ---- Housekeeping cadences ---- */
#define STATUS_INTERVAL_MS      60000UL   // MQTT status publish cadence (1/min)
#define MODULE_STALE_SECS       21600UL   // 6h: prune modules not seen in this long
#define MODULE_PROBE_GRACE_MS   3000UL    // wait this long for a stale module's version reply before dropping it
#define MODULE_POSTPROV_VER_MS  4000UL    // delay after a provisioning ack before the first version query. A freshly-provisioned module writes its new ID to EEPROM and then runs a staggered startup (~150ms x new-ID), so it can be unresponsive for several seconds; wait well past that before the first query.
#define MODULE_VER_RETRY_MS     2500UL    // gap between post-provision version-query retries
#define MODULE_VER_MAX_TRIES    6         // give up after this many version-query attempts (covers ~4s + 5x2.5s ~= 16s)
#define MODULE_SAVE_DEBOUNCE_MS 5000UL    // coalesce NVS writes

/* ---- Module registry sizing ----
   Supports module IDs 0-254 (255 modules). id==255 is reserved as the
   empty-slot / unprovisioned sentinel, so the array needs one slot per usable
   ID. Frame buffers (MSG_MAX_BYTES / TX_MAX_BYTES / MQTT_BUF_SIZE) are sized by
   the 64-flap dump/restore MAP, not by module count -- a frame targets a single
   module -- so they are unaffected by this bound. 3-digit IDs (vs 2) add ~1
   byte to a handful of commands, still far inside TX_MAX_BYTES. */
#define MAX_MODULES         255   // module IDs 0-254

// Duplicate-ID heuristic: two modules at the same ID both answer a by-ID version
// or 'A' query and collide on the half-duplex bus -- the shared framing survives
// but the serials garble, so sfParseResponse rejects the SN. Repeated such
// rejects for ONE ID (within the window) are the signature of a duplicate ID.
#define DUP_ID_REJECT_WINDOW_MS 60000UL   // rolling window for counting corrupt version/all replies
#define DUP_ID_REJECT_THRESHOLD 3         // this many within the window -> flag a possible duplicate ID

/* ---- Persisted module-registry file (FFat) ---- */
#define MODULES_FILE     "/modules.dat"
#define MODULES_MAGIC    0x53464732UL   // "SFG2" (bumped: PersistedModule gained 'acked')

/* ==========================================================================*/


// Early-declared debug flag so DBG() works before cfg is constructed.
// Kept in sync with cfg.serialDebug in loadConfig() and handleApiConfigSettings().
static volatile bool gSerialDebug = false;
// Maintenance mode: when true, external commands arriving via MQTT are ignored
// and not relayed to the RS-485 bus. The web UI / REST API (the gateway itself)
// continue to work normally. Always OFF at boot -- never persisted -- so a
// reboot is a guaranteed return to normal operation.
static volatile bool gMaintenanceMode = false;
// Quiet Time: when true the gateway still accepts and acknowledges every command,
// but does NOT transmit normal display-motion frames to the bus (show character,
// show index, and home), so the flaps stay still during quiet hours. Deliberate
// calibration moves (calibrate, goto, nudge) are still allowed, since those only
// come from an operator actively calibrating. Display tracking is left unchanged
// (it reflects the physically-shown flap). The latest requested display per
// module is remembered so the reels can resync when Quiet Time turns off.
// Runtime-only -- OFF at boot, never persisted -- like maintenance mode.
static volatile bool gQuietTime = false;
// Set when display tracking changes (in rs485Send); the network task publishes
// the HA display-state topic (rate-limited) so HA reflects what's shown without
// spamming. Declared here so rs485Send, defined earlier, can reference it.
static volatile bool gDisplayDirty = false;
// Set for the duration of a web OTA upload. While true the network task skips
// MQTT status/display/discovery publishes so the upload has the heap and CPU it
// needs (these reduce the contiguous heap the WiFi/TCP stack relies on, a known
// cause of mid-upload connection drops). Declared early so the OTA handler and
// the network task can both see it.
static volatile bool gOtaInProgress = false;
// Fallback SoftAP: the AP is only brought up when the station is NOT connected
// to a configured network (so the config page stays reachable). Once the station
// connects, the AP is dropped and the gateway runs STA-only. Tracks whether the
// AP is currently up so we only switch WiFi modes on actual state transitions.
static bool gApActive = false;
#define DBG(...) do { if (gSerialDebug) printf(__VA_ARGS__); } while(0)

struct RtcTime {
  uint16_t year;
  uint8_t  month, day, hour, minute, second;
  bool     valid;
};
static volatile RtcTime rtcNow = {2000,1,1,0,0,0,false};
// POSIX TZ string -- declared here (before cfg) so rtcFormatTime can use it.
static char gPosixTZ[64] = "UTC0";

// Runtime configuration. Declared here (well before its first use in
// rtcNTPSync) because several time/RTC helpers above the main config section
// need cfg.ntpServer / cfg.posixTZ. Defaults and persistence live further down
// in cfgSetDefaults() / loadConfig() / saveConfig().
struct GwConfig {
  char          wifiSSID[64];
  char          wifiPass[64];
  char          mqttHost[64];
  int           mqttPort;
  char          mqttUser[64];
  char          mqttPass[64];
  char          mqttPrefix[32];
  unsigned long rs485Baud;
  uint8_t       rs485DataBits;
  uint8_t       rs485Parity;
  uint8_t       rs485StopBits;
  char          posixTZ[64];   // POSIX TZ string e.g. "EST5EDT,M3.2.0,M11.1.0"
  char          ntpServer[64]; // NTP server hostname (default pool.ntp.org)
  bool          serialDebug;   // enable verbose serial output
  bool          haEnabled;     // publish Home Assistant MQTT discovery + entity state
  char          otaPassword[32]; // OTA update password (blank = no auth)
  uint8_t       gridRows;      // visual display wall: rows (>=1)
  uint8_t       gridCols;      // visual display wall: columns (>=1)
};
GwConfig cfg;
// Mutex protecting setenv/tzset/localtime (not thread-safe in newlib)
static SemaphoreHandle_t     timeMutex     = NULL;
static StaticSemaphore_t     timeMutexBuf;
// Watchdog timestamps -- each task writes millis() here every iteration
static volatile unsigned long wdgRS485Ms   = 0;
static volatile unsigned long gLastRxMs    = 0;  // millis() of last byte received on the bus
static int                    mqttFailCount = 0;  // consecutive MQTT connect failures
static volatile unsigned long wdgNetMs     = 0;
static volatile unsigned long wdgWebMs     = 0;
// Task handles -- used for uxTaskGetStackHighWaterMark on the Status page so
// stack pressure is visible BEFORE it becomes a canary crash.
static TaskHandle_t hTaskRTC = NULL, hTaskRS485 = NULL, hTaskOTA = NULL,
                    hTaskWeb = NULL, hTaskNet = NULL;
// MQTT outbound queue -- RS485/web tasks enqueue; network task publishes

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

static void rtcHwInit() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  uint8_t ctrl = 0x01;
  rtcI2CWrite(PCF85063_CTRL1, &ctrl, 1);
  DBG("[RTC] PCF85063 init OK\n");
}

static void rtcRead() {
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
static bool rtcNTPSync() {
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

static void rtcFormatTime(char* out, size_t outLen) {
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
static unsigned long rtcEpochNow() {
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

struct MqttQItem { char topic[48]; char payload[MQTT_BUF_SIZE]; size_t len; };
// Outbound MQTT publish queue (~25 KB). Lives in PSRAM -- it's drained by the
// network task and written under mqttQMutex, never from an ISR or DMA, so the
// slightly slower PSRAM is fine and it frees ~25 KB of internal RAM. Allocated
// in psramAllocInit(); falls back to internal RAM if PSRAM is unavailable.
static MqttQItem*            mqttQueue     = NULL;
static volatile int          mqttQHead     = 0;
static volatile int          mqttQTail     = 0;
static SemaphoreHandle_t     mqttQMutex    = NULL;
static StaticSemaphore_t     mqttQMutexBuf;

/* ----------------------------------------------------------
   Module registry  (tracks known modules on the bus)
   See MAX_MODULES in the configuration block at the top.
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
};

// Module registry (~14 KB). Lives in PSRAM -- accessed only under sfMutex from
// normal tasks (never an ISR, never DMA, and not during flash writes), and the
// 9600-baud bus is far from a hot loop, so PSRAM latency is negligible while it
// frees ~14 KB of internal RAM. Allocated in psramAllocInit(); falls back to
// internal RAM if PSRAM is unavailable.
static SFModule*            sfModules     = NULL;
static SemaphoreHandle_t sfMutex = NULL;
static StaticSemaphore_t sfMutexBuf;
// Serializes the bus-touching section of rs485Send. Without it, taskWeb (REST),
// taskNetwork (MQTT command, core 1) and taskRS485 (discovery / post-provision,
// core 0) can call rs485Send concurrently and interleave bytes on the shared
// UART -- garbled frames, lost replies, poisoned serials. The collision guard
// only protects TX-vs-RX; this protects TX-vs-TX. Lock order is ALWAYS
// txMutex -> sfMutex (rs485Send -> sfTrackFromFrame): never call rs485Send while
// holding sfMutex (sfTrackFromFrame would also re-take it and self-deadlock).
static SemaphoreHandle_t txMutex = NULL;
static StaticSemaphore_t txMutexBuf;
// Explicit prototypes to prevent Arduino IDE inserting them before SFModule is defined.
static SFModule* sfFindById(uint8_t id);
static SFModule* sfFindBySN(const char* sn);
static SFModule* sfUpsert(uint8_t id, const char* sn);
static inline void sfTouch(SFModule* m);
static int      sfModuleCount = 0;
// Shared single-slot capture for the most recent EEPROM dump response.
// Replaces the per-module dumpData cache (saves 256 bytes * MAX_MODULES of RAM).
// handleApiDump records the id it is waiting for, then polls for a match.
static volatile int           sfDumpWaitId   = -1;   // module id handleApiDump is waiting on
static char                   sfDumpCapture[TX_MAX_BYTES] = "";  // raw dump after 'd:'
static volatile unsigned long sfDumpCaptureTs = 0;   // millis() when captured (0=none)
// Extra fields that ONLY an 'A' (combined) reply carries; a plain 'd' dump and
// older firmware do not provide them. -99 = not provided (the module itself uses
// -1=unknown and -2=released for curIndex, so the sentinel must avoid both). Set by the 'A' parse
// branch BEFORE sfDumpCaptureTs (the ready flag), read by handleApiAll.
static volatile int           sfCaptureAutoHome   = -99;  // 0/1, or -99 n/a
static volatile int           sfCaptureCurIndex   = -99;  // flap index; -1 unknown, -2 released, -99 n/a
static volatile int           sfCaptureReportedId = -99;  // module's self-reported id, -99 n/a
static volatile int           sfCalibWaitId   = -1;  // module id handleApiCalibrate is waiting on
static volatile int           sfCalibSteps    = 0;   // captured steps/rev from m<id>:<steps>
static volatile unsigned long sfCalibCaptureTs = 0;  // millis() when captured (0=none)
// Async calibration job: the POST starts it and returns immediately; the UI
// polls /api/flap/calibrate/status. This keeps the single-threaded web task
// responsive during the ~15s the reel takes to physically measure a revolution
// (a synchronous wait would freeze the whole UI for every other request).
static volatile bool          sfCalibJobActive   = false;  // a job is in flight
static volatile int           sfCalibJobId       = -1;     // module being calibrated
static volatile int           sfCalibJobSteps    = -1;     // measured result (-1 until done)
static volatile unsigned long sfCalibJobDeadline = 0;      // millis() timeout
// Self-diagnostics (module firmware v26+). Two tests run per request:
//   'Q' snapshot  m<id>Q:<resetCause>:<bootCount>:<vcc_mV>:<eepromOk>:<curIndex>
//                 -- instant (no motor), captured synchronously like a dump.
//   'M' mechanical m<id>M:<code>:<min>:<max>:<spreadTenthsPct>
//                 -- drives the motor ~6 revolutions (can take 20-100s on a
//                    large reel), so it uses an async job like calibrate.
static volatile int           sfDiagQWaitId = -1;  // id the Q capture is waiting on
static volatile unsigned long sfDiagQTs     = 0;   // ready flag (0=none)
static volatile int           sfDiagQReset  = 0;
static volatile int           sfDiagQBoot   = 0;
static volatile int           sfDiagQVcc    = 0;
static volatile int           sfDiagQEe     = 0;
static volatile int           sfDiagQCur    = -1;
static volatile int           sfDiagMWaitId = -1;  // id the parser captures M for
static volatile unsigned long sfDiagMTs     = 0;   // M ready flag (0=none)
static volatile int           sfDiagMCode   = -1;
static volatile int           sfDiagMMin    = 0;
static volatile int           sfDiagMMax    = 0;
static volatile int           sfDiagMSpread = 0;
static volatile bool          sfDiagMJobActive = false;  // an M test is in flight
static volatile int           sfDiagMJobId     = -1;
static volatile unsigned long sfDiagMDeadline  = 0;      // millis() timeout
static volatile bool          sfModulesDirty   = false;  // pending NVS save
static volatile unsigned long sfModulesDirtyMs = 0;      // millis() when first dirtied
static bool          ntpSynced   = false; // declared early; also set in taskNetwork

/* ----------------------------------------------------------
   Persistent configuration stored in NVS
---------------------------------------------------------- */
Preferences prefs;


void cfgSetDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.mqttPort      = DEFAULT_MQTT_PORT;
  cfg.rs485Baud     = DEFAULT_BAUD;
  cfg.rs485DataBits = 8;
  cfg.rs485Parity   = 0;
  cfg.rs485StopBits = 1;
  strlcpy(cfg.mqttPrefix, DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));
  strlcpy(cfg.posixTZ, "UTC0", sizeof(cfg.posixTZ));
  strlcpy(cfg.ntpServer, DEFAULT_NTP_SERVER, sizeof(cfg.ntpServer));
  cfg.gridRows = 1;
  cfg.gridCols = 16;
  cfg.serialDebug = false;
  gSerialDebug    = false;
  cfg.haEnabled   = false;
  strlcpy(cfg.otaPassword, "", sizeof(cfg.otaPassword));
  strlcpy(gPosixTZ,    "UTC0", sizeof(gPosixTZ));
}

// Migrate settings from old NVS namespace "rs485gw" to "splitflap".
// Runs once after firmware update; no-op on subsequent boots.
void loadConfig() {
  prefs.begin("splitflap", true);
  strlcpy(cfg.wifiSSID,   prefs.getString("wSSID",  "").c_str(), sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPass,   prefs.getString("wPASS",  "").c_str(), sizeof(cfg.wifiPass));
  strlcpy(cfg.mqttHost,   prefs.getString("mqHost", "").c_str(), sizeof(cfg.mqttHost));
  cfg.mqttPort          = prefs.getInt   ("mqPort",  DEFAULT_MQTT_PORT);
  strlcpy(cfg.mqttUser,   prefs.getString("mqUser", "").c_str(), sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,   prefs.getString("mqPass", "").c_str(), sizeof(cfg.mqttPass));
  strlcpy(cfg.mqttPrefix, prefs.getString("mqPfx",  DEFAULT_MQTT_PREFIX).c_str(), sizeof(cfg.mqttPrefix));
  cfg.rs485Baud       = prefs.getULong("baud",    DEFAULT_BAUD);
  cfg.rs485DataBits   = prefs.getUChar("dbits",   8);
  cfg.rs485Parity     = prefs.getUChar("parity",  0);
  cfg.rs485StopBits   = prefs.getUChar("sbits",   1);
  strlcpy(cfg.posixTZ, prefs.getString("tz", "UTC0").c_str(), sizeof(cfg.posixTZ));
  strlcpy(cfg.ntpServer, prefs.getString("ntp", DEFAULT_NTP_SERVER).c_str(), sizeof(cfg.ntpServer));
  cfg.gridRows = (uint8_t)prefs.getInt("gRows", 1);
  cfg.gridCols = (uint8_t)prefs.getInt("gCols", 16);
  if (cfg.gridRows < 1) cfg.gridRows = 1;
  if (cfg.gridCols < 1) cfg.gridCols = 1;
  cfg.serialDebug = prefs.getBool("dbgSerial", false);
  gSerialDebug    = cfg.serialDebug;
  cfg.haEnabled   = prefs.getBool("haEnabled", false);
  strlcpy(cfg.otaPassword, prefs.getString("otaPass", "").c_str(), sizeof(cfg.otaPassword));
  strlcpy(gPosixTZ, cfg.posixTZ, sizeof(gPosixTZ));
  setenv("TZ", gPosixTZ, 1);
  tzset();
  prefs.end();
}

void saveConfig() {
  prefs.begin("splitflap", false);
  prefs.putString("wSSID",  cfg.wifiSSID);
  prefs.putString("ntp",    cfg.ntpServer);
  prefs.putInt   ("gRows",  cfg.gridRows);
  prefs.putInt   ("gCols",  cfg.gridCols);
  prefs.putString("wPASS",  cfg.wifiPass);
  prefs.putString("mqHost", cfg.mqttHost);
  prefs.putInt   ("mqPort", cfg.mqttPort);
  prefs.putString("mqUser", cfg.mqttUser);
  prefs.putString("mqPass", cfg.mqttPass);
  prefs.putString("mqPfx",  cfg.mqttPrefix);
  prefs.putULong ("baud",   cfg.rs485Baud);
  prefs.putUChar ("dbits",  cfg.rs485DataBits);
  prefs.putUChar ("parity", cfg.rs485Parity);
  prefs.putUChar ("sbits",  cfg.rs485StopBits);
  prefs.putString("tz",     cfg.posixTZ);
  prefs.putBool  ("dbgSerial", cfg.serialDebug);
  prefs.putBool  ("haEnabled", cfg.haEnabled);
  prefs.putString("otaPass",   cfg.otaPassword);
  prefs.end();
}

/* ----------------------------------------------------------
   Message ring buffer
---------------------------------------------------------- */
struct RS485Msg {
  unsigned long timestamp;
  char          dir;
  bool          sanitized;      // TX only: true if the gateway trimmed trailing junk past a complete command
  uint8_t       data[MSG_MAX_BYTES];
  size_t        len;
  char          wallTime[24];   // gateway-TZ string (MQTT / serial debug)
  unsigned long epoch;          // UTC epoch at capture (0 if RTC not valid);
                                // the web UI renders this in the BROWSER's
                                // local timezone -- no gateway TZ config needed
};

// Explicit prototypes - must appear after struct, before function bodies,
// to prevent the Arduino IDE preprocessor inserting them above the struct.
bool sfValidSN(const char* sn);
void ringPush(const RS485Msg& m);
String ringDrain();
void mqttPublishMsg(const RS485Msg& m);
static void mqttPublishStateTopics();          // HA entity state (display/maint/quiet)
static void mqttPublishDisplayState();         // current display string
static void haPublishDiscovery(bool enable);   // HA MQTT discovery (or removal)
void rs485Send(const uint8_t* data, size_t len, bool raw = false);
void sfQueryVersion(int addr);                 // forward (used by stale-probe)

// The bus-monitor ring (64 x ~296 bytes ~= 19 KB) lives in PSRAM, not internal
// RAM. It's a diagnostic log on no hot path, so the slightly slower PSRAM is
// fine, and freeing ~19 KB of internal DRAM gives the WiFi/TCP stack the
// headroom it needs during a large web-OTA upload (which otherwise exhausts the
// heap as receive buffers pile up). Allocated in setup() via ringInit(); falls
// back to internal RAM if PSRAM is unavailable. Never freed.
static RS485Msg*         msgRing       = NULL;
static volatile int      msgHead       = 0;
static volatile int      msgPollCursor = 0;
static StaticSemaphore_t msgMutexBuf;
static SemaphoreHandle_t msgMutex = NULL;

// Allocate the monitor ring (PSRAM preferred). Call once from setup() before
// any task that pushes to the ring is started.
// Allocate a large buffer in PSRAM (preferred) or internal RAM (fallback),
// zeroed. Logs where it landed. Returns NULL only if both allocations fail.
static void* psramAlloc(const char* name, size_t bytes) {
  void* p = NULL;
  if (psramFound()) p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (p) {
    printf("[MEM] %s in PSRAM (%u bytes)\n", name, (unsigned)bytes);
  } else {
    p = malloc(bytes);   // fallback: internal RAM
    printf("[MEM] %s in internal RAM (%u bytes)%s\n", name, (unsigned)bytes,
           psramFound() ? " -- PSRAM alloc failed" : " -- no PSRAM");
  }
  if (p) memset(p, 0, bytes);
  return p;
}

// Allocate the large runtime buffers in PSRAM to free internal RAM (which the
// WiFi/TCP stack needs during a web-OTA upload). Call once from setup() before
// any task or registry init touches these buffers. ~58 KB moved off internal
// RAM in total (monitor ring + MQTT queue + module registry).
static void psramAllocInit() {
  msgRing   = (RS485Msg*) psramAlloc("monitor ring", sizeof(RS485Msg) * MSG_RING_SIZE);
  mqttQueue = (MqttQItem*) psramAlloc("MQTT queue",   sizeof(MqttQItem) * MQTT_Q_SIZE);
  sfModules = (SFModule*) psramAlloc("module registry", sizeof(SFModule) * MAX_MODULES);
}

void ringPush(const RS485Msg& m) {
  if (!msgMutex || !msgRing) return;
  xSemaphoreTake(msgMutex, portMAX_DELAY);
  msgRing[msgHead] = m;
  msgHead = (msgHead + 1) % MSG_RING_SIZE;
  xSemaphoreGive(msgMutex);
}

String ringDrain() {
  if (!msgMutex || !msgRing) return "[]";
  xSemaphoreTake(msgMutex, portMAX_DELAY);
  int head = msgHead;
  xSemaphoreGive(msgMutex);

  String out;
  // Reserve for the actual pending count (~330 bytes/message worst case:
  // 256 escaped chars + JSON overhead). A fixed 512 caused repeated reallocs
  // after a burst filled the ring (up to 64 messages = ~21KB).
  int pending = (head - msgPollCursor + MSG_RING_SIZE) % MSG_RING_SIZE;
  out.reserve((size_t)pending * 330 + 16);
  out = "[";
  bool first = true;
  int i = msgPollCursor;
  while (i != head) {
    const RS485Msg& m = msgRing[i];
    if (!first) out += ',';
    first = false;
    // Stack buffer -- no heap allocation per message
    char ascii[MSG_MAX_BYTES * 2 + 1]; size_t ai = 0;
    for (size_t j = 0; j < m.len && ai < sizeof(ascii)-2; j++) {
      uint8_t b = m.data[j];
      if      (b == '\n' || b == '\r') { /* skip newlines */ }
      else if (b == '"'  ) { ascii[ai++] = '\\'; ascii[ai++] = '"';}  // escape quote
      else if (b == '\\' ) { ascii[ai++] = '\\'; ascii[ai++] = '\\';}// escape backslash
      else if (b >= 32 && b <= 126) { ascii[ai++] = (char)b; }
      else { ascii[ai++] = '.'; }
    }
    ascii[ai] = 0;
    out += "{\"ts\":";      out += m.timestamp;
    out += ",\"ep\":";      out += m.epoch;
    out += ",\"wt\":\"";    out += m.wallTime;  out += '"';
    out += ",\"dir\":\"";   out += m.dir;       out += '"';
    out += ",\"command\":\""; out += ascii;       out += '"';
    out += ",\"len\":";     out += m.len;
    if (m.sanitized) out += ",\"san\":1";
    out += '}';
    i = (i + 1) % MSG_RING_SIZE;
  }
  out += ']';
  msgPollCursor = head;
  return out;
}

/* ----------------------------------------------------------
   RS485 low-level
---------------------------------------------------------- */
HardwareSerial         rs485(1);
volatile unsigned long rxCount = 0;
volatile unsigned long txCount = 0;
volatile unsigned long sfParseRejects = 0;  // corrupt/garbled frames rejected at parse

static uint32_t buildSerialConfig() {
  struct Entry { uint8_t d, p, s; uint32_t c; };
  static const Entry table[] = {
    {8,0,1,SERIAL_8N1},{8,0,2,SERIAL_8N2},
    {8,1,1,SERIAL_8E1},{8,1,2,SERIAL_8E2},
    {8,2,1,SERIAL_8O1},{8,2,2,SERIAL_8O2},
    {7,0,1,SERIAL_7N1},{7,0,2,SERIAL_7N2},
    {7,1,1,SERIAL_7E1},{7,1,2,SERIAL_7E2},
    {7,2,1,SERIAL_7O1},{7,2,2,SERIAL_7O2},
    {6,0,1,SERIAL_6N1},{5,0,1,SERIAL_5N1},
  };
  for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
    if (table[i].d == cfg.rs485DataBits &&
        table[i].p == cfg.rs485Parity   &&
        table[i].s == cfg.rs485StopBits) return table[i].c;
  }
  return SERIAL_8N1;
}

void rs485Begin() {
  // Enlarge the UART RX buffer BEFORE begin(). The default is 256 bytes, but a
  // full EEPROM dump response is ~565 bytes and streams in over ~590ms at 9600
  // baud. If taskRS485 is briefly preempted by other core-0 work while the frame
  // arrives, a 256-byte buffer (plus the 128-byte HW FIFO) can overflow and the
  // driver silently DROPS bytes -- producing mid-frame corruption. A 1024-byte
  // buffer holds a full frame with margin. Must be set before begin() to apply.
  rs485.setRxBufferSize(1024);
  rs485.begin(cfg.rs485Baud, buildSerialConfig(), RS485_RX_PIN, RS485_TX_PIN);
  rs485.setPins(-1, -1, -1, RS485_EN_PIN);
  rs485.setMode(UART_MODE_RS485_HALF_DUPLEX);
  DBG("[RS485] baud=%lu\n", cfg.rs485Baud);
}

// Inspect an outbound frame and update per-module display tracking. This runs
// for EVERY transmitted frame (called from rs485Send), so a well-formed display
// command sent through a raw path -- the Bus Monitor "Send Frame" box, the
// /api/rs485/send endpoint, or the MQTT splitflap/send topic -- updates tracking
// exactly like the high-level helpers do. Recognized forms:
//   m<id>-<char>  / m*-<char>   show character (broadcast with '*')
//   m<id>+<idx>   / m*+<idx>    show flap index (char becomes unknown)
//   m<id>h        / m*h         home (char becomes unknown)
// Anything else (provisioning mX..., version/dump/tuning commands, responses)
// is ignored. addr -1 means broadcast. Takes sfMutex internally; never call
// while already holding it.
static void sfTrackFromFrame(const uint8_t* data, size_t len) {
  // Minimum "m" + addr + cmd = 3 chars (e.g. "m*h"). Must start with 'm'.
  if (len < 3 || data[0] != 'm') return;
  // The provisioning/by-serial frames all use a literal 'X' as the address
  // token (mXadv, mXack, mXI, mXH, mXD, mXW, mXF). Those never change a known
  // module's displayed character, so skip them outright.
  if (data[1] == 'X') return;

  size_t i = 1;
  int addr;
  if (data[i] == '*') {            // broadcast
    addr = -1;
    i++;
  } else if (data[i] >= '0' && data[i] <= '9') {
    long v = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
      v = v * 10 + (data[i] - '0');
      i++;
      if (v > 254) return;         // out of valid id range -> not a display cmd
    }
    addr = (int)v;
  } else {
    return;                        // not an address we recognize
  }
  if (i >= len) return;
  char cmd = (char)data[i];

  if (cmd == '-') {                // show character: next byte is the char
    if (i + 1 >= len) return;
    char c = (char)data[i + 1];
    if (c < 0x20 || c > 0x7E) return;
    sfTrackChar(addr, c);
  } else if (cmd == '+') {         // show index: record index, char unknown
    long idx = 0;
    size_t j = i + 1;
    if (j >= len || data[j] < '0' || data[j] > '9') return;  // need a number
    while (j < len && data[j] >= '0' && data[j] <= '9') {
      idx = idx * 10 + (data[j] - '0');
      j++;
      if (idx > 63) { idx = -1; break; }   // out of flap range -> unknown
    }
    if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (addr < 0) {
        for (int k = 0; k < sfModuleCount; k++) {
          if (sfModules[k].provisioned) {
            sfModules[k].flapIndex = (int)idx;
            sfModules[k].flapChar  = 0;
          }
        }
      } else {
        SFModule* m = sfFindById((uint8_t)addr);
        if (m) { m->flapIndex = (int)idx; m->flapChar = 0; }
      }
      xSemaphoreGive(sfMutex);
    }
  } else if (cmd == 'h' &&         // home: char becomes unknown.
             (i + 1 >= len || data[i + 1] == '\n' || data[i + 1] == '\r')) {
    // Guard against false matches: 'h' must be the whole command, not a prefix
    // of something else. (There is no other 'h...' display command, but this
    // keeps the matcher strict.)
    sfTrackChar(addr, 0);
  }
}

// Parse the address + command of an outbound frame for Quiet Time. Returns the
// command char (or 0 if not a normal addressed display frame) and sets *outAddr
// to the module id, or -1 for broadcast ('*'). Mirrors sfTrackFromFrame's
// address parsing. Used only to classify display-motion frames.
static char sfFrameCmd(const uint8_t* data, size_t len, int* outAddr) {
  *outAddr = -2;
  if (len < 3 || data[0] != 'm') return 0;
  if (data[1] == 'X') return 0;          // by-serial provisioning frame
  size_t i = 1;
  int addr;
  if (data[i] == '*') { addr = -1; i++; }
  else if (data[i] >= '0' && data[i] <= '9') {
    long v = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
      v = v * 10 + (data[i] - '0'); i++;
      if (v > 254) return 0;
    }
    addr = (int)v;
  } else return 0;
  if (i >= len) return 0;
  *outAddr = addr;
  return (char)data[i];
}

// True if the frame is normal display motion that Quiet Time should suppress:
// show character ('-'), show index ('+'), or home ('h'). Deliberate calibration
// moves (calibrate 'c', goto 'g', nudge 's') are intentionally NOT suppressed,
// since they only originate from an operator actively calibrating.
static bool sfFrameIsDisplayMotion(const uint8_t* data, size_t len) {
  int addr;
  char cmd = sfFrameCmd(data, len, &addr);
  if (cmd == 0) return false;
  if (cmd == '-' || cmd == '+') return true;
  if (cmd == 'h') {
    // 'h' must be the whole command (mXh / m*h), not a prefix of something else.
    size_t i = 1;
    if (data[i] == '*') i++;
    else { while (i < len && data[i] >= '0' && data[i] <= '9') i++; }
    size_t after = i + 1;   // byte after the 'h'
    if (after >= len || data[after] == '\n' || data[after] == '\r') return true;
  }
  return false;
}

// Remember the display the host requested while Quiet Time is on, so the reels
// can resync when it turns off. Only show-char/show-index frames carry display
// intent worth replaying; home is suppressed but not queued.
static void sfQuietCapturePending(const uint8_t* data, size_t len) {
  int addr;
  char cmd = sfFrameCmd(data, len, &addr);
  size_t i = 1; if (data[i]=='*') i++; else { while (i<len && data[i]>='0' && data[i]<='9') i++; }
  if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (cmd == '-') {
    if (i + 1 < len) {
      char c = (char)data[i + 1];
      if (addr < 0) {
        for (int k = 0; k < sfModuleCount; k++)
          if (sfModules[k].provisioned) { sfModules[k].pendChar=c; sfModules[k].pendIndex=-1; sfModules[k].hasPend=true; }
      } else {
        SFModule* m = sfFindById((uint8_t)addr);
        if (m) { m->pendChar=c; m->pendIndex=-1; m->hasPend=true; }
      }
    }
  } else if (cmd == '+') {
    long idx = 0; size_t j = i + 1; bool got=false;
    while (j < len && data[j] >= '0' && data[j] <= '9') { idx = idx*10 + (data[j]-'0'); j++; got=true; if (idx>63){idx=-1;break;} }
    if (got) {
      if (addr < 0) {
        for (int k = 0; k < sfModuleCount; k++)
          if (sfModules[k].provisioned) { sfModules[k].pendChar=0; sfModules[k].pendIndex=(int)idx; sfModules[k].hasPend=true; }
      } else {
        SFModule* m = sfFindById((uint8_t)addr);
        if (m) { m->pendChar=0; m->pendIndex=(int)idx; m->hasPend=true; }
      }
    }
  }
  xSemaphoreGive(sfMutex);
}

// True iff `data[0..len)` (caller strips trailing CR/LF first) is a DIRECT,
// numeric-id firmware-version query "m<id>v" with no payload after the 'v'.
// This is the one frame the gateway must transmit WITHOUT a newline terminator
// (see sfQueryVersion() for the half-duplex turnaround reason). Broadcast "m*v",
// by-serial "mX.." frames, and anything with bytes after the 'v' are NOT matched
// and so keep their terminator.
static bool sfIsDirectVersionQuery(const uint8_t* data, size_t len) {
  int addr;
  if (sfFrameCmd(data, len, &addr) != 'v') return false;   // command must be 'v'
  if (addr < 0) return false;                              // numeric id only (reject m*v)
  size_t i = 1;                                            // locate the command char:
  while (i < len && data[i] >= '0' && data[i] <= '9') i++; //   skip 'm', then the digits
  return (i + 1 == len);                                   // 'v' must be the final byte
}

// Given a frame `data[0..len)` (caller strips trailing CR/LF first), return the
// length of the longest prefix that forms a COMPLETE, well-formed known command.
// Bytes beyond that are extraneous and the caller may trim them -- so "m4vDSassa"
// collapses to "m4v" instead of leaning on the module to ignore the junk. This is
// grammar ENFORCEMENT, not guessing: each command's payload shape is fixed by the
// (frozen) protocol. Frames we can't model confidently -- by-serial "mX.." frames
// and any unrecognized command char -- return the full length UNCHANGED, so a
// long restore map or a future command is never truncated. The raw-send bypass
// skips this entirely.
static size_t sfKnownCommandLen(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'm') return len;        // not an m-frame: leave as-is
  if (data[1] == 'X') return len;                   // by-serial frame: pass through untouched
  size_t i = 1;
  bool wildcard = false;
  if (data[i] == '*') {                             // wildcard address
    wildcard = true;
    i++;
  } else if (data[i] >= '0' && data[i] <= '9') {    // numeric id (0..254)
    long v = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') { v = v*10 + (data[i]-'0'); i++; }
    if (v > 254) return len;                         // invalid id: don't touch
  } else {
    return len;                                      // malformed address: leave as-is
  }
  if (i >= len) return len;                           // no command char yet: leave as-is
  char cmd = (char)data[i];
  i++;                                                // consume the command char
  switch (cmd) {
    // Zero-payload commands: complete the instant the command char is read.
    case 'h': case 'c': case 'd':
    case 'e': case 'R': case 'F':
      return i;                                       // trim anything after
    // Version query and combined all-fields dump ('A', v25+): zero-payload when
    // addressed by a numeric id, but a wildcard broadcast may carry an optional
    // "<lo>-<hi>" range (m*v0-49 / m*A0-49) -- pass those through untrimmed.
    case 'v': case 'A':
      return wildcard ? len : i;
    // Show one character: keep exactly one payload byte.
    case '-':
      if (i < len) i++;
      return i;
    // Numeric-payload commands: keep the leading run of digits.
    case '+': case 'o': case 't': case 's':
    case 'g': case 'a': case 'i': {
      size_t d = i;
      while (d < len && data[d] >= '0' && data[d] <= '9') d++;
      return (d == i) ? len : d;                      // no digits where expected: leave as-is
    }
    // Write calibrated position "<index>:<pos>" -- two numeric fields.
    case 'w': {
      size_t d = i;
      while (d < len && data[d] >= '0' && data[d] <= '9') d++;        // index
      if (d == i || d >= len || data[d] != ':') return len;          // malformed: leave as-is
      d++;                                                            // ':'
      size_t p = d;
      while (d < len && data[d] >= '0' && data[d] <= '9') d++;        // position
      return (d == p) ? len : d;                                      // no position digits: leave
    }
    default:
      return len;                                     // unknown command: pass through
  }
}

void rs485Send(const uint8_t* data, size_t len, bool raw) {
  if (!len || len > TX_MAX_BYTES) return;

  // --- Wire framing + sanitization (single choke point for every send path) --
  // Normal path: the gateway owns wire correctness so callers never have to:
  //   1) strip any trailing CR/LF the caller supplied,
  //   2) trim anything past a complete, well-formed known command, so a stray
  //      "m4vDSassa" becomes "m4v" rather than relying on the module to ignore
  //      the junk (see sfKnownCommandLen), then
  //   3) re-add exactly one '\n' terminator -- EXCEPT a direct numeric-id version
  //      query "m<id>v", which must ship bare to dodge the half-duplex turnaround
  //      collision (see sfQueryVersion). So "m1v", "m1v\n", "m1v\r\n", and even
  //      "m1vJUNK" all leave as bare "m1v", while "m5-A" and "m9o2832" leave
  //      correctly newline-terminated (which also spares payload commands the
  //      module's 50 ms idle-timeout wait).
  // Raw path (raw==true -- the Bus Monitor "Raw" toggle, or {"raw":true} on the
  // REST/MQTT send): transmit the caller's exact bytes verbatim, with no trim and
  // no terminator change -- a deliberate debugging escape hatch. Bus-collision
  // guarding, Quiet Time, tracking, logging, and the monitor ring still apply.
  size_t bare;
  bool   appendNL;
  bool   sanitized = false;     // true if sfKnownCommandLen trimmed trailing junk
  if (raw) {
    bare     = len;     // verbatim -- no stripping, no sanitizing
    appendNL = false;   // no terminator added
  } else {
    bare = len;
    while (bare > 0 && (data[bare-1] == '\n' || data[bare-1] == '\r')) bare--;
    if (!bare) return;                                  // nothing but terminators
    size_t preSan = bare;
    bare      = sfKnownCommandLen(data, bare);          // trim trailing junk
    sanitized = (bare < preSan);                        // bytes past a complete command were dropped
    appendNL  = !sfIsDirectVersionQuery(data, bare);    // version query => ship bare
  }
  if (!bare) return;

  // Quiet Time: swallow normal display-motion frames so the flaps stay still.
  // The request is acknowledged (we return as if sent) and the desired display
  // is remembered for resync; nothing reaches the bus and tracking is unchanged.
  if (gQuietTime && sfFrameIsDisplayMotion(data, bare)) {
    sfQuietCapturePending(data, bare);
    DBG("[QUIET] suppressed display frame (%u bytes)\n", (unsigned)bare);
    return;
  }
  // Collision avoidance on the half-duplex bus: if modules are mid-response
  // (e.g. the staggered reply train after a broadcast m*v), transmitting now
  // would fight their drivers, corrupting bytes and destroying the newline
  // terminators (observed as glued/garbled frames and poisoned serial numbers).
  // Hold off until the bus has been quiet for TX_BUS_GUARD_MS, bounded by
  // TX_BUS_WAIT_CAP_MS so we always make progress.
  //
  // From here through the monitor-ring push is the bus-touching critical section.
  // txMutex serializes it across taskWeb / taskNetwork / taskRS485 so two senders
  // can't interleave bytes on the UART. There are no early returns inside, so the
  // mutex is always released. (Held across sfTrackFromFrame/ringPush, which take
  // sfMutex/msgMutex -- order is txMutex -> {sfMutex,msgMutex}, never inverted.)
  if (txMutex) xSemaphoreTake(txMutex, portMAX_DELAY);
  {
    unsigned long waitStart = millis();
    while (millis() - gLastRxMs < TX_BUS_GUARD_MS &&
           millis() - waitStart < TX_BUS_WAIT_CAP_MS) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  rs485.write(data, bare);                                  // the bare command...
  if (appendNL) { const uint8_t nl = '\n'; rs485.write(&nl, 1); }  // ...+ terminator unless version query
  rs485.flush();
  txCount++;
  // Update per-module display tracking from this frame. Doing it here -- the
  // single point every outbound frame passes through -- means raw sends are
  // tracked exactly like the high-level helpers, with no per-path duplication.
  sfTrackFromFrame(data, bare);
  gDisplayDirty = true;   // HA display sensor refresh (network task, rate-limited)
  // Log the transmitted frame (the command without a trailing terminator, for
  // readability -- the raw path may carry one). The monitor ring below keeps the
  // exact on-wire bytes. Cap the debug buffer at MSG_MAX_BYTES; long frames truncate.
  { char dbg[MSG_MAX_BYTES];
    size_t dlen = bare;
    while (dlen > 0 && (data[dlen-1] == '\n' || data[dlen-1] == '\r')) dlen--;
    if (dlen > sizeof(dbg) - 1) dlen = sizeof(dbg) - 1;
    memcpy(dbg, data, dlen); dbg[dlen] = '\0';
    DBG("[TX] %s%s\n", dbg, sanitized ? "  (sanitized)" : ""); }
  RS485Msg m;
  m.timestamp = millis();
  m.dir = 'T';
  m.sanitized = sanitized;
  // Reconstruct the on-wire frame for the monitor ring (bare command plus the
  // terminator we actually sent), bounded to MSG_MAX_BYTES. This keeps TX ring
  // entries consistent with RX entries, which carry their own '\n'.
  size_t ringLen = (bare > MSG_MAX_BYTES) ? MSG_MAX_BYTES : bare;
  memcpy(m.data, data, ringLen);
  if (appendNL && ringLen < MSG_MAX_BYTES) m.data[ringLen++] = '\n';
  m.len = ringLen;
  rtcFormatTime(m.wallTime, sizeof(m.wallTime));
  m.epoch = rtcEpochNow();   // UTC epoch; web UI renders in browser-local time
  ringPush(m);
  mqttPublishMsg(m);
  if (txMutex) xSemaphoreGive(txMutex);
}

// Send a null-terminated ASCII string on RS485
void rs485SendStr(const char* s) {
  rs485Send((const uint8_t*)s, strlen(s));
}

/* ----------------------------------------------------------
   Split-flap protocol helpers
---------------------------------------------------------- */

// Find or create a module registry entry by ID
static SFModule* sfFindById(uint8_t id) {
  for (int i = 0; i < sfModuleCount; i++)
    if (sfModules[i].id == id) return &sfModules[i];
  return NULL;
}

// Find module by serial number
static SFModule* sfFindBySN(const char* sn) {
  for (int i = 0; i < sfModuleCount; i++)
    if (strcmp(sfModules[i].serialNum, sn) == 0) return &sfModules[i];
  return NULL;
}

// A serial number is a module's true identity, but its id changes on
// (de)provision. A stale in-flight frame carrying an old id can briefly create a
// second registry entry for a serial we already track (seen as duplicate cards
// in the grid after a deprovision). This collapses any extra entries sharing a
// serial down to the FIRST one, keeping the registry's "one serial = one module"
// invariant. Caller MUST already hold sfMutex. Returns true if anything changed.
static bool sfDedupeBySNLocked(const char* sn) {
  if (!sn || !sn[0]) return false;
  int first = -1;
  bool changed = false;
  for (int i = 0; i < sfModuleCount; ) {
    if (strcmp(sfModules[i].serialNum, sn) == 0) {
      if (first < 0) { first = i; i++; continue; }
      // Duplicate: drop it (shift the tail down, stable order).
      for (int j = i; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
      sfModuleCount--;
      memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
      changed = true;
    } else i++;
  }
  return changed;
}

// Add or update a module entry
static SFModule* sfUpsert(uint8_t id, const char* sn) {
  SFModule* m = (id != 255) ? sfFindById(id) : sfFindBySN(sn);
  bool isNew = false;
  if (!m) {
    if (sfModuleCount >= MAX_MODULES) return NULL;
    m = &sfModules[sfModuleCount++];
    memset(m, 0, sizeof(SFModule));
    m->id = id;
    m->flapIndex = -1;
    m->flapChar  = 0;
    isNew = true;
  }
  if (sn && sn[0]) strlcpy(m->serialNum, sn, sizeof(m->serialNum));
  m->lastSeen = millis();
  m->probeMs  = 0;   // module is transmitting -> cancel any pending stale probe
  unsigned long ep = rtcEpochNow();
  if (ep) m->lastSeenEpoch = ep;
  if (isNew) sfModulesDirty = true;  // new module -> persist
  return m;
}

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
// the configuration block at the top.)
// ------------------------------------------------------------------
struct PersistedModule {
  uint8_t       id;
  char          serialNum[21];
  bool          provisioned;
  bool          acked;          // provisioning-confirmed (never legacy)
  char          fwVersion[8];
  unsigned long lastSeenEpoch;
};

struct ModulesFileHeader {
  unsigned long magic;   // MODULES_MAGIC
  int           count;   // number of PersistedModule records following
};

static bool sfFsReady = false;   // set true once FFat is mounted

// Mount the FATFS partition. Format on first use if needed.
static void sfFsInit() {
  // Try to mount WITHOUT auto-format first (fast path on every normal boot).
  if (FFat.begin(false)) {
    sfFsReady = true;
    DBG("[MOD] FATFS mounted (%lu KB free)\n",
        (unsigned long)(FFat.freeBytes() / 1024));
    return;
  }
  // First boot after flashing: the partition is unformatted. Formatting a
  // ~10MB partition is a long blocking flash operation -- log it clearly so
  // the delay is expected, and so the watchdog boot grace period covers it.
  printf("[MOD] FATFS not formatted -- formatting now (one-time, may take a while)...\n");
  if (FFat.begin(true)) {       // true = format if mount fails
    sfFsReady = true;
    printf("[MOD] FATFS formatted and mounted (%lu KB free)\n",
           (unsigned long)(FFat.freeBytes() / 1024));
  } else {
    sfFsReady = false;
    printf("[MOD] FATFS mount/format failed -- module persistence disabled\n");
  }
}

// Save the current registry to the FATFS file.
static void sfModulesSave() {
  if (!sfFsReady) return;
  // Build a compact array of durable records under sfMutex.
  static PersistedModule recs[MAX_MODULES];  // static: avoid large stack frame
  int n = 0;
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < sfModuleCount && n < MAX_MODULES; i++) {
    const SFModule& m = sfModules[i];
    recs[n].id            = m.id;
    strlcpy(recs[n].serialNum, m.serialNum, sizeof(recs[n].serialNum));
    recs[n].provisioned   = m.provisioned;
    recs[n].acked         = m.acked;
    strlcpy(recs[n].fwVersion, m.fwVersion, sizeof(recs[n].fwVersion));
    recs[n].lastSeenEpoch = m.lastSeenEpoch;
    n++;
  }
  if (sfMutex) xSemaphoreGive(sfMutex);

  // Write to a temp file then rename, so a crash mid-write can't corrupt
  // the existing good copy.
  File f = FFat.open(MODULES_FILE ".tmp", "w");
  if (!f) { DBG("[MOD] open for write failed\n"); return; }
  ModulesFileHeader hdr = { MODULES_MAGIC, n };
  f.write((const uint8_t*)&hdr, sizeof(hdr));
  if (n > 0) f.write((const uint8_t*)recs, n * sizeof(PersistedModule));
  f.close();
  FFat.remove(MODULES_FILE);
  FFat.rename(MODULES_FILE ".tmp", MODULES_FILE);
  DBG("[MOD] Saved %d modules to FATFS\n", n);
}

// Load persisted modules from the FATFS file at boot, pruning stale entries.
static void sfModulesLoad() {
  if (!sfFsReady) return;
  if (!FFat.exists(MODULES_FILE)) { DBG("[MOD] no saved module file\n"); return; }
  File f = FFat.open(MODULES_FILE, "r");
  if (!f) { DBG("[MOD] open for read failed\n"); return; }

  ModulesFileHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.magic != MODULES_MAGIC || hdr.count <= 0 || hdr.count > MAX_MODULES) {
    DBG("[MOD] bad/empty module file -- skipping\n");
    f.close();
    return;
  }
  static PersistedModule recs[MAX_MODULES];
  size_t want = (size_t)hdr.count * sizeof(PersistedModule);
  size_t got  = f.read((uint8_t*)recs, want);
  f.close();
  if (got != want) {
    DBG("[MOD] file size mismatch (%u != %u) -- skipping\n",
        (unsigned)got, (unsigned)want);
    return;
  }

  unsigned long nowEp = rtcEpochNow();  // 0 if RTC not yet valid
  int loaded = 0, pruned = 0;
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < hdr.count && sfModuleCount < MAX_MODULES; i++) {
    // Prune entries older than MODULE_STALE_SECS (only when we have a
    // valid clock AND a recorded epoch to compare against).
    if (nowEp && recs[i].lastSeenEpoch &&
        nowEp > recs[i].lastSeenEpoch &&
        (nowEp - recs[i].lastSeenEpoch) > MODULE_STALE_SECS) {
      pruned++;
      continue;
    }
    // Skip records whose SN fails validation: a bus collision before the
    // validation fix could have persisted a garbage SN (e.g. a glued frame
    // tail). Dropping the record here HEALS the registry -- the module
    // re-registers with its correct SN on its next version response.
    {
      char snChk[21];
      strlcpy(snChk, recs[i].serialNum, sizeof(snChk));
      if (snChk[0] && !sfValidSN(snChk)) {
        DBG("[MOD] dropping persisted record id=%d with corrupt SN\n", recs[i].id);
        pruned++;
        continue;
      }
    }
    SFModule* m = &sfModules[sfModuleCount++];
    memset(m, 0, sizeof(SFModule));
    m->id            = recs[i].id;
    strlcpy(m->serialNum, recs[i].serialNum, sizeof(m->serialNum));
    m->provisioned   = recs[i].provisioned;
    m->acked         = recs[i].acked;
    strlcpy(m->fwVersion, recs[i].fwVersion, sizeof(m->fwVersion));
    m->lastSeenEpoch = recs[i].lastSeenEpoch;
    m->flapIndex     = -1;
    m->flapChar      = 0;
    m->lastSeen      = 0;   // not seen yet this boot
    loaded++;
  }
  if (sfMutex) xSemaphoreGive(sfMutex);
  DBG("[MOD] Loaded %d modules from FATFS (%d pruned as stale)\n", loaded, pruned);
}

// Wipe both the in-memory registry and the persisted file.
static void sfModulesClear() {
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  sfModuleCount = 0;
  memset(sfModules, 0, sizeof(SFModule) * MAX_MODULES);
  if (sfMutex) xSemaphoreGive(sfMutex);
  if (sfFsReady) FFat.remove(MODULES_FILE);
  sfModulesDirty = false;
  DBG("[MOD] Registry cleared (memory + FATFS)\n");
}

// Two-phase stale handling, called periodically:
//   Phase 1: a module unseen for MODULE_STALE_SECS that isn't being probed is
//            sent a version query (m<id>v) and given MODULE_PROBE_GRACE_MS to
//            reply. A reply (sfTouch) clears the probe and keeps the module.
//   Phase 2: a probed module whose grace window has elapsed without a reply is
//            actually dropped. This avoids evicting a module that has merely
//            been quiet (modules only speak when addressed).
// Version queries are sent OUTSIDE sfMutex (rs485Send re-takes the lock via
// frame tracking, so probing under it would deadlock): IDs to probe are
// collected under the lock, then queried after release.
static void sfModulesPruneStale() {
  unsigned long nowEp = rtcEpochNow();
  if (!nowEp) return;  // no valid clock yet
  unsigned long nowMs = millis();
  bool changed = false;
  static uint8_t toProbe[MAX_MODULES];
  int probeN = 0;

  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < sfModuleCount; ) {
    SFModule& m = sfModules[i];
    bool stale = (m.lastSeenEpoch && nowEp > m.lastSeenEpoch &&
                  (nowEp - m.lastSeenEpoch) > MODULE_STALE_SECS);
    if (stale && m.probeMs == 0) {
      // Phase 1: start a probe -- give it a chance to answer before dropping.
      m.probeMs = nowMs + MODULE_PROBE_GRACE_MS;
      if (probeN < MAX_MODULES) toProbe[probeN++] = m.id;
      i++;
    } else if (stale && m.probeMs != 0 && nowMs >= m.probeMs) {
      // Phase 2: probed and the grace window elapsed with no reply -> drop it.
      for (int j = i; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
      sfModuleCount--;
      memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
      changed = true;
    } else {
      i++;   // fresh, or probe still pending -> leave it
    }
  }
  if (sfMutex) xSemaphoreGive(sfMutex);

  // Send the probes now that the lock is released.
  for (int i = 0; i < probeN; i++) {
    DBG("[MOD] stale module %d -- probing before drop\n", toProbe[i]);
    sfQueryVersion(toProbe[i]);
  }
  if (changed) sfModulesDirty = true;
}

/* ----------------------------------------------------------
   Send split-flap commands
   All generate the ASCII bus protocol and call rs485SendStr()
---------------------------------------------------------- */

// Display a character on one module.  addr=-1 = broadcast.
// The gateway does NOT translate the character to a flap index -- the module
// firmware does that itself. We only ensure it is a printable ASCII byte and
// uppercase it (the flap set is uppercase), then send m<id>-<char> verbatim.
// Record the character a module is now displaying. addr<0 means a broadcast
// was sent, so every known module shows the same character -- update them all.
// Pass c=0 to mark the displayed character as unknown (e.g. after a home, when
// the module has left its previous flap but we can't name the new one without
// the module's flap table). flapIndex is cleared because the gateway tracks the
// character, not the index, on the char path. Caller must already hold sfMutex.
static void sfTrackCharLocked(int addr, char c) {
  if (addr < 0) {
    for (int i = 0; i < sfModuleCount; i++) {
      if (sfModules[i].provisioned) {
        sfModules[i].flapChar  = c;
        sfModules[i].flapIndex = -1;
      }
    }
  } else {
    SFModule* m = sfFindById((uint8_t)addr);
    if (m) { m->flapChar = c; m->flapIndex = -1; }
  }
}

// Mutex-wrapping convenience for callers that are not already holding sfMutex.
static void sfTrackChar(int addr, char c) {
  if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    sfTrackCharLocked(addr, c);
    xSemaphoreGive(sfMutex);
  }
}

void sfSendChar(int addr, char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');  // normalize to uppercase
  if (c < 0x20 || c > 0x7E) return;                     // reject non-printable / non-ASCII
  char buf[24];
  if (addr < 0)
    snprintf(buf, sizeof(buf), "m*-%c\n", c);
  else
    snprintf(buf, sizeof(buf), "m%d-%c\n", addr, c);
  rs485SendStr(buf);
  // Display tracking is handled centrally in rs485Send via sfTrackFromFrame,
  // so every path (including raw frame sends) is covered uniformly.
}

// Display by flap index.  addr=-1 = broadcast.
void sfSendIndex(int addr, int idx) {
  char buf[24];
  if (addr < 0)
    snprintf(buf, sizeof(buf), "m*+%d\n", idx);
  else
    snprintf(buf, sizeof(buf), "m%d+%d\n", addr, idx);
  rs485SendStr(buf);
  // Display tracking handled centrally in rs485Send via sfTrackFromFrame.
}

void sfHomeOffset(int addr, int steps) {
  char buf[32];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*o%d\n", steps);
  else           snprintf(buf, sizeof(buf), "m%do%d\n", addr, steps);
  rs485SendStr(buf);
}
void sfSetTotalSteps(int addr, int steps) {
  char buf[32];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*t%d\n", steps);
  else           snprintf(buf, sizeof(buf), "m%dt%d\n", addr, steps);
  rs485SendStr(buf);
}
void sfNudge(int addr, int steps) {
  char buf[32];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*s%d\n", steps);
  else           snprintf(buf, sizeof(buf), "m%ds%d\n", addr, steps);
  rs485SendStr(buf);
}
void sfGoto(int addr, int step) {
  char buf[32];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*g%d\n", step);
  else           snprintf(buf, sizeof(buf), "m%dg%d\n", addr, step);
  rs485SendStr(buf);
}
void sfWritePos(int addr, int idx, int pos) {
  char buf[32];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*w%d:%d\n", idx, pos);
  else           snprintf(buf, sizeof(buf), "m%dw%d:%d\n", addr, idx, pos);
  rs485SendStr(buf);
}
void sfAutoHome(int addr, int enable) {
  char buf[24];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*a%d\n", enable ? 1 : 0);
  else           snprintf(buf, sizeof(buf), "m%da%d\n", addr, enable ? 1 : 0);
  rs485SendStr(buf);
}
void sfErase(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*e\n");
  else           snprintf(buf, sizeof(buf), "m%de\n", addr);
  rs485SendStr(buf);
}
void sfFactoryReset(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*F\n");
  else           snprintf(buf, sizeof(buf), "m%dF\n", addr);
  rs485SendStr(buf);
}
// Provisioning variants (address mX by serial number)
void sfDumpBySN(const char* sn) {
  char buf[64];
  snprintf(buf, sizeof(buf), "mXD%s\n", sn);
  rs485SendStr(buf);
}
void sfFactoryResetBySN(const char* sn) {
  char buf[64];
  snprintf(buf, sizeof(buf), "mXF%s\n", sn);
  rs485SendStr(buf);
}
void sfRestoreBySN(const char* payload) {
  // payload is the full mXW... command string (caller builds it)
  rs485SendStr(payload);
}

// Home one module or all (addr=-1)
void sfHome(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*h\n");
  else          snprintf(buf, sizeof(buf), "m%dh\n", addr);
  rs485SendStr(buf);
  // Display tracking handled centrally in rs485Send via sfTrackFromFrame
  // (home -> displayed character becomes unknown).
}

// Calibrate one module or all
void sfCalibrate(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*c\n");
  else          snprintf(buf, sizeof(buf), "m%dc\n", addr);
  rs485SendStr(buf);
}

// Query firmware version of a DIRECT (single-id, never broadcast) module.
//
// NOTE: the omitted trailing '\n' is NOT a module protocol requirement. The
// module accepts BOTH "m<id>v" and "m<id>v\n" and answers either one -- it acts
// on the 'v' byte itself. We drop the newline to work around a GATEWAY-SIDE
// limitation in our half-duplex bus turnaround, as follows:
//
// A module answers a direct version query SYNCHRONOUSLY, the instant it parses
// the 'v' byte -- with zero assembly delay (unlike a dump, which builds its
// EEPROM string before raising DE, so its reply is naturally late enough to be
// safe). Our problem is on the gateway: the ESP32's hardware-managed DE keeps us
// driving the line until the WHOLE frame has clocked out, and our UART receiver
// is off while we transmit, so we can't release the bus and flip to RX fast
// enough to catch that immediate reply. A trailing '\n' keeps us transmitting
// for one extra byte-time (~1 ms at 9600 baud) AFTER the module has already
// started replying -- the reply's leading bytes land while we're still driving
// the line (and deaf), so they're lost and no [RX] frame is ever assembled. That
// was the long-standing "version query gets no reply, but a dump right after
// works" symptom -- a gateway turnaround race, not a module quirk.
//
// Dropping the newline shortens our transmit window so we've already released
// the bus and are listening by the time the module answers (verified on
// hardware: a newline-less send always replies; the byte-identical "m<id>v\n"
// never did). The BROADCAST "m*v\n" path is separate and keeps its newline: a
// wildcard query collects an optional ID range and the module fires a staggered,
// DEFERRED reply on the '\n' (or a 50 ms idle timeout), so there is no
// turnaround race there. sfQueryVersion is only ever called with a concrete id.
//
// (Belt and suspenders: rs485Send() now normalizes framing for EVERY sender, so
// even a hand-typed "m<id>v\n" from the bus monitor or a REST/MQTT raw send is
// shipped bare. This helper still emits the bare form directly to document intent.)
void sfQueryVersion(int addr) {
  char buf[16];
  snprintf(buf, sizeof(buf), "m%dv", addr);   // no '\n' -- gateway turnaround workaround, see note
  rs485SendStr(buf);
}

// Arm the shared dump-capture slot for `id`, send `frame`, then wait up to
// `timeoutMs` for the module's reply -- a 'd' or 'A' response that sfParseResponse
// stores in sfDumpCapture. Returns true and copies the captured dump into `out`
// if a reply arrived. Runs on taskWeb; touches wdgWebMs so a long wait is
// watchdog-safe. The capture slot is single-use, which is safe because the
// synchronous web server serves one request at a time.
static bool sfSendAndCaptureDump(int id, const char* frame, unsigned long timeoutMs,
                                 char* out, size_t outLen) {
  sfDumpCapture[0] = 0;
  sfDumpCaptureTs  = 0;
  sfCaptureAutoHome = sfCaptureCurIndex = sfCaptureReportedId = -99;  // A-only; cleared per attempt
  sfDumpWaitId     = id;
  rs485SendStr(frame);
  unsigned long deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    wdgWebMs = millis();
    vTaskDelay(pdMS_TO_TICKS(10));
    if (sfDumpCaptureTs != 0) { strlcpy(out, sfDumpCapture, outLen); return true; }
  }
  return false;
}

// Send a 'Q' diagnostics snapshot to `id` and wait up to `timeoutMs` for the
// instant reply (no motor movement). Result lands in the sfDiagQ* globals via
// sfParseResponse. Returns true on a reply. Runs on taskWeb; watchdog-safe.
static bool sfSendAndCaptureQ(int id, unsigned long timeoutMs) {
  sfDiagQTs     = 0;
  sfDiagQWaitId = id;
  char frame[16];
  snprintf(frame, sizeof(frame), "m%dQ\n", id);
  rs485SendStr(frame);
  unsigned long deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    wdgWebMs = millis();
    vTaskDelay(pdMS_TO_TICKS(10));
    if (sfDiagQTs != 0) { sfDiagQWaitId = -1; return true; }
  }
  sfDiagQWaitId = -1;
  return false;
}

// Send a direct version query to `id` and wait up to `timeoutMs` for the reply
// to land in the registry (lastSeen advances AND fwVersion populated). On a
// reply, fills any non-NULL out params and returns true. Runs on taskWeb.
static bool sfSendVersionAndWait(int id, unsigned long timeoutMs,
                                 char* fwOut, size_t fwLen,
                                 char* snOut, size_t snLen,
                                 unsigned long* lastSeenOut) {
  unsigned long seenBefore = 0;
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* mb = sfFindById((uint8_t)id);
  if (mb) seenBefore = mb->lastSeen;
  xSemaphoreGive(sfMutex);
  sfQueryVersion(id);   // bare 'v' -- collision-safe
  unsigned long deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    wdgWebMs = millis();
    vTaskDelay(pdMS_TO_TICKS(10));
    bool got = false;
    xSemaphoreTake(sfMutex, portMAX_DELAY);
    SFModule* mx = sfFindById((uint8_t)id);
    if (mx && mx->lastSeen != seenBefore && mx->fwVersion[0]) {
      if (fwOut) strlcpy(fwOut, mx->fwVersion, fwLen);
      if (snOut) strlcpy(snOut, mx->serialNum, snLen);
      if (lastSeenOut) *lastSeenOut = mx->lastSeen;
      got = true;
    }
    xSemaphoreGive(sfMutex);
    if (got) return true;
  }
  return false;
}

// Set module ID (address it by current ID, give it a new one)
void sfSetId(int currentAddr, int newId) {
  char buf[24];
  snprintf(buf, sizeof(buf), "m%di%d\n", currentAddr, newId);
  rs485SendStr(buf);
  SFModule* m = sfFindById((uint8_t)currentAddr);
  if (m) m->id = (uint8_t)newId;
}

// Provisioning: assign ID to unprovisioned module by serial number
// Deprovision: send reset command to one module (by ID) or all
void sfDeprovision(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*R\n");
  else          snprintf(buf, sizeof(buf), "m%dR\n", addr);
  rs485SendStr(buf);
  // Remove from local registry. Shift the tail down (stable order) rather than
  // swapping the last entry into the gap, so the Modules grid keeps a consistent
  // order after a deprovision -- matching sfModulesPruneStale's removal.
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  if (addr < 0) {
    sfModuleCount = 0;
    memset(sfModules, 0, sizeof(SFModule) * MAX_MODULES);
  } else {
    for (int i = 0; i < sfModuleCount; i++) {
      if (sfModules[i].id == (uint8_t)addr) {
        for (int j = i; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
        sfModuleCount--;
        memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
        break;
      }
    }
  }
  xSemaphoreGive(sfMutex);
  sfModulesDirty = true;   // persist the removal
}

void sfProvision(const char* sn, int newId) {
  char buf[48];
  snprintf(buf, sizeof(buf), "mXI%s:%d\n", sn, newId);
  rs485SendStr(buf);
}

// Home a module by serial number
void sfHomeBySN(const char* sn) {
  char buf[48];
  snprintf(buf, sizeof(buf), "mXH%s\n", sn);
  rs485SendStr(buf);
}

// Set auto-home flag
void sfSetAutoHome(int addr, bool enable) {
  char buf[20];
  snprintf(buf, sizeof(buf), "m%da%d\n", addr, enable ? 1 : 0);
  rs485SendStr(buf);
}

// Factory reset a module (preserves ID)
// Send a text string across a sequence of module IDs starting at startAddr.
// Each character is sent to startAddr, startAddr+1, ... up to strlen(text).
void sfSendText(int startAddr, const char* text, bool blankUnused) {
  size_t len = strlen(text);
  for (size_t i = 0; i < len; i++) {
    // sfSendChar uppercases and rejects non-printable bytes itself; the module
    // firmware maps the character to a flap index. The gateway does not need
    // its own character table. A non-ASCII byte is simply dropped by sfSendChar.
    sfSendChar((int)(startAddr + i), text[i]);
    delay(10); // inter-message gap to avoid bus collision
  }
  // Optionally blank any previously-set modules beyond the text length
  // (caller passes blankUnused=true when overwriting a display row)
  (void)blankUnused; // extensible for future use
}

// Set Quiet Time on/off. On the falling edge (on -> off) the reels are resynced
// to the last display each module was asked to show while quiet, so the physical
// display catches up to the most recent request. Safe to call from any task; the
// resync sends through rs485Send, which now transmits normally (quiet is off).
void sfSetQuietTime(bool on) {
  bool was = gQuietTime;
  gQuietTime = on;
  if (was && !on) {
    // Snapshot pending requests under the lock, clear them, then send unlocked
    // (rs485Send must not be called while holding sfMutex).
    struct Pend { int id; char ch; int idx; };
    static Pend list[MAX_MODULES];
    int n = 0;
    if (xSemaphoreTake(sfMutex, portMAX_DELAY) == pdTRUE) {
      for (int i = 0; i < sfModuleCount && n < MAX_MODULES; i++) {
        if (sfModules[i].hasPend) {
          list[n].id  = sfModules[i].id;
          list[n].ch  = sfModules[i].pendChar;
          list[n].idx = sfModules[i].pendIndex;
          n++;
          sfModules[i].hasPend = false;
        }
      }
      xSemaphoreGive(sfMutex);
    }
    for (int i = 0; i < n; i++) {
      if (list[i].ch) sfSendChar(list[i].id, list[i].ch);
      else if (list[i].idx >= 0) sfSendIndex(list[i].id, list[i].idx);
    }
    if (n) printf("[QUIET] off -- resynced %d module(s) to last requested display\n", n);
  }
  printf("[QUIET] Quiet Time %s\n", on ? "ENABLED" : "disabled");
}

/* ----------------------------------------------------------
   Parse responses from modules (called from the RS485 receive task)
   Format examples:
     m38v:12\n           version response
     m38:4096\n          calibration result (steps per rev)
     m38d:2832:4096:\n   EEPROM dump
     mXadv:AABBCCDD...\n advertisement from unprovisioned module
     mXack:AABBCC...:5\n provisioning acknowledgement
---------------------------------------------------------- */

void mqttPublishSFEvent(const char* event, const char* payload);  // forward


// Update a module's activity timestamps (millis + RTC epoch for persistence).
static inline void sfTouch(SFModule* m) {
  if (!m) return;
  m->lastSeen = millis();
  m->probeMs  = 0;   // any activity means it's alive -> cancel any pending probe
  unsigned long ep = rtcEpochNow();
  if (ep) m->lastSeenEpoch = ep;
}

// A serial number is 4..20 alphanumeric characters (in practice 20 hex chars
// from the module's chip ID). Bus collisions destroy frame terminators and can
// glue responses together, producing SN tokens containing ':' or raw garbage
// bytes; storing one of those poisons the registry (and FATFS), after which
// every SN-addressed command (mXD<sn>, mXW<sn>, ...) silently fails. Validate
// before EVERY store and on load so corruption can never enter or persist.
bool sfValidSN(const char* sn) {
  if (!sn || !sn[0]) return false;
  size_t n = strlen(sn);
  if (n < 4 || n > 20) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)sn[i];
    if (!isalnum(c)) return false;
  }
  return true;
}

// Apply the version-bearing fields from a 'v' or 'A' response to the registry.
// Validates the serial (a glued/garbled frame yields a corrupt SN whose storage
// would poison the registry and FATFS); returns false if so, so the caller can
// reject the whole frame. `fwCopy` is the already-normalized firmware string
// ("?" when empty); `sn` may be empty (legacy version-only reply), in which case
// only the firmware is updated. The entry is re-found by id under sfMutex (the
// upsert pointer can be invalidated by concurrent compaction), and any other
// entry holding the same serial is collapsed so a serial maps to one record.
static bool sfApplyVersionFields(uint8_t id, const char* fwCopy, const char* sn) {
  if (sn && sn[0] && !sfValidSN(sn)) return false;
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* mm = sfFindById(id);
  if (mm) {
    if (strcmp(mm->fwVersion, fwCopy) != 0) sfModulesDirty = true;  // persist new fw
    strlcpy(mm->fwVersion, fwCopy, sizeof(mm->fwVersion));
    // A clean version/all reply parsed for this ID -> the duplicate-ID suspicion
    // (if any) is resolved; reset the heuristic.
    mm->dupRejectCount = 0;
    mm->dupRejectTs    = 0;
    mm->dupSuspect     = false;
    if (sn && sn[0]) {
      strlcpy(mm->serialNum, sn, sizeof(mm->serialNum));
      for (int k = 0; k < sfModuleCount; k++) {
        if (&sfModules[k] != mm && strcmp(sfModules[k].serialNum, sn) == 0) {
          for (int j = k; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
          sfModuleCount--;
          memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
          sfModulesDirty = true;
          break;
        }
      }
    }
  }
  xSemaphoreGive(sfMutex);
  return true;
}

// Record a corrupt-SN version/all reject for `id` and advance the duplicate-ID
// heuristic. Returns true EXACTLY ONCE -- the moment the suspicion first latches
// -- so the caller emits a single warning rather than spamming. Counts within a
// rolling window so isolated transient collisions decay instead of accumulating;
// only persistent same-ID corruption (the duplicate-ID signature) crosses the
// threshold. Runs under sfMutex internally.
static bool sfNoteCorruptVersionReply(uint8_t id) {
  bool newlyLatched = false;
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* m = sfFindById(id);
  if (m) {
    unsigned long now = millis();
    if (m->dupRejectTs && (now - m->dupRejectTs) <= DUP_ID_REJECT_WINDOW_MS) {
      if (m->dupRejectCount < 255) m->dupRejectCount++;
    } else {
      m->dupRejectCount = 1;   // first reject, or the window lapsed -> restart
    }
    m->dupRejectTs = now;
    if (!m->dupSuspect && m->dupRejectCount >= DUP_ID_REJECT_THRESHOLD) {
      m->dupSuspect = true;
      newlyLatched = true;
    }
  }
  xSemaphoreGive(sfMutex);
  return newlyLatched;
}

void sfParseResponse(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'm') return;

  // Convert to null-terminated string for easier parsing.
  // Sized for long inbound frames (a full dump response is ~590 bytes).
  // NOTE: static (not stack) -- sfParseResponse is called only from taskRS485
  // (single caller, no reentrancy), and a 768-byte stack buffer here would
  // overflow that task's 6KB stack. Keeping it in .bss avoids the overflow.
  static char buf[TX_MAX_BYTES + 1];
  size_t copyLen = (len < TX_MAX_BYTES) ? len : TX_MAX_BYTES;
  memcpy(buf, data, copyLen);
  buf[copyLen] = 0;
  // Strip trailing \r\n
  for (int i = (int)copyLen - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r'); i--)
    buf[i] = 0;

  // -- Provisioning advertisement: mXadv:<serialNumber>
  if (strncmp(buf, "mXadv:", 6) == 0) {
    const char* sn = buf + 6;
    if (!sfValidSN(sn)) {
      DBG("[SF] rejecting adv with corrupt SN: %s\n", sn);
      sfParseRejects++;
      return;
    }
    xSemaphoreTake(sfMutex, portMAX_DELAY);
    SFModule* m = sfFindBySN(sn);
    if (!m) {
      m = sfUpsert(255, sn);
      if (m) m->provisioned = false;
      DBG("[SF] Unprovisioned adv: %s\n", sn);
    } else if (m->provisioned || m->id != 255) {
      // The module is advertising (so it has NO assigned id) but we still hold a
      // provisioned/old-id entry for this serial -- e.g. it was just
      // deprovisioned, or a stale in-flight frame re-created an id entry. The
      // serial is the true identity, so reclaim this same entry in place rather
      // than leaving a duplicate: mark it unprovisioned and clear the stale id.
      m->provisioned = false;
      m->id = 255;
      sfModulesDirty = true;
      DBG("[SF] Re-advertised known SN %s -- reset to unprovisioned\n", sn);
    }
    if (m) sfTouch(m);
    if (sfDedupeBySNLocked(sn)) sfModulesDirty = true;
    // Purge any "ghost" entries: a provisioned record with an EMPTY serial is
    // invalid (every provisioned module has a serial) -- it's the residue of a
    // stale in-flight frame that re-created an id entry for a module that has
    // since been deprovisioned and is now advertising. Drop them so they don't
    // show as blank/duplicate cards in the grid.
    for (int k = 0; k < sfModuleCount; ) {
      if (sfModules[k].provisioned && sfModules[k].serialNum[0] == '\0') {
        for (int j = k; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
        sfModuleCount--;
        memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
        sfModulesDirty = true;
      } else k++;
    }
    xSemaphoreGive(sfMutex);
    mqttPublishSFEvent("adv", sn);
    return;
  }

  // -- Provisioning ack. The module replies mXack<sn>:<id>. Some firmware
  // revisions insert a colon after the token (mXack:<sn>:<id>); accept BOTH.
  // Match the 5-char "mXack" token, then skip an optional ':' before the serial.
  // (A strict "mXack:" match missed the colon-less form the modules actually
  // send -- so the ack handler, and the post-provision version query inside it,
  // never ran, leaving a blank entry with no serial number or firmware version.)
  if (strncmp(buf, "mXack", 5) == 0) {
    const char* rest = buf + 5;
    if (*rest == ':') rest++;            // tolerate optional colon after "mXack"
    char tmp[48];
    strlcpy(tmp, rest, sizeof(tmp));
    char* colon = strrchr(tmp, ':');     // separates <sn> from <id>
    if (colon) {
      *colon = 0;
      const char* sn = tmp;
      int newId = atoi(colon + 1);
      if (!sfValidSN(sn)) {
        DBG("[SF] rejecting ack with corrupt SN: %s\n", sn);
      sfParseRejects++;
        return;
      }
      xSemaphoreTake(sfMutex, portMAX_DELAY);
      SFModule* m = sfFindBySN(sn);
      if (!m) m = sfUpsert((uint8_t)newId, sn);
      if (m) {
        m->id = (uint8_t)newId;
        m->provisioned = true;
        m->acked = true;                         // acked provisioning -> never legacy
        m->verDueMs = millis() + MODULE_POSTPROV_VER_MS;  // schedule deferred version query
        m->verTries = 0;
        sfTouch(m);
        sfModulesDirty = true;
      }
      xSemaphoreGive(sfMutex);
      char payload[64];
      snprintf(payload, sizeof(payload), "{\"sn\":\"%s\",\"id\":%d}", sn, newId);
      mqttPublishSFEvent("ack", payload);
      // The post-provision version query is issued by taskRS485 once verDueMs
      // passes (see the deferred-query sweep) -- NOT inline here. Sending it
      // immediately fires before the module is ready to answer on its new ID, so
      // no reply comes back. The short delay lets it settle first.
    }
    return;
  }

  // -- Normal module response: m<id><cmd>:<data>
  // Parse the address first
  const char* p = buf + 1; // skip leading 'm'
  char idStr[8] = {0};
  int  idLen = 0;
  while (*p && isdigit((unsigned char)*p) && idLen < 7) idStr[idLen++] = *p++;
  if (idLen == 0) return;
  uint8_t id = (uint8_t)atoi(idStr);

  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* m = sfUpsert(id, NULL);
  if (m) { m->provisioned = true; sfTouch(m); }
  xSemaphoreGive(sfMutex);

  if (!m) return;
  char cmd = *p++;

  // Version response: m<id>v:<version>:<moduleId>:<serialNumber>
  // Legacy format m<id>v:<version> also accepted.
  if (cmd == 'v' && *p == ':') {
    char verBuf[64];
    strlcpy(verBuf, p + 1, sizeof(verBuf));
    // Strip trailing whitespace/newlines
    for (int k = (int)strlen(verBuf)-1; k >= 0 && (verBuf[k] == '\n' || verBuf[k] == '\r' || verBuf[k] == ' '); k--)
      verBuf[k] = '\0';
    // Split into up to 3 fields on ':'
    char* field[3] = {nullptr, nullptr, nullptr};
    field[0] = verBuf;
    int fi = 1;
    for (char* cp = verBuf; *cp && fi < 3; cp++) {
      if (*cp == ':') { *cp = '\0'; field[fi++] = cp + 1; }
    }
    int reportedId = (field[1] && field[1][0]) ? atoi(field[1]) : -1;
    char fwCopy[8] = "?";
    if (field[0] && field[0][0]) strlcpy(fwCopy, field[0], sizeof(fwCopy));
    // Validate + write fwVersion/serial under the lock (re-finds the entry by id,
    // collapses any duplicate-serial record). A corrupt SN rejects the frame.
    if (!sfApplyVersionFields((uint8_t)id, fwCopy, field[2])) {
      DBG("[SF] rejecting corrupt version response for module %d (sn:%s)\n",
          id, field[2] ? field[2] : "");
      sfParseRejects++;
      if (sfNoteCorruptVersionReply((uint8_t)id)) {
        DBG("[SF] *** possible DUPLICATE ID %d on the bus: %d corrupt version replies "
            "(framing intact, serial garbled) -- two modules may share this ID ***\n",
            id, DUP_ID_REJECT_THRESHOLD);
        char wpl[72];
        snprintf(wpl, sizeof(wpl), "{\"id\":%d,\"reason\":\"duplicate_id_suspected\"}", id);
        mqttPublishSFEvent("warning", wpl);
      }
      return;
    }
    DBG("[SF] Module %d fw:%s reportedId:%d sn:%s\n",
                  id, fwCopy, reportedId, field[2] ? field[2] : "");
    char payload[96];
    snprintf(payload, sizeof(payload),
      "{\"id\":%d,\"ver\":\"%s\",\"reportedId\":%d,\"sn\":\"%s\"}",
      id, fwCopy, reportedId, field[2] ? field[2] : "");
    mqttPublishSFEvent("version", payload);
  }
  // Calibration result: m<id>:<steps>
  else if (cmd == ':') {
    int steps = atoi(p);
    DBG("[SF] Module %d calibrated: %d steps/rev\n", id, steps);
    if (sfCalibWaitId == (int)id) {
      sfCalibSteps     = steps;
      sfCalibCaptureTs = millis();
    }
    char payload[48];
    snprintf(payload, sizeof(payload), "{\"id\":%d,\"stepsPerRev\":%d}", id, steps);
    mqttPublishSFEvent("calibrated", payload);
  }
  // EEPROM dump: m<id>d:<homeOffset>:<totalSteps>:<map>
  else if (cmd == 'd' && *p == ':') {
    DBG("[SF] Module %d dump: %s\n", id, p + 1);
    // Capture into the shared single-slot buffer IF a dump request is
    // waiting for this module id (set by handleApiDump). No per-module cache.
    // Buffers sized for a full dump response (~590 bytes for a 64-flap map).
    // static (not stack): only taskRS485 reaches here, and 768-byte stack
    // buffers would overflow its 6KB stack.
    static char clean[TX_MAX_BYTES];
    strlcpy(clean, p + 1, sizeof(clean));
    size_t dl = strlen(clean);
    while (dl > 0 && (clean[dl-1] == '\n' || clean[dl-1] == '\r')) clean[--dl] = 0;
    if (sfDumpWaitId == (int)id) {
      strlcpy(sfDumpCapture, clean, sizeof(sfDumpCapture));
      sfDumpCaptureTs = millis();
    }
    // Size the MQTT-event payload to the queue slot. The REST dump path
    // (handleApiDump) carries the full untruncated dump; this MQTT event is an
    // optional notification, so matching the queue size avoids a truncation gap.
    static char payload[MQTT_BUF_SIZE];
    snprintf(payload, sizeof(payload), "{\"id\":%d,\"dump\":\"%s\"}", id, clean);
    mqttPublishSFEvent("dump", payload);
  }
  // Combined all-fields dump (firmware v25+): a single reply carrying everything
  // the 'v' and 'd' responses do, plus autoHome and the live current index:
  //   m<id>A:<version>:<moduleId>:<serialNumber>:<homeOffset>:<totalSteps>:<autoHome>:<curIndex>:<map>
  // We update fwVersion + serialNum exactly like the version response, and
  // reconstruct the "<homeOffset>:<totalSteps>:<map>" dump portion into the same
  // capture slot the 'd' path uses -- so a single 'A' satisfies both a version
  // refresh and a dump read in one bus transaction (see handleApiAll).
  else if (cmd == 'A' && *p == ':') {
    static char aBuf[TX_MAX_BYTES];     // static: taskRS485's 6KB stack can't hold this
    strlcpy(aBuf, p + 1, sizeof(aBuf));
    for (int k = (int)strlen(aBuf)-1; k >= 0 && (aBuf[k]=='\n'||aBuf[k]=='\r'||aBuf[k]==' '); k--) aBuf[k] = 0;
    // Split into the 7 scalar fields plus the trailing map (which has no ':').
    // f: 0 ver, 1 modId, 2 sn, 3 homeOffset, 4 totalSteps, 5 autoHome, 6 curIndex, 7 map
    char* f[8] = {0}; f[0] = aBuf; int fi = 1;
    for (char* cp = aBuf; *cp && fi < 8; cp++) { if (*cp == ':') { *cp = 0; f[fi++] = cp + 1; } }
    char fwCopy[8] = "?";
    if (f[0] && f[0][0]) strlcpy(fwCopy, f[0], sizeof(fwCopy));
    int reportedId = (f[1] && f[1][0]) ? atoi(f[1]) : -1;
    // Validate + write fwVersion/serial under the lock (shared with the 'v' path).
    if (!sfApplyVersionFields((uint8_t)id, fwCopy, f[2])) {
      DBG("[SF] rejecting corrupt all-fields response for module %d (sn:%s)\n", id, f[2] ? f[2] : "");
      sfParseRejects++;
      if (sfNoteCorruptVersionReply((uint8_t)id)) {
        DBG("[SF] *** possible DUPLICATE ID %d on the bus: %d corrupt version replies "
            "(framing intact, serial garbled) -- two modules may share this ID ***\n",
            id, DUP_ID_REJECT_THRESHOLD);
        char wpl[72];
        snprintf(wpl, sizeof(wpl), "{\"id\":%d,\"reason\":\"duplicate_id_suspected\"}", id);
        mqttPublishSFEvent("warning", wpl);
      }
      return;
    }
    // Reconstruct the dump portion into the shared capture slot if a request is
    // waiting (handleApiAll arms it), so the existing wait machinery works. The
    // 'A'-only extras (autoHome, curIndex, self-reported id) ride along in
    // dedicated globals -- they can't go in the dump string without breaking
    // parseDump, which expects exactly ho:ts:map. All set BEFORE the ready flag.
    if (sfDumpWaitId == (int)id) {
      snprintf(sfDumpCapture, sizeof(sfDumpCapture), "%s:%s:%s",
               f[3] ? f[3] : "", f[4] ? f[4] : "", f[7] ? f[7] : "");
      sfCaptureAutoHome   = (f[5] && f[5][0]) ? atoi(f[5]) : -99;
      sfCaptureCurIndex   = (f[6] && f[6][0]) ? atoi(f[6]) : -99;
      sfCaptureReportedId = reportedId;   // module's self-reported id (f[1])
      sfDumpCaptureTs = millis();
    }
    DBG("[SF] Module %d ALL fw:%s reportedId:%d sn:%s ho:%s ts:%s\n",
        id, fwCopy, reportedId, f[2] ? f[2] : "", f[3] ? f[3] : "", f[4] ? f[4] : "");
    // Publish both a version and a dump event, since 'A' carries both.
    char vpl[96];
    snprintf(vpl, sizeof(vpl), "{\"id\":%d,\"ver\":\"%s\",\"reportedId\":%d,\"sn\":\"%s\"}",
             id, fwCopy, reportedId, f[2] ? f[2] : "");
    mqttPublishSFEvent("version", vpl);
    static char dpl[MQTT_BUF_SIZE];
    snprintf(dpl, sizeof(dpl), "{\"id\":%d,\"dump\":\"%s:%s:%s\"}",
             id, f[3] ? f[3] : "", f[4] ? f[4] : "", f[7] ? f[7] : "");
    mqttPublishSFEvent("dump", dpl);
  }
  // Diagnostics snapshot (firmware v26+): m<id>Q:<resetCause>:<bootCount>:<vcc_mV>:<eepromOk>:<curIndex>
  else if (cmd == 'Q' && *p == ':') {
    char qb[48];
    strlcpy(qb, p + 1, sizeof(qb));
    for (int k = (int)strlen(qb)-1; k >= 0 && (qb[k]=='\n'||qb[k]=='\r'||qb[k]==' '); k--) qb[k] = 0;
    char* qf[5] = {0}; qf[0] = qb; int qi = 1;
    for (char* cp = qb; *cp && qi < 5; cp++) { if (*cp == ':') { *cp = 0; qf[qi++] = cp + 1; } }
    int rc   = (qf[0] && qf[0][0]) ? atoi(qf[0]) : 0;
    int boot = (qf[1] && qf[1][0]) ? atoi(qf[1]) : 0;
    int vcc  = (qf[2] && qf[2][0]) ? atoi(qf[2]) : 0;
    int ee   = (qf[3] && qf[3][0]) ? atoi(qf[3]) : 0;
    int cur  = (qf[4] && qf[4][0]) ? atoi(qf[4]) : -1;
    DBG("[SF] Module %d diag Q: rc=%d boot=%d vcc=%d ee=%d cur=%d\n", id, rc, boot, vcc, ee, cur);
    if (sfDiagQWaitId == (int)id) {
      sfDiagQReset = rc; sfDiagQBoot = boot; sfDiagQVcc = vcc; sfDiagQEe = ee; sfDiagQCur = cur;
      sfDiagQTs = millis();
    }
    char payload[96];
    snprintf(payload, sizeof(payload),
      "{\"id\":%d,\"resetCause\":%d,\"bootCount\":%d,\"vcc\":%d,\"eepromOk\":%d,\"curIndex\":%d}",
      id, rc, boot, vcc, ee, cur);
    mqttPublishSFEvent("diag", payload);
  }
  // Mechanical self-test (firmware v26+): m<id>M:<code>:<min>:<max>:<spreadTenthsPct>
  else if (cmd == 'M' && *p == ':') {
    char mb[48];
    strlcpy(mb, p + 1, sizeof(mb));
    for (int k = (int)strlen(mb)-1; k >= 0 && (mb[k]=='\n'||mb[k]=='\r'||mb[k]==' '); k--) mb[k] = 0;
    char* mf[4] = {0}; mf[0] = mb; int mi = 1;
    for (char* cp = mb; *cp && mi < 4; cp++) { if (*cp == ':') { *cp = 0; mf[mi++] = cp + 1; } }
    int code = (mf[0] && mf[0][0]) ? atoi(mf[0]) : -1;
    int mn   = (mf[1] && mf[1][0]) ? atoi(mf[1]) : 0;
    int mx   = (mf[2] && mf[2][0]) ? atoi(mf[2]) : 0;
    int spr  = (mf[3] && mf[3][0]) ? atoi(mf[3]) : 0;
    DBG("[SF] Module %d diag M: code=%d min=%d max=%d spread=%d\n", id, code, mn, mx, spr);
    if (sfDiagMWaitId == (int)id) {
      sfDiagMCode = code; sfDiagMMin = mn; sfDiagMMax = mx; sfDiagMSpread = spr;
      sfDiagMTs = millis();
    }
    char payload[80];
    snprintf(payload, sizeof(payload),
      "{\"id\":%d,\"code\":%d,\"min\":%d,\"max\":%d,\"spreadTenths\":%d}",
      id, code, mn, mx, spr);
    mqttPublishSFEvent("diag", payload);
  }
}

/* ----------------------------------------------------------
   MQTT
---------------------------------------------------------- */
WiFiClient   wifiClient;
WiFiClient   mqttWifiClient;        // persistent client for PubSubClient
PubSubClient mqtt(mqttWifiClient);  // mqttInit() configures timeouts on this

static unsigned long lastStatusMs = 0;
static unsigned long          lastDispPubMs   = 0;
static unsigned long mqttRetryMs  = 0;

// mqttTopic() removed -- all call sites use snprintf char arrays
// Safe MQTT publish from any task -- enqueues for the network task to drain.
static void mqttEnqueue(const char* topic, const char* payload, size_t len) {
  if (!mqttQMutex || !mqttQueue) return;
  if (xSemaphoreTake(mqttQMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  int next = (mqttQHead + 1) % MQTT_Q_SIZE;
  if (next != mqttQTail) {
    strlcpy(mqttQueue[mqttQHead].topic, topic, sizeof(mqttQueue[0].topic));
    size_t copy = (len < sizeof(mqttQueue[0].payload)-1) ? len : sizeof(mqttQueue[0].payload)-1;
    memcpy(mqttQueue[mqttQHead].payload, payload, copy);
    mqttQueue[mqttQHead].payload[copy] = 0;
    mqttQueue[mqttQHead].len = copy;
    mqttQHead = next;
  }
  xSemaphoreGive(mqttQMutex);
}

void mqttPublishMsg(const RS485Msg& m) {
  if (!mqtt.connected()) return;
  // Build ASCII representation of frame (no heap allocation)
  char ascii[MSG_MAX_BYTES + 1]; size_t ai = 0;
  for (size_t ii = 0; ii < m.len && ai < sizeof(ascii)-1; ii++) {
    uint8_t b = m.data[ii];
    if      (b == '\n' || b == '\r') { /* skip */ }
    else if (b >= 32 && b <= 126)    { ascii[ai++] = (char)b; }
    else                             { ascii[ai++] = '.'; }
  }
  ascii[ai] = 0;
  // Build JSON with snprintf -- avoids JsonDocument heap allocation in hot path.
  // The monitor frame (ascii) is capped at MSG_MAX_BYTES, so the JSON is at most
  // ~313 bytes; a 384-byte buffer is ample and keeps this off-stack pressure low
  // (this runs in taskRS485, which has a modest stack).
  char buf[384];
  size_t n = (size_t)snprintf(buf, sizeof(buf),
    "{\"ts\":%lu,\"wt\":\"%s\",\"command\":\"%s\"}",
    m.timestamp, m.wallTime, ascii);
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  char _t[80];
  snprintf(_t, sizeof(_t), "%s/%s", cfg.mqttPrefix, m.dir=='R'?"rx":"tx");
  mqttEnqueue(_t, buf, n);
}

void mqttPublishSFEvent(const char* event, const char* payload) {
  char _t[80];
  snprintf(_t,sizeof(_t),"%s/flap/%s",cfg.mqttPrefix,event);
  mqttEnqueue(_t, payload, strlen(payload));
}

static void mqttPublishStatus() {
  if (!mqtt.connected()) return;
  char timeBuf[24];
  rtcFormatTime(timeBuf, sizeof(timeBuf));
  char ip[20];
  IPAddress lip = WiFi.localIP();
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", lip[0], lip[1], lip[2], lip[3]);
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  // Full diagnostic set -- mirrors the [WDG] heartbeat so Home Assistant can
  // surface the same health signals (min heap, largest block, fragmentation,
  // parse rejects, and per-task stack high-water marks).
  unsigned freeHeap = ESP.getFreeHeap();
  unsigned minHeap  = ESP.getMinFreeHeap();
  unsigned maxBlk   = ESP.getMaxAllocHeap();
  unsigned frag     = freeHeap ? (unsigned)(100 - (maxBlk * 100UL / freeHeap)) : 0;
  unsigned s485 = hTaskRS485 ? uxTaskGetStackHighWaterMark(hTaskRS485) : 0;
  unsigned sWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
  unsigned sNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
  unsigned sOta = hTaskOTA   ? uxTaskGetStackHighWaterMark(hTaskOTA)   : 0;
  unsigned sRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
  char buf[640];
  size_t n = (size_t)snprintf(buf, sizeof(buf),
    "{\"uptime\":%lu,\"rx\":%lu,\"tx\":%lu,\"rej\":%lu,\"modules\":%d,"
    "\"time\":\"%s\",\"ntpSynced\":%s,\"heap\":%u,\"minheap\":%u,"
    "\"maxblk\":%u,\"frag\":%u,\"rssi\":%d,\"wifi\":%s,"
    "\"stk485\":%u,\"stkweb\":%u,\"stknet\":%u,\"stkota\":%u,\"stkrtc\":%u,"
    "\"ip\":\"%s\",\"url\":\"http://%s/\",\"version\":\"%s\","
    "\"maintenance\":%s,\"quiet\":%s}",
    millis()/1000, rxCount, txCount, sfParseRejects, sfModuleCount,
    timeBuf, ntpSynced?"true":"false", freeHeap, minHeap,
    maxBlk, frag, rssi, (WiFi.status()==WL_CONNECTED)?"true":"false",
    s485, sWeb, sNet, sOta, sRtc,
    ip, ip, FW_VERSION, gMaintenanceMode?"true":"false", gQuietTime?"true":"false");
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  char _t[80];
  snprintf(_t, sizeof(_t), "%s/status", cfg.mqttPrefix);
  mqttEnqueue(_t, buf, n);
}

// Assemble the best-known display string from per-module tracked characters,
// in module-id order across the configured grid. Unknown chars render as '?'.
static void mqttPublishDisplayState() {
  if (!mqtt.connected()) return;
  int cells = (int)cfg.gridRows * (int)cfg.gridCols;
  if (cells < 1) cells = 1;
  if (cells > MAX_MODULES) cells = MAX_MODULES;
  char str[MAX_MODULES + 1];
  int outLen = 0;
  if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (int id = 0; id < cells; id++) {
      SFModule* m = sfFindById((uint8_t)id);
      char c = ' ';
      if (m && m->provisioned) {
        if (m->flapChar >= 0x20 && m->flapChar <= 0x7E) c = m->flapChar;
        else c = '?';   // present but char unknown (e.g. after home / index set)
      }
      str[outLen++] = c;
    }
    xSemaphoreGive(sfMutex);
  }
  str[outLen] = '\0';
  char _t[80];
  snprintf(_t, sizeof(_t), "%s/display/state", cfg.mqttPrefix);
  mqttEnqueue(_t, str, outLen);
}

// Publish the HA-facing entity state topics (maintenance + quiet switches, and
// the display string). Called on connect, on any toggle, and when display
// tracking changes. No-op unless HA integration is enabled.
static void mqttPublishStateTopics() {
  if (!mqtt.connected() || !cfg.haEnabled) return;
  char t[80];
  snprintf(t, sizeof(t), "%s/maintenance/state", cfg.mqttPrefix);
  mqttEnqueue(t, gMaintenanceMode ? "ON" : "OFF", gMaintenanceMode ? 2 : 3);
  snprintf(t, sizeof(t), "%s/quiet/state", cfg.mqttPrefix);
  mqttEnqueue(t, gQuietTime ? "ON" : "OFF", gQuietTime ? 2 : 3);
  mqttPublishDisplayState();
}

// Publish (or remove) Home Assistant MQTT discovery configs. When enable is
// true, retained config messages are sent under homeassistant/<comp>/<node>/...
// so HA auto-creates the entities; when false, empty retained payloads are sent
// to the same topics to delete them. Uses HA's abbreviated discovery keys and a
// shared base-topic (~) to keep each payload within MQTT_BUF_SIZE. All entities
// share one device block (linked by the gateway's chip id) so they group under a
// single HA device. Diagnostic sensors read fields from the <prefix>/status JSON.
static void haPublishDiscovery(bool enable) {
  if (!mqtt.connected()) return;
  char node[24];
  snprintf(node, sizeof(node), "sfgw_%08X", (uint32_t)ESP.getEfuseMac());
  const char* pfx = cfg.mqttPrefix;
  char topic[160];
  char pl[MQTT_BUF_SIZE];

  // Shared fragments: device block + availability. avty_t uses <prefix>/availability.
  // Kept compact; HA merges the device block across entities by identifier.
  char dev[200];
  snprintf(dev, sizeof(dev),
    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Split-Flap Gateway\",\"mf\":\"Anthropic SFGW\",\"mdl\":\"ESP32-S3 RS485\",\"sw\":\"%s\"},"
    "\"avty_t\":\"%s/availability\"", node, FW_VERSION, pfx);

  // Helper macro-like lambda is not available; emit each entity inline.
  // 1) Display text entity: set/echo the whole display string.
  snprintf(topic, sizeof(topic), "homeassistant/text/%s/display/config", node);
  if (enable) {
    snprintf(pl, sizeof(pl),
      "{\"name\":\"Display\",\"uniq_id\":\"%s_display\",\"cmd_t\":\"%s/display/set\","
      "\"stat_t\":\"%s/display/state\",\"max\":255,%s}", node, pfx, pfx, dev);
    mqttEnqueue(topic, pl, strlen(pl));
  } else mqttEnqueue(topic, "", 0);

  // 2) Maintenance mode switch.
  snprintf(topic, sizeof(topic), "homeassistant/switch/%s/maintenance/config", node);
  if (enable) {
    snprintf(pl, sizeof(pl),
      "{\"name\":\"Maintenance Mode\",\"uniq_id\":\"%s_maint\",\"cmd_t\":\"%s/maintenance/set\","
      "\"stat_t\":\"%s/maintenance/state\",\"ic\":\"mdi:wrench\",%s}", node, pfx, pfx, dev);
    mqttEnqueue(topic, pl, strlen(pl));
  } else mqttEnqueue(topic, "", 0);

  // 3) Quiet Time switch.
  snprintf(topic, sizeof(topic), "homeassistant/switch/%s/quiet/config", node);
  if (enable) {
    snprintf(pl, sizeof(pl),
      "{\"name\":\"Quiet Time\",\"uniq_id\":\"%s_quiet\",\"cmd_t\":\"%s/quiet/set\","
      "\"stat_t\":\"%s/quiet/state\",\"ic\":\"mdi:volume-off\",%s}", node, pfx, pfx, dev);
    mqttEnqueue(topic, pl, strlen(pl));
  } else mqttEnqueue(topic, "", 0);

  // 4) Diagnostic sensors -- all read the <prefix>/status JSON via value_template.
  //    Mirrors the [WDG] heartbeat: heap/min/maxblk/frag, parse rejects, the five
  //    per-task stack high-water marks, plus connectivity and identity.
  struct DiagS { const char* obj; const char* name; const char* fld; const char* unit; const char* dc; const char* ic; };
  static const DiagS diags[] = {
    {"modules", "Modules",        "modules", NULL,  NULL,              "mdi:view-grid"},
    {"uptime",  "Uptime",         "uptime",  "s",   "duration",        NULL},
    {"heap",    "Free Heap",      "heap",    "B",   NULL,              "mdi:memory"},
    {"minheap", "Min Free Heap",  "minheap", "B",   NULL,              "mdi:memory"},
    {"maxblk",  "Max Alloc Block","maxblk",  "B",   NULL,              "mdi:memory"},
    {"frag",    "Heap Fragmentation","frag", "%",   NULL,              "mdi:chart-donut"},
    {"rssi",    "WiFi Signal",    "rssi",    "dBm", "signal_strength", NULL},
    {"rx",      "Frames Received","rx",      NULL,  NULL,              "mdi:download-network"},
    {"tx",      "Frames Sent",    "tx",      NULL,  NULL,              "mdi:upload-network"},
    {"rej",     "Parse Rejects",  "rej",     NULL,  NULL,              "mdi:alert-circle"},
    {"stk485",  "Stack RS485",    "stk485",  "B",   NULL,              "mdi:layers"},
    {"stkweb",  "Stack Web",      "stkweb",  "B",   NULL,              "mdi:layers"},
    {"stknet",  "Stack Network",  "stknet",  "B",   NULL,              "mdi:layers"},
    {"stkota",  "Stack OTA",      "stkota",  "B",   NULL,              "mdi:layers"},
    {"stkrtc",  "Stack RTC",      "stkrtc",  "B",   NULL,              "mdi:layers"},
    {"ip",      "IP Address",     "ip",      NULL,  NULL,              "mdi:ip-network"},
    {"url",     "Gateway URL",    "url",     NULL,  NULL,              "mdi:web"},
    {"version", "Firmware",       "version", NULL,  NULL,              "mdi:chip"},
  };
  for (unsigned i = 0; i < sizeof(diags)/sizeof(diags[0]); i++) {
    const DiagS& d = diags[i];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", node, d.obj);
    if (enable) {
      int p = snprintf(pl, sizeof(pl),
        "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s/status\","
        "\"val_tpl\":\"{{ value_json.%s }}\",\"ent_cat\":\"diagnostic\"",
        d.name, node, d.obj, pfx, d.fld);
      if (d.unit) p += snprintf(pl+p, sizeof(pl)-p, ",\"unit_of_meas\":\"%s\"", d.unit);
      if (d.dc)   p += snprintf(pl+p, sizeof(pl)-p, ",\"dev_cla\":\"%s\"", d.dc);
      if (d.ic)   p += snprintf(pl+p, sizeof(pl)-p, ",\"ic\":\"%s\"", d.ic);
      snprintf(pl+p, sizeof(pl)-p, ",%s}", dev);
      mqttEnqueue(topic, pl, strlen(pl));
    } else mqttEnqueue(topic, "", 0);
  }

  printf("[HA] Discovery %s (node %s)\n", enable ? "published" : "removed", node);
}
//   <prefix>/flap/set      {"id":5,"char":"A"}  or  {"id":5,"index":3}
//                          {"id":-1,"text":"HELLO","start":0}  multi-module text
//   <prefix>/flap/home     {"id":5}  or  {"id":-1}  (broadcast)
//   <prefix>/flap/provision {"sn":"AABBCC...","id":5}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Control topics are handled FIRST, before the maintenance-mode gate, so the
  // maintenance and quiet switches remain reachable over MQTT/HA even while
  // maintenance mode is on (otherwise you could never turn it back off remotely).
  if (length < MQTT_BUF_SIZE) {
    static char cbuf[64];
    size_t cl = length < sizeof(cbuf) - 1 ? length : sizeof(cbuf) - 1;
    memcpy(cbuf, payload, cl); cbuf[cl] = 0;
    // Trim trailing whitespace/newline and detect on/off
    while (cl && (cbuf[cl-1]=='\n'||cbuf[cl-1]=='\r'||cbuf[cl-1]==' ')) cbuf[--cl]=0;
    bool on = (strcasecmp(cbuf, "ON") == 0 || strcasecmp(cbuf, "true") == 0 ||
               strcasecmp(cbuf, "1") == 0);
    char ctl[80];
    snprintf(ctl, sizeof(ctl), "%s/maintenance/set", cfg.mqttPrefix);
    if (strcmp(topic, ctl) == 0) {
      gMaintenanceMode = on;
      printf("[MAINT] Maintenance mode %s (MQTT)\n", on ? "ENABLED" : "disabled");
      mqttPublishStateTopics();
      return;
    }
    snprintf(ctl, sizeof(ctl), "%s/quiet/set", cfg.mqttPrefix);
    if (strcmp(topic, ctl) == 0) {
      sfSetQuietTime(on);
      mqttPublishStateTopics();
      return;
    }
  }
  // Maintenance mode: ignore all externally-originated DISPLAY commands. Nothing
  // from MQTT is relayed to the bus while this is on; only the gateway's own web
  // UI / REST API can drive the display.
  if (gMaintenanceMode) {
    DBG("[MQTT] ignored (maintenance mode): %s\n", topic);
    return;
  }
  if (length >= MQTT_BUF_SIZE) return;
  // static (not stack): mqttCallback is invoked only from mqtt.loop() in
  // taskNetwork (single caller, no reentrancy). Three ~768-byte stack buffers
  // here plus the rs485Send/mqttPublishMsg call chain would push taskNetwork's
  // 6KB stack toward overflow -- the same failure mode that crashed taskRS485.
  static char buf[MQTT_BUF_SIZE + 1];
  memcpy(buf, payload, length);
  buf[length] = 0;

  // Compare topic using char* to avoid heap String allocation on every message
  char sendTopic[80], setTopic[80], homeTopic[80], provTopic[80];
  snprintf(sendTopic, sizeof(sendTopic), "%s/send",           cfg.mqttPrefix);
  snprintf(setTopic,  sizeof(setTopic),  "%s/flap/set",       cfg.mqttPrefix);
  snprintf(homeTopic, sizeof(homeTopic), "%s/flap/home",      cfg.mqttPrefix);
  snprintf(provTopic, sizeof(provTopic), "%s/flap/provision", cfg.mqttPrefix);
  char dispTopic[80];
  snprintf(dispTopic, sizeof(dispTopic), "%s/display/set", cfg.mqttPrefix);

  // HA display text entity: a plain string to show across the whole display,
  // starting at module 0. Handled before JSON parse since the payload is text.
  if (strcmp(topic, dispTopic) == 0) {
    // strip a single trailing newline if present
    size_t L = strlen(buf);
    if (L && (buf[L-1]=='\n'||buf[L-1]=='\r')) buf[L-1]=0;
    DBG("[MQTT] display set: %s\n", buf);
    sfSendText(0, buf, false);
    mqttPublishDisplayState();
    return;
  }

  // Handle the raw send topic before attempting JSON parse:
  // Accept either a plain ASCII frame ("m9h\n") or JSON ({"data":"m9h\n"}).
  if (strcmp(topic, sendTopic) == 0) {
    const char* d = nullptr;
    bool raw = false;   // optional {"raw":true} -> send verbatim, bypass sanitization
    // Sized for long commands (e.g. a full restore) sent as a plain frame.
    // static: see note on buf above (single-caller context, stack pressure).
    static char plainBuf[TX_MAX_BYTES + 1];
    if (buf[0] == '{') {
      // Try JSON
      JsonDocument doc;
      if (deserializeJson(doc, buf) == DeserializationError::Ok) {
        d = doc["data"] | "";
        raw = doc["raw"] | false;
      }
    } else {
      // Plain string -- use as-is
      strlcpy(plainBuf, buf, sizeof(plainBuf));
      d = plainBuf;
    }
    if (d && d[0]) {
      { char _dbg[MSG_MAX_BYTES]; size_t _dl = strlen(d);
        if (_dl > 0 && (d[_dl-1]=='\n'||d[_dl-1]=='\r')) _dl--;
        memcpy(_dbg, d, _dl); _dbg[_dl] = '\0';
        DBG("[MQTT->BUS] %s\n", _dbg); }
      static uint8_t outBuf[TX_MAX_BYTES];  // static: see note on buf above
      size_t  outLen = min(strlen(d), (size_t)TX_MAX_BYTES);
      memcpy(outBuf, d, outLen);
      rs485Send(outBuf, outLen, raw);
    }
    return;
  }

  // All other topics require JSON
  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) return;

  if (strcmp(topic, setTopic) == 0) {
    int id = doc["id"] | -99;
    if (id == -99) return;
    // Text mode: send a string across sequential modules
    const char* text = doc["text"] | "";
    if (strlen(text) > 0) {
      int start = doc["start"] | id;
      sfSendText(start, text, false);
      return;
    }
    // Single char
    const char* ch = doc["char"] | "";
  DBG("[API] show char '%c' on module %d\n", ch[0], id);
    if (strlen(ch) > 0) { sfSendChar(id, ch[0]); return; }
    // Index
    if (doc["index"].is<int>()) { sfSendIndex(id, doc["index"].as<int>()); }
  }
  else if (strcmp(topic, homeTopic) == 0) {
    int id = doc["id"] | -1;
    sfHome(id < 0 ? -1 : id);
  }
  else if (strcmp(topic, provTopic) == 0) {
    const char* sn = doc["sn"] | "";
    int newId = doc["id"] | -1;
  DBG("[API] provision SN %s -> ID %d\n", sn, newId);
    if (strlen(sn) > 0 && newId >= 0) sfProvision(sn, newId);
  }
}

// Called once from setup(). Initialises MQTT settings that allocate heap
// and must not be called repeatedly.
static void mqttInit() {
  mqttWifiClient.setTimeout(5000);  // 5s TCP connect timeout
  mqtt.setClient(mqttWifiClient);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(MQTT_BUF_SIZE);
  mqtt.setKeepAlive(15);  // 15s keepalive -- detect dead connections faster
  mqtt.setSocketTimeout(5);  // 5s socket timeout
  if (strlen(cfg.mqttHost)) mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
}

static void mqttConnect() {
  if (!strlen(cfg.mqttHost)) return;
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "splitflap-%08X", (uint32_t)ESP.getEfuseMac());
  printf("[MQTT] Connecting to %s:%d...\n", cfg.mqttHost, cfg.mqttPort);
  // Last Will & Testament: the broker publishes "offline" to <prefix>/availability
  // (retained) if we drop without a clean disconnect. HA uses this to mark every
  // entity unavailable. We publish "online" ourselves on connect (the birth).
  char availT[80];
  snprintf(availT, sizeof(availT), "%s/availability", cfg.mqttPrefix);
  bool ok = strlen(cfg.mqttUser)
    ? mqtt.connect(clientId, cfg.mqttUser, cfg.mqttPass, availT, 0, true, "offline")
    : mqtt.connect(clientId, NULL, NULL, availT, 0, true, "offline");
  if (ok) {
    printf("[MQTT] Connected\n");
    // Birth: mark available (retained).
    mqtt.publish(availT, "online", true);
    // Use char arrays not String to avoid heap fragmentation
    char t[80];
    snprintf(t,sizeof(t),"%s/send",           cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/flap/set",       cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/flap/home",      cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/flap/provision", cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/display/set",    cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/maintenance/set",cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/quiet/set",      cfg.mqttPrefix); mqtt.subscribe(t);
    // Home Assistant integration: publish discovery + initial entity state.
    if (cfg.haEnabled) {
      haPublishDiscovery(true);
      mqttPublishStateTopics();
    }
  } else {
    int st = mqtt.state();
    const char* why;
    switch (st) {                       // PubSubClient state codes
      case -4: why = "timeout";               break;
      case -3: why = "connection lost";       break;
      case -2: why = "connect failed (TCP)";  break;
      case -1: why = "disconnected";          break;
      case  1: why = "bad protocol";          break;
      case  2: why = "bad client id";         break;
      case  3: why = "server unavailable";    break;
      case  4: why = "bad credentials";       break;
      case  5: why = "not authorized";        break;
      default: why = "unknown";               break;
    }
    printf("[MQTT] Failed rc=%d (%s)\n", st, why);
  }
}

/* ----------------------------------------------------------
   Web server
---------------------------------------------------------- */
WebServer server(80);

static void sendJsonError(int code, const char* msg) {
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
  server.send(code, "application/json", buf);
}

// -- GET /  (main dashboard)
void handleRoot() {
  wdgWebMs = millis();  // streaming response can take a while
  // Cap per-write blocking so a stalled browser cannot wedge taskWeb.
  // If the client stops ACKing, writes fail fast instead of hanging.
  server.client().setTimeout(3000);  // 3s per socket operation
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent("<!DOCTYPE html><html lang=\"en\"><head>");
  server.sendContent("<meta charset=\"UTF-8\">");
  server.sendContent("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  server.sendContent("<title>Split-Flap Gateway</title>");
  server.sendContent("<style>");
  server.sendContent(":root{--bg:#1a1a2e;--card:#16213e;--acc:#0f3460;--hi:#e94560;--txt:#eaeaea;--dim:#888;--grn:#4caf50;--ylw:#ffc107;--qt:#26c6da}");
  server.sendContent("*{box-sizing:border-box;margin:0;padding:0}");
  server.sendContent("body{background:var(--bg);color:var(--txt);font-family:\"Segoe UI\",sans-serif;font-size:14px}");
  server.sendContent("header{background:var(--acc);padding:12px 18px;display:flex;align-items:center;justify-content:space-between}");
  server.sendContent("header h1{font-size:1.1rem}");
  server.sendContent("#badge{font-size:.75rem;padding:3px 10px;border-radius:12px;background:var(--hi)}#badge.ok{background:var(--grn)}");
  server.sendContent("nav{display:flex;gap:6px;padding:8px 18px;background:var(--card);flex-wrap:wrap}");
  server.sendContent("nav a{color:var(--dim);text-decoration:none;padding:4px 14px;border-radius:4px;border:1px solid var(--acc);cursor:pointer}");
  server.sendContent("nav a.on,nav a:hover{background:var(--hi);color:#fff;border-color:var(--hi)}");
  server.sendContent(".pane{display:none;padding:14px 18px}.pane.on{display:block}");
  server.sendContent(".card{background:var(--card);border-radius:8px;padding:14px;margin-bottom:12px;border:1px solid var(--acc)}");
  server.sendContent(".card h2{font-size:.95rem;margin-bottom:10px;color:var(--hi)}");
  server.sendContent("label{display:block;color:var(--dim);font-size:.8rem;margin:7px 0 2px}");
  server.sendContent("input,select,textarea{width:100%;background:#0d1b2a;border:1px solid var(--acc);color:var(--txt);padding:6px 9px;border-radius:4px;font-size:.9rem}");
  server.sendContent("textarea{resize:vertical}");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("button{margin-top:8px;padding:7px 16px;background:var(--hi);border:none;color:#fff;border-radius:4px;cursor:pointer}");
  server.sendContent("button.sec{background:var(--acc)}button.ylw{background:var(--ylw);color:#000}");
  server.sendContent("#log{background:#080818;border:1px solid var(--acc);border-radius:6px;padding:4px 0;height:380px;overflow-y:auto;font-family:monospace;font-size:.8rem}");
  server.sendContent(".logrow{display:grid;grid-template-columns:152px 28px 1fr;gap:0 8px;padding:4px 8px;border-bottom:1px solid #0d1b2a;align-items:start;line-height:1.4}");
  server.sendContent(".logrow:hover{background:#0d1b2a}");
  server.sendContent(".logrow.rx .ldir{color:#4dd0e1}.logrow.tx .ldir{color:#ffb74d}");
  server.sendContent(".lts{color:var(--dim);font-size:.68rem;padding-top:2px}.ldir{font-weight:bold}");
  server.sendContent(".lraw{color:#ddd;word-break:break-all}");
  server.sendContent(".ldesc{grid-column:3;color:#888;font-size:.72rem;padding-top:1px}");
  server.sendContent(".row{display:flex;gap:10px;flex-wrap:wrap}.row>*{flex:1;min-width:140px}");
  server.sendContent(".grid2{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:8px}");
  server.sendContent(".grid3{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px}");
  server.sendContent(".stat{background:#0d1b2a;border-radius:8px;padding:12px 8px;text-align:center;border:1px solid #14263a}");
  server.sendContent(".stat .v{font-size:1.15rem;font-weight:bold;color:var(--hi);word-break:break-all}.stat .k{font-size:.68rem;color:var(--dim);margin-top:3px;text-transform:uppercase;letter-spacing:.04em}");
  server.sendContent(".sgh{color:var(--dim);font-size:.78rem;text-transform:uppercase;letter-spacing:.08em;margin:0 0 8px 2px}");
  server.sendContent(".stat .v.vok{color:var(--grn)}.stat .v.vwarn{color:var(--ylw)}.stat .v.vbad{color:var(--hi)}");
  server.sendContent(".mod{background:#0d1b2a;border-radius:6px;padding:8px 8px 30px 8px;border:1px solid var(--acc);font-size:.8rem;position:relative}");
  server.sendContent(".wall{display:inline-block;background:#1a1a1a;border:1px solid #333;border-radius:10px;padding:14px;overflow-x:auto;max-width:100%}");
  server.sendContent(".wallrow{display:flex;gap:6px;justify-content:center}.wallrow+.wallrow{margin-top:6px}");
  server.sendContent(".flap{position:relative;width:46px;height:62px;background:#0a0a0a;border-radius:5px;box-shadow:inset 0 0 0 1px #2a2a2a;display:flex;align-items:center;justify-content:center;font-family:'Courier New',monospace;font-weight:700;font-size:2rem;color:#f5f5f5;flex:0 0 auto}");
  server.sendContent(".flap::before{content:'';position:absolute;left:0;right:0;top:50%;height:1px;background:#000;box-shadow:0 1px 0 #1f1f1f;z-index:1}");
  server.sendContent(".flap.empty{background:#141414;color:#333;box-shadow:inset 0 0 0 1px #222}.flap.unknown{color:#555}");
  server.sendContent(".wallwrap{text-align:center}.wallmeta{font-size:.76rem;color:var(--dim);margin-top:8px}");
  server.sendContent(".verbadge{font-size:.55em;font-weight:normal;color:var(--dim);vertical-align:middle;border:1px solid var(--acc);border-radius:10px;padding:1px 7px;margin-left:6px}");
  server.sendContent(".micons{position:absolute;bottom:6px;right:6px;display:flex;gap:8px}");
  server.sendContent(".micon{cursor:pointer;font-size:1rem;line-height:1;opacity:.78;background:none;border:none;padding:2px}.micon:hover{opacity:1}.micon.del{color:var(--hi)}.micon.dis{opacity:.25;cursor:not-allowed}.micon.dis:hover{opacity:.25}");
  server.sendContent(".mlegacy{display:inline-block;font-size:.62rem;font-weight:bold;color:#1a1206;background:var(--ylw);border-radius:3px;padding:1px 5px;margin-left:4px;vertical-align:middle}");
  server.sendContent(".mfield{display:flex;justify-content:space-between;gap:10px;padding:3px 0;border-bottom:1px solid #14263a;font-size:.82rem}.mk{color:var(--dim)}.mv{color:var(--txt);word-break:break-all;text-align:right}");
  server.sendContent(".mmap{font-family:monospace;font-size:.72rem;color:var(--dim);background:#0a1622;border-radius:5px;padding:6px;margin-top:6px;max-height:140px;overflow:auto;word-break:break-all}");
  server.sendContent(".dbtn{display:block;width:100%;text-align:left;margin:6px 0;padding:10px;border-radius:6px;border:1px solid var(--hi);background:#1a0d0d;color:var(--txt);cursor:pointer}.dbtn:hover{background:#2a1414}.dbtn small{display:block;color:var(--dim);font-size:.74rem;margin-top:2px}");
  server.sendContent(".mod .mid{font-size:1rem;font-weight:bold;color:var(--ylw)}.mod .mc{font-size:.75rem;color:var(--dim)}");
  server.sendContent(".hdr-right{display:flex;align-items:center;gap:14px}.maint-toggle{display:flex;align-items:center;gap:6px;cursor:pointer;font-size:.82rem;color:var(--dim);user-select:none}.maint-toggle input{width:auto;margin:0;cursor:pointer}.maint-toggle.active{color:var(--ylw);font-weight:bold}body.maint-on{box-shadow:inset 0 0 0 3px var(--ylw)}.maint-banner{display:none;background:var(--ylw);color:#1a1206;text-align:center;padding:5px;font-size:.82rem;font-weight:bold}body.maint-on .maint-banner{display:block}");
  server.sendContent(".quiet-toggle{display:flex;align-items:center;gap:6px;cursor:pointer;font-size:.82rem;color:var(--dim);user-select:none}.quiet-toggle input{width:auto;margin:0;cursor:pointer}.quiet-toggle.active{color:var(--qt);font-weight:bold}body.quiet-on{box-shadow:inset 0 0 0 3px var(--qt)}body.maint-on.quiet-on{box-shadow:inset 0 0 0 3px var(--ylw),inset 0 0 0 6px var(--qt)}.quiet-banner{display:none;background:var(--qt);color:#06222a;text-align:center;padding:5px;font-size:.82rem;font-weight:bold}body.quiet-on .quiet-banner{display:block}");
  server.sendContent(".unprovisioned{border-color:var(--hi)}");
  server.sendContent("#sr{font-size:.8rem;color:var(--grn);min-height:16px;margin-top:5px}");
  server.sendContent(".sendrow{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin-top:8px}.raw-toggle{display:inline-flex;align-items:center;gap:6px;cursor:pointer;font-size:.82rem;color:var(--dim);user-select:none;margin:0}.raw-toggle input{width:auto;margin:0;cursor:pointer}.lsan{display:inline-block;margin-left:8px;padding:0 5px;border-radius:3px;background:var(--ylw);color:#1a1206;font-size:.66rem;font-weight:bold;letter-spacing:.03em;vertical-align:middle}");
  server.sendContent(".cmods{display:grid;gap:5px;grid-template-columns:repeat(auto-fill,minmax(48px,1fr))}.cmod{text-align:center;padding:6px 4px;background:#0d1b2a;border:1px solid var(--acc);border-radius:5px;cursor:pointer;font-family:monospace;font-size:.85rem;color:var(--txt)}.cmod:hover{border-color:var(--hi)}.cmod.sel{background:var(--hi);border-color:var(--hi);color:#fff;font-weight:bold}.cmod .csn{display:block;font-size:.6rem;color:var(--dim)}.cmod.sel .csn{color:#ffd}.cmod.known{border-color:var(--grn)}.cmod.legacy{border-color:var(--ylw)}.cmod.unknown{opacity:.6}.cmod .csn.lg{color:var(--ylw)}.cmods.single{display:flex;flex-wrap:wrap}");
  server.sendContent(".cedit{display:flex;gap:14px;flex-wrap:wrap;background:#0a0a0a;border-radius:6px;padding:10px 14px;margin-bottom:8px}.cedit .cb{flex:1;min-width:210px}.cedit .ck{font-size:.68rem;color:var(--dim);letter-spacing:.05em;margin-bottom:4px}.cedit .cer{display:flex;gap:5px;align-items:center}.cedit .cer input{flex:1;min-width:60px;font-family:monospace;font-size:1.15rem;font-weight:bold;text-align:center}.cedit .cer input.ho{color:var(--grn)}.cedit .cer input.ts{color:var(--ylw)}.cedit .cer button{margin:0;padding:7px 11px;white-space:nowrap;font-size:.82rem}");
  server.sendContent(".cnudge{display:flex;gap:4px;flex-wrap:wrap;margin:8px 0;align-items:center}.cnudge button{margin:0;padding:5px 9px;font-size:.8rem;background:var(--acc)}.cnudge button.neg{background:#5a2030}.cnudge button.pos{background:#1f5a2a}.cnudge .lbl{font-size:.72rem;color:var(--dim);padding:0 4px}");
  server.sendContent(".tnudge{display:flex;gap:4px;flex-wrap:wrap;margin:6px 0}.tnudge button{flex:1;min-width:34px;margin:0;padding:7px 3px;font-size:.78rem}.tnudge button.neg{background:#5a2030}.tnudge button.pos{background:#1f5a2a}.tnudge .lbl{flex:0 0 auto;align-self:center;font-size:.68rem;color:var(--dim);padding:0 3px}");
  server.sendContent(".cmap{display:grid;grid-template-columns:repeat(auto-fill,minmax(64px,1fr));gap:5px;margin-top:8px}.cc{background:#0d1b2a;border:1px solid var(--acc);border-radius:5px;padding:5px 2px;text-align:center;cursor:pointer}.cc:hover{border-color:var(--hi)}.cc .cch{font-size:1.05rem;font-weight:bold;font-family:monospace}.cc .ccv{font-size:.7rem;color:var(--dim);font-family:monospace}.cc.custom{border-color:var(--grn)}.cc.custom .ccv{color:var(--grn)}.cc .sw{display:inline-block;width:14px;height:14px;border-radius:2px;vertical-align:middle;border:1px solid #555}");
  server.sendContent(".calnote{display:flex;align-items:center;gap:10px;flex-wrap:wrap;background:#0d1b2a;border:1px solid var(--acc);border-radius:6px;padding:9px 12px;margin-bottom:10px}.calnote .cntxt{flex:1;min-width:220px;font-size:.8rem;color:var(--dim)}.calnote button{margin:0;white-space:nowrap}");
  server.sendContent(".calmaint{display:flex;align-items:center;gap:10px;flex-wrap:wrap;background:#2a230a;border:1px solid var(--ylw);border-radius:6px;padding:9px 12px;margin-bottom:10px}.calmaint .cmtxt{flex:1;min-width:220px;font-size:.8rem;color:var(--ylw)}.calmaint button{margin:0;white-space:nowrap;background:var(--ylw);color:#1a1206;font-weight:bold}body.maint-on .calmaint{display:none}");
  server.sendContent(".tunebox{background:var(--card);border:2px solid var(--hi);border-radius:10px;padding:18px;max-width:340px;width:90%}.tunebox h3{color:var(--hi);font-size:1.1rem;margin-bottom:2px}.tunebox .exp{font-size:.8rem;color:var(--dim);margin-bottom:10px}.tunebox button{width:100%;margin-top:8px;padding:10px}.tunebox .bgoto{background:var(--acc)}.tunebox .block{background:var(--grn)}.tunebox .brev{background:var(--hi)}.tunebox .bcancel{background:#333}");
  server.sendContent(".wizbtn{background:var(--grn);margin:8px 0 0}.wizbox{background:var(--card);border:2px solid var(--grn);border-radius:10px;padding:20px;max-width:380px;width:92%;text-align:center}.wizbox h3{color:var(--grn);font-size:1.1rem;margin:0 0 4px}.wizprog{font-size:.78rem;color:var(--dim);margin-bottom:10px}.wizbar{height:6px;background:#0a0a0a;border-radius:3px;overflow:hidden;margin-bottom:14px}.wizbar>div{height:100%;background:var(--grn);width:0;transition:width .2s}.wizchar{font-size:3.4rem;font-weight:bold;font-family:monospace;line-height:1.1;margin:6px 0}.wizchar .wsw{display:inline-block;width:56px;height:56px;border-radius:6px;vertical-align:middle;border:1px solid #555}.wizsub{font-size:.82rem;color:var(--dim);margin-bottom:10px}.wiztarget{font-size:.9rem;color:var(--ylw);font-family:monospace;margin-bottom:8px}.wiznudge{display:flex;gap:4px;flex-wrap:wrap;justify-content:center;margin:8px 0}.wiznudge button{flex:1;min-width:36px;margin:0;padding:8px 3px;font-size:.8rem}.wiznudge button.neg{background:#5a2030}.wiznudge button.pos{background:#1f5a2a}.wizbox .wconfirm{width:100%;margin-top:10px;padding:11px;background:var(--grn);font-weight:bold}.wizbox .wreset{width:100%;margin-top:8px;padding:9px;background:var(--hi)}.wizrow{display:flex;gap:6px;margin-top:8px}.wizrow button{flex:1;margin:0;padding:9px}.wizrow .wback{background:var(--acc)}.wizrow .wskip{background:#444}.wizrow .wexit{background:var(--hi)}.wizstat{margin-top:8px;font-size:.76rem;color:var(--ylw);min-height:14px}");
  server.sendContent("</style></head><body>");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("<header><h1>Split-Flap Gateway <span class=\"verbadge\">v" FW_VERSION "</span></h1><div class=\"hdr-right\"><label class=\"quiet-toggle\" title=\"When on, the gateway accepts commands but does not move any flaps for normal display updates (calibration still works). Reels resync when turned off.\"><input id=\"quietChkHdr\" type=\"checkbox\" onchange=\"toggleQuietHdr()\"><span class=\"quiet-lbl\">Quiet Time</span></label><label class=\"maint-toggle\" title=\"When on, commands received via MQTT are ignored and not relayed to the bus. The web UI keeps working.\"><input id=\"maintChk\" type=\"checkbox\" onchange=\"toggleMaint()\"><span class=\"maint-lbl\">Maintenance</span></label><span id=\"badge\">...</span></div></header>");
  server.sendContent("<div class=\"maint-banner\">MAINTENANCE MODE - external MQTT commands are being ignored</div>");
  server.sendContent("<div class=\"quiet-banner\">QUIET TIME - display updates are paused; flaps will not move (calibration still works)</div>");
  server.sendContent("<nav>");
  server.sendContent("<a class=\"on\" onclick=\"show('modules',this)\">Modules</a>");
  server.sendContent("<a onclick=\"show('display',this)\">Display</a>");
  server.sendContent("<a onclick=\"show('provision',this)\">Provision</a>");
  server.sendContent("<a onclick=\"show('calib',this)\">Calibration</a>");
  server.sendContent("<a onclick=\"show('monitor',this)\">Bus Monitor</a>");
  server.sendContent("<a onclick=\"show('settings',this)\">Settings</a>");
  server.sendContent("<a onclick=\"show('backup',this)\">Backup</a>");
  server.sendContent("<a onclick=\"show('statusp',this)\">Status</a>");
  server.sendContent("</nav>");
  server.sendContent("<div id=\"pane-modules\" class=\"pane on\">");
  server.sendContent("<div class=\"card\"><h2>Known Modules</h2><div id=\"modGrid\" class=\"grid2\">Loading...</div><button class=\"sec\" onclick=\"refreshModules()\" title=\"Broadcasts m*v to all modules so they report their version and serial number\">&#x21bb; Identify All</button><span id=\"refreshR\" style=\"margin-left:10px;font-size:.8rem;color:var(--grn)\"></span></div>");
  server.sendContent("<div id=\"modModal\" style=\"display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.75);z-index:200;align-items:center;justify-content:center;\" onclick=\"if(event.target===this)closeModal()\"><div style=\"background:var(--card);border:1px solid var(--acc);border-radius:10px;padding:22px;max-width:560px;width:90%;max-height:82vh;overflow-y:auto;\"><div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:10px\"><span style=\"font-size:1.05rem;font-weight:600;color:var(--hi)\" id=\"modModalTitle\">Module EEPROM</span><button class=\"sec\" onclick=\"closeModal()\" style=\"margin:0;padding:3px 10px;font-size:.8rem\">&#x2715;</button></div><div id=\"modModalBody\" style=\"font-size:.85rem\"></div><div style=\"margin-top:14px;display:flex;gap:8px;align-items:center;flex-wrap:wrap\"><button class=\"sec\" onclick=\"refreshDump()\">&#x21bb; Refresh</button><span id=\"modModalStatus\" style=\"font-size:.78rem;color:var(--ylw)\"></span></div></div></div>");
  server.sendContent("<div id=\"delModal\" style=\"display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.75);z-index:200;align-items:center;justify-content:center;\" onclick=\"if(event.target===this)closeDelModal()\"><div style=\"background:var(--card);border:1px solid var(--hi);border-radius:10px;padding:22px;max-width:460px;width:90%;\"><div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:12px\"><span style=\"font-size:1.05rem;font-weight:600;color:var(--hi)\" id=\"delModalTitle\">Destructive Actions</span><button class=\"sec\" onclick=\"closeDelModal()\" style=\"margin:0;padding:3px 10px;font-size:.8rem\">&#x2715;</button></div>");
  server.sendContent("<p style=\"font-size:.82rem;color:var(--dim);margin-bottom:10px\">These actions cannot be undone. You will be asked to confirm.</p>");
  server.sendContent("<button class=\"dbtn\" onclick=\"delAction('erase')\">Erase EEPROM<small>Clears the calibration flap map. The module keeps its ID but loses per-flap positions until re-calibrated.</small></button>");
  server.sendContent("<button class=\"dbtn\" onclick=\"delAction('factoryreset')\">Factory Reset<small>Restores all module settings to firmware defaults (home offset, total steps, flap map).</small></button>");
  server.sendContent("<button class=\"dbtn\" onclick=\"delAction('deprovision')\">De-provision<small>Sends Reset (R): the module forgets its assigned ID and returns to advertising for re-provisioning.</small></button>");
  server.sendContent("<div id=\"delModalStatus\" style=\"margin-top:10px;font-size:.8rem;color:var(--ylw)\"></div></div></div>");
  server.sendContent("</div>");  // close pane-modules
  server.sendContent("<div id=\"pane-display\" class=\"pane\"><div class=\"card\"><h2>Live Display</h2><div class=\"wallwrap\"><div id=\"wall\" class=\"wall\"></div><div class=\"wallmeta\" id=\"wallMeta\">Loading...</div></div></div>");
  server.sendContent("<div class=\"card\"><h2>Send Text to Display</h2>");
  server.sendContent("<label>Text</label><input id=\"dispText\" type=\"text\" placeholder=\"HELLO WORLD\" style=\"text-transform:uppercase\">");
  server.sendContent("<label>Start Module ID</label><input id=\"dispStart\" type=\"number\" value=\"0\" min=\"0\" max=\"253\">");
  server.sendContent("<button onclick=\"sendText()\">Send to Display</button><div id=\"dr\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
  server.sendContent("<div class=\"card\"><h2>Send Single Character</h2><div class=\"row\"><div><label>Module ID (-1 = all)</label><input id=\"scId\" type=\"number\" value=\"-1\" min=\"-1\" max=\"254\"></div><div><label>Character</label><input id=\"scChar\" type=\"text\" maxlength=\"1\" value=\"A\" style=\"text-transform:uppercase\"></div></div><button onclick=\"sendChar()\">Send Character</button><div id=\"scr\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
  server.sendContent("<div class=\"card\"><h2>Send by Index</h2><div class=\"row\"><div><label>Module ID</label><input id=\"idxId\" type=\"number\" value=\"0\" min=\"0\" max=\"254\"></div><div><label>Flap Index (0-63)</label><input id=\"idxVal\" type=\"number\" value=\"0\" min=\"0\" max=\"63\"></div></div><button onclick=\"sendIndex()\">Send Index</button></div></div>");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("<div id=\"pane-provision\" class=\"pane\">");
  server.sendContent("<div class=\"card\"><h2>Unprovisioned Modules</h2>");
  server.sendContent("<p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">Modules without an ID broadcast their serial number every 10-15 seconds. Use Home to identify which physical module it is, then assign an ID.</p>");
  server.sendContent("<div id=\"unprovList\">Loading...</div></div>");
  server.sendContent("<div class=\"card\"><h2>Provision: Assign ID by Serial Number</h2>");
  server.sendContent("<div class=\"row\"><div><label>Serial Number</label><input id=\"provSN\" type=\"text\" placeholder=\"AABBCCDD...\"></div><div><label>New ID (0-254)</label><input id=\"provId\" type=\"number\" min=\"0\" max=\"254\" value=\"1\"></div></div>");
  server.sendContent("<button onclick=\"doProvision()\">Assign ID</button>");
  server.sendContent("<div id=\"provR\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
  server.sendContent("<div class=\"card\"><h2>De-provision Module</h2>");
  server.sendContent("<p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">Sends the Reset (R) command which erases the stored ID and returns the module to unprovisioned state.</p>");
  server.sendContent("<div class=\"row\"><div><label>Module ID (-1 = all modules)</label><input id=\"deprovId\" type=\"number\" value=\"-1\" min=\"-1\" max=\"254\"></div></div>");
  server.sendContent("<button class=\"ylw\" onclick=\"doDeprovision()\">De-provision</button>");
  server.sendContent("<div id=\"deprovR\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
  server.sendContent("</div>");
  wdgWebMs = millis();
  server.sendContent("<div id=\"pane-calib\" class=\"pane\">");
  server.sendContent("<div class=\"calmaint\"><span class=\"cmtxt\">Maintenance mode is strongly recommended before calibrating, so external commands received via MQTT cannot move the reels mid-calibration.</span><button onclick=\"calEnableMaint()\">Turn On Maintenance</button></div>");
  server.sendContent("<div class=\"card\"><h2>Calibration</h2>");
  server.sendContent("<p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">Two ways to calibrate, both using the same nudge controls and writing to each module's EEPROM: sweep the <b>whole board</b> flap-by-flap (fast for a full wall -- every module shows the same flap at once and you nudge only the ones that look wrong), or pick a <b>single module</b> below to adjust it on its own.</p>");
  server.sendContent("<button class=\"wizbtn\" onclick=\"bmStart()\">&#x1f3b4; Calibrate Whole Board (all modules at once)</button>");
  server.sendContent("<p style=\"font-size:.78rem;color:var(--dim);margin:10px 0 4px\">Or pick one module to calibrate individually:</p>");
  server.sendContent("<div id=\"calMods\" class=\"cmods\">Loading modules...</div>");
  server.sendContent("<div style=\"display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin-top:8px\"><button class=\"sec\" onclick=\"calLoadModules()\" style=\"margin:0\">&#x21bb; Refresh</button><span style=\"color:var(--dim);font-size:.8rem\">or tune any ID:</span><input id=\"calAnyId\" type=\"number\" min=\"0\" max=\"254\" placeholder=\"0-254\" style=\"width:90px;margin:0\"><button onclick=\"calSelectAny()\" style=\"margin:0\">Go</button></div></div>");
  server.sendContent("<div id=\"calDetail\" class=\"card\" style=\"display:none\">");
  server.sendContent("<div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:8px\"><h2 id=\"calTitle\" style=\"margin:0\">Module</h2><button class=\"sec\" onclick=\"calRefresh()\" style=\"margin:0;padding:4px 10px;font-size:.8rem\">&#x21bb; Re-read EEPROM</button></div>");
  server.sendContent("<div id=\"calStatus\" style=\"font-size:.78rem;color:var(--ylw);min-height:15px;margin-bottom:6px\"></div>");
  server.sendContent("<div class=\"calnote\"><span class=\"cntxt\">New module? Running Calibrate is recommended to confirm this reel's actual step count and home offset. It spins one full revolution to measure steps/rev, then saves the result.</span><button onclick=\"calCountSteps()\">Calibrate</button></div>");
  server.sendContent("<div class=\"cedit\"><div class=\"cb\"><div class=\"ck\">HOME OFFSET</div><div class=\"cer\"><input id=\"calHoIn\" class=\"ho\" type=\"number\" min=\"0\"><button onclick=\"calSaveHo()\">Save</button><button class=\"sec\" onclick=\"calRevertHo()\" title=\"Reset to default 2832\">Revert</button></div></div><div class=\"cb\"><div class=\"ck\">TOTAL STEPS</div><div class=\"cer\"><input id=\"calTsIn\" class=\"ts\" type=\"number\" min=\"1\"><button onclick=\"calSaveTs()\">Save</button><button class=\"sec\" onclick=\"calRevertTs()\" title=\"Reset to default 4096\">Revert</button><button class=\"sec\" id=\"calCountBtn\" onclick=\"calCountSteps()\" title=\"Run the calibrate command: the reel spins one full revolution to measure its steps per revolution\">Count Steps</button></div></div></div>");
  server.sendContent("<div class=\"cnudge\"><button class=\"neg\" onclick=\"calNudge(-32)\">-32</button><button class=\"neg\" onclick=\"calNudge(-16)\">-16</button><button class=\"neg\" onclick=\"calNudge(-4)\">-4</button><button class=\"neg\" onclick=\"calNudge(-1)\">-1</button><span class=\"lbl\">NUDGE OFFSET</span><button class=\"pos\" onclick=\"calNudge(1)\">+1</button><button class=\"pos\" onclick=\"calNudge(4)\">+4</button><button class=\"pos\" onclick=\"calNudge(16)\">+16</button><button class=\"pos\" onclick=\"calNudge(32)\">+32</button></div>");
  server.sendContent("<p style=\"font-size:.72rem;color:var(--dim);margin:0 0 8px\">Nudge moves the reel and saves the offset instantly. Press Home to verify.</p>");
  server.sendContent("<button class=\"sec\" onclick=\"calHomeMotor()\" style=\"margin:0\">Home Motor</button>");
  server.sendContent("<h2 style=\"margin-top:14px\">Character Map</h2>");
  server.sendContent("<p style=\"font-size:.78rem;color:var(--dim);margin-bottom:4px\">Green = custom EEPROM value. Grey = firmware default. Click a character to tune its position, or run the wizard to step through every flap.</p>");
  server.sendContent("<button class=\"wizbtn\" onclick=\"calWizStart()\">&#x1f9ed; Calibration Wizard (step through all flaps)</button>");
  server.sendContent("<div id=\"calMap\" class=\"cmap\"></div></div>");
  server.sendContent("<div id=\"tuneModal\" style=\"display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.75);z-index:200;align-items:center;justify-content:center\" onclick=\"if(event.target===this)calCloseTune()\">");
  server.sendContent("<div class=\"tunebox\"><h3 id=\"tuneTitle\">Tune</h3><div class=\"exp\" id=\"tuneExp\">Expected: -</div><label>Absolute Target Step</label><input id=\"tuneVal\" type=\"number\" min=\"0\"><div class=\"tnudge\"><button class=\"neg\" onclick=\"calTuneNudge(-32)\">-32</button><button class=\"neg\" onclick=\"calTuneNudge(-16)\">-16</button><button class=\"neg\" onclick=\"calTuneNudge(-4)\">-4</button><button class=\"neg\" onclick=\"calTuneNudge(-1)\">-1</button><button class=\"pos\" onclick=\"calTuneNudge(1)\">+1</button><button class=\"pos\" onclick=\"calTuneNudge(4)\">+4</button><button class=\"pos\" onclick=\"calTuneNudge(16)\">+16</button><button class=\"pos\" onclick=\"calTuneNudge(32)\">+32</button></div><p style=\"font-size:.72rem;color:var(--dim);margin:2px 0 6px\">Adjust the value, then Test Position to move there. Lock to EEPROM when it looks right.</p><button class=\"bgoto\" onclick=\"calTuneGoto()\">Test Position (GOTO)</button><button class=\"block\" onclick=\"calTuneLock()\">Lock to EEPROM</button><button class=\"brev\" onclick=\"calTuneRevert()\">Revert to Default</button><button class=\"bcancel\" onclick=\"calCloseTune()\">Cancel</button><div id=\"tuneStatus\" style=\"margin-top:8px;font-size:.78rem;color:var(--ylw);min-height:15px\"></div></div></div>");
  server.sendContent("<div id=\"wizModal\" style=\"display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.8);z-index:210;align-items:center;justify-content:center\">");
  server.sendContent("<div class=\"wizbox\"><h3>Calibration Wizard</h3><div class=\"wizprog\" id=\"wizProg\">-</div><div class=\"wizbar\"><div id=\"wizBar\"></div></div><div style=\"font-size:.8rem;color:var(--dim)\">This flap should be showing on the reel:</div><div class=\"wizchar\" id=\"wizChar\">-</div><div class=\"wizsub\" id=\"wizSub\">Look at the module. If it is centered, confirm. If not, nudge until it is.</div><div class=\"wiztarget\" id=\"wizTarget\">step -</div><div class=\"wiznudge\"><button class=\"neg\" onclick=\"calWizNudge(-32)\">-32</button><button class=\"neg\" onclick=\"calWizNudge(-16)\">-16</button><button class=\"neg\" onclick=\"calWizNudge(-4)\">-4</button><button class=\"neg\" onclick=\"calWizNudge(-1)\">-1</button><button class=\"pos\" onclick=\"calWizNudge(1)\">+1</button><button class=\"pos\" onclick=\"calWizNudge(4)\">+4</button><button class=\"pos\" onclick=\"calWizNudge(16)\">+16</button><button class=\"pos\" onclick=\"calWizNudge(32)\">+32</button></div><button class=\"wreset\" onclick=\"calWizReset()\">Reset to Default</button><button class=\"wconfirm\" onclick=\"calWizConfirm()\">Confirm &amp; Next &#x2192;</button><div class=\"wizrow\"><button class=\"wback\" onclick=\"calWizBack()\">&#x2190; Back</button><button class=\"wskip\" onclick=\"calWizSkip()\">Skip</button><button class=\"wexit\" onclick=\"calWizExit()\">Exit</button></div><div class=\"wizstat\" id=\"wizStat\"></div></div></div>");
  server.sendContent("<div id=\"wizIntroModal\" style=\"display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.8);z-index:210;align-items:center;justify-content:center\">");
  server.sendContent("<div class=\"wizbox\"><div id=\"wizStep1\"><h3>Before you start (1 of 2)</h3><p style=\"font-size:.86rem;color:var(--txt);margin:6px 0 4px\">Have you run <b>Calibrate (Count Steps)</b> on this module to confirm the reel's actual step count?</p><p style=\"font-size:.78rem;color:var(--dim);margin:0 0 14px\">The wizard relies on the measured step count to place each flap. If it is wrong, every flap will be off. Calibrate first if you haven't.</p><button class=\"wconfirm\" onclick=\"calWizStep2()\">Yes -- step count is confirmed</button><button class=\"wreset\" style=\"background:var(--acc)\" onclick=\"calWizIntroCalibrate()\">No -- run Calibrate now</button><button class=\"wskip\" style=\"width:100%;margin-top:8px;padding:9px;background:#444\" onclick=\"calWizIntroCancel()\">Cancel</button></div>");
  server.sendContent("<div id=\"wizStep2\" style=\"display:none\"><h3>Before you start (2 of 2)</h3><p style=\"font-size:.86rem;color:var(--txt);margin:6px 0 4px\">Has the <b>home position</b> been confirmed? When homed, the blank (black) flap should sit centered in the window.</p><p style=\"font-size:.78rem;color:var(--dim);margin:0 0 14px\">This is the home offset. If the blank flap is off-center, every character will be too. Confirm it now if you haven't.</p><button class=\"wconfirm\" onclick=\"calWizIntroYes()\">Yes -- home is confirmed, start wizard</button><button class=\"wreset\" style=\"background:var(--acc)\" onclick=\"calWizHomeStage()\">Confirm / adjust home position now</button><button class=\"wskip\" style=\"width:100%;margin-top:8px;padding:9px;background:#444\" onclick=\"calWizIntroCancel()\">Cancel</button></div>");
  server.sendContent("<div id=\"wizStep3\" style=\"display:none\"><h3>Confirm home position</h3><div style=\"font-size:.8rem;color:var(--dim)\">This flap should be showing on the reel (homed):</div><div class=\"wizchar\"><span class=\"wsw\" style=\"background:#000\"></span></div><div class=\"wizsub\">BLANK (home). Press Re-home, then nudge until the blank flap is centered. Nudges save instantly.</div><div class=\"wiztarget\" id=\"wizHoVal\">offset -</div><div class=\"wiznudge\"><button class=\"neg\" onclick=\"calWizHomeNudge(-32)\">-32</button><button class=\"neg\" onclick=\"calWizHomeNudge(-16)\">-16</button><button class=\"neg\" onclick=\"calWizHomeNudge(-4)\">-4</button><button class=\"neg\" onclick=\"calWizHomeNudge(-1)\">-1</button><button class=\"pos\" onclick=\"calWizHomeNudge(1)\">+1</button><button class=\"pos\" onclick=\"calWizHomeNudge(4)\">+4</button><button class=\"pos\" onclick=\"calWizHomeNudge(16)\">+16</button><button class=\"pos\" onclick=\"calWizHomeNudge(32)\">+32</button></div><button class=\"wreset\" style=\"background:var(--acc)\" onclick=\"calWizHomeRehome()\">Re-home (verify)</button><button class=\"wconfirm\" onclick=\"calWizIntroYes()\">Home is centered -- start wizard</button><button class=\"wskip\" style=\"width:100%;margin-top:8px;padding:9px;background:#444\" onclick=\"calWizStep2Back()\">Back</button><div class=\"wizstat\" id=\"wizHoStat\"></div></div></div></div>");
  server.sendContent("<div id=\"bmModal\" style=\"display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.8);z-index:190;align-items:center;justify-content:center\"><div class=\"wizbox\" style=\"max-width:560px\"><h3>Calibrate Whole Board</h3><div class=\"wizprog\" id=\"bmProg\">-</div><div class=\"wizbar\"><div id=\"bmBar\"></div></div><div style=\"font-size:.8rem;color:var(--dim)\">Every module should be showing this flap:</div><div class=\"wizchar\" id=\"bmChar\">-</div><div class=\"wizsub\">Look at the wall. Click any module showing the wrong flap to nudge it (same controls as the single-module tuner), then advance to the next flap.</div><div id=\"bmGrid\" class=\"cmods\" style=\"margin:10px 0\">Loading...</div><div class=\"wizrow\"><button class=\"wback\" onclick=\"bmPrev()\">&#x2190; Prev</button><button class=\"wskip\" onclick=\"bmSkip()\">Skip</button><button class=\"wconfirm\" style=\"margin-top:0\" onclick=\"bmNext()\">Next &#x2192;</button></div><div class=\"wizrow\" style=\"align-items:center\"><span style=\"font-size:.76rem;color:var(--dim)\">Jump to flap:</span><input id=\"bmJumpIn\" type=\"number\" min=\"1\" max=\"64\" style=\"width:64px;margin:0\"><button class=\"sec\" style=\"margin:0\" onclick=\"bmJump()\">Go</button><button class=\"wexit\" onclick=\"bmFinish()\">Finish</button></div><div class=\"wizstat\" id=\"bmStat\"></div></div></div>");
  server.sendContent("</div>");
  server.sendContent("<div id=\"pane-backup\" class=\"pane\">");
  server.sendContent("<div class=\"card\"><h2>Backup Calibration</h2>");
  server.sendContent("<p style=\"font-size:.85rem;color:var(--dim);margin-bottom:10px\">Reads the EEPROM calibration from every known module (by serial number) and saves it to a JSON file you can download. Make sure all modules have been identified first (see the Modules tab).</p>");
  server.sendContent("<button onclick=\"doBackup()\">Create Backup File</button>");
  server.sendContent("<div id=\"backupProg\" style=\"margin-top:10px;font-size:.85rem;color:var(--dim)\"></div>");
  server.sendContent("<div id=\"backupR\" style=\"margin-top:6px;font-size:.85rem\"></div>");
  server.sendContent("</div>");
  server.sendContent("<div class=\"card\"><h2>Restore Calibration</h2>");
  server.sendContent("<p style=\"font-size:.85rem;color:var(--dim);margin-bottom:10px\">Upload a backup file to restore calibration to each module by serial number. Modules are matched by serial number, so IDs can differ from when the backup was made.</p>");
  server.sendContent("<div class=\"row\"><div style=\"flex:1\"><label>Backup file</label><input id=\"restoreFile\" type=\"file\" accept=\".json,application/json\"></div></div>");
  server.sendContent("<label style=\"display:flex;align-items:center;gap:8px;margin:10px 0;cursor:pointer\"><input id=\"preserveId\" type=\"checkbox\" checked style=\"width:auto;margin:0\"><span>Preserve current module IDs <span style=\"color:var(--dim);font-size:.8rem\">(uncheck to also reassign IDs from the backup)</span></span></label>");
  server.sendContent("<button class=\"ylw\" onclick=\"doRestore()\">Restore from File</button>");
  server.sendContent("<div id=\"restoreProg\" style=\"margin-top:10px;font-size:.85rem;color:var(--dim)\"></div>");
  server.sendContent("<div id=\"restoreR\" style=\"margin-top:6px;font-size:.85rem\"></div>");
  server.sendContent("</div>");
  server.sendContent("</div>");
  server.sendContent("<div id=\"pane-monitor\" class=\"pane\"><div class=\"card\"><h2>RS485 Bus Monitor</h2><div id=\"log\"></div>");
  server.sendContent("<div style=\"display:flex;gap:10px;margin-top:8px;align-items:center;flex-wrap:wrap\">");
  server.sendContent("<button class=\"sec\" onclick=\"clearLog()\">Clear</button>");
  server.sendContent("<button class=\"sec\" onclick=\"downloadLog()\">Download Log</button>");
  server.sendContent("<label style=\"margin:0;display:flex;align-items:center;gap:5px;color:var(--txt)\"><input type=\"checkbox\" id=\"asc\" checked style=\"width:auto\" onchange=\"saveMonPrefs()\"> Auto-scroll</label>");
  server.sendContent("<label style=\"margin:0;display:flex;align-items:center;gap:5px;color:var(--txt)\" title=\"Stops fetching new frames. Frames beyond the 64-entry ring are lost while paused.\"><input type=\"checkbox\" id=\"lpause\" style=\"width:auto\" onchange=\"saveMonPrefs()\"> Pause</label>");
  server.sendContent("<span id=\"logCount\" style=\"font-size:.74rem;color:var(--dim)\">0 frames</span></div></div>");
  server.sendContent("<div class=\"card\"><h2>Send Frame</h2><label>Data (ASCII)</label><textarea id=\"sdata\" rows=\"2\" placeholder=\"m5-A\" onkeydown=\"if(event.key===&#39;Enter&#39;&&!event.shiftKey){event.preventDefault();doSend();}\"></textarea><div id=\"sr\"></div><div class=\"sendrow\"><button onclick=\"doSend()\">Send</button><label class=\"raw-toggle\"><input type=\"checkbox\" id=\"sraw\"> Raw &mdash; send verbatim, skip sanitizing &amp; framing (debug)</label></div></div></div>");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("<div id=\"pane-settings\" class=\"pane\">");
  server.sendContent("<div class=\"card\"><h2>WiFi</h2>");
  server.sendContent("<label>SSID</label><input id=\"wSSID\" type=\"text\" placeholder=\"Network name\">");
  server.sendContent("<label>Password</label><input id=\"wPASS\" type=\"password\" placeholder=\"Leave blank for open network\">");
  server.sendContent("<p style=\"font-size:.82rem;color:var(--dim);margin-top:6px\">AP fallback: <strong style=\"color:var(--txt)\">Split-Flap-GW</strong> / <strong style=\"color:var(--txt)\">12345678</strong> &nbsp; <span style=\"color:var(--ylw)\">http://192.168.4.1</span></p>");
  server.sendContent("<button onclick=\"saveWifi()\">Save WiFi</button>");
  server.sendContent("<div id=\"wr\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div>");
  server.sendContent("</div>");
  server.sendContent("<div class=\"card\"><h2>MQTT</h2>");
  server.sendContent("<div class=\"row\"><div><label>Broker Host / IP</label><input id=\"mqH\" placeholder=\"192.168.1.10\"></div><div><label>Port</label><input id=\"mqP\" type=\"number\" value=\"1883\"></div></div>");
  server.sendContent("<div class=\"row\"><div><label>Username</label><input id=\"mqU\"></div><div><label>Password</label><input id=\"mqPw\" type=\"password\"></div></div>");
  server.sendContent("<label>Topic Prefix</label><input id=\"mqPfx\" placeholder=\"splitflap\">");
  server.sendContent("<p style=\"font-size:.77rem;color:var(--dim);margin-top:4px\">Pub: .../rx &nbsp; .../tx &nbsp; .../status &nbsp; .../flap/adv &nbsp; .../flap/ack<br>Sub: .../flap/set &nbsp; .../flap/home &nbsp; .../flap/provision &nbsp; .../send</p>");
  server.sendContent("<button onclick=\"saveMqtt()\">Save MQTT</button> <button class=\"sec\" onclick=\"testMqtt()\">Test Connection</button>");
  server.sendContent("<div id=\"mr\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div>");
  server.sendContent("</div>");
  server.sendContent("<div class=\"card\"><h2>Timezone</h2>");
  server.sendContent("<label>Timezone</label><select id=\"tzSel\"><option value=\"UTC0\">UTC</option><option value=\"PST8PDT,M3.2.0,M11.1.0\">US Pacific (UTC-8/-7)</option><option value=\"MST7MDT,M3.2.0,M11.1.0\">US Mountain (UTC-7/-6)</option><option value=\"MST7\">US Mountain AZ (UTC-7 no DST)</option><option value=\"CST6CDT,M3.2.0,M11.1.0\">US Central (UTC-6/-5)</option><option value=\"EST5EDT,M3.2.0,M11.1.0\">US Eastern (UTC-5/-4)</option><option value=\"BRT3BRST,M10.3.0,M2.3.0\">Sao Paulo (UTC-3/-2)</option><option value=\"AZOT1AZOST,M3.5.0/0,M10.5.0/1\">Azores (UTC-1/0)</option><option value=\"GMT0BST,M3.5.0/1,M10.5.0\">London (UTC+0/+1)</option><option value=\"CET-1CEST,M3.5.0,M10.5.0/3\">Paris/Berlin (UTC+1/+2)</option><option value=\"EET-2EEST,M3.5.0/3,M10.5.0/4\">Helsinki/Athens (UTC+2/+3)</option><option value=\"MSK-3\">Moscow (UTC+3 no DST)</option><option value=\"GST-4\">Dubai (UTC+4 no DST)</option><option value=\"PKT-5\">Karachi (UTC+5 no DST)</option><option value=\"BST-6\">Dhaka (UTC+6 no DST)</option><option value=\"ICT-7\">Bangkok (UTC+7 no DST)</option><option value=\"CST-8\">Shanghai/HK/Singapore (UTC+8)</option><option value=\"JST-9\">Tokyo (UTC+9 no DST)</option><option value=\"AEST-10AEDT,M10.1.0,M4.1.0/3\">Sydney (UTC+10/+11)</option><option value=\"NZST-12NZDT,M9.5.0,M4.1.0/3\">Auckland (UTC+12/+13)</option></select>");

  server.sendContent("<p style=\"font-size:.82rem;color:var(--dim);margin-top:4px\">Used for bus monitor timestamps. Takes effect immediately after saving.</p>");
  server.sendContent("<label style=\"margin-top:10px\">NTP Server</label><input id=\"ntpSrv\" type=\"text\" placeholder=\"pool.ntp.org\">");
  server.sendContent("<p style=\"font-size:.82rem;color:var(--dim);margin-top:4px\">Hostname of the time server used to sync the clock. Default: pool.ntp.org</p>");
  server.sendContent("<button onclick=\"saveTz()\">Save Time Settings</button>");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("<div id=\"tzR\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div>");
  server.sendContent("</div>");
  server.sendContent("<div class=\"card\"><h2>Display Layout</h2><p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">How the modules are physically arranged, for the Live Display on the Display tab. Modules map left-to-right, top-to-bottom by ID (module 0 = top-left).</p><div class=\"row\"><div><label>Rows</label><input id=\"gRows\" type=\"number\" value=\"1\" min=\"1\" max=\"64\"></div><div><label>Columns</label><input id=\"gCols\" type=\"number\" value=\"16\" min=\"1\" max=\"64\"></div></div><button onclick=\"saveGrid()\">Save Layout</button><div id=\"gridR\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
  server.sendContent("<div class=\"card\"><h2>Serial Debug</h2><p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">Enable verbose serial output on the native USB serial port (115200 baud). Shows every RX/TX frame, MQTT events, and module activity.</p><label style=\"display:flex;align-items:center;gap:10px;cursor:pointer;color:var(--txt)\"><input type=\"checkbox\" id=\"dbgChk\" style=\"width:auto\">Enable Serial Debug Output</label><button onclick=\"saveDebug()\" style=\"margin-top:10px\">Save</button><div id=\"dbgR\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
  server.sendContent("<div class=\"card\"><h2>Home Assistant</h2><p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">Publish MQTT auto-discovery so the gateway appears in Home Assistant as a device with a display text control, Maintenance and Quiet Time switches, and diagnostic sensors. Requires MQTT to be configured. Leave off to avoid publishing discovery/state topics you don't need.</p><label style=\"display:flex;align-items:center;gap:10px;cursor:pointer;color:var(--txt)\"><input type=\"checkbox\" id=\"haChk\" style=\"width:auto\">Enable Home Assistant integration</label><button onclick=\"saveHa()\" style=\"margin-top:10px\">Save</button><div id=\"haR\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
  server.sendContent("<div class=\"card\"><h2>Quiet Time</h2><p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">When on, the gateway still accepts and acknowledges commands but does not move any flaps for normal display updates (calibration still works). When turned off, the reels resync to the last requested display. Not saved across reboots.</p><label style=\"display:flex;align-items:center;gap:10px;cursor:pointer;color:var(--txt)\"><input type=\"checkbox\" id=\"quietChk\" style=\"width:auto\" onchange=\"toggleQuiet()\">Quiet Time enabled</label><div id=\"quietR\" style=\"margin-top:6px;font-size:.82rem;color:var(--ylw)\"></div></div>");
  server.sendContent("<div class=\"card\"><h2>OTA Firmware Update</h2><p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">Upload new firmware directly from your browser ? no USB cable or Arduino IDE required.</p><a href=\"/ota\" target=\"_blank\" style=\"display:inline-block;margin-bottom:12px;padding:7px 16px;background:var(--hi);color:#fff;border-radius:4px;text-decoration:none;font-size:.9rem\">Open Firmware Updater &rarr;</a><label style=\"margin-top:8px\">OTA Password</label><input id=\"otaPw\" type=\"password\" placeholder=\"Leave blank for no password\"><p style=\"font-size:.77rem;color:var(--dim);margin-top:4px\">Protects ArduinoOTA (IDE/command-line) uploads. The web updater above is always accessible.</p><button onclick=\"saveOTA()\" style=\"margin-top:6px\">Save OTA Password</button><div id=\"otaR\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
  server.sendContent("</div>");
  server.sendContent("<div id=\"pane-statusp\" class=\"pane\">");
  server.sendContent("<div class=\"card\"><h3 class=\"sgh\">Network</h3><div class=\"grid3\">");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-ip\">-</div><div class=\"k\">WiFi IP</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-ap\">-</div><div class=\"k\">AP IP</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-mq\">-</div><div class=\"k\">MQTT</div></div>");
  server.sendContent("</div></div>");
  server.sendContent("<div class=\"card\"><h3 class=\"sgh\">System Health</h3><div class=\"grid3\">");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-up\">-</div><div class=\"k\">Uptime</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-hp\">-</div><div class=\"k\">Free Heap</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-mh\">-</div><div class=\"k\">Min Heap Ever</div></div>");
  server.sendContent("<div class=\"stat\" title=\"Lowest minimum-ever free stack across tasks. Trending toward 0 warns of a stack overflow before it crashes.\"><div class=\"v\" id=\"s-stk\">-</div><div class=\"k\">Stack Min (task)</div></div>");
  server.sendContent("</div></div>");
  server.sendContent("<div class=\"card\"><h3 class=\"sgh\">RS-485 Bus</h3><div class=\"grid3\">");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-rx\">-</div><div class=\"k\">Frames RX</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-tx\">-</div><div class=\"k\">Frames TX</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-mod\">-</div><div class=\"k\">Modules</div></div>");
  server.sendContent("</div></div>");
  server.sendContent("<div class=\"card\"><h3 class=\"sgh\">Clock</h3><div class=\"grid3\">");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-rtc\">-</div><div class=\"k\">Gateway Time</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-ntp\">-</div><div class=\"k\">NTP Sync</div></div>");
  server.sendContent("</div></div></div>");
  server.sendContent("<div id=\"diagModal\" style=\"display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.75);z-index:200;align-items:center;justify-content:center\" onclick=\"if(event.target===this)closeDiag()\"><div style=\"background:var(--card);border:2px solid var(--acc);border-radius:10px;padding:18px;max-width:520px;width:92%;max-height:88vh;overflow:auto\"><div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:8px\"><h3 id=\"diagTitle\" style=\"margin:0;color:var(--hi)\">Diagnostics</h3><button class=\"sec\" style=\"margin:0;padding:4px 12px\" onclick=\"closeDiag()\">Close</button></div><div id=\"diagBody\" style=\"font-size:.85rem\">-</div></div></div>");
  server.sendContent("<script>");
  server.sendContent("function show(id,el){document.querySelectorAll(\".pane\").forEach(function(p){p.classList.remove(\"on\");});document.querySelectorAll(\"nav a\").forEach(function(a){a.classList.remove(\"on\");});document.getElementById(\"pane-\"+id).classList.add(\"on\");el.classList.add(\"on\");if(id===\"display\"){startWall();}else{stopWall();}if(id===\"calib\"){calLoadModules();}if(id===\"modules\"){loadModules();}}");
  server.sendContent("var _wallTimer=null;function buildWall(s){var w=document.getElementById(\"wall\");if(!w)return;var cells=s.cells||[];var html=\"\";var idx=0;for(var r=0;r<s.rows;r++){html+=\"<div class='wallrow'>\";for(var c=0;c<s.cols;c++){var v=(idx<cells.length)?cells[idx]:null;var cls=\"flap\",disp=\"\";if(v===null){cls+=\" empty\";disp=\"\";}else if(v===\"?\"){cls+=\" unknown\";disp=\"?\";}else{disp=v===\" \"?\"&nbsp;\":v;}html+=\"<div class='\"+cls+\"'>\"+disp+\"</div>\";idx++;}html+=\"</div>\";}w.innerHTML=html;var known=cells.filter(function(x){return x!==null;}).length;document.getElementById(\"wallMeta\").textContent=s.rows+\" x \"+s.cols+\" grid - \"+known+\" module(s) mapped\";}");
  server.sendContent("function refreshWall(){fetch(\"/api/display/state\").then(function(r){return r.json();}).then(buildWall).catch(function(){var m=document.getElementById(\"wallMeta\");if(m)m.textContent=\"Could not load display state\";});}");
  server.sendContent("function startWall(){refreshWall();if(_wallTimer)clearInterval(_wallTimer);_wallTimer=setInterval(function(){if(document.getElementById(\"pane-display\").classList.contains(\"on\"))refreshWall();},1500);}");
  server.sendContent("function stopWall(){if(_wallTimer){clearInterval(_wallTimer);_wallTimer=null;}}");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("var FC=\" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw\";");
  server.sendContent("function sfDecode(raw){var s=raw.replace(/[\\r\\n]+$/,\"\");if(s.length<2||s[0]!==\"m\")return \"\";if(s[1]===\"X\"){if(s.indexOf(\"mXadv:\")==0)return \"ADV  unprovisioned SN: \"+s.slice(6);if(s.indexOf(\"mXack:\")==0){var r=s.slice(6),ci=r.lastIndexOf(\":\");return ci>=0?\"ACK  SN \"+r.slice(0,ci)+\" -> ID \"+r.slice(ci+1):\"ACK \"+r;}if(s[2]===\"I\"){var ci2=s.indexOf(\":\",3);return ci2>=0?\"PROVISION  SN \"+s.slice(3,ci2)+\" -> ID \"+s.slice(ci2+1):\"PROVISION \"+s.slice(3);}if(s[2]===\"H\")return \"PROVISION  home SN \"+s.slice(3);if(s[2]===\"D\")return \"DUMP       SN \"+s.slice(3);if(s[2]===\"A\")return \"ALL FIELDS SN \"+s.slice(3);if(s[2]===\"F\")return \"FACTORY RST SN \"+s.slice(3);if(s[2]===\"W\")return \"RESTORE    \"+s.slice(3);return \"PROVISIONING \"+s.slice(2);}var p=1,id=\"\",bc=false;while(p<s.length&&(s[p]===\"*\"||s[p]>=\"0\"&&s[p]<=\"9\")){if(s[p]===\"*\")bc=true;else id+=s[p];p++;}var who=bc?\"ALL\":\"#\"+id;if(p>=s.length)return who+\" (incomplete)\";var cmd=s[p],rest=s.slice(p+1);if(cmd===\"-\"){var fi=FC.indexOf(rest[0]||\"\");var sfx=fi>=0?\" (idx \"+fi+\")\":\"\";return \"SHOW CHAR    \"+who+\" -> [\"+(rest[0]||\"?\")+\"]\"+sfx;}if(cmd===\"+\"){var n=parseInt(rest);var ch=isNaN(n)?\"?\":(FC[n]||\"?\");return \"SHOW INDEX   \"+who+\" -> \"+n+\" [\"+ch+\"]\";}if(cmd===\"h\")return \"HOME         \"+who;if(cmd===\"c\")return rest?\"CALIB RESP   \"+who+\" \"+rest+\" steps/rev\":\"CALIBRATE    \"+who;if(cmd===\"o\")return \"HOME OFFSET  \"+who+\" = \"+rest+\" steps\";if(cmd===\"t\")return \"TOTAL STEPS  \"+who+\" = \"+rest;if(cmd===\"s\")return \"NUDGE        \"+who+\" \"+rest+\" steps\";if(cmd===\"g\")return \"GOTO STEP    \"+who+\" -> step \"+rest;if(cmd===\"w\"){var wci=rest.indexOf(\":\");return wci>=0?\"WRITE POS    \"+who+\" idx \"+rest.slice(0,wci)+\" -> \"+rest.slice(wci+1)+\" steps\":\"WRITE POS    \"+who+\" \"+rest;}if(cmd===\"i\")return \"SET ID       \"+who+\" -> ID \"+rest;if(cmd===\"a\")return \"AUTO-HOME    \"+who+(rest===\"1\"?\" ON\":\" OFF\");if(cmd===\"d\")return rest&&rest[0]===\":\"?\"DUMP RESP    \"+who+\" \"+rest.slice(1):\"DUMP?        \"+who;if(cmd===\"e\")return \"ERASE MAP    \"+who;if(cmd===\":\")return \"CALIB RESP   \"+who+\" \"+rest+\" steps/rev\";if(cmd===\"v\"){if(rest&&rest[0]===\":\"){var vp=rest.slice(1).split(\":\");var vs=\"VERSION RESP \"+who+\" fw:\"+vp[0];if(vp.length>1&&vp[1]!==\"\")vs+=\" id:\"+vp[1];if(vp.length>2&&vp[2]!==\"\")vs+=\" sn:\"+vp[2];return vs;}return \"VERSION?     \"+who;}if(cmd===\"A\"){if(rest&&rest[0]===\":\"){var ap=rest.slice(1).split(\":\");var as=\"ALL RESP     \"+who+\" fw:\"+ap[0];if(ap[1])as+=\" id:\"+ap[1];if(ap[2])as+=\" sn:\"+ap[2];if(ap[3]!==undefined)as+=\" ho:\"+ap[3];if(ap[4]!==undefined)as+=\" ts:\"+ap[4];if(ap[5]!==undefined)as+=\" ah:\"+ap[5];if(ap[6]!==undefined)as+=\" ci:\"+ap[6];return as;}return \"ALL FIELDS   \"+who;}if(cmd===\"R\")return \"RESET PROV   \"+who;if(cmd===\"F\")return \"FACTORY RST  \"+who;return \"CMD [\"+cmd+\"] \"+who+(rest?\" \"+rest:\"\");}");
  server.sendContent("var _lfc=0;var _loglines=[];function loadMonPrefs(){try{var a=localStorage.getItem(\"sfgw_asc\");if(a!==null)document.getElementById(\"asc\").checked=(a===\"1\");var p=localStorage.getItem(\"sfgw_pause\");if(p!==null)document.getElementById(\"lpause\").checked=(p===\"1\");}catch(e){}}function saveMonPrefs(){try{localStorage.setItem(\"sfgw_asc\",document.getElementById(\"asc\").checked?\"1\":\"0\");localStorage.setItem(\"sfgw_pause\",document.getElementById(\"lpause\").checked?\"1\":\"0\");}catch(e){}}function downloadLog(){if(!_loglines.length){return;}var blob=new Blob([_loglines.join(\"\\n\")+\"\\n\"],{type:\"text/plain\"});var url=URL.createObjectURL(blob);var a=document.createElement(\"a\");var ts=new Date().toISOString().replace(/[:.]/g,\"-\").slice(0,19);a.href=url;a.download=\"splitflap-buslog-\"+ts+\".txt\";document.body.appendChild(a);a.click();document.body.removeChild(a);URL.revokeObjectURL(url);}loadMonPrefs();");
  server.sendContent("function clearLog(){document.getElementById(\"log\").innerHTML=\"\";_lfc=0;document.getElementById(\"logCount\").textContent=\"0 frames\";}");
  server.sendContent("function appendLogRow(m){var log=document.getElementById(\"log\");var ascii=(m.command||\"\").replace(/[\\r\\n]/g,\"\");var desc=sfDecode(ascii);var safeR=ascii.replace(/&/g,\"&amp;\").replace(/</g,\"&lt;\").replace(/>/g,\"&gt;\");var safeD=desc.replace(/&/g,\"&amp;\").replace(/</g,\"&lt;\").replace(/>/g,\"&gt;\");var ts=m.ep?new Date(m.ep*1000).toLocaleTimeString([],{hour12:false}):(m.wt?m.wt:(m.ts<60000?m.ts+\"ms\":Math.floor(m.ts/1000)+\"s\"));var row=document.createElement(\"div\");row.className=\"logrow \"+(m.dir===\"R\"?\"rx\":\"tx\");row.innerHTML=\"<span class=\\\"lts\\\">\"+ts+\"</span>\"+\"<span class=\\\"ldir\\\">\"+(m.dir===\"R\"?\"RX\":\"TX\")+\"</span>\"+\"<span class=\\\"lraw\\\">\"+safeR+\"</span>\";if(desc)row.innerHTML+=\"<span class=\\\"ldesc\\\">\"+safeD+\"</span>\";if(m.san)row.innerHTML+=\"<span class=\\\"lsan\\\">sanitized</span>\";log.appendChild(row);_loglines.push(ts+\" \"+(m.dir===\"R\"?\"RX\":\"TX\")+\" \"+ascii+(desc?\"  [\"+desc+\"]\":\"\")+(m.san?\"  (sanitized)\":\"\"));if(_loglines.length>5000)_loglines.splice(0,_loglines.length-5000);_lfc++;document.getElementById(\"logCount\").textContent=_lfc+\" frame\"+(_lfc===1?\"\":\"s\");if(document.getElementById(\"asc\").checked)log.scrollTop=log.scrollHeight;}");
  server.sendContent("setInterval(function(){if(document.getElementById(\"lpause\").checked)return;fetch(\"/api/rs485/messages\").then(function(r){return r.json();}).then(function(arr){arr.forEach(appendLogRow);}).catch(function(){});},600);");
  server.sendContent("function doSend(){var data=document.getElementById(\"sdata\").value.trim();if(!data){document.getElementById(\"sr\").textContent=\"Nothing to send.\";return;}var raw=document.getElementById(\"sraw\").checked;fetch(\"/api/rs485/send\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({data:data,raw:raw})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"sr\").textContent=j.ok?(\"Sent \"+j.bytes+\" bytes\"+(j.raw?\" (raw)\":\"\")):(\"Error: \"+j.error);}).catch(function(e){document.getElementById(\"sr\").textContent=\"Error: \"+e;});}");
  server.sendContent("function apiFlapCmd(path,body){return fetch(path,{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify(body)}).then(function(r){return r.json();});}");
  server.sendContent("function parseDump(raw){if(!raw)return{error:\"No data\"};var parts=raw.split(\":\");if(parts.length<2)return{error:\"Invalid format\",raw:raw};var r={homeOffset:parseInt(parts[0]),totalSteps:parseInt(parts[1]),map:{}};if(parts[2])parts[2].split(\",\").forEach(function(e){var kv=e.split(\"=\");if(kv.length===2&&kv[0]!==\"\")r.map[parseInt(kv[0])]=parseInt(kv[1]);});return r;}");
  server.sendContent("function refreshModules(){var el=document.getElementById(\"refreshR\");el.textContent=\"Identifying...\";fetch(\"/api/flap/identify\",{method:\"POST\"}).then(function(r){return r.json();}).then(function(j){el.textContent=j.ok?\"List cleared, identifying all modules -- refreshing in 2s\":\"Error: \"+j.error;if(j.ok)setTimeout(function(){loadModules();},2000);setTimeout(function(){loadModules();el.textContent=\"\";},7000);}).catch(function(e){el.textContent=\"Error: \"+e;});}");
  server.sendContent("function doBackup(){var prog=document.getElementById(\"backupProg\");var res=document.getElementById(\"backupR\");res.textContent=\"\";prog.textContent=\"Loading module list...\";fetch(\"/api/flap/modules\").then(function(r){return r.json();}).then(function(mods){var targets=mods.filter(function(m){return m.sn&&m.sn.length>0;});if(!targets.length){prog.textContent=\"\";res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">No modules with serial numbers found. Run Identify All first.</span>\";return;}var out={version:1,created:new Date().toISOString(),modules:[]};var i=0,okN=0,failN=0;function next(){if(i>=targets.length){prog.textContent=\"\";if(!out.modules.length){res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">No calibration data could be read.</span>\";return;}var blob=new Blob([JSON.stringify(out,null,2)],{type:\"application/json\"});var url=URL.createObjectURL(blob);var a=document.createElement(\"a\");var ts=new Date().toISOString().replace(/[:.]/g,\"-\").slice(0,19);a.href=url;a.download=\"splitflap-backup-\"+ts+\".json\";document.body.appendChild(a);a.click();document.body.removeChild(a);URL.revokeObjectURL(url);res.innerHTML=\"<span style=\\\"color:var(--grn)\\\">Backup created: \"+okN+\" module(s) saved\"+(failN?\", \"+failN+\" failed\":\"\")+\".</span>\";return;}var m=targets[i];prog.textContent=\"Reading module \"+(i+1)+\" of \"+targets.length+\" (SN \"+m.sn+\")...\";fetch(\"/api/flap/dump\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:m.id})}).then(function(r){return r.json();}).then(function(d){if(d.ok&&d.dump){out.modules.push({sn:m.sn,id:m.id,dump:d.dump});okN++;}else{failN++;}i++;next();}).catch(function(){failN++;i++;next();});}next();}).catch(function(e){prog.textContent=\"\";res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">Error: \"+e+\"</span>\";});}function parseBackupDump(raw){var parts=raw.split(\":\");if(parts.length<2)return null;var ho=parseInt(parts[0]),ts=parseInt(parts[1]);if(isNaN(ho)||isNaN(ts))return null;var map=parts.length>2?parts.slice(2).join(\":\"):\"\";return {homeOffset:ho,totalSteps:ts,map:map};}function doRestore(){var prog=document.getElementById(\"restoreProg\");var res=document.getElementById(\"restoreR\");var fileInput=document.getElementById(\"restoreFile\");var preserve=document.getElementById(\"preserveId\").checked;res.textContent=\"\";if(!fileInput.files||!fileInput.files.length){res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">Choose a backup file first.</span>\";return;}var reader=new FileReader();reader.onload=function(){var data;try{data=JSON.parse(reader.result);}catch(e){res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">Invalid JSON file.</span>\";return;}if(!data||!Array.isArray(data.modules)||!data.modules.length){res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">No modules found in backup file.</span>\";return;}var mods=data.modules,i=0,okN=0,failN=0;function next(){if(i>=mods.length){prog.textContent=\"\";res.innerHTML=\"<span style=\\\"color:var(--grn)\\\">Restore complete: \"+okN+\" module(s)\"+(failN?\", \"+failN+\" failed\":\"\")+\".</span>\"+(preserve?\"\":\" IDs were reassigned from the backup.\");return;}var m=mods[i];if(!m.sn){failN++;i++;next();return;}var p=parseBackupDump(m.dump||\"\");if(!p){failN++;i++;next();return;}prog.textContent=\"Restoring module \"+(i+1)+\" of \"+mods.length+\" (SN \"+m.sn+\")...\";fetch(\"/api/flap/restorebysn\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({sn:m.sn,homeOffset:p.homeOffset,totalSteps:p.totalSteps,map:p.map})}).then(function(r){return r.json();}).then(function(d){if(!d.ok){failN++;i++;next();return;}if(!preserve&&typeof m.id===\"number\"&&m.id>=0){fetch(\"/api/flap/provision\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({sn:m.sn,id:m.id})}).then(function(r){return r.json();}).then(function(){okN++;i++;next();}).catch(function(){okN++;i++;next();});}else{okN++;i++;next();}}).catch(function(){failN++;i++;next();});}next();};reader.onerror=function(){res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">Could not read file.</span>\";};reader.readAsText(fileInput.files[0]);}");
  server.sendContent("function loadModules(){fetch(\"/api/flap/modules\").then(function(r){return r.json();}).then(function(arr){var g=document.getElementById(\"modGrid\");if(!arr.length){g.innerHTML=\"<p style='color:var(--dim)'>No modules detected yet.</p>\";return;}var h=\"\";arr.forEach(function(m){var legacy=m.provisioned&&m.lastSeen>0&&!m.fwVersion&&!m.sn&&!m.acked;var cls=m.provisioned?\"mod\":\"mod unprovisioned\";var idStr=m.provisioned?(\"ID: <span class='mid'>\"+m.id+\"</span>\"+(legacy?\"<span class='mlegacy' title='Firmware v7 or earlier: no serial number, no provisioning, no factory reset. Homing and calibration are fully supported.'>LEGACY</span>\":\"\")+(m.dupSuspect?\"<span style='display:inline-block;margin-left:6px;padding:0 5px;border-radius:3px;background:var(--hi);color:#fff;font-size:.66rem;font-weight:bold;vertical-align:middle' title='Repeated corrupt version replies for this ID -- two modules may share it. Deprovision and reassign unique IDs.'>DUP ID?</span>\":\"\")):\"<span style='color:var(--hi)'>Unprovisioned</span>\";var charStr=m.flapChar?\"Showing: <b>\"+m.flapChar+\"</b>\":\"\";var delBtn=legacy?(\"<button class='micon del dis' title='Not available on legacy (v7) modules' onclick=\\\"event.stopPropagation()\\\">&#x1f5d1;</button>\"):(\"<button class='micon del' title='Destructive actions' onclick=\\\"openDel(\"+m.id+\")\\\">&#x1f5d1;</button>\");var fwn=m.fwVersion?parseInt(m.fwVersion):0;var diagBtn=(m.provisioned&&fwn>=26)?(\"<button class='micon' title='Run self-diagnostics (mechanical + stats)' onclick=\\\"runDiag(\"+m.id+\")\\\">&#x1fa7a;</button>\"):\"\";var icons=m.provisioned?(\"<div class='micons'>\"+\"<button class='micon' title='Home' onclick=\\\"modHome(\"+m.id+\")\\\">&#x2302;</button>\"+\"<button class='micon' title='Info / EEPROM' onclick=\\\"openInfo(\"+m.id+\")\\\">&#x2139;</button>\"+diagBtn+delBtn+\"</div>\"):\"\";var snStr=legacy?\"SN: <span style='color:var(--dim)'>n/a (legacy)</span>\":(\"SN: \"+m.sn);var fwStr=legacy?\"<br><span class='mc'>FW: v7 or earlier</span>\":(m.fwVersion?\"<br><span class='mc'>FW: \"+(m.fwVersion==\"<18\"?\"older (pre-v18)\":(\"v\"+m.fwVersion))+\"</span>\":\"\");h+=\"<div class='\"+cls+\"' data-mid='\"+m.id+\"'>\"+icons+idStr+\"<br><span class='mc'>\"+snStr+\"</span>\"+(charStr?\"<br><span class='mc'>\"+charStr+\"</span>\":\"\")+fwStr+\"</div>\";});g.innerHTML=h;}).catch(function(){document.getElementById(\"modGrid\").innerHTML=\"Error loading modules\";});}");
  server.sendContent("loadModules();setInterval(loadModules,5000);");
  server.sendContent("function loadUnprovisioned(){fetch(\"/api/flap/modules\").then(function(r){return r.json();}).then(function(arr){var el=document.getElementById(\"unprovList\");var up=arr.filter(function(m){return !m.provisioned;});if(!up.length){el.innerHTML=\"<p style=\\\"color:var(--dim)\\\">No unprovisioned modules seen yet.</p>\";return;}var h=\"\";up.forEach(function(m){h+=\"<div style=\\\"display:flex;align-items:center;gap:8px;margin-bottom:8px;flex-wrap:wrap\\\">\"+\"<code style=\\\"color:var(--ylw);flex:1;min-width:160px\\\">\"+m.sn+\"</code>\"+\"<button class=\\\"sec\\\" style=\\\"margin:0;padding:4px 10px;font-size:.78rem\\\" onclick=\\\"doHomeSN('\"+m.sn+\"')\\\" title=\\\"Home this module to identify it\\\">Home</button>\"+\"</div>\";});el.innerHTML=h;}).catch(function(){});}");
  server.sendContent("setInterval(loadUnprovisioned,10000);loadUnprovisioned();");
  server.sendContent("function modHome(id){apiFlapCmd(\"/api/flap/home\",{id:id}).then(function(j){loadModules();});}var _infoId=-1;var _delId=-1;function openInfo(id){_infoId=id;document.getElementById(\"modModalTitle\").textContent=\"Module #\"+id;document.getElementById(\"modModal\").style.display=\"flex\";fetchInfo(id);}function refreshDump(){if(_infoId>=0)fetchInfo(_infoId);}function closeModal(){document.getElementById(\"modModal\").style.display=\"none\";_infoId=-1;}");
  server.sendContent("function fetchInfo(id){var b=document.getElementById(\"modModalBody\");b.innerHTML=\"<p style='color:var(--dim)'>Reading module #\"+id+\" ...</p>\";var st=document.getElementById(\"modModalStatus\");st.textContent=\"\";fetch(\"/api/flap/modules\").then(function(r){return r.json();}).then(function(arr){var m=null;arr.forEach(function(x){if(x.id===id)m=x;});var fwn=(m&&m.fwVersion)?parseInt(m.fwVersion):0;function done(d,fresh){if(m&&d){if(d.ver)m.fwVersion=d.ver;if(d.sn)m.sn=d.sn;}b.innerHTML=renderInfo(m,d);st.textContent=(d&&d.stale)?\"EEPROM is cached -- click Refresh for a fresh read\":fresh;}if(fwn>=25){fetch(\"/api/flap/all\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:id})}).then(function(r){return r.json();}).then(function(a){done(a,\"Version + EEPROM read in one request (A)\");}).catch(function(e){b.innerHTML=renderInfo(m,{ok:false,error:String(e)});});}else{var vr=null;fetch(\"/api/flap/version\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:id})}).then(function(r){return r.json();}).catch(function(){return {};}).then(function(v){vr=v;return fetch(\"/api/flap/dump\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:id})});}).then(function(r){return r.json();}).then(function(d){if(m&&vr){if(vr.ver)m.fwVersion=vr.ver;if(vr.sn)m.sn=vr.sn;}done(d,\"EEPROM read fresh from module\");}).catch(function(e){b.innerHTML=renderInfo(m,{ok:false,error:String(e)});});}}).catch(function(e){b.innerHTML=\"<p style='color:var(--hi)'>Error: \"+e+\"</p>\";});}");
  server.sendContent("function mrow(k,v){return \"<div class='mfield'><span class='mk'>\"+k+\"</span><span class='mv'>\"+v+\"</span></div>\";}function renderInfo(m,d){var h=\"\";if(m){var legacy=m.provisioned&&m.lastSeen>0&&!m.fwVersion&&!m.sn&&!m.acked;if(m.dupSuspect)h+=mrow(\"<span style='color:var(--hi)'>Possible Duplicate ID</span>\",\"<span style='color:var(--hi)'>Repeated corrupt version replies; two modules may share ID \"+m.id+\". Deprovision and reassign unique IDs.</span>\");h+=mrow(\"Module ID\",m.id)+mrow(\"Serial Number\",legacy?\"n/a (legacy module)\":(m.sn||\"-\"))+mrow(\"Provisioned\",m.provisioned?(legacy?\"yes (hardcoded ID)\":\"yes\"):\"no\")+mrow(\"Firmware\",m.fwVersion?(\"v\"+m.fwVersion):(legacy?\"v7 or earlier\":\"unknown\"))+mrow(\"Last Char\",m.flapChar?m.flapChar:\"-\")+mrow(\"Last Seen\",m.lastSeenEpoch?new Date(m.lastSeenEpoch*1000).toLocaleString():(m.lastSeen?\"seen (clock not set)\":\"-\"));if(d&&typeof d.reportedId===\"number\"&&d.reportedId>=0&&d.reportedId!==m.id)h+=mrow(\"Self-Reported ID\",\"<span style='color:var(--hi)'>\"+d.reportedId+\" (differs from address \"+m.id+\")</span>\");}else{h+=mrow(\"Module ID\",\"(not in registry)\");}h+=\"<div class='sgh' style='margin-top:12px'>EEPROM</div>\";if(!d||!d.ok){h+=\"<p style='color:var(--hi)'>\"+((d&&d.error)?d.error:\"No EEPROM data\")+\"</p>\";return h;}var p=parseDump(d.dump||\"\");if(p.error){h+=\"<p style='color:var(--hi)'>\"+p.error+\"</p>\";return h;}h+=mrow(\"Home Offset\",p.homeOffset+\" steps\")+mrow(\"Steps / Rev\",p.totalSteps);if(d.autoHome===0||d.autoHome===1)h+=mrow(\"Auto-Home\",d.autoHome?\"On (home on boot)\":\"Off (restore saved position)\");if(typeof d.curIndex===\"number\"&&d.curIndex!==-99)h+=mrow(\"Current Flap\",d.curIndex===-2?\"released\":(d.curIndex===-1?\"unknown\":d.curIndex));var keys=Object.keys(p.map);h+=mrow(\"Calibrated Flaps\",keys.length+\" / 64\");if(keys.length){h+=\"<div class='mmap'>\";keys.forEach(function(k){h+=\"[\"+k+\"]=\"+p.map[k]+\" \";});h+=\"</div>\";}return h;}");
  server.sendContent("function openDel(id){_delId=id;document.getElementById(\"delModalTitle\").textContent=\"Module #\"+id+\" -- Destructive Actions\";document.getElementById(\"delModalStatus\").textContent=\"\";document.getElementById(\"delModal\").style.display=\"flex\";}function closeDelModal(){document.getElementById(\"delModal\").style.display=\"none\";_delId=-1;}function delAction(kind){if(_delId<0)return;var names={erase:\"Erase EEPROM\",factoryreset:\"Factory Reset\",deprovision:\"De-provision\"};if(!confirm(names[kind]+\" on module #\"+_delId+\"? This cannot be undone.\"))return;var st=document.getElementById(\"delModalStatus\");st.textContent=names[kind]+\" in progress...\";apiFlapCmd(\"/api/flap/\"+kind,{id:_delId}).then(function(j){st.textContent=j.ok?(names[kind]+\" sent.\"):(\"Error: \"+(j.error||\"failed\"));if(j.ok){setTimeout(function(){closeDelModal();loadModules();},900);}}).catch(function(e){st.textContent=\"Error: \"+e;});}");
  server.sendContent("function sendChar(){var id=parseInt(document.getElementById(\"scId\").value);var ch=document.getElementById(\"scChar\").value.toUpperCase().slice(0,1);var r=document.getElementById(\"scr\");if(!ch){r.textContent=\"Enter a character\";return;}apiFlapCmd(\"/api/flap/char\",{id:id,\"char\":ch}).then(function(j){r.textContent=j.ok?(\"Sent '\"+ch+\"' to \"+(id<0?\"all modules\":\"module #\"+id)):(\"Error: \"+(j.error||\"failed\"));}).catch(function(e){r.textContent=\"Error: \"+e;});}");
  server.sendContent("function sendText(){var text=document.getElementById(\"dispText\").value.toUpperCase();var start=parseInt(document.getElementById(\"dispStart\").value);apiFlapCmd(\"/api/flap/text\",{text:text,start:start}).then(function(j){document.getElementById(\"dr\").textContent=j.ok?\"Sent \"+j.chars+\" chars\":\"Error: \"+j.error;});}");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("function sendIndex(){var id=parseInt(document.getElementById(\"idxId\").value);var idx=parseInt(document.getElementById(\"idxVal\").value);apiFlapCmd(\"/api/flap/index\",{id:id,index:idx}).then(function(j){document.getElementById(\"dr\").textContent=j.ok?\"Sent\":\"Error: \"+j.error;});}");
  server.sendContent("function doHomeSN(sn){document.getElementById(\"provSN\").value=sn;apiFlapCmd(\"/api/flap/homebysn\",{sn:sn}).then(function(j){var el=document.getElementById(\"provR\");if(el)el.textContent=j.ok?\"Homing SN: \"+sn+\" - check which module moves\":\"Error: \"+j.error;});}");
  server.sendContent("function doProvision(){var sn=document.getElementById(\"provSN\").value;var id=parseInt(document.getElementById(\"provId\").value);apiFlapCmd(\"/api/flap/provision\",{sn:sn,id:id}).then(function(j){document.getElementById(\"provR\").textContent=j.ok?\"Provisioning sent\":\"Error: \"+j.error;});}");
  server.sendContent("function doDeprovision(){var id=parseInt(document.getElementById(\"deprovId\").value);apiFlapCmd(\"/api/flap/deprovision\",{id:id}).then(function(j){document.getElementById(\"deprovR\").textContent=j.ok?(\"De-provisioned \"+(id<0?\"all modules\":\"module \"+id)):\"Error: \"+j.error;if(j.ok){loadModules();loadUnprovisioned();}});}");
  server.sendContent("function saveWifi(){fetch(\"/api/config/wifi\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({ssid:document.getElementById(\"wSSID\").value,pass:document.getElementById(\"wPASS\").value})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"wr\").textContent=j.ok?\"WiFi saved - reconnecting...\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"wr\").textContent=\"Error: \"+e;});}");
  server.sendContent("function testMqtt(){var el=document.getElementById(\"mr\");el.textContent=\"Testing...\";fetch(\"/api/mqtt/test\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({host:document.getElementById(\"mqH\").value,port:parseInt(document.getElementById(\"mqP\").value)||1883,user:document.getElementById(\"mqU\").value,pass:document.getElementById(\"mqPw\").value})}).then(function(r){return r.json();}).then(function(j){el.textContent=j.ok?\"Broker OK - connected and authenticated\":\"Failed: \"+(j.detail||j.error||\"unknown\");}).catch(function(e){el.textContent=\"Error: \"+e;});}");
  server.sendContent("function saveMqtt(){fetch(\"/api/config/mqtt\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({host:document.getElementById(\"mqH\").value,port:parseInt(document.getElementById(\"mqP\").value),user:document.getElementById(\"mqU\").value,pass:document.getElementById(\"mqPw\").value,prefix:document.getElementById(\"mqPfx\").value})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"mr\").textContent=j.ok?\"MQTT saved - reconnecting...\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"mr\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveTz(){var sel=document.getElementById(\"tzSel\");var ntp=document.getElementById(\"ntpSrv\").value.trim();fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({posixTZ:sel.value,ntpServer:ntp})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"tzR\").textContent=j.ok?\"Time settings saved\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"tzR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveGrid(){var rows=parseInt(document.getElementById(\"gRows\").value)||1;var cols=parseInt(document.getElementById(\"gCols\").value)||1;fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({gridRows:rows,gridCols:cols})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"gridR\").textContent=j.ok?\"Layout saved\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"gridR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveOTA(){fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({otaPassword:document.getElementById(\"otaPw\").value})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"otaR\").textContent=j.ok?\"OTA password saved\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"otaR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveDebug(){var v=document.getElementById(\"dbgChk\").checked;fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({serialDebug:v})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"dbgR\").textContent=j.ok?(v?\"Debug enabled\":\"Debug disabled\"):\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"dbgR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveHa(){var v=document.getElementById(\"haChk\").checked;fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({haEnabled:v})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"haR\").textContent=j.ok?(v?\"Home Assistant integration enabled\":\"Home Assistant integration disabled\"):\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"haR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function toggleQuiet(){var v=document.getElementById(\"quietChk\").checked;fetch(\"/api/quiet\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({on:v})}).then(function(r){return r.json();}).then(function(j){if(typeof setQuietUI===\"function\")setQuietUI(j.on);document.getElementById(\"quietR\").textContent=j.on?\"Quiet Time ON -- flaps will not move for display updates\":\"Quiet Time off -- reels resynced to last requested display\";}).catch(function(e){document.getElementById(\"quietChk\").checked=!v;document.getElementById(\"quietR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function setMaintUI(on){var chk=document.getElementById(\"maintChk\");if(chk)chk.checked=!!on;var lbl=document.querySelector(\".maint-toggle\");if(lbl)lbl.classList.toggle(\"active\",!!on);document.body.classList.toggle(\"maint-on\",!!on);}function toggleMaint(){var chk=document.getElementById(\"maintChk\");var on=chk.checked;fetch(\"/api/maintenance\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({on:on})}).then(function(r){return r.json();}).then(function(j){setMaintUI(j.on);}).catch(function(){chk.checked=!on;setMaintUI(!on);});}function calEnableMaint(){var chk=document.getElementById(\"maintChk\");if(chk&&!chk.checked){chk.checked=true;toggleMaint();}}");
  server.sendContent("function setQuietUI(on){var chk=document.getElementById(\"quietChkHdr\");if(chk)chk.checked=!!on;var lbl=document.querySelector(\".quiet-toggle\");if(lbl)lbl.classList.toggle(\"active\",!!on);document.body.classList.toggle(\"quiet-on\",!!on);var sc=document.getElementById(\"quietChk\");if(sc)sc.checked=!!on;}function toggleQuietHdr(){var chk=document.getElementById(\"quietChkHdr\");var on=chk.checked;fetch(\"/api/quiet\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({on:on})}).then(function(r){return r.json();}).then(function(j){setQuietUI(j.on);}).catch(function(){chk.checked=!on;setQuietUI(!on);});}");
  server.sendContent("function pollStatus(){fetch(\"/api/status\").then(function(r){return r.json();}).then(function(s){var up=s.uptime,ud=Math.floor(up/86400),uh=Math.floor(up%86400/3600),um=Math.floor(up%3600/60),us=up%60;document.getElementById(\"s-up\").textContent=(ud?ud+\"d \":\"\")+(uh||ud?uh+\"h \":\"\")+um+\"m \"+us+\"s\";document.getElementById(\"s-rx\").textContent=s.rx;document.getElementById(\"s-tx\").textContent=s.tx;document.getElementById(\"s-ip\").textContent=s.ip;document.getElementById(\"s-ap\").textContent=s.apip;var hp=document.getElementById(\"s-hp\");hp.textContent=Math.round(s.heap/1024)+\" KB\";hp.className=\"v \"+(s.heap>=40000?\"vok\":(s.heap>=25000?\"vwarn\":\"vbad\"));var mq=document.getElementById(\"s-mq\");mq.textContent=s.mqtt?\"Connected\":\"Off\";mq.className=\"v \"+(s.mqtt?\"vok\":\"\");document.getElementById(\"s-mod\").textContent=s.modules;if(s.minheap){var mh=document.getElementById(\"s-mh\");mh.textContent=Math.round(s.minheap/1024)+\" KB\";mh.className=\"v \"+(s.minheap>=40000?\"vok\":(s.minheap>=25000?\"vwarn\":\"vbad\"));}if(s.stk){var sm=null,sn=\"\";for(var k in s.stk){if(sm===null||s.stk[k]<sm){sm=s.stk[k];sn=k;}}var st=document.getElementById(\"s-stk\");st.textContent=sm+\" B (\"+sn+\")\";st.className=\"v \"+(sm>=800?\"vok\":(sm>=400?\"vwarn\":\"vbad\"));}if(document.getElementById(\"s-rtc\"))document.getElementById(\"s-rtc\").textContent=s.time||\"--\";var ntp=document.getElementById(\"s-ntp\");if(ntp){ntp.textContent=s.ntpSynced?\"Synced\":\"Pending\";ntp.className=\"v \"+(s.ntpSynced?\"vok\":\"vwarn\");}var b=document.getElementById(\"badge\");b.textContent=s.wifi?\"WiFi: \"+s.ip:\"AP only\";b.className=s.wifi?\"ok\":\"\";if(typeof s.maint!==\"undefined\")setMaintUI(s.maint);if(typeof s.quiet!==\"undefined\")setQuietUI(s.quiet);}).catch(function(){});}setInterval(pollStatus,3000);pollStatus();");
  server.sendContent("fetch(\"/api/config\").then(function(r){return r.json();}).then(function(c){document.getElementById(\"wSSID\").value=c.wSSID||\"\";document.getElementById(\"mqH\").value=c.mqHost||\"\";document.getElementById(\"mqP\").value=c.mqPort||1883;document.getElementById(\"mqU\").value=c.mqUser||\"\";document.getElementById(\"mqPfx\").value=c.mqPfx||\"splitflap\";if(c.posixTZ){var sel=document.getElementById(\"tzSel\");for(var j=0;j<sel.options.length;j++){if(sel.options[j].value===c.posixTZ){sel.selectedIndex=j;break;}}}if(c.ntpServer){document.getElementById(\"ntpSrv\").value=c.ntpServer;}if(c.gridRows){document.getElementById(\"gRows\").value=c.gridRows;}if(c.gridCols){document.getElementById(\"gCols\").value=c.gridCols;}var hc=document.getElementById(\"haChk\");if(hc)hc.checked=!!c.haEnabled;var dc=document.getElementById(\"dbgChk\");if(dc)dc.checked=!!c.serialDebug;});fetch(\"/api/quiet\").then(function(r){return r.json();}).then(function(q){var qc=document.getElementById(\"quietChk\");if(qc)qc.checked=!!q.on;}).catch(function(){});");
  server.sendContent("var CAL_CHARS=\" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw\";var CAL_COLORS={r:\"#e23b3b\",o:\"#ff9f0a\",y:\"#ffd60a\",g:\"#2fb84a\",b:\"#3b82f6\",p:\"#a855f7\",w:\"#e8e8e8\"};");
  server.sendContent("var _calId=-1,_calTotal=4096,_calHo=0,_calMap={},_tuneIdx=-1;var CAL_UNSET=65535,CAL_DEF_HO=2832,CAL_DEF_TS=4096;var _wizIdx=-1,_wizTarget=0;");
server.sendContent("var _bmFlap=-1,_bmSt=null,_bmArr=[],_bmCorrected={},_bmMaintWas=false;");
  server.sendContent("function calDefault(i){var ts=(_calTotal&&_calTotal>0)?_calTotal:CAL_DEF_TS;return Math.floor((i*ts)/64);}");
  server.sendContent("function calParseDump(raw){var r={homeOffset:0,totalSteps:4096,map:{}};if(!raw)return r;var parts=raw.split(\":\");r.homeOffset=parseInt(parts[0]);r.totalSteps=parseInt(parts[1]);if(isNaN(r.totalSteps))r.totalSteps=4096;if(parts.length>2){var rest=parts.slice(2).join(\":\");rest.split(\",\").forEach(function(e){var kv=e.split(\"=\");if(kv.length===2&&kv[0]!==\"\"){var k=parseInt(kv[0]),v=parseInt(kv[1]);if(!isNaN(k)&&!isNaN(v)&&v!==CAL_UNSET)r.map[k]=v;}});}return r;}");
  server.sendContent("function calLayout(cb){Promise.all([fetch(\"/api/display/state\").then(function(r){return r.json();}),fetch(\"/api/flap/modules\").then(function(r){return r.json();})]).then(function(res){cb(res[0]||{},res[1]||[]);}).catch(function(){cb(null,null);});}");
  server.sendContent("function calBuildGrid(el,st,arr,cellFn){var rows=st.rows||1,cols=st.cols||16,count=rows*cols;el.classList.remove(\"single\");el.style.gridTemplateColumns=\"repeat(\"+cols+\",minmax(0,1fr))\";var byId={};arr.forEach(function(m){if(m&&m.provisioned)byId[m.id]=m;});var h=\"\";for(var id=0;id<count;id++){h+=cellFn(id,byId[id]);}el.innerHTML=h;}");
  server.sendContent("function calLoadModules(){var el=document.getElementById(\"calMods\");el.textContent=\"Loading layout...\";calLayout(function(st,arr){if(!st){el.innerHTML=\"<span style='color:var(--hi)'>Error loading layout</span>\";return;}calBuildGrid(el,st,arr,function(id,m){var legacy=m&&m.lastSeen>0&&!m.fwVersion&&!m.sn&&!m.acked;var known=m&&m.fwVersion;var cls=\"cmod\";if(id===_calId)cls+=\" sel\";if(legacy)cls+=\" legacy\";else if(known)cls+=\" known\";else if(!m)cls+=\" unknown\";var sub=legacy?\"<span class='csn lg'>v7</span>\":(m&&m.sn?\"<span class='csn'>\"+m.sn.slice(-4)+\"</span>\":\"<span class='csn'>--</span>\");return \"<div class='\"+cls+\"' data-id='\"+id+\"' onclick='calSelect(\"+id+\")'>\"+id+sub+\"</div>\";});});}");
  server.sendContent("function bmStart(){var chk=document.getElementById(\"maintChk\");_bmMaintWas=chk?chk.checked:false;calEnableMaint();_bmCorrected={};document.getElementById(\"bmModal\").style.display=\"flex\";document.getElementById(\"bmStat\").textContent=\"Loading board layout...\";calLayout(function(st,arr){if(!st){document.getElementById(\"bmStat\").textContent=\"Error loading layout.\";return;}_bmSt=st;_bmArr=arr;_bmFlap=0;bmShow();});}");
  server.sendContent("function bmShow(){if(_bmFlap<0)return;var i=_bmFlap,n=CAL_CHARS.length;document.getElementById(\"bmProg\").textContent=\"Flap \"+(i+1)+\" of \"+n+\" -- \"+calCharName(i);document.getElementById(\"bmBar\").style.width=Math.round(i/n*100)+\"%\";document.getElementById(\"bmChar\").innerHTML=calCharDisp(i);document.getElementById(\"bmStat\").textContent=\"\";calApi(\"/api/flap/index\",{id:-1,index:i},function(j){if(!j.ok)document.getElementById(\"bmStat\").textContent=\"Move error: \"+j.error;});bmRenderGrid();}");
  server.sendContent("function bmRenderGrid(){var el=document.getElementById(\"bmGrid\");if(!_bmSt){el.textContent=\"No layout.\";return;}var done=_bmCorrected[_bmFlap]||{};calBuildGrid(el,_bmSt,_bmArr,function(id,m){if(!m)return \"<div class='cmod unknown' style='opacity:.3;cursor:default'>\"+id+\"</div>\";var legacy=m.lastSeen>0&&!m.fwVersion&&!m.sn&&!m.acked;var cls=\"cmod\"+(done[id]?\" known\":\"\")+(legacy?\" legacy\":\"\");var sub=m.dupSuspect?\"<span class='csn' style='color:var(--hi)'>DUP?</span>\":(done[id]?\"<span class='csn' style='color:var(--grn)'>fixed</span>\":\"<span class='csn'>\"+(m.sn?m.sn.slice(-4):\"--\")+\"</span>\");return \"<div class='\"+cls+\"' onclick='bmCell(\"+id+\")'>\"+id+sub+\"</div>\";});}");
  server.sendContent("function bmCell(id){if(_bmFlap<0)return;var m=null;_bmArr.forEach(function(x){if(x.id===id)m=x;});if(!m)return;if(m.dupSuspect&&!confirm(\"Module \"+id+\" looks like a DUPLICATE ID -- nudging may move or save the wrong module. Continue?\"))return;document.getElementById(\"bmStat\").textContent=\"Reading module \"+id+\"...\";_calId=id;fetch(\"/api/flap/dump\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:id})}).then(function(r){return r.json();}).then(function(d){if(!d.ok){document.getElementById(\"bmStat\").textContent=\"Error reading module \"+id+\": \"+(d.error||\"no response\");return;}var pp=calParseDump(d.dump||\"\");_calHo=pp.homeOffset;_calTotal=pp.totalSteps;_calMap=pp.map;document.getElementById(\"bmStat\").textContent=\"\";calOpenTune(_bmFlap);}).catch(function(e){document.getElementById(\"bmStat\").textContent=\"Error: \"+e;});}");
  server.sendContent("function bmNext(){if(_bmFlap<CAL_CHARS.length-1){_bmFlap++;bmShow();}else{document.getElementById(\"bmStat\").textContent=\"Last flap reached -- click Finish.\";}}");
  server.sendContent("function bmPrev(){if(_bmFlap>0){_bmFlap--;bmShow();}}");
  server.sendContent("function bmSkip(){bmNext();}");
  server.sendContent("function bmJump(){var v=parseInt(document.getElementById(\"bmJumpIn\").value);if(isNaN(v)||v<1||v>CAL_CHARS.length){document.getElementById(\"bmStat\").textContent=\"Enter a flap number 1-\"+CAL_CHARS.length+\".\";return;}_bmFlap=v-1;bmShow();}");
  server.sendContent("function bmFinish(){var chk=document.getElementById(\"maintChk\");if(chk&&chk.checked!==_bmMaintWas){chk.checked=_bmMaintWas;toggleMaint();}document.getElementById(\"bmModal\").style.display=\"none\";_bmFlap=-1;}");
  server.sendContent("function calSelectAny(){var v=parseInt(document.getElementById(\"calAnyId\").value);if(isNaN(v)||v<0||v>254){alert(\"Enter an ID from 0 to 254.\");return;}calSelect(v);}");
  server.sendContent("function calSelect(id){_calId=id;document.querySelectorAll(\"#calMods .cmod\").forEach(function(el){el.classList.toggle(\"sel\",parseInt(el.dataset.id)===id);});var cell=document.querySelector(\"#calMods .cmod[data-id='\"+id+\"']\");var tag=\"\";if(cell){if(cell.classList.contains(\"legacy\"))tag=\" (legacy v7)\";else if(cell.classList.contains(\"unknown\"))tag=\" (not yet seen)\";}document.getElementById(\"calDetail\").style.display=\"block\";document.getElementById(\"calTitle\").textContent=\"Module \"+id+tag;calRefresh();}");
  server.sendContent("function calRefresh(){if(_calId<0)return;var st=document.getElementById(\"calStatus\");st.textContent=\"Reading EEPROM from module \"+_calId+\"...\";document.getElementById(\"calHoIn\").value=\"\";document.getElementById(\"calTsIn\").value=\"\";document.getElementById(\"calMap\").innerHTML=\"\";fetch(\"/api/flap/dump\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:_calId})}).then(function(r){return r.json();}).then(function(d){if(!d.ok){st.textContent=\"Error: \"+(d.error||\"no response from module\");return;}var p=calParseDump(d.dump||\"\");_calHo=p.homeOffset;_calTotal=p.totalSteps;_calMap=p.map;document.getElementById(\"calHoIn\").value=isNaN(p.homeOffset)?\"\":p.homeOffset;document.getElementById(\"calTsIn\").value=p.totalSteps;st.textContent=d.stale?\"Cached EEPROM -- click Re-read for a fresh value\":\"EEPROM read fresh from module\";calRenderMap();}).catch(function(e){st.textContent=\"Error: \"+e;});}");
  server.sendContent("function calSwatch(ch){var c=CAL_COLORS[ch];return c?\"<span class='sw' style='background:\"+c+\"'></span>\":ch;}");
  server.sendContent("function calRenderMap(){var el=document.getElementById(\"calMap\");var h=\"\";for(var i=0;i<CAL_CHARS.length;i++){var ch=CAL_CHARS[i];var has=_calMap.hasOwnProperty(i);var val=has?_calMap[i]:calDefault(i);var disp=(ch===\" \")?\"<span class='sw' style='background:#000'></span>\":(CAL_COLORS[ch]?calSwatch(ch):ch);h+=\"<div class='cc\"+(has?\" custom\":\"\")+\"' onclick='calOpenTune(\"+i+\")'><div class='cch'>\"+disp+\"</div><div class='ccv'>\"+val+\"</div></div>\";}el.innerHTML=h;}");
  server.sendContent("function calApi(path,body,cb){fetch(path,{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify(body)}).then(function(r){return r.json();}).then(cb).catch(function(e){cb({ok:false,error:String(e)});});}");
  server.sendContent("function calSaveHo(){var v=parseInt(document.getElementById(\"calHoIn\").value);if(isNaN(v)||v<0){document.getElementById(\"calStatus\").textContent=\"Enter a valid home offset.\";return;}calApi(\"/api/flap/homeoffset\",{id:_calId,steps:v},function(j){document.getElementById(\"calStatus\").textContent=j.ok?\"Home offset saved (\"+v+\"). Click Home Motor to verify.\":\"Error: \"+j.error;if(j.ok)_calHo=v;});}");
  server.sendContent("function calRevertHo(){document.getElementById(\"calHoIn\").value=CAL_DEF_HO;calApi(\"/api/flap/homeoffset\",{id:_calId,steps:CAL_DEF_HO},function(j){document.getElementById(\"calStatus\").textContent=j.ok?\"Home offset reset to default (\"+CAL_DEF_HO+\").\":\"Error: \"+j.error;if(j.ok)_calHo=CAL_DEF_HO;});}");
  server.sendContent("function calSaveTs(){var v=parseInt(document.getElementById(\"calTsIn\").value);if(isNaN(v)||v<1){document.getElementById(\"calStatus\").textContent=\"Enter a valid total steps.\";return;}calApi(\"/api/flap/totalsteps\",{id:_calId,steps:v},function(j){document.getElementById(\"calStatus\").textContent=j.ok?\"Total steps saved (\"+v+\").\":\"Error: \"+j.error;if(j.ok){_calTotal=v;calRenderMap();}});}");
  server.sendContent("function calRevertTs(){document.getElementById(\"calTsIn\").value=CAL_DEF_TS;calApi(\"/api/flap/totalsteps\",{id:_calId,steps:CAL_DEF_TS},function(j){document.getElementById(\"calStatus\").textContent=j.ok?\"Total steps reset to default (\"+CAL_DEF_TS+\").\":\"Error: \"+j.error;if(j.ok){_calTotal=CAL_DEF_TS;calRenderMap();}});}");
  server.sendContent("function calCountSteps(){if(_calId<0)return;var btn=document.getElementById(\"calCountBtn\");var st=document.getElementById(\"calStatus\");if(!confirm(\"Count steps on module \"+_calId+\"? The reel will spin a full revolution to measure its steps per revolution.\"))return;btn.disabled=true;btn.textContent=\"Counting...\";st.textContent=\"Calibrating module \"+_calId+\" -- the reel is spinning, please wait (up to ~15s)...\";var jobId=_calId;calApi(\"/api/flap/calibrate\",{id:jobId},function(j){if(!j.ok||!j.started){st.textContent=\"Error: \"+(j.error||\"could not start calibration\");btn.disabled=false;btn.textContent=\"Count Steps\";return;}var tries=0;var poll=setInterval(function(){tries++;if(tries>40){clearInterval(poll);st.textContent=\"Calibration timed out (no response).\";btn.disabled=false;btn.textContent=\"Count Steps\";return;}fetch(\"/api/flap/calibrate/status\").then(function(r){return r.json();}).then(function(s){if(s.state===\"pending\")return;clearInterval(poll);if(s.state===\"done\"&&typeof s.stepsPerRev===\"number\"){var n=s.stepsPerRev;if(_calId===jobId){document.getElementById(\"calTsIn\").value=n;_calTotal=n;calRenderMap();}st.textContent=\"Measured \"+n+\" steps/rev. Saving to module...\";calApi(\"/api/flap/totalsteps\",{id:jobId,steps:n},function(j2){st.textContent=j2.ok?(\"Total steps measured and saved: \"+n+\".\"):(\"Measured \"+n+\" but save failed: \"+j2.error);btn.disabled=false;btn.textContent=\"Count Steps\";});}else{st.textContent=(s.state===\"timeout\")?\"Calibration timed out (no response from module).\":\"Calibration failed.\";btn.disabled=false;btn.textContent=\"Count Steps\";}}).catch(function(){});},500);});}");
  server.sendContent("function calNudge(d){if(_calId<0)return;calApi(\"/api/flap/nudge\",{id:_calId,steps:d},function(j){if(j.ok){_calHo+=d;document.getElementById(\"calHoIn\").value=_calHo;document.getElementById(\"calStatus\").textContent=\"Nudged \"+(d>0?\"+\":\"\")+d+\" (offset now \"+_calHo+\"). Saved instantly.\";}else{document.getElementById(\"calStatus\").textContent=\"Error: \"+j.error;}});}");
  server.sendContent("function calHomeMotor(){if(_calId<0)return;calApi(\"/api/flap/home\",{id:_calId},function(j){document.getElementById(\"calStatus\").textContent=j.ok?\"Homing module \"+_calId+\"...\":\"Error: \"+j.error;});}");
  server.sendContent("function calOpenTune(i){_tuneIdx=i;var ch=CAL_CHARS[i];var has=_calMap.hasOwnProperty(i);var cur=has?_calMap[i]:calDefault(i);var label=(ch===\" \")?\"(blank)\":(CAL_COLORS[ch]?ch.toUpperCase()+\" (color)\":ch);document.getElementById(\"tuneTitle\").textContent=\"Tune: \"+label;document.getElementById(\"tuneExp\").textContent=\"Default: \"+calDefault(i)+(has?\"  |  Current EEPROM: \"+cur:\"  (using default)\");document.getElementById(\"tuneVal\").value=cur;document.getElementById(\"tuneStatus\").textContent=\"\";document.getElementById(\"tuneModal\").style.display=\"flex\";}");
  server.sendContent("function calCloseTune(){document.getElementById(\"tuneModal\").style.display=\"none\";_tuneIdx=-1;}");
  server.sendContent("function calTuneNudge(d){var el=document.getElementById(\"tuneVal\");var v=(parseInt(el.value)||0)+d;if(v<0)v=0;el.value=v;}");
  server.sendContent("function calTuneGoto(){if(_tuneIdx<0)return;var v=parseInt(document.getElementById(\"tuneVal\").value);if(isNaN(v)||v<0){document.getElementById(\"tuneStatus\").textContent=\"Enter a valid step.\";return;}calApi(\"/api/flap/goto\",{id:_calId,step:v},function(j){document.getElementById(\"tuneStatus\").textContent=j.ok?\"Moving to step \"+v+\"... watch the reel.\":\"Error: \"+j.error;});}");
  server.sendContent("function calTuneLock(){if(_tuneIdx<0)return;var v=parseInt(document.getElementById(\"tuneVal\").value);if(isNaN(v)||v<0){document.getElementById(\"tuneStatus\").textContent=\"Enter a valid step.\";return;}calApi(\"/api/flap/writepos\",{id:_calId,idx:_tuneIdx,pos:v},function(j){if(j.ok){_calMap[_tuneIdx]=v;if(_bmFlap>=0){if(!_bmCorrected[_bmFlap])_bmCorrected[_bmFlap]={};_bmCorrected[_bmFlap][_calId]=true;bmRenderGrid();}document.getElementById(\"tuneStatus\").textContent=\"Locked to EEPROM at step \"+v+\".\";calRenderMap();setTimeout(calCloseTune,700);}else{document.getElementById(\"tuneStatus\").textContent=\"Error: \"+j.error;}});}");
  server.sendContent("function calTuneRevert(){if(_tuneIdx<0)return;var def=calDefault(_tuneIdx);calApi(\"/api/flap/writepos\",{id:_calId,idx:_tuneIdx,pos:CAL_UNSET},function(j){if(j.ok){delete _calMap[_tuneIdx];document.getElementById(\"tuneVal\").value=def;document.getElementById(\"tuneStatus\").textContent=\"Unset in EEPROM -- now uses default (\"+def+\").\";calRenderMap();setTimeout(calCloseTune,800);}else{document.getElementById(\"tuneStatus\").textContent=\"Error: \"+j.error;}});}");
  server.sendContent("function calCharDisp(i){var ch=CAL_CHARS[i];if(ch===\" \")return \"<span class='wsw' style='background:#000'></span>\";if(CAL_COLORS[ch])return \"<span class='wsw' style='background:\"+CAL_COLORS[ch]+\"'></span>\";return ch;}");
  server.sendContent("function calCharName(i){var ch=CAL_CHARS[i];if(ch===\" \")return \"BLANK (home)\";if(CAL_COLORS[ch])return ch.toUpperCase()+\" color flap\";return \"'\"+ch+\"'\";}");
  server.sendContent("function calWizIntroStage(n){document.getElementById(\"wizStep1\").style.display=(n===1)?\"block\":\"none\";document.getElementById(\"wizStep2\").style.display=(n===2)?\"block\":\"none\";document.getElementById(\"wizStep3\").style.display=(n===3)?\"block\":\"none\";}");
  server.sendContent("function calWizStart(){if(_calId<0)return;calWizIntroStage(1);document.getElementById(\"wizIntroModal\").style.display=\"flex\";}");
  server.sendContent("function calWizIntroCancel(){document.getElementById(\"wizIntroModal\").style.display=\"none\";}");
  server.sendContent("function calWizIntroCalibrate(){document.getElementById(\"wizIntroModal\").style.display=\"none\";calCountSteps();}");
  server.sendContent("function calWizStep2(){calWizIntroStage(2);}");
  server.sendContent("function calWizStep2Back(){calWizIntroStage(2);}");
  server.sendContent("function calWizHomeStage(){calWizIntroStage(3);document.getElementById(\"wizHoVal\").textContent=\"offset \"+_calHo;document.getElementById(\"wizHoStat\").textContent=\"Homing...\";calApi(\"/api/flap/home\",{id:_calId},function(j){document.getElementById(\"wizHoStat\").textContent=j.ok?\"Homed. Is the blank flap centered?\":\"Home error: \"+j.error;});}");
  server.sendContent("function calWizHomeNudge(d){calApi(\"/api/flap/nudge\",{id:_calId,steps:d},function(j){if(j.ok){_calHo+=d;document.getElementById(\"calHoIn\").value=_calHo;document.getElementById(\"wizHoVal\").textContent=\"offset \"+_calHo;document.getElementById(\"wizHoStat\").textContent=\"Nudged \"+(d>0?\"+\":\"\")+d+\" (offset \"+_calHo+\"). Saved.\";}else{document.getElementById(\"wizHoStat\").textContent=\"Error: \"+j.error;}});}");
  server.sendContent("function calWizHomeRehome(){document.getElementById(\"wizHoStat\").textContent=\"Re-homing...\";calApi(\"/api/flap/home\",{id:_calId},function(j){document.getElementById(\"wizHoStat\").textContent=j.ok?\"Re-homed. Check the blank flap is centered.\":\"Home error: \"+j.error;});}");
  server.sendContent("function calWizIntroYes(){document.getElementById(\"wizIntroModal\").style.display=\"none\";_wizIdx=0;document.getElementById(\"wizModal\").style.display=\"flex\";calWizShow();}");
  server.sendContent("function calWizShow(){if(_wizIdx>=CAL_CHARS.length){calWizFinish();return;}var i=_wizIdx;_wizTarget=_calMap.hasOwnProperty(i)?_calMap[i]:calDefault(i);document.getElementById(\"wizProg\").textContent=\"Flap \"+(i+1)+\" of \"+CAL_CHARS.length+\" -- \"+calCharName(i);document.getElementById(\"wizBar\").style.width=Math.round((i)/CAL_CHARS.length*100)+\"%\";document.getElementById(\"wizChar\").innerHTML=calCharDisp(i);document.getElementById(\"wizTarget\").textContent=\"step \"+_wizTarget+(_calMap.hasOwnProperty(i)?\" (custom)\":\" (default)\");document.getElementById(\"wizStat\").textContent=\"\";calApi(\"/api/flap/goto\",{id:_calId,step:_wizTarget},function(j){if(!j.ok)document.getElementById(\"wizStat\").textContent=\"Move error: \"+j.error;});}");
  server.sendContent("function calWizNudge(d){_wizTarget+=d;if(_wizTarget<0)_wizTarget=0;document.getElementById(\"wizTarget\").textContent=\"step \"+_wizTarget+\" (adjusting)\";calApi(\"/api/flap/goto\",{id:_calId,step:_wizTarget},function(j){if(!j.ok)document.getElementById(\"wizStat\").textContent=\"Move error: \"+j.error;});}");
  server.sendContent("function calWizReset(){var i=_wizIdx;var def=calDefault(i);var st=document.getElementById(\"wizStat\");_wizTarget=def;document.getElementById(\"wizTarget\").textContent=\"step \"+def+\" (default)\";if(_calMap.hasOwnProperty(i)){calApi(\"/api/flap/writepos\",{id:_calId,idx:i,pos:CAL_UNSET},function(j){if(j.ok){delete _calMap[i];st.textContent=\"Reset to default (\"+def+\") and cleared custom value.\";}else{st.textContent=\"Reset error: \"+j.error;}});}else{st.textContent=\"Already at default (\"+def+\").\";}calApi(\"/api/flap/goto\",{id:_calId,step:def},function(j){});}");
  server.sendContent("function calWizConfirm(){var i=_wizIdx;var def=calDefault(i);var had=_calMap.hasOwnProperty(i);var st=document.getElementById(\"wizStat\");if(_wizTarget!==def){calApi(\"/api/flap/writepos\",{id:_calId,idx:i,pos:_wizTarget},function(j){if(j.ok){_calMap[i]=_wizTarget;st.textContent=\"Saved custom position \"+_wizTarget+\".\";_wizIdx++;setTimeout(calWizShow,300);}else{st.textContent=\"Save error: \"+j.error;}});}else if(had){calApi(\"/api/flap/writepos\",{id:_calId,idx:i,pos:CAL_UNSET},function(j){if(j.ok){delete _calMap[i];st.textContent=\"Matches default -- cleared custom value.\";_wizIdx++;setTimeout(calWizShow,300);}else{st.textContent=\"Save error: \"+j.error;}});}else{st.textContent=\"Already at default -- no change needed.\";_wizIdx++;setTimeout(calWizShow,250);}}");
  server.sendContent("function calWizSkip(){_wizIdx++;calWizShow();}");
  server.sendContent("function calWizBack(){if(_wizIdx>0)_wizIdx--;calWizShow();}");
  server.sendContent("function calWizFinish(){document.getElementById(\"wizModal\").style.display=\"none\";_wizIdx=-1;calRenderMap();document.getElementById(\"calStatus\").textContent=\"Calibration wizard complete -- all flaps reviewed.\";}");
  server.sendContent("function calWizExit(){if(!confirm(\"Exit the wizard? Flaps you already confirmed are saved; the rest are unchanged.\"))return;document.getElementById(\"wizModal\").style.display=\"none\";_wizIdx=-1;calRenderMap();}");
  server.sendContent("var _diagId=-1;");
  server.sendContent("function closeDiag(){document.getElementById(\"diagModal\").style.display=\"none\";}");
  server.sendContent("function diagRow(k,v,warn){return \"<tr><td style='color:var(--dim);padding:3px 12px 3px 0;vertical-align:top;white-space:nowrap'>\"+k+\"</td><td style='\"+(warn?\"color:var(--hi);font-weight:bold\":\"\")+\"'>\"+v+\"</td></tr>\";}");
  server.sendContent("function diagResetText(rc){if(rc===0)return \"none recorded (0)\";var b=[];if(rc&1)b.push(\"power-on\");if(rc&2)b.push(\"brown-out (supply dipped)\");if(rc&4)b.push(\"external/reset pin\");if(rc&8)b.push(\"watchdog (recovered from a hang)\");if(rc&16)b.push(\"software\");if(!b.length)b.push(\"unknown\");return b.join(\", \")+\" (0x\"+rc.toString(16)+\")\";}");
  server.sendContent("function diagQSection(q){if(!q)return \"<h4 style='margin:0 0 6px'>Stats snapshot (Q)</h4><p style='color:var(--hi)'>No response to the stats query -- the module did not answer (check it is powered and on the bus).</p>\";var rows=\"\";var rc=q.resetCause;var rcWarn=((rc&2)||(rc&8))?true:false;rows+=diagRow(\"Last reset cause\",diagResetText(rc),rcWarn);rows+=diagRow(\"Boot count\",q.bootCount+\" (since counter reset; wraps at 255)\",false);var vWarn=(q.vcc>0)&&((q.vcc>=4000)?(q.vcc<4500):(q.vcc<3000));var vtxt=(q.vcc>0?(q.vcc/1000).toFixed(2)+\" V\":\"n/a\")+(vWarn?\" -- LOW (risks brown-outs under load)\":\"\");rows+=diagRow(\"Supply voltage\",vtxt,vWarn);rows+=diagRow(\"EEPROM verify\",q.eepromOk?\"passed\":\"FAILED -- calibration storage may be unreliable\",!q.eepromOk);var ci=(q.curIndex===-1)?\"unknown (needs homing)\":((q.curIndex===-2)?\"released\":q.curIndex);rows+=diagRow(\"Current flap index\",ci,false);return \"<h4 style='margin:0 0 6px'>Stats snapshot (Q)</h4><table style='border-collapse:collapse;width:100%'>\"+rows+\"</table>\";}");
  server.sendContent("function diagMSection(s){var code=s.code;var spread=(s.spreadTenths/10).toFixed(1);var verdict,vcol,detail;if(code===0){verdict=\"OK -- motor, driver and supply are healthy.\";vcol=\"var(--grn)\";detail=\"All revolutions were consistent (spread \"+spread+\"%, within the 5% tolerance).\";}else if(code===1){verdict=\"INCONSISTENT -- intermittent missed steps.\";vcol=\"var(--ylw)\";detail=\"Revolutions varied by \"+spread+\"% (over the 5% limit). Likely mechanical drag or binding, a weak/sagging supply, or a failing motor driver.\";}else if(code===2){verdict=\"NO MOTION -- the reel did not turn.\";vcol=\"var(--hi)\";detail=\"Over a full revolution the home sensor never saw the magnet enter and leave. Likely an open motor coil, a dead driver, or a jam. A dead Hall sensor gives the same result -- if the motor is clearly turning, suspect the sensor.\";}else{verdict=\"Unknown result (code \"+code+\").\";vcol=\"var(--hi)\";detail=\"\";}var rows=diagRow(\"Result\",\"<span style='color:\"+vcol+\";font-weight:bold'>\"+verdict+\"</span>\",false);if(s.min||s.max)rows+=diagRow(\"Steps/rev (min..max)\",s.min+\" .. \"+s.max,false);rows+=diagRow(\"Spread\",spread+\"%\",code===1);return \"<h4 style='margin:0 0 6px'>Mechanical self-test (M)</h4><table style='border-collapse:collapse;width:100%'>\"+rows+\"</table>\"+(detail?\"<p style='font-size:.8rem;color:var(--dim);margin-top:6px'>\"+detail+\"</p>\":\"\");}");
  server.sendContent("function runDiag(id){_diagId=id;document.getElementById(\"diagTitle\").textContent=\"Diagnostics -- Module \"+id;var b=document.getElementById(\"diagBody\");b.innerHTML=\"<p style='color:var(--dim)'>Reading stats snapshot...</p>\";document.getElementById(\"diagModal\").style.display=\"flex\";fetch(\"/api/flap/diag\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:id})}).then(function(r){return r.json();}).then(function(j){if(!j.ok){b.innerHTML=\"<p style='color:var(--hi)'>Error: \"+(j.error||\"could not start diagnostics\")+\"</p>\";return;}var html=diagQSection(j.q);html+=\"<div id='diagMsec' style='margin-top:14px'><h4 style='margin:0 0 6px'>Mechanical self-test (M)</h4><p style='color:var(--ylw)'>Motor is spinning through several revolutions -- this can take 20-60s. Please wait...</p></div>\";b.innerHTML=html;var tries=0;var poll=setInterval(function(){tries++;if(tries>115){clearInterval(poll);var e=document.getElementById(\"diagMsec\");if(e)e.innerHTML=\"<h4 style='margin:0 0 6px'>Mechanical self-test (M)</h4><p style='color:var(--hi)'>Timed out waiting for the result.</p>\";return;}fetch(\"/api/flap/diag/status\").then(function(r){return r.json();}).then(function(st){if(st.state===\"pending\")return;clearInterval(poll);var e=document.getElementById(\"diagMsec\");if(!e)return;if(st.state===\"done\"){e.innerHTML=diagMSection(st);}else if(st.state===\"timeout\"){e.innerHTML=\"<h4 style='margin:0 0 6px'>Mechanical self-test (M)</h4><p style='color:var(--hi)'>No response from the module (timed out). The motor may be jammed, or the module reset during the test.</p>\";}else{e.innerHTML=\"<h4 style='margin:0 0 6px'>Mechanical self-test (M)</h4><p style='color:var(--hi)'>The test did not run.</p>\";}}).catch(function(){});},1000);}).catch(function(e){b.innerHTML=\"<p style='color:var(--hi)'>Error: \"+e+\"</p>\";});}");
  server.sendContent("</script></body></html>");
  server.sendContent("");
  server.sendContent("");

}

void handleApiMessages() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", ringDrain());
}

void handleApiSend() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* d = doc["data"] | "";
  bool raw = doc["raw"] | false;   // optional: send verbatim, bypassing sanitization
  uint8_t outBuf[TX_MAX_BYTES];
  size_t  outLen = min(strlen(d), (size_t)TX_MAX_BYTES);
  memcpy(outBuf, d, outLen);
  if (!outLen) { sendJsonError(400, "Empty data"); return; }
  rs485Send(outBuf, outLen, raw);
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%zu,\"raw\":%s}", outLen, raw ? "true" : "false");
  server.send(200, "application/json", resp);
}

// GET /api/flap/modules
// Streamed with chunked transfer + a small per-module stack buffer instead of
// building one large heap String. This avoids the alloc/free of a multi-KB
// String on every poll (the UI polls this every few seconds), which was a
// meaningful contributor to long-run heap fragmentation. The sfMutex is taken
// only briefly to snapshot each entry -- never held across the (potentially
// blocking) sendContent network write, which could otherwise stall taskRS485.
void handleApiModules() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");

  // Emit modules sorted by ID so the grid is always ordered (newly provisioned
  // modules slot into place instead of appearing at the end). Build a sorted
  // index order under the lock first; unprovisioned entries (id==255) naturally
  // sort to the end. Then snapshot+send each entry, re-checking under the lock.
  static uint8_t order[MAX_MODULES];
  int count = 0;
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  count = sfModuleCount;
  for (int i = 0; i < count; i++) order[i] = (uint8_t)i;
  // Insertion sort the index array by the modules' IDs (stable, small N).
  for (int a = 1; a < count; a++) {
    uint8_t key = order[a];
    uint8_t keyId = sfModules[key].id;
    int b = a - 1;
    while (b >= 0 && sfModules[order[b]].id > keyId) { order[b + 1] = order[b]; b--; }
    order[b + 1] = key;
  }
  xSemaphoreGive(sfMutex);

  int emitted = 0;
  for (int k = 0; k < count; k++) {
    int idx = order[k];
    // Snapshot this entry under the lock, then release before formatting/sending.
    SFModule m;
    bool valid = false;
    xSemaphoreTake(sfMutex, portMAX_DELAY);
    if (idx < sfModuleCount) { m = sfModules[idx]; valid = true; }
    xSemaphoreGive(sfMutex);
    if (!valid) continue;   // list shrank (prune/deprovision) mid-iteration

    char flapBuf[2] = {0, 0};
    if (m.flapChar >= 32 && m.flapChar <= 126 && m.flapChar != '"') flapBuf[0] = m.flapChar;
    char obj[288];
    snprintf(obj, sizeof(obj),
      "%s{\"id\":%d,\"sn\":\"%s\",\"provisioned\":%s,\"acked\":%s,\"flapIndex\":%d,"
      "\"flapChar\":\"%s\",\"fwVersion\":\"%s\",\"lastSeen\":%lu,\"lastSeenEpoch\":%lu,"
      "\"dupSuspect\":%s}",
      emitted ? "," : "", (int)m.id, m.serialNum, m.provisioned ? "true" : "false",
      m.acked ? "true" : "false",
      m.flapIndex, flapBuf, m.fwVersion, m.lastSeen, m.lastSeenEpoch,
      m.dupSuspect ? "true" : "false");
    server.sendContent(obj);
    emitted++;
  }
  server.sendContent("]");
  server.sendContent("");   // terminate the chunked response
}

// GET /api/display/state -- the data behind the visual "display wall". Returns
// the configured grid dimensions plus the character each cell is showing. Cells
// are addressed by module ID mapped left-to-right, top-to-bottom (cell index =
// row*cols + col == module id), matching how text is distributed across modules.
// A cell shows: the tracked character if known, "?" if the module exists but its
// char is unknown (e.g. after a home or index-set), or null if no module has
// that id. Kept small so the UI can poll it cheaply.
void handleApiDisplayState() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  int rows = cfg.gridRows < 1 ? 1 : cfg.gridRows;
  int cols = cfg.gridCols < 1 ? 1 : cfg.gridCols;
  int cells = rows * cols;
  // cellChar: 0 = no module at this id, 1 = module present but char unknown,
  // otherwise the printable character. Filled under the mutex, JSON built after.
  static char cellChar[64 * 64];   // matches the 64x64 grid cap enforced in settings
  if (cells > (int)sizeof(cellChar)) cells = sizeof(cellChar);
  memset(cellChar, 0, cells);
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < sfModuleCount; i++) {
    const SFModule& m = sfModules[i];
    if (!m.provisioned) continue;
    if (m.id < cells) {
      char c = m.flapChar;
      cellChar[m.id] = (c >= 32 && c <= 126) ? c : 1;  // 1 = known module, char unknown
    }
  }
  xSemaphoreGive(sfMutex);

  // Stream the response (chunked) from the static cellChar snapshot rather than
  // building a multi-KB heap String for a frequently-polled endpoint. The mutex
  // was already released above, so nothing is held across these network writes.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  char head[48];
  snprintf(head, sizeof(head), "{\"rows\":%d,\"cols\":%d,\"cells\":[", rows, cols);
  server.sendContent(head);
  // Emit cells in batches to keep the number of tiny network writes down.
  char batch[256]; size_t bl = 0;
  for (int i = 0; i < cells; i++) {
    char cellBuf[8]; int cn;
    char c = cellChar[i];
    if (c == 0)                       cn = snprintf(cellBuf, sizeof(cellBuf), "%snull", i ? "," : "");
    else if (c == 1)                  cn = snprintf(cellBuf, sizeof(cellBuf), "%s\"?\"", i ? "," : "");
    else if (c == '"' || c == '\\')   cn = snprintf(cellBuf, sizeof(cellBuf), "%s\"\\%c\"", i ? "," : "", c);
    else                              cn = snprintf(cellBuf, sizeof(cellBuf), "%s\"%c\"", i ? "," : "", c);
    if (cn < 0) cn = 0;
    if (bl + (size_t)cn >= sizeof(batch)) { server.sendContent(batch); bl = 0; }
    memcpy(batch + bl, cellBuf, cn); bl += cn;
  }
  if (bl) { batch[bl] = 0; server.sendContent(batch); }
  server.sendContent("]}");
  server.sendContent("");   // terminate the chunked response
}

// POST /api/flap/char   {"id":5,"char":"A"}   id=-1 for broadcast
void handleApiChar() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  const char* ch = doc["char"] | "";
  if (!ch[0]) { sendJsonError(400, "Missing char"); return; }
  sfSendChar(id, ch[0]);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/index  {"id":5,"index":3}
void handleApiIndex() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id  = doc["id"]    | -1;
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= 64) { sendJsonError(400, "Invalid index (0-63)"); return; }
  DBG("[API] show index %d on module %d\n", idx, id);
  sfSendIndex(id, idx);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/text   {"text":"HELLO","start":0}
void handleApiText() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* text = doc["text"] | "";
  int start = doc["start"] | 0;
  if (!text[0]) { sendJsonError(400, "Empty text"); return; }
  sfSendText(start, text, false);
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"chars\":%zu}", strlen(text));
  server.send(200, "application/json", resp);
}

// POST /api/flap/home   {"id":5}  or  {"id":-1}
void handleApiHome() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  DBG("[API] home module %d\n", id);
  sfHome(id);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/calibrate  {"id":5}
// Starts an asynchronous calibration. Sends m<id>c and returns immediately so
// the single-threaded web server stays responsive while the reel physically
// measures a revolution (~6.5s, up to 15s). The module replies m<id>:<steps>,
// captured in sfParseResponse; the UI polls /api/flap/calibrate/status for the
// result. The module saves the measured value to its own EEPROM as part of
// calibration. A broadcast (id<0) is fire-and-forget.
void handleApiCalibrate() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  DBG("[API] calibrate module %d\n", id);

  // Broadcast: no single reply to wait for.
  if (id < 0) {
    sfCalibrate(id);
    server.send(200, "application/json", "{\"ok\":true,\"broadcast\":true}");
    return;
  }

  // Reject a second start while one is already running for a different module;
  // re-starting the same module is allowed (re-arms the capture).
  if (sfCalibJobActive && sfCalibJobId != id) {
    char busy[96];
    snprintf(busy, sizeof(busy),
      "{\"ok\":false,\"error\":\"calibration already running for module %d\"}",
      sfCalibJobId);
    server.send(409, "application/json", busy);
    return;
  }

  // Arm the capture slot and the job, then send the calibrate command.
  sfCalibSteps       = 0;
  sfCalibCaptureTs   = 0;
  sfCalibWaitId      = id;
  sfCalibJobActive   = true;
  sfCalibJobId       = id;
  sfCalibJobSteps    = -1;
  sfCalibJobDeadline = millis() + 15000;
  sfCalibrate(id);

  char out[64];
  snprintf(out, sizeof(out), "{\"ok\":true,\"started\":true,\"id\":%d}", id);
  server.send(200, "application/json", out);
}

// POST /api/flap/diag {id}
// Runs both module self-diagnostics (firmware v26+). The 'Q' snapshot is instant
// and returned in THIS response; the 'M' mechanical self-test spins the motor
// for several revolutions, so it runs as an async job -- the UI polls
// /api/flap/diag/status for the M result (mirrors the calibrate job pattern).
void handleApiDiag() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  if (id < 0 || id > 254) { sendJsonError(400, "valid id required"); return; }
  DBG("[API] diagnostics module %d\n", id);

  // Reject if a mechanical test is already running for a different module.
  if (sfDiagMJobActive && sfDiagMJobId != id) {
    char busy[96];
    snprintf(busy, sizeof(busy),
      "{\"ok\":false,\"error\":\"diagnostics already running for module %d\"}",
      sfDiagMJobId);
    server.send(409, "application/json", busy);
    return;
  }

  // 1) Instant stats snapshot ('Q') -- captured synchronously (no motor).
  bool qok = sfSendAndCaptureQ(id, 1500);

  // 2) Mechanical self-test ('M') -- long (motor spins ~6 revolutions). Arm the
  //    capture + job, fire the command, and return immediately.
  sfDiagMTs        = 0;
  sfDiagMCode      = -1;
  sfDiagMWaitId    = id;
  sfDiagMJobActive = true;
  sfDiagMJobId     = id;
  sfDiagMDeadline  = millis() + 100000UL;
  char mframe[16];
  snprintf(mframe, sizeof(mframe), "m%dM\n", id);
  rs485SendStr(mframe);

  char out[200];
  if (qok) {
    snprintf(out, sizeof(out),
      "{\"ok\":true,\"started\":true,\"id\":%d,"
      "\"q\":{\"resetCause\":%d,\"bootCount\":%d,\"vcc\":%d,\"eepromOk\":%d,\"curIndex\":%d}}",
      id, sfDiagQReset, sfDiagQBoot, sfDiagQVcc, sfDiagQEe, sfDiagQCur);
  } else {
    snprintf(out, sizeof(out), "{\"ok\":true,\"started\":true,\"id\":%d,\"q\":null}", id);
  }
  server.send(200, "application/json", out);
}

// GET /api/flap/diag/status
// Poll target for the async mechanical self-test:
//   {"ok":true,"state":"pending","id":N}
//   {"ok":true,"state":"done","id":N,"code":C,"min":..,"max":..,"spreadTenths":..}
//   {"ok":true,"state":"timeout","id":N}
//   {"ok":true,"state":"idle"}
void handleApiDiagStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char out[160];
  if (sfDiagMJobActive) {
    if (sfDiagMTs != 0) {
      sfDiagMJobActive = false;
      sfDiagMWaitId    = -1;
      snprintf(out, sizeof(out),
        "{\"ok\":true,\"state\":\"done\",\"id\":%d,\"code\":%d,\"min\":%d,\"max\":%d,\"spreadTenths\":%d}",
        sfDiagMJobId, sfDiagMCode, sfDiagMMin, sfDiagMMax, sfDiagMSpread);
      server.send(200, "application/json", out);
      return;
    }
    if ((long)(millis() - sfDiagMDeadline) >= 0) {
      sfDiagMJobActive = false;
      sfDiagMWaitId    = -1;
      snprintf(out, sizeof(out), "{\"ok\":true,\"state\":\"timeout\",\"id\":%d}", sfDiagMJobId);
      server.send(200, "application/json", out);
      return;
    }
    snprintf(out, sizeof(out), "{\"ok\":true,\"state\":\"pending\",\"id\":%d}", sfDiagMJobId);
    server.send(200, "application/json", out);
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"state\":\"idle\"}");
}

// GET /api/flap/calibrate/status
// Poll target for the async calibration job. Reports one of:
//   {"ok":true,"state":"idle"}                          no job has run
//   {"ok":true,"state":"pending","id":N}                still measuring
//   {"ok":true,"state":"done","id":N,"stepsPerRev":S}   result ready
//   {"ok":true,"state":"timeout","id":N}                no reply within window
// The "done"/"timeout" result latches until the next start (or until read), so
// a poll that arrives right after completion still sees it.
void handleApiCalibrateStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char out[96];

  if (sfCalibJobActive) {
    // Capture arrives via sfParseResponse setting sfCalibCaptureTs.
    if (sfCalibCaptureTs != 0) {
      sfCalibJobSteps  = sfCalibSteps;
      sfCalibJobActive = false;
      sfCalibWaitId    = -1;
      snprintf(out, sizeof(out),
        "{\"ok\":true,\"state\":\"done\",\"id\":%d,\"stepsPerRev\":%d}",
        sfCalibJobId, sfCalibJobSteps);
      server.send(200, "application/json", out);
      return;
    }
    if ((long)(millis() - sfCalibJobDeadline) >= 0) {
      sfCalibJobActive = false;
      sfCalibWaitId    = -1;
      snprintf(out, sizeof(out),
        "{\"ok\":true,\"state\":\"timeout\",\"id\":%d}", sfCalibJobId);
      server.send(200, "application/json", out);
      return;
    }
    snprintf(out, sizeof(out),
      "{\"ok\":true,\"state\":\"pending\",\"id\":%d}", sfCalibJobId);
    server.send(200, "application/json", out);
    return;
  }

  // No active job. If a result was latched from the last run, report it once.
  if (sfCalibJobSteps >= 0) {
    snprintf(out, sizeof(out),
      "{\"ok\":true,\"state\":\"done\",\"id\":%d,\"stepsPerRev\":%d}",
      sfCalibJobId, sfCalibJobSteps);
    sfCalibJobSteps = -1;   // consume the latched result
    server.send(200, "application/json", out);
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"state\":\"idle\"}");
}

// POST /api/flap/version  {"id":5}
void handleApiVersion() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  DBG("[API] version query module %d\n", id);
  if (id < 0 || id > 254) { sendJsonError(400, "id required (0-254)"); return; }

  // Send a direct version query and wait for a fresh reply. The window scales
  // mildly with id (broadcast-stagger headroom); a direct query usually answers
  // in ~35-70ms now that the newline collision is fixed (see sfQueryVersion).
  char          fwVer[8]     = "";
  char          sn[21]       = "";
  unsigned long repLastSeen  = 0;
  unsigned long waitMs   = 500UL + (unsigned long)(id > 25 ? 25 : id) * 100UL;
  bool gotReply = sfSendVersionAndWait(id, waitMs, fwVer, sizeof(fwVer),
                                       sn, sizeof(sn), &repLastSeen);

  if (gotReply) {
    char out[128];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"id\":%d,\"ver\":\"%s\",\"sn\":\"%s\",\"stale\":false,\"lastSeen\":%lu}",
             id, fwVer, sn, repLastSeen);
    DBG("[API] version response: id=%d ver=%s sn=%s\n", id, fwVer, sn);
    server.send(200, "application/json", out);
  } else {
    // Timed out -- check if we already have a cached version from before
    char cachedVer[8] = "";
    char cachedSn[21] = "";
    int           cachedId      = -1;
    unsigned long cachedLastSeen = 0;
    xSemaphoreTake(sfMutex, portMAX_DELAY);
    SFModule* mc = sfFindById((uint8_t)id);
    if (mc && mc->fwVersion[0]) {
      strlcpy(cachedVer, mc->fwVersion, sizeof(cachedVer));
      strlcpy(cachedSn,  mc->serialNum, sizeof(cachedSn));
      cachedId      = mc->id;
      cachedLastSeen = mc->lastSeen;
    } else if (mc) {
      // Known module, no cached version yet, and this query timed out. Do NOT
      // stamp any sentinel: a direct version query is reliable now that the
      // newline-collision is fixed, so a timeout here is a transient miss (bus
      // busy, momentary contention), not evidence the module lacks the command.
      // Return what we have (id/sn) and let the next poll re-query.
      strlcpy(cachedSn,  mc->serialNum, sizeof(cachedSn));
      cachedId      = mc->id;
      cachedLastSeen = mc->lastSeen;
    }
    xSemaphoreGive(sfMutex);
    if (cachedId >= 0) {
      // Return stale cached data
      char out[160];
      snprintf(out, sizeof(out),
               "{\"ok\":true,\"id\":%d,\"ver\":\"%s\",\"sn\":\"%s\",\"stale\":true,\"lastSeen\":%lu}",
               cachedId, cachedVer, cachedSn, cachedLastSeen);
      DBG("[API] version timeout for module %d -- returning stale data: ver=%s\n", id, cachedVer);
      server.send(200, "application/json", out);
    } else {
      DBG("[API] version query timeout for module %d (no cached data)\n", id);
      server.send(200, "application/json",
                  "{\"ok\":false,\"error\":\"no response from module\"}");
    }
  }
}

// POST /api/flap/provision  {"sn":"AABBCC...","id":5}
void handleApiProvision() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn = doc["sn"] | "";
  int newId = doc["id"] | -1;
  if (!sn[0] || newId < 0) { sendJsonError(400, "sn and id required"); return; }
  sfProvision(sn, newId);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/deprovision  {"id":5} or {"id":-1} for all
void handleApiDeprovision() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -999;
  if (id == -999) { sendJsonError(400, "id required"); return; }
  DBG("[API] deprovision module %d\n", id);
  sfDeprovision(id);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/homebysn  {"sn":"AABBCC..."}
void handleApiHomeBySN() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn = doc["sn"] | "";
  if (!sn[0]) { sendJsonError(400, "sn required"); return; }
  DBG("[API] home by SN %s\n", sn);
  sfHomeBySN(sn);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Use snprintf to avoid JsonDocument heap allocation (called every 3s by browser)
  char rtcBuf[24]; rtcFormatTime(rtcBuf, sizeof(rtcBuf));
  IPAddress lip = WiFi.localIP(), aip = WiFi.softAPIP();
  // Per-task minimum-ever free stack (bytes). A value trending toward 0 is an
  // early warning of the stack-canary crash class.
  unsigned stk485 = hTaskRS485 ? uxTaskGetStackHighWaterMark(hTaskRS485) : 0;
  unsigned stkWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
  unsigned stkNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
  unsigned stkOta = hTaskOTA   ? uxTaskGetStackHighWaterMark(hTaskOTA)   : 0;
  unsigned stkRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
  char out[560];
  snprintf(out, sizeof(out),
    "{\"uptime\":%lu,\"rx\":%lu,\"tx\":%lu,\"baud\":%lu,"
    "\"wifi\":%s,\"ip\":\"%d.%d.%d.%d\",\"apip\":\"%d.%d.%d.%d\","
    "\"heap\":%u,\"minheap\":%u,\"mqtt\":%s,\"modules\":%d,"
    "\"stk\":{\"rs485\":%u,\"web\":%u,\"net\":%u,\"ota\":%u,\"rtc\":%u},"
    "\"time\":\"%s\",\"ntpSynced\":%s,\"maint\":%s,\"quiet\":%s}",
    millis()/1000, rxCount, txCount, cfg.rs485Baud,
    (WiFi.status()==WL_CONNECTED)?"true":"false",
    lip[0],lip[1],lip[2],lip[3],
    aip[0],aip[1],aip[2],aip[3],
    ESP.getFreeHeap(), ESP.getMinFreeHeap(),
    mqtt.connected()?"true":"false",
    sfModuleCount,
    stk485, stkWeb, stkNet, stkOta, stkRtc,
    rtcBuf,
    ntpSynced?"true":"false",
    gMaintenanceMode?"true":"false",
    gQuietTime?"true":"false");
  server.send(200, "application/json", out);
}

void handleApiConfigGet() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  JsonDocument doc;
  doc["wSSID"]    = cfg.wifiSSID;
  doc["mqHost"]   = cfg.mqttHost;
  doc["mqPort"]   = cfg.mqttPort;
  doc["mqUser"]   = cfg.mqttUser;
  doc["mqPfx"]    = cfg.mqttPrefix;
  doc["baud"]     = cfg.rs485Baud;
  doc["dataBits"] = cfg.rs485DataBits;
  doc["parity"]   = cfg.rs485Parity;
  doc["stopBits"] = cfg.rs485StopBits;
  doc["posixTZ"]    = cfg.posixTZ;
  doc["ntpServer"]  = cfg.ntpServer;
  doc["gridRows"]   = cfg.gridRows;
  doc["gridCols"]   = cfg.gridCols;
  doc["serialDebug"]   = cfg.serialDebug;
  doc["haEnabled"]     = cfg.haEnabled;
  doc["otaPasswordSet"] = (strlen(cfg.otaPassword) > 0);
  char out[640];
  serializeJson(doc, out, sizeof(out));
  server.send(200, "application/json", out);
}

void handleApiConfigWifi() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  strlcpy(cfg.wifiSSID, doc["ssid"] | "", sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPass, doc["pass"] | "", sizeof(cfg.wifiPass));
  saveConfig();
  DBG("[CFG] WiFi SSID set to '%s'\n", cfg.wifiSSID);
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  WiFi.disconnect();
}

void handleApiConfigMqtt() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  strlcpy(cfg.mqttHost,   doc["host"]   | "", sizeof(cfg.mqttHost));
  cfg.mqttPort          = doc["port"]   | DEFAULT_MQTT_PORT;
  strlcpy(cfg.mqttUser,   doc["user"]   | "", sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,   doc["pass"]   | "", sizeof(cfg.mqttPass));
  strlcpy(cfg.mqttPrefix, doc["prefix"] | DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));
  saveConfig();
  DBG("[CFG] MQTT broker set to %s:%d  prefix=%s\n", cfg.mqttHost, cfg.mqttPort, cfg.mqttPrefix);
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  mqtt.disconnect();
}

void handleApiConfigRS485() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  unsigned long newBaud = doc["baud"]     | cfg.rs485Baud;
  cfg.rs485DataBits     = doc["dataBits"] | cfg.rs485DataBits;
  cfg.rs485Parity       = doc["parity"]   | cfg.rs485Parity;
  cfg.rs485StopBits     = doc["stopBits"] | cfg.rs485StopBits;
  bool baudChanged      = (newBaud != cfg.rs485Baud);
  cfg.rs485Baud         = newBaud;
  saveConfig();
  if (baudChanged) { rs485.end(); rs485Begin(); }
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/config/settings  -- save all settings in one call
void handleApiConfigSettings() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  // WiFi
  if (doc["ssid"].is<const char*>()) strlcpy(cfg.wifiSSID, doc["ssid"] | "", sizeof(cfg.wifiSSID));
  if (doc["pass"].is<const char*>()) strlcpy(cfg.wifiPass, doc["pass"] | "", sizeof(cfg.wifiPass));
  // MQTT
  if (doc["mqHost"].is<const char*>())   strlcpy(cfg.mqttHost,   doc["mqHost"]   | "", sizeof(cfg.mqttHost));
  if (doc["mqPort"].is<int>())           cfg.mqttPort = doc["mqPort"];
  if (doc["mqUser"].is<const char*>())   strlcpy(cfg.mqttUser,   doc["mqUser"]   | "", sizeof(cfg.mqttUser));
  if (doc["mqPass"].is<const char*>())   strlcpy(cfg.mqttPass,   doc["mqPass"]   | "", sizeof(cfg.mqttPass));
  if (doc["mqPfx"].is<const char*>())    strlcpy(cfg.mqttPrefix, doc["mqPfx"]    | DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));
  // RS485
  unsigned long newBaud = doc["baud"] | cfg.rs485Baud;
  cfg.rs485DataBits  = doc["dataBits"] | cfg.rs485DataBits;
  cfg.rs485Parity    = doc["parity"]   | cfg.rs485Parity;
  cfg.rs485StopBits  = doc["stopBits"] | cfg.rs485StopBits;
  // Timezone
  // OTA password update
  if (doc["otaPassword"].is<const char*>()) {
    strlcpy(cfg.otaPassword, doc["otaPassword"] | "", sizeof(cfg.otaPassword));
    saveConfig();
    if (strlen(cfg.otaPassword) > 0) {
      ArduinoOTA.setPassword(cfg.otaPassword);
    }
    printf("[CFG] OTA password updated\n");
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  // Serial debug toggle
  if (doc["serialDebug"].is<bool>()) {
    cfg.serialDebug = doc["serialDebug"].as<bool>();
    gSerialDebug    = cfg.serialDebug;
    saveConfig();
    printf("[CFG] Serial debug %s\n", cfg.serialDebug ? "enabled" : "disabled");
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  // Home Assistant integration toggle
  if (doc["haEnabled"].is<bool>()) {
    bool was = cfg.haEnabled;
    cfg.haEnabled = doc["haEnabled"].as<bool>();
    saveConfig();
    printf("[CFG] Home Assistant integration %s\n", cfg.haEnabled ? "enabled" : "disabled");
    if (mqtt.connected()) {
      if (cfg.haEnabled && !was) { haPublishDiscovery(true); mqttPublishStateTopics(); }
      else if (!cfg.haEnabled && was) { haPublishDiscovery(false); }  // remove entities
    }
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (doc["posixTZ"].is<const char*>()) {
    strlcpy(cfg.posixTZ, doc["posixTZ"] | "UTC0", sizeof(cfg.posixTZ));
    strlcpy(gPosixTZ, cfg.posixTZ, sizeof(gPosixTZ));
    setenv("TZ", gPosixTZ, 1);
    tzset();
    ntpSynced = false;
    DBG("[CFG] Timezone set to %s\n", cfg.posixTZ);
  }
  if (doc["ntpServer"].is<const char*>()) {
    strlcpy(cfg.ntpServer, doc["ntpServer"] | DEFAULT_NTP_SERVER, sizeof(cfg.ntpServer));
    if (!cfg.ntpServer[0]) strlcpy(cfg.ntpServer, DEFAULT_NTP_SERVER, sizeof(cfg.ntpServer));
    ntpSynced = false;   // re-sync against the new server on next network tick
    DBG("[CFG] NTP server set to %s\n", cfg.ntpServer);
  }
  if (doc["gridRows"].is<int>() || doc["gridCols"].is<int>()) {
    int gr = doc["gridRows"] | cfg.gridRows;
    int gc = doc["gridCols"] | cfg.gridCols;
    if (gr < 1)   gr = 1;
    if (gr > 64)  gr = 64;   // sane upper bounds for the visual wall
    if (gc < 1)   gc = 1;
    if (gc > 64)  gc = 64;
    cfg.gridRows = (uint8_t)gr;
    cfg.gridCols = (uint8_t)gc;
    DBG("[CFG] Display grid set to %dx%d (rows x cols)\n", gr, gc);
  }
  bool baudChanged = (newBaud != cfg.rs485Baud);
  cfg.rs485Baud = newBaud;
  saveConfig();
  if (baudChanged) { rs485.end(); rs485Begin(); }
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  // Only disconnect/reconnect if WiFi or MQTT credentials were in the payload
  bool hasWifi = doc["ssid"].is<const char*>() || doc["pass"].is<const char*>();
  bool hasMqtt = doc["mqHost"].is<const char*>() || doc["mqPort"].is<int>();
  if (hasMqtt) mqtt.disconnect();
  if (hasWifi) WiFi.disconnect();
}

void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// ---------------------------------------------------------------------------
// OTA update support
// ---------------------------------------------------------------------------
static void otaInit() {
  // Hostname shown in Arduino IDE port list
  ArduinoOTA.setHostname("splitflap-gw");

  // Optional password protection
  if (strlen(cfg.otaPassword) > 0) {
    ArduinoOTA.setPassword(cfg.otaPassword);
  }

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    printf("[OTA] Starting %s update\n", type.c_str());
  });
  ArduinoOTA.onEnd([]() {
    printf("[OTA] Update complete -- rebooting\n");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static uint8_t lastPct = 255;
    uint8_t pct = (uint8_t)(progress * 100 / total);
    if (pct != lastPct && pct % 10 == 0) {
      printf("[OTA] Progress: %u%%\n", pct);
      lastPct = pct;
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    const char* msg = "Unknown";
    if      (error == OTA_AUTH_ERROR)    msg = "Auth failed";
    else if (error == OTA_BEGIN_ERROR)   msg = "Begin failed";
    else if (error == OTA_CONNECT_ERROR) msg = "Connect failed";
    else if (error == OTA_RECEIVE_ERROR) msg = "Receive failed";
    else if (error == OTA_END_ERROR)     msg = "End failed";
    printf("[OTA] Error: %s\n", msg);
  });

  ArduinoOTA.begin();
  // ArduinoOTA.begin() started the mDNS responder with our hostname; also
  // advertise the web UI so browsers can reach http://splitflap-gw.local
  MDNS.addService("http", "tcp", 80);
  printf("[OTA] Ready (hostname: splitflap-gw, web UI at http://splitflap-gw.local)\n");
}

// OTA runs in its own task so ArduinoOTA.handle() is called frequently
// without blocking the web server or RS485 tasks.
void taskOTA(void* pv) {
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      ArduinoOTA.handle();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}


// ---------------------------------------------------------------------------
// Web-based OTA firmware upload
// ---------------------------------------------------------------------------
void handleOTAPage() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Served as a standalone page at /ota so the upload iframe works cleanly
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Firmware Update</title>"
    "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eaeaea;padding:30px}"
    "h2{color:#e94560}progress{width:100%;height:20px;margin-top:10px}"
    "input[type=file]{color:#eaeaea}button{margin-top:10px;padding:8px 20px;"
    "background:#e94560;border:none;color:#fff;border-radius:4px;cursor:pointer}"
    "#status{margin-top:14px;font-size:.9rem}</style></head><body>"
    "<h2>Firmware Update</h2>"
    "<p style='color:#888;font-size:.85rem'>Select a compiled .bin file. "
    "The gateway will reboot automatically after a successful upload.</p>"
    "<input type='file' id='fw' accept='.bin'>"
    "<br><button onclick='upload()'>Upload Firmware</button>"
    "<progress id='prog' value='0' max='100' style='display:none'></progress>"
    "<div id='status'></div>"
    "<script>"
    "function upload(){"
    "var f=document.getElementById('fw').files[0];"
    "if(!f){document.getElementById('status').textContent='No file selected.';return;}"
    "var fd=new FormData();fd.append('firmware',f,f.name);"
    "var xhr=new XMLHttpRequest();"
    "xhr.upload.onprogress=function(e){"
    "if(e.lengthComputable){"
    "var p=Math.round(e.loaded*100/e.total);"
    "document.getElementById('prog').style.display='';"
    "document.getElementById('prog').value=p;"
    "document.getElementById('status').textContent='Uploading: '+p+'%';}"
    "};"
    "xhr.onload=function(){"
    "if(xhr.status===200){"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(76,175,80)\">Upload successful! Rebooting... This page will stop responding; wait ~20s and reload.</span>';"
    "}else{"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(233,69,96)\">Error: '+(xhr.responseText||'upload failed')+'</span>';}"
    "};"
    "xhr.onerror=function(){"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(233,69,96)\">Upload failed (connection error).</span>';"
    "};"
    "xhr.open('POST','/api/ota/upload');"
    "xhr.send(fd);"
    "}"
    "</script></body></html>";
  server.send(200, "text/html", html);
}

// Bring the fallback SoftAP up or down, switching WiFi mode accordingly.
//   AP up   -> WIFI_AP_STA (AP for config + station keeps trying/holding link)
//   AP down -> WIFI_STA    (station only; no AP buffers/beacons)
// Only acts on an actual change so we never thrash the radio. The AP is a
// fallback: callers bring it up when the station is down (or no credentials are
// configured) and drop it once the station connects.
static void wifiSetApActive(bool up) {
  if (up == gApActive) return;
  if (up) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
    IPAddress b = WiFi.softAPIP();
    printf("[WiFi] Fallback AP up: %s  %d.%d.%d.%d\n", DEFAULT_AP_SSID, b[0],b[1],b[2],b[3]);
  } else {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    printf("[WiFi] Fallback AP down -- station-only\n");
  }
  gApActive = up;
}

// Tracks whether the in-progress OTA upload has hit a fatal error, so we can
// reject cleanly at the end instead of rebooting into a half-written image.
static bool otaUploadFailed = false;

// Restore normal WiFi after a failed/aborted OTA. During an upload we force
// modem sleep off and (if it was up) the AP down to free RAM; afterwards we
// reconcile to the correct state: AP stays down while the station is connected,
// and comes back as a fallback only if the station is not connected.
static void otaRestoreWifi() {
  WiFi.setSleep(true);
  // AP is a fallback: bring it back only if the station is not connected.
  bool staUp = (WiFi.status() == WL_CONNECTED);
  wifiSetApActive(!staUp);
}

void handleOTAUpload() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  // The firmware binary arrives as a multipart/form-data file part. The ESP32
  // WebServer streams it to us in chunks via server.upload(); the empty POST
  // body handler registered alongside this callback sends the final response.
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaUploadFailed = false;
    printf("[OTA] Web upload start: %s\n", upload.filename.c_str());
    // Quiesce the gateway for the duration of the upload: stop MQTT publishing
    // and free its buffers so the WiFi/TCP stack has the contiguous heap the
    // upload needs. gOtaInProgress also makes the network task skip its periodic
    // status/display/discovery publishes. This addresses mid-upload connection
    // drops seen under heap fragmentation (esp. with Home Assistant enabled).
    gOtaInProgress = true;
    if (mqtt.connected()) { mqtt.disconnect(); printf("[OTA] MQTT paused during upload\n"); }
    // Free internal RAM for the transfer. A large firmware streams in faster than
    // flash can absorb it, so WiFi/lwIP receive buffers pile up; on this already
    // heap-constrained, fragmented gateway that can exhaust the heap mid-upload
    // (observed min-free-heap dropping to ~512 bytes -> connection reset). Two
    // levers help: (a) drop the SoftAP so its interface buffers/housekeeping are
    // released (only safe when the station is connected, else we'd lose access),
    // and (b) disable modem sleep so the station drains the RX queue at full
    // speed, reducing buffer buildup. Both are restored if the upload fails.
    if (WiFi.status() == WL_CONNECTED) {
      wifiSetApActive(false);   // drop fallback AP if it happens to be up
      WiFi.setSleep(false);
      printf("[OTA] AP down + modem sleep off for upload (heap=%u)\n", ESP.getFreeHeap());
    }
    // UPDATE_SIZE_UNKNOWN lets the Update library size the partition itself.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaUploadFailed = true;
      gOtaInProgress = false;
      otaRestoreWifi();
      printf("[OTA] Begin failed (%s) -- aborting upload\n", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // handleClient() does not return during a multipart upload, so the web task
    // can't touch its watchdog from its loop -- feed it here on every chunk so a
    // large/slow upload can't trip the 30s web-stall reboot.
    wdgWebMs = millis();
    // Skip writing once we've failed, so we don't keep feeding a dead Update.
    if (!otaUploadFailed) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        otaUploadFailed = true;
        printf("[OTA] Write error (%s) -- aborting upload\n", Update.errorString());
        Update.abort();
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaUploadFailed) {
      Update.abort();
      gOtaInProgress = false;
      otaRestoreWifi();
      printf("[OTA] Upload ended in failed state -- image discarded\n");
      // Response is sent by the POST body handler (sendOTAUploadResult).
    } else if (Update.end(true)) {   // true = set the new image as bootable
      printf("[OTA] Web upload complete (%u bytes) -- verified, rebooting\n",
             upload.totalSize);
      // gOtaInProgress stays set: we reboot momentarily; no need to resume.
    } else {
      otaUploadFailed = true;
      gOtaInProgress = false;
      otaRestoreWifi();
      printf("[OTA] Update.end failed (%s) -- incomplete or corrupt image\n",
             Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    otaUploadFailed = true;
    gOtaInProgress = false;
    Update.abort();
    otaRestoreWifi();
    printf("[OTA] Upload aborted by client -- image discarded\n");
  }
}

// Final response for the OTA upload POST. Runs after the whole multipart body
// (and thus all handleOTAUpload chunk callbacks) has been processed, so by now
// otaUploadFailed reflects the true outcome. On success we reply 200 then
// reboot into the freshly flashed image; on failure we reply 500 and stay up.
void sendOTAUploadResult() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (otaUploadFailed || !Update.isFinished()) {
    server.send(500, "text/plain",
                "Update failed -- firmware not flashed. Device left unchanged.");
    return;
  }
  server.send(200, "text/plain", "OK");
  delay(500);        // let the response flush to the browser before we restart
  ESP.restart();
}


// ?? New command handlers ??????????????????????????????????????????

void handleApiHomeOffset() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id    = doc["id"]    | -99;
  int steps = doc["steps"] | -9999;
  if (id == -99 || steps == -9999) { sendJsonError(400, "id and steps required"); return; }
  DBG("[API] home offset module %d -> %d steps\n", id, steps);
  sfHomeOffset(id, steps);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiTotalSteps() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id    = doc["id"]    | -99;
  int steps = doc["steps"] | -1;
  if (id == -99 || steps < 0) { sendJsonError(400, "id and steps required"); return; }
  DBG("[API] total steps module %d -> %d\n", id, steps);
  sfSetTotalSteps(id, steps);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiNudge() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id    = doc["id"]    | -99;
  int steps = doc["steps"] | -9999;
  if (id == -99 || steps == -9999) { sendJsonError(400, "id and steps required"); return; }
  DBG("[API] nudge module %d by %d steps\n", id, steps);
  sfNudge(id, steps);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiGoto() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id   = doc["id"]   | -99;
  int step = doc["step"] | -1;
  if (id == -99 || step < 0) { sendJsonError(400, "id and step required"); return; }
  DBG("[API] goto module %d step %d\n", id, step);
  sfGoto(id, step);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiWritePos() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id  = doc["id"]  | -99;
  int idx = doc["idx"] | -1;
  int pos = doc["pos"] | -1;
  if (id == -99 || idx < 0 || pos < 0) { sendJsonError(400, "id, idx and pos required"); return; }
  DBG("[API] write pos module %d idx=%d pos=%d\n", id, idx, pos);
  sfWritePos(id, idx, pos);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiAutoHome() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id     = doc["id"]     | -99;
  int enable = doc["enable"] | -1;
  if (id == -99 || enable < 0) { sendJsonError(400, "id and enable required"); return; }
  DBG("[API] auto-home module %d -> %s\n", id, enable ? "on" : "off");
  sfAutoHome(id, enable);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiErase() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -99;
  if (id == -99) { sendJsonError(400, "id required"); return; }
  DBG("[API] erase map module %d\n", id);
  sfErase(id);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiFactoryReset() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -99;
  if (id == -99) { sendJsonError(400, "id required"); return; }
  DBG("[API] factory reset module %d\n", id);
  sfFactoryReset(id);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiDumpBySN() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn = doc["sn"] | "";
  if (!sn[0]) { sendJsonError(400, "sn required"); return; }
  DBG("[API] dump by SN %s\n", sn);
  sfDumpBySN(sn);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiFactoryResetBySN() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn = doc["sn"] | "";
  if (!sn[0]) { sendJsonError(400, "sn required"); return; }
  DBG("[API] factory reset by SN %s\n", sn);
  sfFactoryResetBySN(sn);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiRestoreBySN() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* sn         = doc["sn"]         | "";
  int         homeOffset = doc["homeOffset"]  | -9999;
  int         totalSteps = doc["totalSteps"]  | -1;
  const char* map        = doc["map"]         | "";
  if (!sn[0] || homeOffset == -9999 || totalSteps < 0) {
    sendJsonError(400, "sn, homeOffset, totalSteps required"); return;
  }
  // Build mXW<sn>:<ho>:<ts>:<map>\n. The map can be large; snprintf into a
  // bounded static buffer (off taskWeb's stack) and reject anything that would
  // overflow a single frame -- a truncated restore command would corrupt the
  // module's EEPROM, so it's safer to refuse than to send a partial map.
  static char cmd[TX_MAX_BYTES + 1];
  int n = snprintf(cmd, sizeof(cmd), "mXW%s:%d:%d:%s\n", sn, homeOffset, totalSteps, map);
  if (n < 0 || (size_t)n >= sizeof(cmd)) {
    sendJsonError(400, "restore payload too large for one frame"); return;
  }
  DBG("[API] restore by SN %s\n", sn);
  rs485SendStr(cmd);
  server.send(200, "application/json", "{\"ok\":true}");
}


void handleApiDump() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -99;
  if (id < 0 || id > 254) { sendJsonError(400, "id required (0-254)"); return; }
  DBG("[API] dump module %d\n", id);

  char sn[21] = "";
  char fwVer[8] = "";
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* m = sfFindById((uint8_t)id);
  if (m) {
    strlcpy(sn,    m->serialNum, sizeof(sn));
    strlcpy(fwVer, m->fwVersion, sizeof(fwVer));
  }
  xSemaphoreGive(sfMutex);

  // Parse firmware version number (strip leading 'v' if present)
  const char* verStr = (fwVer[0] == 'v' || fwVer[0] == 'V') ? fwVer + 1 : fwVer;
  int fwVerNum = atoi(verStr);

  // Read fresh: prefer the serial-number dump mXD<sn> for fw>=15 with a known
  // SN; fall back to m<id>d if that gets no reply. A full 64-flap dump is ~565
  // bytes (~590ms to transmit at 9600 baud) plus the module's EEPROM-read time,
  // so the wait must be well over 500ms; 1200ms lets mXD fully respond before any
  // fallback, avoiding a half-duplex collision with a late reply.
  char rawDump[TX_MAX_BYTES] = "";
  bool gotReply = false;
  if (fwVerNum >= 15 && sn[0]) {
    DBG("[API] dump module %d via SN %s (fw=%d)\n", id, sn, fwVerNum);
    char f[40]; snprintf(f, sizeof(f), "mXD%s\n", sn);
    gotReply = sfSendAndCaptureDump(id, f, 1200, rawDump, sizeof(rawDump));
  }
  if (!gotReply) {
    DBG("[API] dump module %d via ID (fw=%d)\n", id, fwVerNum);
    char f[16]; snprintf(f, sizeof(f), "m%dd\n", id);
    gotReply = sfSendAndCaptureDump(id, f, 1200, rawDump, sizeof(rawDump));
  }
  sfDumpWaitId = -1;  // disarm capture

  if (gotReply) {
    // JSON-escape the dump, then format the reply (static buffers: off taskWeb's
    // stack, and the synchronous server serves one request at a time). sn is
    // validated alphanumeric, so it needs no escaping.
    static char escDump[TX_MAX_BYTES * 2];
    size_t ei = 0;
    for (const char* p2 = rawDump; *p2 && ei < sizeof(escDump) - 2; p2++) {
      if (*p2 == '"' || *p2 == '\\') escDump[ei++] = '\\';
      escDump[ei++] = *p2;
    }
    escDump[ei] = 0;
    static char out[TX_MAX_BYTES * 2 + 96];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"id\":%d,\"sn\":\"%s\",\"dump\":\"%s\",\"stale\":false}",
             id, sn, escDump);
    server.send(200, "application/json", out);
  } else {
    server.send(200, "application/json",
      "{\"ok\":false,\"error\":\"no response from module\"}");
  }
}

// POST /api/flap/all  {"id":N}
// Refresh a module's COMPLETE state -- firmware version, serial, and EEPROM dump.
// For a module known to be v25+ this is a SINGLE bus transaction using the
// combined 'A' command, instead of a version query followed by a dump. For older
// firmware (or if 'A' times out) it falls back to the classic version-then-dump
// sequence. Returns the same dump string the /api/flap/dump endpoint does, plus
// the refreshed ver/sn, so the Info dialog can render from one response.
void handleApiAll() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -99;
  if (id < 0 || id > 254) { sendJsonError(400, "id required (0-254)"); return; }
  DBG("[API] all (version+EEPROM) module %d\n", id);

  char sn[21] = "", fwVer[8] = "";
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* m = sfFindById((uint8_t)id);
  if (m) { strlcpy(sn, m->serialNum, sizeof(sn)); strlcpy(fwVer, m->fwVersion, sizeof(fwVer)); }
  xSemaphoreGive(sfMutex);
  const char* verStr = (fwVer[0] == 'v' || fwVer[0] == 'V') ? fwVer + 1 : fwVer;
  int fwVerNum = atoi(verStr);

  static char rawDump[TX_MAX_BYTES];   // static: keeps taskWeb's stack clear of a 768B frame
  rawDump[0] = 0;
  bool gotReply = false;
  const char* mode = "A";

  if (fwVerNum >= 25) {
    // Known v25+: one combined 'A' transaction. If it times out the module is
    // offline -- a v+d fallback would almost certainly also time out, just ~2s
    // slower -- so we return stale rather than fall back. The next version read
    // (Identify, stale-probe, explicit query) self-corrects the cache if the
    // module was in fact downgraded or swapped for an older one.
    char f[64];
    if (sn[0]) { DBG("[API] all via mXA %s\n", sn); snprintf(f, sizeof(f), "mXA%s\n", sn); }
    else       { DBG("[API] all via m%dA\n", id);   snprintf(f, sizeof(f), "m%dA\n", id); }
    gotReply = sfSendAndCaptureDump(id, f, 1300, rawDump, sizeof(rawDump));  // 'A' ~570ms + assembly
  } else {
    // Unknown or older firmware: version query, then dump (two transactions).
    // Wait for the version reply to clear the bus before the dump goes out.
    mode = "vd";
    sfSendVersionAndWait(id, 700, fwVer, sizeof(fwVer), sn, sizeof(sn), NULL);
    verStr   = (fwVer[0] == 'v' || fwVer[0] == 'V') ? fwVer + 1 : fwVer;
    fwVerNum = atoi(verStr);
    char f[40];
    if (fwVerNum >= 15 && sn[0]) snprintf(f, sizeof(f), "mXD%s\n", sn);
    else                         snprintf(f, sizeof(f), "m%dd\n", id);
    gotReply = sfSendAndCaptureDump(id, f, 1300, rawDump, sizeof(rawDump));
  }
  sfDumpWaitId = -1;  // disarm capture
  // Snapshot the 'A'-only extras the parse left behind (n/a == -99 for the v+d
  // path or a stale read). Safe to read now: the slot is disarmed, so no later
  // reply can overwrite them before we format.
  int aAutoHome   = sfCaptureAutoHome;
  int aCurIndex   = sfCaptureCurIndex;
  int aReportedId = sfCaptureReportedId;

  // Read the freshest version/serial the reply left in the registry.
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* mf = sfFindById((uint8_t)id);
  if (mf) { strlcpy(sn, mf->serialNum, sizeof(sn)); strlcpy(fwVer, mf->fwVersion, sizeof(fwVer)); }
  xSemaphoreGive(sfMutex);

  // JSON-escape the dump, then format the reply (static buffers: off taskWeb's
  // stack; the synchronous server serves one request at a time). sn is validated
  // alphanumeric and fwVer is a version token, so neither needs escaping.
  static char escDump[TX_MAX_BYTES * 2];
  size_t ei = 0;
  for (const char* p2 = rawDump; *p2 && ei < sizeof(escDump) - 2; p2++) {
    if (*p2 == '"' || *p2 == '\\') escDump[ei++] = '\\';
    escDump[ei++] = *p2;
  }
  escDump[ei] = 0;
  static char out[TX_MAX_BYTES * 2 + 128];
  snprintf(out, sizeof(out),
           "{\"ok\":true,\"id\":%d,\"ver\":\"%s\",\"sn\":\"%s\",\"dump\":\"%s\","
           "\"autoHome\":%d,\"curIndex\":%d,\"reportedId\":%d,\"stale\":%s,\"mode\":\"%s\"}",
           id, fwVer, sn, escDump, aAutoHome, aCurIndex, aReportedId,
           gotReply ? "false" : "true", mode);
  server.send(200, "application/json", out);
}

void handleApiIdentify() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  DBG("[API] identify all -- clearing registry and broadcasting m*v\n");
  // Wipe both the in-memory list and the persisted copy, then re-discover.
  sfModulesClear();
  rs485SendStr("m*v\n");
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/mqtt/test {host?,port?,user?,pass?} -- tries the given (or saved)
// broker settings WITHOUT touching the live connection, so settings can be
// verified before saving. Two phases: TCP reachability (3s cap), then a real
// MQTT CONNECT/CONNACK using a throwaway client. Runs on taskWeb (8KB stack;
// the temporary client objects are small and its packet buffer is heap).
void handleApiMqttTest() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char host[64]; int port = cfg.mqttPort;
  char user[48], pass[64];
  strlcpy(host, cfg.mqttHost, sizeof(host));
  strlcpy(user, cfg.mqttUser, sizeof(user));
  strlcpy(pass, cfg.mqttPass, sizeof(pass));
  if (server.hasArg("plain") && server.arg("plain").length() > 1) {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
      if (doc["host"].is<const char*>()) strlcpy(host, doc["host"] | "", sizeof(host));
      if (doc["port"].is<int>())         port = doc["port"] | cfg.mqttPort;
      if (doc["user"].is<const char*>()) strlcpy(user, doc["user"] | "", sizeof(user));
      if (doc["pass"].is<const char*>()) strlcpy(pass, doc["pass"] | "", sizeof(pass));
    }
  }
  if (!host[0]) { sendJsonError(400, "no broker host configured"); return; }
  DBG("[MQTT] testing %s:%d\n", host, port);

  wdgWebMs = millis();
  WiFiClient testNet;
  // Phase 1: TCP reachability with an explicit 3s cap.
  if (!testNet.connect(host, (uint16_t)port, 3000)) {
    server.send(200, "application/json",
      "{\"ok\":false,\"tcp\":false,\"mqtt\":false,"
      "\"error\":\"TCP connect failed (host/port unreachable)\"}");
    return;
  }
  wdgWebMs = millis();
  // Phase 2: real MQTT CONNECT on the already-open socket. PubSubClient skips
  // its own TCP connect when the client is connected, so this only exchanges
  // CONNECT/CONNACK. CONNACK from a live broker arrives in milliseconds.
  PubSubClient testMq(testNet);
  testMq.setBufferSize(128);   // CONNECT/CONNACK only -- keep the heap use tiny
  bool mqOk;
  if (user[0]) mqOk = testMq.connect("sfgw-test", user, pass);
  else         mqOk = testMq.connect("sfgw-test");
  int state = testMq.state();
  testMq.disconnect();
  testNet.stop();
  wdgWebMs = millis();

  const char* why = "";
  switch (state) {                       // PubSubClient state codes
    case  0: why = "connected";                       break;
    case  1: why = "bad protocol version";            break;
    case  2: why = "client id rejected";              break;
    case  3: why = "broker unavailable";              break;
    case  4: why = "bad username or password";        break;
    case  5: why = "not authorized";                  break;
    case -2: why = "network failed during handshake"; break;
    case -4: why = "broker did not respond (timeout)";break;
    default: why = "connection failed";               break;
  }
  char out[160];
  snprintf(out, sizeof(out),
    "{\"ok\":%s,\"tcp\":true,\"mqtt\":%s,\"state\":%d,\"detail\":\"%s\"}",
    mqOk ? "true" : "false", mqOk ? "true" : "false", state, why);
  server.send(200, "application/json", out);
}

void handleApiMaintenance() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // GET returns current state; POST {"on":true|false} sets it.
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Bad JSON"); return;
    }
    if (!doc["on"].is<bool>()) { sendJsonError(400, "'on' (bool) required"); return; }
    gMaintenanceMode = doc["on"].as<bool>();
    printf("[MAINT] Maintenance mode %s\n", gMaintenanceMode ? "ENABLED" : "disabled");
    mqttPublishStateTopics();
  }
  char out[40];
  snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s}",
           gMaintenanceMode ? "true" : "false");
  server.send(200, "application/json", out);
}

// GET returns Quiet Time state; POST {"on":true|false} sets it.
void handleApiQuiet() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Bad JSON"); return;
    }
    if (!doc["on"].is<bool>()) { sendJsonError(400, "'on' (bool) required"); return; }
    sfSetQuietTime(doc["on"].as<bool>());
    mqttPublishStateTopics();
  }
  char out[40];
  snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s}", gQuietTime ? "true" : "false");
  server.send(200, "application/json", out);
}

void webInit() {
  server.on("/",                     HTTP_GET,     handleRoot);
  server.on("/ota",                  HTTP_GET,     handleOTAPage);
  server.on("/api/ota/upload",       HTTP_POST,    sendOTAUploadResult, handleOTAUpload);
  server.on("/api/rs485/messages",   HTTP_GET,     handleApiMessages);
  server.on("/api/rs485/send",       HTTP_POST,    handleApiSend);
  server.on("/api/rs485/send",       HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/modules",     HTTP_GET,     handleApiModules);
  server.on("/api/display/state",    HTTP_GET,     handleApiDisplayState);
  server.on("/api/flap/identify",    HTTP_POST,    handleApiIdentify);
  server.on("/api/flap/identify",    HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/char",        HTTP_POST,    handleApiChar);
  server.on("/api/flap/char",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/index",       HTTP_POST,    handleApiIndex);
  server.on("/api/flap/index",       HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/text",        HTTP_POST,    handleApiText);
  server.on("/api/flap/text",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/home",        HTTP_POST,    handleApiHome);
  server.on("/api/flap/home",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/calibrate",   HTTP_POST,    handleApiCalibrate);
  server.on("/api/flap/calibrate",   HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/calibrate/status", HTTP_GET, handleApiCalibrateStatus);
  server.on("/api/flap/diag",        HTTP_POST,    handleApiDiag);
  server.on("/api/flap/diag",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/diag/status", HTTP_GET,     handleApiDiagStatus);
  server.on("/api/flap/version",     HTTP_POST,    handleApiVersion);
  server.on("/api/flap/version",     HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/provision",   HTTP_POST,    handleApiProvision);
  server.on("/api/flap/deprovision", HTTP_POST,    handleApiDeprovision);
  server.on("/api/flap/deprovision", HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/homebysn",        HTTP_POST,    handleApiHomeBySN);
  server.on("/api/flap/homebysn",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/provision",       HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/homeoffset",      HTTP_POST,    handleApiHomeOffset);
  server.on("/api/flap/homeoffset",      HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/totalsteps",      HTTP_POST,    handleApiTotalSteps);
  server.on("/api/flap/totalsteps",      HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/nudge",           HTTP_POST,    handleApiNudge);
  server.on("/api/flap/nudge",           HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/goto",            HTTP_POST,    handleApiGoto);
  server.on("/api/flap/goto",            HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/writepos",        HTTP_POST,    handleApiWritePos);
  server.on("/api/flap/writepos",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/autohome",        HTTP_POST,    handleApiAutoHome);
  server.on("/api/flap/autohome",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/erase",           HTTP_POST,    handleApiErase);
  server.on("/api/flap/erase",           HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/factoryreset",    HTTP_POST,    handleApiFactoryReset);
  server.on("/api/flap/factoryreset",    HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/dump",              HTTP_POST,    handleApiDump);
  server.on("/api/flap/dump",              HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/all",               HTTP_POST,    handleApiAll);
  server.on("/api/flap/all",               HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/dumpbysn",          HTTP_POST,    handleApiDumpBySN);
  server.on("/api/flap/dumpbysn",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/factoryresetbysn",HTTP_POST,    handleApiFactoryResetBySN);
  server.on("/api/flap/factoryresetbysn",HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/restorebysn",     HTTP_POST,    handleApiRestoreBySN);
  server.on("/api/flap/restorebysn",     HTTP_OPTIONS, handleOptions);
  server.on("/api/status",           HTTP_GET,     handleApiStatus);
  server.on("/api/mqtt/test",        HTTP_POST,    handleApiMqttTest);
  server.on("/api/mqtt/test",        HTTP_OPTIONS, handleOptions);
  server.on("/api/maintenance",      HTTP_GET,     handleApiMaintenance);
  server.on("/api/maintenance",      HTTP_POST,    handleApiMaintenance);
  server.on("/api/maintenance",      HTTP_OPTIONS, handleOptions);
  server.on("/api/quiet",            HTTP_GET,     handleApiQuiet);
  server.on("/api/quiet",            HTTP_POST,    handleApiQuiet);
  server.on("/api/quiet",            HTTP_OPTIONS, handleOptions);
  server.on("/api/config",           HTTP_GET,     handleApiConfigGet);
  server.on("/api/config/wifi",      HTTP_POST,    handleApiConfigWifi);
  server.on("/api/config/wifi",      HTTP_OPTIONS, handleOptions);
  server.on("/api/config/mqtt",      HTTP_POST,    handleApiConfigMqtt);
  server.on("/api/config/mqtt",      HTTP_OPTIONS, handleOptions);
  server.on("/api/config/rs485",     HTTP_POST,    handleApiConfigRS485);
  server.on("/api/config/rs485",     HTTP_OPTIONS, handleOptions);
  server.on("/api/config/settings",  HTTP_POST,    handleApiConfigSettings);
  server.on("/api/config/settings",  HTTP_OPTIONS, handleOptions);
  server.begin();
  printf("[Web] HTTP server started\n");
}

/* ----------------------------------------------------------
   FreeRTOS tasks
---------------------------------------------------------- */

// RS485 receive + response parsing (Core 0)
//
// The split-flap protocol uses newline-terminated ASCII messages that always
// start with 'm'.  We accumulate bytes byte-by-byte into a line buffer and
// only commit a complete message to the ring buffer when we see '\n' (or when
// the buffer is about to overflow).  This guarantees every ring buffer entry
// is exactly one complete protocol message regardless of how the UART delivers
// the bytes (split across reads, multiple messages in one read, etc.).
void taskRS485(void* pv) {
  // Receive accumulator sized for long inbound frames (e.g. a full EEPROM dump
  // response m<id>d:<offset>:<steps>:<map> can reach ~590 bytes). The monitor
  // ring entry (RS485Msg.data) stays at MSG_MAX_BYTES, so the ring copy below
  // is truncated for display while the full frame is parsed.
  static uint8_t lineBuf[TX_MAX_BYTES];
  static size_t  lineLen = 0;

  // Startup discovery: if we booted with an empty registry (first boot, or after
  // an Identify-All / cleared list), broadcast m*v so every module on the bus
  // reports its version + serial and populates the initial list. Delayed briefly
  // so the bus driver and the RX path here are fully up before we transmit.
  {
    vTaskDelay(pdMS_TO_TICKS(1500));
    bool empty;
    if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
    empty = (sfModuleCount == 0);
    if (sfMutex) xSemaphoreGive(sfMutex);
    if (empty) {
      printf("[MOD] empty registry at boot -- broadcasting m*v to discover modules\n");
      rs485SendStr("m*v\n");
    }
  }

  while (true) {
    while (rs485.available()) {
      int b = rs485.read();
      if (b < 0) break;
      uint8_t c = (uint8_t)b;

      // Touch the watchdog inside the byte loop too: a sustained burst of
      // bus traffic (each completed frame triggers rtcFormatTime + MQTT +
      // parse) could otherwise keep us in this inner loop past the 30s
      // RS485 watchdog threshold and trigger a false stall reboot.
      wdgRS485Ms = millis();
      gLastRxMs  = wdgRS485Ms;   // bus activity marker for TX collision avoidance

      // If we see an 'm' and the buffer already has content that doesn't
      // start with 'm', discard the stale partial frame and start fresh.
      if (c == 'm' && lineLen > 0 && lineBuf[0] != 'm') {
        lineLen = 0;
      }

      // Start accumulating only once we've seen the leading 'm'.
      if (lineLen == 0 && c != 'm') {
        continue;  // skip noise / framing bytes before the message start
      }

      // Append byte, guarding against overflow.
      if (lineLen < TX_MAX_BYTES - 1) {
        lineBuf[lineLen++] = c;
      } else {
        // Buffer full without a newline -- corrupt/oversized frame, discard.
        lineLen = 0;
        continue;
      }

      // Newline = end of message.  Commit to ring buffer.
      if (c == '\n') {
        rxCount++;
        RS485Msg m;
        m.timestamp = millis();
        m.dir       = 'R';
        m.sanitized = false;   // RX frames are never sanitized
        // The monitor ring entry is fixed-size; store at most MSG_MAX_BYTES.
        // The full frame is still parsed below -- this copy is display-only.
        size_t ringLen = (lineLen > MSG_MAX_BYTES) ? MSG_MAX_BYTES : lineLen;
        m.len       = ringLen;
        memcpy(m.data, lineBuf, ringLen);
        rtcFormatTime(m.wallTime, sizeof(m.wallTime));
        m.epoch     = rtcEpochNow();  // UTC epoch for browser-local display
        // Log the received frame (strip trailing newline for readability)
        { char dbg[MSG_MAX_BYTES]; size_t dlen = (ringLen > 0) ? ringLen-1 : 0;
          if (dlen > sizeof(dbg) - 1) dlen = sizeof(dbg) - 1;
          memcpy(dbg, lineBuf, dlen); dbg[dlen] = '\0';
          DBG("[RX] %s  (%s)\n", dbg, m.wallTime); }
        ringPush(m);
        mqttPublishMsg(m);
        sfParseResponse(lineBuf, lineLen);   // full frame, not the truncated copy
        lineLen = 0;
      }
    }

    // Deferred post-provision version queries (with retry). When a module's
    // verDueMs arrives we query its version; if no version has come back yet we
    // re-arm for another attempt, up to MODULE_VER_MAX_TRIES. A module is
    // considered done the moment its fwVersion is populated (the version-response
    // handler fills it). Done here (not inline in the ack handler) so the module
    // has settled on its new ID and will actually reply. IDs to query are
    // collected under the lock and sent unlocked (rs485Send re-takes sfMutex via
    // frame tracking -> querying under it would deadlock).
    {
      unsigned long nowMs = millis();
      static uint8_t verDue[MAX_MODULES];
      int verN = 0;
      if (sfMutex && xSemaphoreTake(sfMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < sfModuleCount && verN < MAX_MODULES; i++) {
          SFModule& m = sfModules[i];
          if (!m.verDueMs || nowMs < m.verDueMs) continue;   // not due
          if (m.fwVersion[0]) { m.verDueMs = 0; continue; }  // already known -> stop
          if (m.verTries >= MODULE_VER_MAX_TRIES) {          // gave up
            m.verDueMs = 0;
            // Exhausted the post-provision retries without a version reply. With
            // the newline-collision fixed a direct version query is reliable, so
            // reaching this point is rare (a module that was busy/off-bus the
            // whole window). Just stop the deferred sweep and leave fwVersion
            // empty; the Info dialog or the next Identify will re-query and fill
            // it in. No sentinel is stamped -- mislabeling a healthy module as
            // "older" was a workaround for the (now-fixed) collision bug.
            DBG("[MOD] module %d: no version after %d post-provision tries -- will re-query on demand\n",
                m.id, m.verTries);
            continue;
          }
          verDue[verN++] = m.id;
          m.verTries++;
          m.verDueMs = nowMs + MODULE_VER_RETRY_MS;           // re-arm for a retry
        }
        xSemaphoreGive(sfMutex);
      }
      for (int i = 0; i < verN; i++) {
        DBG("[MOD] post-provision version query -> module %d\n", verDue[i]);
        sfQueryVersion(verDue[i]);
      }
    }

    wdgRS485Ms = millis();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void taskRTC(void* pv) {
  while (true) { rtcRead(); vTaskDelay(pdMS_TO_TICKS(1000)); }
}

void taskWeb(void* pv) {
  unsigned long clientSince = 0;   // millis() when current client first seen
  while (true) {
    wdgWebMs = millis();      // touch BEFORE handling (covers in-handler stalls)
    server.handleClient();

    // Proactively close any client that lingers connected for too long.
    // The ESP32 WebServer keeps a half-open connection in HC_WAIT_READ for
    // up to HTTP_MAX_DATA_WAIT; a browser (notably Chrome/Safari) that opens
    // a speculative socket and never completes the request can otherwise
    // wedge handleClient() and stall the web task -> "Web=0" watchdog reboot.
    WiFiClient c = server.client();
    if (c && c.connected()) {
      if (clientSince == 0) clientSince = millis();
      else if (millis() - clientSince > 8000UL) {   // 8s hard cap per connection
        c.stop();                                    // force-close the stale socket
        clientSince = 0;
      }
    } else {
      clientSince = 0;   // no client connected -- reset the timer
    }

    wdgWebMs = millis();      // touch AFTER handling
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static bool          staWasUp    = false;
static unsigned long wifiRetryMs = 0;
static unsigned long staDownSince = 0;   // millis() the station last dropped (0 = up/never)

void taskNetwork(void* pv) {
  // WiFi init done in setup() - this task only polls and reconnects
  while (true) {
    bool staUp = (WiFi.status() == WL_CONNECTED);
    if (staUp && !staWasUp) {
      staWasUp = true;
      wifiSetApActive(false);   // station is up -> drop the fallback AP
      if (!ntpSynced) ntpSynced = rtcNTPSync();
      { IPAddress _a = WiFi.localIP();
  printf("[WiFi] Connected IP=%d.%d.%d.%d\n", _a[0],_a[1],_a[2],_a[3]); }
    } else if (!staUp && staWasUp) {
      staWasUp = false;
      staDownSince = millis();
      printf("[WiFi] Disconnected\n");
    }
    // Fallback AP: if the station has been down for a grace period (and a
    // network is configured), bring the AP up so the gateway stays reachable.
    // If no network is configured the AP was already raised at boot. Skipped
    // during OTA (the AP is intentionally down to free RAM for the upload).
    if (!staUp && !gApActive && !gOtaInProgress && strlen(cfg.wifiSSID) &&
        staDownSince && millis() - staDownSince > 20000UL) {
      printf("[WiFi] Station down 20s -- raising fallback AP\n");
      wifiSetApActive(true);
    }
    if (!staUp && strlen(cfg.wifiSSID) && millis() - wifiRetryMs > 15000UL) {
      wifiRetryMs = millis();
      WiFi.disconnect();
      WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    }
    if (staUp && strlen(cfg.mqttHost) && !gOtaInProgress) {
      if (!mqtt.connected() && millis() - mqttRetryMs > 30000UL) {
        mqttRetryMs = millis();
        mqttConnect();
        if (!mqtt.connected()) {
          mqttFailCount++;
          // After 5 consecutive failures with WiFi "up", the TCP stack
          // is likely wedged. Force a full WiFi reconnect to recover.
          if (mqttFailCount >= 5) {
            printf("[MQTT] %d consecutive failures -- forcing WiFi reconnect\n",
                   mqttFailCount);
            mqttFailCount = 0;
            WiFi.disconnect(true);
            delay(500);
            WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
            wifiRetryMs = millis();
          }
        } else {
          mqttFailCount = 0;
        }
      }
      if (mqtt.connected()) {
        mqtt.loop();
        // Drain the outbound queue -- all mqtt.publish calls happen here
        if (mqttQMutex && mqttQueue && xSemaphoreTake(mqttQMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          while (mqttQTail != mqttQHead) {
            MqttQItem& item = mqttQueue[mqttQTail];
            mqtt.publish(item.topic, (uint8_t*)item.payload, item.len, false);
            mqttQTail = (mqttQTail + 1) % MQTT_Q_SIZE;
          }
          xSemaphoreGive(mqttQMutex);
        }
      }
    }
    if (!gOtaInProgress && millis() - lastStatusMs > STATUS_INTERVAL_MS) {
      lastStatusMs = millis();
      mqttPublishStatus();
    }
    // Refresh the HA display sensor when tracking changed, rate-limited to avoid
    // spamming HA's recorder. No-op unless HA integration is enabled. Skipped
    // during an OTA upload to keep heap/CPU free for the transfer.
    if (!gOtaInProgress && gDisplayDirty && cfg.haEnabled && millis() - lastDispPubMs > 1500) {
      gDisplayDirty = false;
      lastDispPubMs = millis();
      mqttPublishDisplayState();
    }

    // Persist the module registry if it changed (debounced to limit NVS wear).
    if (sfModulesDirty) {
      if (sfModulesDirtyMs == 0) sfModulesDirtyMs = millis();
      if (millis() - sfModulesDirtyMs > MODULE_SAVE_DEBOUNCE_MS) {
        sfModulesSave();
        sfModulesDirty   = false;
        sfModulesDirtyMs = 0;
      }
    }
    // Periodically prune stale modules (once a minute is plenty).
    static unsigned long lastPruneMs = 0;
    if (millis() - lastPruneMs > 60000UL) {
      lastPruneMs = millis();
      sfModulesPruneStale();
    }

    wdgNetMs = millis();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/* ----------------------------------------------------------
   setup / loop
---------------------------------------------------------- */
void setup() {
  // 1. Mutexes first - must exist before any task touches shared data
  msgMutex   = xSemaphoreCreateMutexStatic(&msgMutexBuf);
  sfMutex    = xSemaphoreCreateMutexStatic(&sfMutexBuf);
  timeMutex  = xSemaphoreCreateMutexStatic(&timeMutexBuf);
  mqttQMutex = xSemaphoreCreateMutexStatic(&mqttQMutexBuf);
  txMutex    = xSemaphoreCreateMutexStatic(&txMutexBuf);
  psramAllocInit();   // allocate large buffers (ring + MQTT queue + registry) in PSRAM

  // Debug output via native USB CDC (USB CDC On Boot: Enabled).
  // Port appears as /dev/cu.usbmodem* on macOS, COMx on Windows, /dev/ttyACM0 on Linux.
  // Connect at 115200 baud.
  Serial.begin(115200);
  { unsigned long t = millis(); while (!Serial && millis() - t < 3000) delay(10); }
  delay(200);
  printf("\n[Boot] Split-Flap Gateway v%s\n", FW_VERSION);
  // Reset reason + chip/heap snapshot -- the first thing to check after an
  // unexpected reboot. PANIC/INT_WDT/TASK_WDT point at firmware faults;
  // BROWNOUT points at power. Pair this with the last [WDG] line before the gap.
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char* rs = "OTHER";
    switch (rr) {
      case ESP_RST_POWERON:  rs = "POWERON";  break;
      case ESP_RST_SW:       rs = "SW";       break;
      case ESP_RST_PANIC:    rs = "PANIC";    break;
      case ESP_RST_INT_WDT:  rs = "INT_WDT";  break;
      case ESP_RST_TASK_WDT: rs = "TASK_WDT"; break;
      case ESP_RST_WDT:      rs = "WDT";      break;
      case ESP_RST_BROWNOUT: rs = "BROWNOUT"; break;
      case ESP_RST_DEEPSLEEP:rs = "DEEPSLEEP";break;
      default: break;
    }
    printf("[Boot] reset=%s heap=%u psram=%u flash=%uKB sdk=%s\n",
           rs, ESP.getFreeHeap(), ESP.getPsramSize(),
           ESP.getFlashChipSize()/1024, ESP.getSdkVersion());
  }

  // 2. Load config and init module registry
  cfgSetDefaults();
  loadConfig();
  memset(sfModules, 0, sizeof(SFModule) * MAX_MODULES);
  for (int i = 0; i < MAX_MODULES; i++) sfModules[i].id = 255;
  sfModuleCount = 0;

  // 3. I2C + RTC (must be before WiFi so timestamps work from boot)
  rtcHwInit();
  rtcRead(); // load whatever time is stored in RTC chip

  // Restore sticky module list from the FATFS file (prunes entries older
  // than 6h). Mount the filesystem first; done after rtcRead() so
  // rtcEpochNow() can evaluate staleness.
  sfFsInit();
  sfModulesLoad();

  // 4. RS485
  rs485Begin();

  // 5. WiFi - MUST be initialised here on the main Arduino task.
  // The SoftAP is a FALLBACK only: start in station mode and connect to the
  // configured network. If no network is configured, bring the fallback AP up
  // immediately so the gateway is reachable for first-time setup. If a network
  // is configured but the station fails to connect, the network task brings the
  // fallback AP up after a grace period (and drops it again once the station
  // connects), so a working WiFi link never leaves the AP running.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  if (strlen(cfg.wifiSSID)) {
    WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    staDownSince = millis();   // start the fallback grace timer from boot
    printf("[WiFi] STA connecting to %s...\n", cfg.wifiSSID);
  } else {
    wifiSetApActive(true);   // no credentials -> fallback AP for setup
    printf("[WiFi] No network configured -- fallback AP only\n");
  }

  // 6. Web server
  otaInit();
  mqttInit();
  webInit();

  // 7. Spawn tasks after WiFi stack is ready
  xTaskCreatePinnedToCore(taskRTC,     "RTC",     2048, NULL, 2, &hTaskRTC,   0);
  xTaskCreatePinnedToCore(taskRS485,   "RS485",   6144, NULL, 3, &hTaskRS485, 0);
  xTaskCreatePinnedToCore(taskOTA,     "OTA",     4096, NULL, 1, &hTaskOTA,   1);
  xTaskCreatePinnedToCore(taskWeb,     "Web",     8192, NULL, 2, &hTaskWeb,   0);
  xTaskCreatePinnedToCore(taskNetwork, "Network", 6144, NULL, 1, &hTaskNet,   1);

  printf("[Boot] Ready\n");
}

void loop() {
  static unsigned long lastWdgCheck = 0;
  unsigned long now = millis();
  if (now - lastWdgCheck >= 30000UL) {
    lastWdgCheck = now;
    // Rich periodic telemetry for troubleshooting. Heap + min-ever heap +
    // largest free block (fragmentation: a big gap between freeHeap and
    // maxAlloc signals fragmentation, a common pre-crash signature). Per-task
    // stack high-water marks catch the canary-overflow class before it fires.
    // rx/tx/parse-reject counters surface bus health and corruption rates.
    unsigned freeHeap = ESP.getFreeHeap();
    unsigned minHeap  = ESP.getMinFreeHeap();
    unsigned maxBlk   = ESP.getMaxAllocHeap();
    unsigned s485 = hTaskRS485 ? uxTaskGetStackHighWaterMark(hTaskRS485) : 0;
    unsigned sWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
    unsigned sNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
    unsigned sOta = hTaskOTA   ? uxTaskGetStackHighWaterMark(hTaskOTA)   : 0;
    unsigned sRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
    printf("[WDG] up=%lus heap=%u min=%u maxblk=%u frag=%u%% "
           "stk(485/web/net/ota/rtc)=%u/%u/%u/%u/%u "
           "rx=%lu tx=%lu rej=%lu wifi=%d ap=%d rssi=%d mqtt=%d mods=%d\n",
           now/1000, freeHeap, minHeap, maxBlk,
           freeHeap ? (unsigned)(100 - (maxBlk * 100UL / freeHeap)) : 0,
           s485, sWeb, sNet, sOta, sRtc,
           rxCount, txCount, sfParseRejects,
           (int)(WiFi.status()==WL_CONNECTED),
           (int)gApActive,
           (WiFi.status()==WL_CONNECTED) ? (int)WiFi.RSSI() : 0,
           (int)mqtt.connected(), sfModuleCount);

    // Boot grace period: skip stall detection for the first 60s. The first
    // boot after flashing formats the FATFS partition (a long blocking flash
    // operation), and WiFi/MQTT bring-up can briefly skew task scheduling.
    // Rebooting during this window would be a false positive.
    if (now < 60000UL) {
      // still arm the low-heap emergency check below, but skip stall logic
    } else {
      // Detect stalled tasks. A heartbeat in the future (wdg > now) can only
      // come from a transient timing skew during boot -- treat it as healthy
      // rather than letting the unsigned subtraction underflow to a huge value.
      bool ok485 = (wdgRS485Ms == 0 || wdgRS485Ms > now || now - wdgRS485Ms < 30000UL);
      bool okWeb  = (wdgWebMs  == 0 || wdgWebMs  > now || now - wdgWebMs  < 120000UL);
      bool okNet  = (wdgNetMs  == 0 || wdgNetMs  > now || now - wdgNetMs  < 30000UL);
      if (!ok485 || !okWeb || !okNet) {
        printf("[WDG] STALL: RS485=%d Web=%d Net=%d (age485=%lus ageWeb=%lus ageNet=%lus heap=%u) -- rebooting\n",
                      ok485, okWeb, okNet,
                      wdgRS485Ms ? (now - wdgRS485Ms)/1000 : 0,
                      wdgWebMs   ? (now - wdgWebMs)/1000   : 0,
                      wdgNetMs   ? (now - wdgNetMs)/1000   : 0,
                      ESP.getFreeHeap());
        delay(200);
        ESP.restart();
      }
    }
    // Emergency reboot if heap falls critically low (< 20KB)
    if (ESP.getFreeHeap() < 20000) {
      printf("[WDG] CRITICAL: heap=%u -- rebooting\n", ESP.getFreeHeap());
      delay(200);
      ESP.restart();
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
}

/*
 * ============================================================
 * SPLIT-FLAP PROTOCOL (firmware v6-v12, 9600 baud 8N1)
 * ============================================================
 * Bus format:   m<ADDR><CMD>[data]\n
 * Addresses:    decimal (e.g. m5, m38, m005)
 *               broadcast: m* or m**
 *               provisioning: mX...
 * Char set:     " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw"
 *
 * Commands (gateway -> module):
 *   m<id>-<C>        display character C
 *   m<id>+<n>        display flap index n (0-63)
 *   m<id>h           home the reel
 *   m<id>c           calibrate steps/rev (replies m<id>:<steps>\n)
 *   m<id>v           query firmware version (replies m<id>v:<ver>\n)
 *   m<id>d           dump EEPROM (replies m<id>d:<ho>:<ts>:<map>\n)
 *   m<id>i<n>        set module ID to n
 *   m<id>a<0|1>      set auto-home flag
 *   m<id>o<n>        set home offset (steps)
 *   m<id>t<n>        set total steps per revolution
 *   m<id>s<n>        nudge forward n steps, add to home offset
 *   m<id>g<n>        go to raw step position n
 *   m<id>w<i>:<p>    write calibrated step pos p for flap index i
 *   m<id>e           erase flap position map
 *   m<id>R           reset provisioning (module becomes unprovisioned)
 *   m<id>F           factory reset calibration (preserves ID)
 *   mXI<sn>:<id>     assign ID <id> to module with serial <sn>
 *   mXH<sn>          home module with serial <sn>
 *
 * Responses (module -> gateway):
 *   m<id>v:<ver>\n   firmware version
 *   m<id>:<steps>\n  calibration result (steps per revolution)
 *   m<id>d:<ho>:<ts>:<map>\n  EEPROM dump
 *   mXadv:<sn>\n     advertisement from unprovisioned module
 *   mXack:<sn>:<id>\n provisioning confirmation
 *
 * ============================================================
 * REST API
 * ============================================================
 *   GET  /api/flap/modules              list all known modules
 *   POST /api/flap/char                 {"id":5,"char":"A"}   id=-1 = broadcast
 *   POST /api/flap/index                {"id":5,"index":3}
 *   POST /api/flap/text                 {"text":"HELLO","start":0}
 *   POST /api/flap/home                 {"id":5}  or  {"id":-1}
 *   POST /api/flap/calibrate            {"id":5}
 *   POST /api/flap/version              {"id":5}
 *   POST /api/flap/provision            {"sn":"AABBCC...","id":5}
 *   GET  /api/rs485/messages            raw bus frames since last poll
 *   POST /api/rs485/send                {"data":"m5-A\n"}
 *   GET  /api/status                    system status
 *   GET  /api/config                    current config (no passwords)
 *   POST /api/config/wifi               {"ssid":"...","pass":"..."}
 *   POST /api/config/mqtt               {"host":"...","port":1883,...}
 *   POST /api/config/rs485              {"baud":9600,...}
 *
 * ============================================================
 * MQTT TOPICS  (default prefix "splitflap")
 * ============================================================
 *   Subscribe to push commands:
 *     splitflap/send                {"data":"m5-A\n"}
 *     splitflap/flap/set            {"id":5,"char":"A"}
 *                                 {"id":5,"index":3}
 *                                 {"id":-1,"text":"HELLO","start":0}
 *     splitflap/flap/home           {"id":5}
 *     splitflap/flap/provision      {"sn":"AABBCC...","id":5}
 *
 *   Published automatically:
 *     splitflap/rx                  every received raw frame
 *     splitflap/tx                  every transmitted raw frame
 *     splitflap/status              heartbeat every 10 s
 *     splitflap/flap/adv            unprovisioned module advertisement
 *     splitflap/flap/ack            provisioning confirmation
 *     splitflap/flap/version        firmware version response
 *     splitflap/flap/calibrated     calibration result
 *     splitflap/flap/dump           EEPROM dump response
 */
