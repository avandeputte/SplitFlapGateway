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

// Early-declared debug flag so DBG() works before cfg is constructed.
// Kept in sync with cfg.serialDebug in loadConfig() and handleApiConfigSettings().
static volatile bool gSerialDebug = false;
// Maintenance mode: when true, external commands arriving via MQTT are ignored
// and not relayed to the RS-485 bus. The web UI / REST API (the gateway itself)
// continue to work normally. Always OFF at boot -- never persisted -- so a
// reboot is a guaranteed return to normal operation.
static volatile bool gMaintenanceMode = false;
#define DBG(...) do { if (gSerialDebug) printf(__VA_ARGS__); } while(0)

/* ----------------------------------------------------------
   Pin definitions  (Waveshare ESP32-S3-RS485-CAN)
---------------------------------------------------------- */
#define RS485_TX_PIN   17
#define RS485_RX_PIN   18
#define RS485_EN_PIN   21

/* ----------------------------------------------------------
   I2C + PCF85063 RTC  (SDA=39, SCL=38, I2C addr 0x51)
   NTP syncs the RTC on first WiFi connection.
---------------------------------------------------------- */
#define I2C_SDA_PIN       39
#define I2C_SCL_PIN       38
#define PCF85063_ADDR     0x51
#define PCF85063_SEC_REG  0x04
#define PCF85063_CTRL1    0x00
#define RTC_YEAR_OFFSET   2000  // PCF85063 reg 6 is 0-99 = 2000-2099
#define DEFAULT_NTP_SERVER "pool.ntp.org"   // overridable via Settings
#define NTP_TIMEOUT_MS    8000UL

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

/* ----------------------------------------------------------
   Compile-time defaults
---------------------------------------------------------- */
#define DEFAULT_AP_SSID      "Split-Flap-GW"
#define DEFAULT_AP_PASS      "12345678"
#define DEFAULT_BAUD         9600UL
#define DEFAULT_MQTT_PORT    1883
#define DEFAULT_MQTT_PREFIX  "splitflap"
#define FW_VERSION           "1.3"   // gateway firmware version (UI + boot log)
#define MSG_RING_SIZE        64
#define MSG_MAX_BYTES        256
// Outbound frames may be longer than the 256-byte monitor-ring entry size --
// a full 64-flap restore command (mXW<sn>:<offset>:<steps>:<map>) can reach
// ~620 bytes. TX_MAX_BYTES bounds what rs485Send will transmit; the monitor
// ring still stores only the first MSG_MAX_BYTES for display.
#define TX_MAX_BYTES         768
// Half-duplex collision avoidance: before transmitting, wait until the bus has
// been quiet for TX_BUS_GUARD_MS (so we never stomp on an in-flight module
// response train), capped at TX_BUS_WAIT_CAP_MS so a noisy bus can't block
// transmit forever. 12ms ~= 12 byte-times at 9600 baud.
#define TX_BUS_GUARD_MS      12
#define TX_BUS_WAIT_CAP_MS   400
#define MQTT_BUF_SIZE       768   // holds a full restore command via MQTT
#define MQTT_Q_SIZE 32
struct MqttQItem { char topic[48]; char payload[MQTT_BUF_SIZE]; size_t len; };
static MqttQItem             mqttQueue[MQTT_Q_SIZE];
static volatile int          mqttQHead     = 0;
static volatile int          mqttQTail     = 0;
static SemaphoreHandle_t     mqttQMutex    = NULL;
static StaticSemaphore_t     mqttQMutexBuf;
#define STATUS_INTERVAL_MS   60000UL   // MQTT status publish cadence (1/min)
#define MODULE_STALE_SECS    21600UL   // 6h: prune modules not seen in this long
#define MODULE_SAVE_DEBOUNCE_MS 5000UL // coalesce NVS writes

/* ----------------------------------------------------------
   Module registry  (tracks known modules on the bus)
---------------------------------------------------------- */
// Supports module IDs 0-254 (255 modules). id==255 is reserved as the
// empty-slot / unprovisioned sentinel, so the array needs one slot per usable
// ID. Frame buffers (MSG_MAX_BYTES / TX_MAX_BYTES / MQTT_BUF_SIZE) are sized by
// the 64-flap dump/restore MAP, not by module count -- a frame targets a single
// module -- so they are unaffected by this bound. 3-digit IDs (vs 2) add ~1
// byte to a handful of commands, still far inside TX_MAX_BYTES.
#define MAX_MODULES         255   // module IDs 0-254

struct SFModule {
  uint8_t  id;               // 0-254; 255 = slot empty
  char     serialNum[21];    // hex serial from advertisement (0-terminated)
  bool     provisioned;      // false = advertising (id==255 from adv)
  int      flapIndex;        // last known flap index (-1 = unknown)
  char     flapChar;         // last known displayed char (0 = unknown)
  char     fwVersion[8];     // firmware version string
  unsigned long lastSeen;    // millis() of last activity (resets on reboot)
  unsigned long lastSeenEpoch; // RTC wall-clock epoch of last activity (survives reboot)
};

static SFModule sfModules[MAX_MODULES];
static SemaphoreHandle_t sfMutex = NULL;
static StaticSemaphore_t sfMutexBuf;
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
static volatile int           sfCalibWaitId   = -1;  // module id handleApiCalibrate is waiting on
static volatile int           sfCalibSteps    = 0;   // captured steps/rev from m<id>:<steps>
static volatile unsigned long sfCalibCaptureTs = 0;  // millis() when captured (0=none)
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
  prefs.putString("otaPass",   cfg.otaPassword);
  prefs.end();
}

/* ----------------------------------------------------------
   Message ring buffer
---------------------------------------------------------- */
struct RS485Msg {
  unsigned long timestamp;
  char          dir;
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
void rs485Send(const uint8_t* data, size_t len);

static RS485Msg          msgRing[MSG_RING_SIZE];
static volatile int      msgHead       = 0;
static volatile int      msgPollCursor = 0;
static StaticSemaphore_t msgMutexBuf;
static SemaphoreHandle_t msgMutex = NULL;

void ringPush(const RS485Msg& m) {
  if (!msgMutex) return;
  xSemaphoreTake(msgMutex, portMAX_DELAY);
  msgRing[msgHead] = m;
  msgHead = (msgHead + 1) % MSG_RING_SIZE;
  xSemaphoreGive(msgMutex);
}

String ringDrain() {
  if (!msgMutex) return "[]";
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

void rs485Send(const uint8_t* data, size_t len) {
  if (!len || len > TX_MAX_BYTES) return;
  // Collision avoidance on the half-duplex bus: if modules are mid-response
  // (e.g. the staggered reply train after a broadcast m*v), transmitting now
  // would fight their drivers, corrupting bytes and destroying the newline
  // terminators (observed as glued/garbled frames and poisoned serial numbers).
  // Hold off until the bus has been quiet for TX_BUS_GUARD_MS, bounded by
  // TX_BUS_WAIT_CAP_MS so we always make progress.
  {
    unsigned long waitStart = millis();
    while (millis() - gLastRxMs < TX_BUS_GUARD_MS &&
           millis() - waitStart < TX_BUS_WAIT_CAP_MS) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  rs485.write(data, len);
  rs485.flush();
  txCount++;
  // Update per-module display tracking from this frame. Doing it here -- the
  // single point every outbound frame passes through -- means raw sends are
  // tracked exactly like the high-level helpers, with no per-path duplication.
  sfTrackFromFrame(data, len);
  // Log the transmitted frame (strip trailing newline for readability).
  // Cap the debug buffer at MSG_MAX_BYTES; long frames are truncated in the log.
  { char dbg[MSG_MAX_BYTES];
    size_t dlen = (len > 0 && data[len-1] == '\n') ? len-1 : len;
    if (dlen > sizeof(dbg) - 1) dlen = sizeof(dbg) - 1;
    memcpy(dbg, data, dlen); dbg[dlen] = '\0';
    DBG("[TX] %s\n", dbg); }
  RS485Msg m;
  m.timestamp = millis();
  m.dir = 'T';
  // The monitor ring entry is fixed-size; store at most MSG_MAX_BYTES bytes.
  // The full frame was already transmitted above -- this copy is for display only.
  size_t ringLen = (len > MSG_MAX_BYTES) ? MSG_MAX_BYTES : len;
  m.len = ringLen;
  memcpy(m.data, data, ringLen);
  rtcFormatTime(m.wallTime, sizeof(m.wallTime));
  m.epoch = rtcEpochNow();   // UTC epoch; web UI renders in browser-local time
  ringPush(m);
  mqttPublishMsg(m);
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
// records written as raw bytes.
// ------------------------------------------------------------------
#define MODULES_FILE     "/modules.dat"
#define MODULES_MAGIC    0x53464731UL   // "SFG1"

struct PersistedModule {
  uint8_t       id;
  char          serialNum[21];
  bool          provisioned;
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
  memset(sfModules, 0, sizeof(sfModules));
  if (sfMutex) xSemaphoreGive(sfMutex);
  if (sfFsReady) FFat.remove(MODULES_FILE);
  sfModulesDirty = false;
  DBG("[MOD] Registry cleared (memory + FATFS)\n");
}

// Prune in-memory entries not seen for MODULE_STALE_SECS. Called periodically.
static void sfModulesPruneStale() {
  unsigned long nowEp = rtcEpochNow();
  if (!nowEp) return;  // no valid clock yet
  bool changed = false;
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < sfModuleCount; ) {
    SFModule& m = sfModules[i];
    if (m.lastSeenEpoch && nowEp > m.lastSeenEpoch &&
        (nowEp - m.lastSeenEpoch) > MODULE_STALE_SECS) {
      // Remove by shifting the tail down
      for (int j = i; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
      sfModuleCount--;
      memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
      changed = true;
    } else {
      i++;
    }
  }
  if (sfMutex) xSemaphoreGive(sfMutex);
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

// Query firmware version of a module
void sfQueryVersion(int addr) {
  char buf[16];
  snprintf(buf, sizeof(buf), "m%dv\n", addr);
  rs485SendStr(buf);
}

// Query EEPROM dump of a module
void sfQueryDump(int addr) {
  char buf[16];
  snprintf(buf, sizeof(buf), "m%dd\n", addr);
  rs485SendStr(buf);
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
  // Remove from local registry
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  if (addr < 0) {
    sfModuleCount = 0;
  } else {
    for (int i = 0; i < sfModuleCount; i++) {
      if (sfModules[i].id == (uint8_t)addr) {
        sfModules[i] = sfModules[--sfModuleCount];
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

void sfParseResponse(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'm') return;

  // Convert to null-terminated string for easier parsing.
  // Sized for long inbound frames (a full dump response is ~590 bytes).
  // NOTE: static (not stack) -- sfParseResponse is called only from taskRS485
  // (single caller, no reentrancy), and a 768-byte stack buffer here would
  // overflow that task's 4KB stack. Keeping it in .bss avoids the overflow.
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
    }
    if (m) sfTouch(m);
    xSemaphoreGive(sfMutex);
    mqttPublishSFEvent("adv", sn);
    return;
  }

  // -- Provisioning ack: mXack:<sn>:<id>
  if (strncmp(buf, "mXack:", 6) == 0) {
    char tmp[48];
    strlcpy(tmp, buf + 6, sizeof(tmp));
    char* colon = strrchr(tmp, ':');
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
      if (m) { m->id = (uint8_t)newId; m->provisioned = true; sfTouch(m); sfModulesDirty = true; }
      xSemaphoreGive(sfMutex);
      char payload[64];
      snprintf(payload, sizeof(payload), "{\"sn\":\"%s\",\"id\":%d}", sn, newId);
      mqttPublishSFEvent("ack", payload);
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
    // A glued/garbled frame (bus collision destroying the newline between two
    // responses) shows up here as an SN token with embedded ':' or raw garbage
    // bytes. Reject the WHOLE response: a frame with a corrupt tail cannot be
    // trusted, and storing its SN would poison the registry and FATFS, breaking
    // every SN-addressed command for that module from then on.
    if (field[2] && field[2][0] && !sfValidSN(field[2])) {
      DBG("[SF] rejecting corrupt version response for module %d (sn:%s)\n",
          id, field[2]);
      sfParseRejects++;
      return;
    }
    int reportedId = (field[1] && field[1][0]) ? atoi(field[1]) : -1;
    // Write registry fields under the mutex, re-finding the entry by id: the
    // pointer from the earlier upsert can be invalidated by concurrent array
    // compaction (sfModulesPruneStale runs every 60s on taskNetwork, and
    // deprovision can run from taskWeb) -- writing through a stale pointer
    // would corrupt a DIFFERENT module's record.
    char fwCopy[8] = "?";
    if (field[0] && field[0][0]) strlcpy(fwCopy, field[0], sizeof(fwCopy));
    xSemaphoreTake(sfMutex, portMAX_DELAY);
    SFModule* mm = sfFindById((uint8_t)id);
    if (mm) {
      strlcpy(mm->fwVersion, fwCopy, sizeof(mm->fwVersion));
      if (field[2] && field[2][0]) {
        strlcpy(mm->serialNum, field[2], sizeof(mm->serialNum));
      }
    }
    xSemaphoreGive(sfMutex);
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
    // buffers would overflow its 4KB stack.
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
}

/* ----------------------------------------------------------
   MQTT
---------------------------------------------------------- */
WiFiClient   wifiClient;
WiFiClient   mqttWifiClient;        // persistent client for PubSubClient
PubSubClient mqtt(mqttWifiClient);  // mqttInit() configures timeouts on this

static unsigned long lastStatusMs = 0;
static unsigned long mqttRetryMs  = 0;

// mqttTopic() removed -- all call sites use snprintf char arrays
// Safe MQTT publish from any task -- enqueues for the network task to drain.
static void mqttEnqueue(const char* topic, const char* payload, size_t len) {
  if (!mqttQMutex) return;
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
  char buf[320];
  size_t n = (size_t)snprintf(buf, sizeof(buf),
    "{\"uptime\":%lu,\"rx\":%lu,\"tx\":%lu,\"modules\":%d,"
    "\"time\":\"%s\",\"ntpSynced\":%s,\"heap\":%u}",
    millis()/1000, rxCount, txCount, sfModuleCount,
    timeBuf, ntpSynced?"true":"false", ESP.getFreeHeap());
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  char _t[80];
  snprintf(_t, sizeof(_t), "%s/status", cfg.mqttPrefix);
  mqttEnqueue(_t, buf, n);
}

// MQTT incoming message handler
// Supports:
//   <prefix>/send          {"data":"..."}  raw RS485 send
//   <prefix>/flap/set      {"id":5,"char":"A"}  or  {"id":5,"index":3}
//                          {"id":-1,"text":"HELLO","start":0}  multi-module text
//   <prefix>/flap/home     {"id":5}  or  {"id":-1}  (broadcast)
//   <prefix>/flap/provision {"sn":"AABBCC...","id":5}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Maintenance mode: ignore all externally-originated commands. Nothing from
  // MQTT is relayed to the bus while this is on; only the gateway's own web UI
  // / REST API can drive the display.
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

  // Handle the raw send topic before attempting JSON parse:
  // Accept either a plain ASCII frame ("m9h\n") or JSON ({"data":"m9h\n"}).
  if (strcmp(topic, sendTopic) == 0) {
    const char* d = nullptr;
    // Sized for long commands (e.g. a full restore) sent as a plain frame.
    // static: see note on buf above (single-caller context, stack pressure).
    static char plainBuf[TX_MAX_BYTES + 1];
    if (buf[0] == '{') {
      // Try JSON
      JsonDocument doc;
      if (deserializeJson(doc, buf) == DeserializationError::Ok) {
        d = doc["data"] | "";
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
      rs485Send(outBuf, outLen);
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
  bool ok = strlen(cfg.mqttUser)
    ? mqtt.connect(clientId, cfg.mqttUser, cfg.mqttPass)
    : mqtt.connect(clientId);
  if (ok) {
    printf("[MQTT] Connected\n");
    // Use char arrays not String to avoid heap fragmentation
    char t[80];
    snprintf(t,sizeof(t),"%s/send",           cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/flap/set",       cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/flap/home",      cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/flap/provision", cfg.mqttPrefix); mqtt.subscribe(t);
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
  server.sendContent(":root{--bg:#1a1a2e;--card:#16213e;--acc:#0f3460;--hi:#e94560;--txt:#eaeaea;--dim:#888;--grn:#4caf50;--ylw:#ffc107}");
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
  server.sendContent(".unprovisioned{border-color:var(--hi)}");
  server.sendContent("#sr{font-size:.8rem;color:var(--grn);min-height:16px;margin-top:5px}");
  server.sendContent(".cmods{display:grid;gap:5px;grid-template-columns:repeat(auto-fill,minmax(48px,1fr))}.cmod{text-align:center;padding:6px 4px;background:#0d1b2a;border:1px solid var(--acc);border-radius:5px;cursor:pointer;font-family:monospace;font-size:.85rem;color:var(--txt)}.cmod:hover{border-color:var(--hi)}.cmod.sel{background:var(--hi);border-color:var(--hi);color:#fff;font-weight:bold}.cmod .csn{display:block;font-size:.6rem;color:var(--dim)}.cmod.sel .csn{color:#ffd}.cmod.known{border-color:var(--grn)}.cmod.legacy{border-color:var(--ylw)}.cmod.unknown{opacity:.6}.cmod .csn.lg{color:var(--ylw)}.cmods.single{display:flex;flex-wrap:wrap}");
  server.sendContent(".cedit{display:flex;gap:14px;flex-wrap:wrap;background:#0a0a0a;border-radius:6px;padding:10px 14px;margin-bottom:8px}.cedit .cb{flex:1;min-width:210px}.cedit .ck{font-size:.68rem;color:var(--dim);letter-spacing:.05em;margin-bottom:4px}.cedit .cer{display:flex;gap:5px;align-items:center}.cedit .cer input{flex:1;min-width:60px;font-family:monospace;font-size:1.15rem;font-weight:bold;text-align:center}.cedit .cer input.ho{color:var(--grn)}.cedit .cer input.ts{color:var(--ylw)}.cedit .cer button{margin:0;padding:7px 11px;white-space:nowrap;font-size:.82rem}");
  server.sendContent(".cnudge{display:flex;gap:4px;flex-wrap:wrap;margin:8px 0;align-items:center}.cnudge button{margin:0;padding:5px 9px;font-size:.8rem;background:var(--acc)}.cnudge button.neg{background:#5a2030}.cnudge button.pos{background:#1f5a2a}.cnudge .lbl{font-size:.72rem;color:var(--dim);padding:0 4px}");
  server.sendContent(".tnudge{display:flex;gap:4px;flex-wrap:wrap;margin:6px 0}.tnudge button{flex:1;min-width:34px;margin:0;padding:7px 3px;font-size:.78rem}.tnudge button.neg{background:#5a2030}.tnudge button.pos{background:#1f5a2a}.tnudge .lbl{flex:0 0 auto;align-self:center;font-size:.68rem;color:var(--dim);padding:0 3px}");
  server.sendContent(".cmap{display:grid;grid-template-columns:repeat(auto-fill,minmax(64px,1fr));gap:5px;margin-top:8px}.cc{background:#0d1b2a;border:1px solid var(--acc);border-radius:5px;padding:5px 2px;text-align:center;cursor:pointer}.cc:hover{border-color:var(--hi)}.cc .cch{font-size:1.05rem;font-weight:bold;font-family:monospace}.cc .ccv{font-size:.7rem;color:var(--dim);font-family:monospace}.cc.custom{border-color:var(--grn)}.cc.custom .ccv{color:var(--grn)}.cc .sw{display:inline-block;width:14px;height:14px;border-radius:2px;vertical-align:middle}");
  server.sendContent(".tunebox{background:var(--card);border:2px solid var(--hi);border-radius:10px;padding:18px;max-width:340px;width:90%}.tunebox h3{color:var(--hi);font-size:1.1rem;margin-bottom:2px}.tunebox .exp{font-size:.8rem;color:var(--dim);margin-bottom:10px}.tunebox button{width:100%;margin-top:8px;padding:10px}.tunebox .bgoto{background:var(--acc)}.tunebox .block{background:var(--grn)}.tunebox .brev{background:var(--hi)}.tunebox .bcancel{background:#333}");
  server.sendContent("</style></head><body>");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("<header><h1>Split-Flap Gateway <span class=\"verbadge\">v" FW_VERSION "</span></h1><div class=\"hdr-right\"><label class=\"maint-toggle\" title=\"When on, commands received via MQTT are ignored and not relayed to the bus. The web UI keeps working.\"><input id=\"maintChk\" type=\"checkbox\" onchange=\"toggleMaint()\"><span class=\"maint-lbl\">Maintenance</span></label><span id=\"badge\">...</span></div></header>");
  server.sendContent("<div class=\"maint-banner\">MAINTENANCE MODE - external MQTT commands are being ignored</div>");
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
  server.sendContent("<div class=\"card\"><h2>Module Calibration</h2>");
  server.sendContent("<p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">Select any module -- known or not -- to view and adjust its home offset, total steps, and per-character flap positions. Changes are written to the module's EEPROM.</p>");
  server.sendContent("<div id=\"calMods\" class=\"cmods\">Loading modules...</div>");
  server.sendContent("<div style=\"display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin-top:8px\"><button class=\"sec\" onclick=\"calLoadModules()\" style=\"margin:0\">&#x21bb; Refresh</button><span style=\"color:var(--dim);font-size:.8rem\">or tune any ID:</span><input id=\"calAnyId\" type=\"number\" min=\"0\" max=\"254\" placeholder=\"0-254\" style=\"width:90px;margin:0\"><button onclick=\"calSelectAny()\" style=\"margin:0\">Go</button></div></div>");
  server.sendContent("<div id=\"calDetail\" class=\"card\" style=\"display:none\">");
  server.sendContent("<div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:8px\"><h2 id=\"calTitle\" style=\"margin:0\">Module</h2><button class=\"sec\" onclick=\"calRefresh()\" style=\"margin:0;padding:4px 10px;font-size:.8rem\">&#x21bb; Re-read EEPROM</button></div>");
  server.sendContent("<div id=\"calStatus\" style=\"font-size:.78rem;color:var(--ylw);min-height:15px;margin-bottom:6px\"></div>");
  server.sendContent("<div class=\"cedit\"><div class=\"cb\"><div class=\"ck\">HOME OFFSET</div><div class=\"cer\"><input id=\"calHoIn\" class=\"ho\" type=\"number\" min=\"0\"><button onclick=\"calSaveHo()\">Save</button><button class=\"sec\" onclick=\"calRevertHo()\" title=\"Reset to default 2832\">Revert</button></div></div><div class=\"cb\"><div class=\"ck\">TOTAL STEPS</div><div class=\"cer\"><input id=\"calTsIn\" class=\"ts\" type=\"number\" min=\"1\"><button onclick=\"calSaveTs()\">Save</button><button class=\"sec\" onclick=\"calRevertTs()\" title=\"Reset to default 4096\">Revert</button><button class=\"sec\" id=\"calCountBtn\" onclick=\"calCountSteps()\" title=\"Run the calibrate command: the reel spins one full revolution to measure its steps per revolution\">Count Steps</button></div></div></div>");
  server.sendContent("<div class=\"cnudge\"><button class=\"neg\" onclick=\"calNudge(-32)\">-32</button><button class=\"neg\" onclick=\"calNudge(-16)\">-16</button><button class=\"neg\" onclick=\"calNudge(-4)\">-4</button><button class=\"neg\" onclick=\"calNudge(-1)\">-1</button><span class=\"lbl\">NUDGE OFFSET</span><button class=\"pos\" onclick=\"calNudge(1)\">+1</button><button class=\"pos\" onclick=\"calNudge(4)\">+4</button><button class=\"pos\" onclick=\"calNudge(16)\">+16</button><button class=\"pos\" onclick=\"calNudge(32)\">+32</button></div>");
  server.sendContent("<p style=\"font-size:.72rem;color:var(--dim);margin:0 0 8px\">Nudge moves the reel and saves the offset instantly. Press Home to verify.</p>");
  server.sendContent("<button class=\"sec\" onclick=\"calHomeMotor()\" style=\"margin:0\">Home Motor</button>");
  server.sendContent("<h2 style=\"margin-top:14px\">Character Map</h2>");
  server.sendContent("<p style=\"font-size:.78rem;color:var(--dim);margin-bottom:4px\">Green = custom EEPROM value. Grey = firmware default. Click a character to tune its position.</p>");
  server.sendContent("<div id=\"calMap\" class=\"cmap\"></div></div>");
  server.sendContent("<div id=\"tuneModal\" style=\"display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.75);z-index:200;align-items:center;justify-content:center\" onclick=\"if(event.target===this)calCloseTune()\">");
  server.sendContent("<div class=\"tunebox\"><h3 id=\"tuneTitle\">Tune</h3><div class=\"exp\" id=\"tuneExp\">Expected: -</div><label>Absolute Target Step</label><input id=\"tuneVal\" type=\"number\" min=\"0\"><div class=\"tnudge\"><button class=\"neg\" onclick=\"calTuneNudge(-32)\">-32</button><button class=\"neg\" onclick=\"calTuneNudge(-16)\">-16</button><button class=\"neg\" onclick=\"calTuneNudge(-4)\">-4</button><button class=\"neg\" onclick=\"calTuneNudge(-1)\">-1</button><button class=\"pos\" onclick=\"calTuneNudge(1)\">+1</button><button class=\"pos\" onclick=\"calTuneNudge(4)\">+4</button><button class=\"pos\" onclick=\"calTuneNudge(16)\">+16</button><button class=\"pos\" onclick=\"calTuneNudge(32)\">+32</button></div><p style=\"font-size:.72rem;color:var(--dim);margin:2px 0 6px\">Adjust the value, then Test Position to move there. Lock to EEPROM when it looks right.</p><button class=\"bgoto\" onclick=\"calTuneGoto()\">Test Position (GOTO)</button><button class=\"block\" onclick=\"calTuneLock()\">Lock to EEPROM</button><button class=\"brev\" onclick=\"calTuneRevert()\">Revert to Default</button><button class=\"bcancel\" onclick=\"calCloseTune()\">Cancel</button><div id=\"tuneStatus\" style=\"margin-top:8px;font-size:.78rem;color:var(--ylw);min-height:15px\"></div></div></div>");
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
  server.sendContent("<div class=\"card\"><h2>Send Frame</h2><label>Data (ASCII)</label><textarea id=\"sdata\" rows=\"2\" placeholder=\"m5-A\" onkeydown=\"if(event.key===&#39;Enter&#39;&&!event.shiftKey){event.preventDefault();doSend();}\"></textarea><div id=\"sr\"></div><button onclick=\"doSend()\">Send</button></div></div>");
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
  server.sendContent("<script>");
  server.sendContent("function show(id,el){document.querySelectorAll(\".pane\").forEach(function(p){p.classList.remove(\"on\");});document.querySelectorAll(\"nav a\").forEach(function(a){a.classList.remove(\"on\");});document.getElementById(\"pane-\"+id).classList.add(\"on\");el.classList.add(\"on\");if(id===\"display\"){startWall();}else{stopWall();}if(id===\"calib\"){calLoadModules();}}");
  server.sendContent("var _wallTimer=null;function buildWall(s){var w=document.getElementById(\"wall\");if(!w)return;var cells=s.cells||[];var html=\"\";var idx=0;for(var r=0;r<s.rows;r++){html+=\"<div class='wallrow'>\";for(var c=0;c<s.cols;c++){var v=(idx<cells.length)?cells[idx]:null;var cls=\"flap\",disp=\"\";if(v===null){cls+=\" empty\";disp=\"\";}else if(v===\"?\"){cls+=\" unknown\";disp=\"?\";}else{disp=v===\" \"?\"&nbsp;\":v;}html+=\"<div class='\"+cls+\"'>\"+disp+\"</div>\";idx++;}html+=\"</div>\";}w.innerHTML=html;var known=cells.filter(function(x){return x!==null;}).length;document.getElementById(\"wallMeta\").textContent=s.rows+\" x \"+s.cols+\" grid - \"+known+\" module(s) mapped\";}");
  server.sendContent("function refreshWall(){fetch(\"/api/display/state\").then(function(r){return r.json();}).then(buildWall).catch(function(){var m=document.getElementById(\"wallMeta\");if(m)m.textContent=\"Could not load display state\";});}");
  server.sendContent("function startWall(){refreshWall();if(_wallTimer)clearInterval(_wallTimer);_wallTimer=setInterval(function(){if(document.getElementById(\"pane-display\").classList.contains(\"on\"))refreshWall();},1500);}");
  server.sendContent("function stopWall(){if(_wallTimer){clearInterval(_wallTimer);_wallTimer=null;}}");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("var FC=\" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw\";");
  server.sendContent("function sfDecode(raw){var s=raw.replace(/[\\r\\n]+$/,\"\");if(s.length<2||s[0]!==\"m\")return \"\";if(s[1]===\"X\"){if(s.indexOf(\"mXadv:\")==0)return \"ADV  unprovisioned SN: \"+s.slice(6);if(s.indexOf(\"mXack:\")==0){var r=s.slice(6),ci=r.lastIndexOf(\":\");return ci>=0?\"ACK  SN \"+r.slice(0,ci)+\" -> ID \"+r.slice(ci+1):\"ACK \"+r;}if(s[2]===\"I\"){var ci2=s.indexOf(\":\",3);return ci2>=0?\"PROVISION  SN \"+s.slice(3,ci2)+\" -> ID \"+s.slice(ci2+1):\"PROVISION \"+s.slice(3);}if(s[2]===\"H\")return \"PROVISION  home SN \"+s.slice(3);if(s[2]===\"D\")return \"DUMP       SN \"+s.slice(3);if(s[2]===\"F\")return \"FACTORY RST SN \"+s.slice(3);if(s[2]===\"W\")return \"RESTORE    \"+s.slice(3);return \"PROVISIONING \"+s.slice(2);}var p=1,id=\"\",bc=false;while(p<s.length&&(s[p]===\"*\"||s[p]>=\"0\"&&s[p]<=\"9\")){if(s[p]===\"*\")bc=true;else id+=s[p];p++;}var who=bc?\"ALL\":\"#\"+id;if(p>=s.length)return who+\" (incomplete)\";var cmd=s[p],rest=s.slice(p+1);if(cmd===\"-\"){var fi=FC.indexOf(rest[0]||\"\");var sfx=fi>=0?\" (idx \"+fi+\")\":\"\";return \"SHOW CHAR    \"+who+\" -> [\"+(rest[0]||\"?\")+\"]\"+sfx;}if(cmd===\"+\"){var n=parseInt(rest);var ch=isNaN(n)?\"?\":(FC[n]||\"?\");return \"SHOW INDEX   \"+who+\" -> \"+n+\" [\"+ch+\"]\";}if(cmd===\"h\")return \"HOME         \"+who;if(cmd===\"c\")return rest?\"CALIB RESP   \"+who+\" \"+rest+\" steps/rev\":\"CALIBRATE    \"+who;if(cmd===\"o\")return \"HOME OFFSET  \"+who+\" = \"+rest+\" steps\";if(cmd===\"t\")return \"TOTAL STEPS  \"+who+\" = \"+rest;if(cmd===\"s\")return \"NUDGE        \"+who+\" \"+rest+\" steps\";if(cmd===\"g\")return \"GOTO STEP    \"+who+\" -> step \"+rest;if(cmd===\"w\"){var wci=rest.indexOf(\":\");return wci>=0?\"WRITE POS    \"+who+\" idx \"+rest.slice(0,wci)+\" -> \"+rest.slice(wci+1)+\" steps\":\"WRITE POS    \"+who+\" \"+rest;}if(cmd===\"i\")return \"SET ID       \"+who+\" -> ID \"+rest;if(cmd===\"a\")return \"AUTO-HOME    \"+who+(rest===\"1\"?\" ON\":\" OFF\");if(cmd===\"d\")return rest&&rest[0]===\":\"?\"DUMP RESP    \"+who+\" \"+rest.slice(1):\"DUMP?        \"+who;if(cmd===\"e\")return \"ERASE MAP    \"+who;if(cmd===\":\")return \"CALIB RESP   \"+who+\" \"+rest+\" steps/rev\";if(cmd===\"v\"){if(rest&&rest[0]===\":\"){var vp=rest.slice(1).split(\":\");var vs=\"VERSION RESP \"+who+\" fw:\"+vp[0];if(vp.length>1&&vp[1]!==\"\")vs+=\" id:\"+vp[1];if(vp.length>2&&vp[2]!==\"\")vs+=\" sn:\"+vp[2];return vs;}return \"VERSION?     \"+who;}if(cmd===\"R\")return \"RESET PROV   \"+who;if(cmd===\"F\")return \"FACTORY RST  \"+who;return \"CMD [\"+cmd+\"] \"+who+(rest?\" \"+rest:\"\");}");
  server.sendContent("var _lfc=0;var _loglines=[];function loadMonPrefs(){try{var a=localStorage.getItem(\"sfgw_asc\");if(a!==null)document.getElementById(\"asc\").checked=(a===\"1\");var p=localStorage.getItem(\"sfgw_pause\");if(p!==null)document.getElementById(\"lpause\").checked=(p===\"1\");}catch(e){}}function saveMonPrefs(){try{localStorage.setItem(\"sfgw_asc\",document.getElementById(\"asc\").checked?\"1\":\"0\");localStorage.setItem(\"sfgw_pause\",document.getElementById(\"lpause\").checked?\"1\":\"0\");}catch(e){}}function downloadLog(){if(!_loglines.length){return;}var blob=new Blob([_loglines.join(\"\\n\")+\"\\n\"],{type:\"text/plain\"});var url=URL.createObjectURL(blob);var a=document.createElement(\"a\");var ts=new Date().toISOString().replace(/[:.]/g,\"-\").slice(0,19);a.href=url;a.download=\"splitflap-buslog-\"+ts+\".txt\";document.body.appendChild(a);a.click();document.body.removeChild(a);URL.revokeObjectURL(url);}loadMonPrefs();");
  server.sendContent("function clearLog(){document.getElementById(\"log\").innerHTML=\"\";_lfc=0;document.getElementById(\"logCount\").textContent=\"0 frames\";}");
  server.sendContent("function appendLogRow(m){var log=document.getElementById(\"log\");var ascii=(m.command||\"\").replace(/[\\r\\n]/g,\"\");var desc=sfDecode(ascii);var safeR=ascii.replace(/&/g,\"&amp;\").replace(/</g,\"&lt;\").replace(/>/g,\"&gt;\");var safeD=desc.replace(/&/g,\"&amp;\").replace(/</g,\"&lt;\").replace(/>/g,\"&gt;\");var ts=m.ep?new Date(m.ep*1000).toLocaleTimeString([],{hour12:false}):(m.wt?m.wt:(m.ts<60000?m.ts+\"ms\":Math.floor(m.ts/1000)+\"s\"));var row=document.createElement(\"div\");row.className=\"logrow \"+(m.dir===\"R\"?\"rx\":\"tx\");row.innerHTML=\"<span class=\\\"lts\\\">\"+ts+\"</span>\"+\"<span class=\\\"ldir\\\">\"+(m.dir===\"R\"?\"RX\":\"TX\")+\"</span>\"+\"<span class=\\\"lraw\\\">\"+safeR+\"</span>\";if(desc)row.innerHTML+=\"<span class=\\\"ldesc\\\">\"+safeD+\"</span>\";log.appendChild(row);_loglines.push(ts+\" \"+(m.dir===\"R\"?\"RX\":\"TX\")+\" \"+ascii+(desc?\"  [\"+desc+\"]\":\"\"));if(_loglines.length>5000)_loglines.splice(0,_loglines.length-5000);_lfc++;document.getElementById(\"logCount\").textContent=_lfc+\" frame\"+(_lfc===1?\"\":\"s\");if(document.getElementById(\"asc\").checked)log.scrollTop=log.scrollHeight;}");
  server.sendContent("setInterval(function(){if(document.getElementById(\"lpause\").checked)return;fetch(\"/api/rs485/messages\").then(function(r){return r.json();}).then(function(arr){arr.forEach(appendLogRow);}).catch(function(){});},600);");
  server.sendContent("function doSend(){var data=document.getElementById(\"sdata\").value.trim();if(!data){document.getElementById(\"sr\").textContent=\"Nothing to send.\";return;}fetch(\"/api/rs485/send\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({data:data})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"sr\").textContent=j.ok?\"Sent \"+j.bytes+\" bytes\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"sr\").textContent=\"Error: \"+e;});}");
  server.sendContent("function apiFlapCmd(path,body){return fetch(path,{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify(body)}).then(function(r){return r.json();});}");
  server.sendContent("function parseDump(raw){if(!raw)return{error:\"No data\"};var parts=raw.split(\":\");if(parts.length<2)return{error:\"Invalid format\",raw:raw};var r={homeOffset:parseInt(parts[0]),totalSteps:parseInt(parts[1]),map:{}};if(parts[2])parts[2].split(\",\").forEach(function(e){var kv=e.split(\"=\");if(kv.length===2&&kv[0]!==\"\")r.map[parseInt(kv[0])]=parseInt(kv[1]);});return r;}");
  server.sendContent("function refreshModules(){var el=document.getElementById(\"refreshR\");el.textContent=\"Identifying...\";fetch(\"/api/flap/identify\",{method:\"POST\"}).then(function(r){return r.json();}).then(function(j){el.textContent=j.ok?\"List cleared, identifying all modules -- refreshing in 2s\":\"Error: \"+j.error;if(j.ok)setTimeout(function(){loadModules();},2000);setTimeout(function(){loadModules();el.textContent=\"\";},7000);}).catch(function(e){el.textContent=\"Error: \"+e;});}");
  server.sendContent("function doBackup(){var prog=document.getElementById(\"backupProg\");var res=document.getElementById(\"backupR\");res.textContent=\"\";prog.textContent=\"Loading module list...\";fetch(\"/api/flap/modules\").then(function(r){return r.json();}).then(function(mods){var targets=mods.filter(function(m){return m.sn&&m.sn.length>0;});if(!targets.length){prog.textContent=\"\";res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">No modules with serial numbers found. Run Identify All first.</span>\";return;}var out={version:1,created:new Date().toISOString(),modules:[]};var i=0,okN=0,failN=0;function next(){if(i>=targets.length){prog.textContent=\"\";if(!out.modules.length){res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">No calibration data could be read.</span>\";return;}var blob=new Blob([JSON.stringify(out,null,2)],{type:\"application/json\"});var url=URL.createObjectURL(blob);var a=document.createElement(\"a\");var ts=new Date().toISOString().replace(/[:.]/g,\"-\").slice(0,19);a.href=url;a.download=\"splitflap-backup-\"+ts+\".json\";document.body.appendChild(a);a.click();document.body.removeChild(a);URL.revokeObjectURL(url);res.innerHTML=\"<span style=\\\"color:var(--grn)\\\">Backup created: \"+okN+\" module(s) saved\"+(failN?\", \"+failN+\" failed\":\"\")+\".</span>\";return;}var m=targets[i];prog.textContent=\"Reading module \"+(i+1)+\" of \"+targets.length+\" (SN \"+m.sn+\")...\";fetch(\"/api/flap/dump\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:m.id})}).then(function(r){return r.json();}).then(function(d){if(d.ok&&d.dump){out.modules.push({sn:m.sn,id:m.id,dump:d.dump});okN++;}else{failN++;}i++;next();}).catch(function(){failN++;i++;next();});}next();}).catch(function(e){prog.textContent=\"\";res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">Error: \"+e+\"</span>\";});}function parseBackupDump(raw){var parts=raw.split(\":\");if(parts.length<2)return null;var ho=parseInt(parts[0]),ts=parseInt(parts[1]);if(isNaN(ho)||isNaN(ts))return null;var map=parts.length>2?parts.slice(2).join(\":\"):\"\";return {homeOffset:ho,totalSteps:ts,map:map};}function doRestore(){var prog=document.getElementById(\"restoreProg\");var res=document.getElementById(\"restoreR\");var fileInput=document.getElementById(\"restoreFile\");var preserve=document.getElementById(\"preserveId\").checked;res.textContent=\"\";if(!fileInput.files||!fileInput.files.length){res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">Choose a backup file first.</span>\";return;}var reader=new FileReader();reader.onload=function(){var data;try{data=JSON.parse(reader.result);}catch(e){res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">Invalid JSON file.</span>\";return;}if(!data||!Array.isArray(data.modules)||!data.modules.length){res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">No modules found in backup file.</span>\";return;}var mods=data.modules,i=0,okN=0,failN=0;function next(){if(i>=mods.length){prog.textContent=\"\";res.innerHTML=\"<span style=\\\"color:var(--grn)\\\">Restore complete: \"+okN+\" module(s)\"+(failN?\", \"+failN+\" failed\":\"\")+\".</span>\"+(preserve?\"\":\" IDs were reassigned from the backup.\");return;}var m=mods[i];if(!m.sn){failN++;i++;next();return;}var p=parseBackupDump(m.dump||\"\");if(!p){failN++;i++;next();return;}prog.textContent=\"Restoring module \"+(i+1)+\" of \"+mods.length+\" (SN \"+m.sn+\")...\";fetch(\"/api/flap/restorebysn\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({sn:m.sn,homeOffset:p.homeOffset,totalSteps:p.totalSteps,map:p.map})}).then(function(r){return r.json();}).then(function(d){if(!d.ok){failN++;i++;next();return;}if(!preserve&&typeof m.id===\"number\"&&m.id>=0){fetch(\"/api/flap/provision\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({sn:m.sn,id:m.id})}).then(function(r){return r.json();}).then(function(){okN++;i++;next();}).catch(function(){okN++;i++;next();});}else{okN++;i++;next();}}).catch(function(){failN++;i++;next();});}next();};reader.onerror=function(){res.innerHTML=\"<span style=\\\"color:var(--hi)\\\">Could not read file.</span>\";};reader.readAsText(fileInput.files[0]);}");
  server.sendContent("function loadModules(){fetch(\"/api/flap/modules\").then(function(r){return r.json();}).then(function(arr){var g=document.getElementById(\"modGrid\");if(!arr.length){g.innerHTML=\"<p style='color:var(--dim)'>No modules detected yet.</p>\";return;}var h=\"\";arr.forEach(function(m){var legacy=m.provisioned&&m.lastSeen>0&&!m.fwVersion;var cls=m.provisioned?\"mod\":\"mod unprovisioned\";var idStr=m.provisioned?(\"ID: <span class='mid'>\"+m.id+\"</span>\"+(legacy?\"<span class='mlegacy' title='Firmware v7 or earlier: no serial number, no provisioning, no factory reset. Homing and calibration are fully supported.'>LEGACY</span>\":\"\")):\"<span style='color:var(--hi)'>Unprovisioned</span>\";var charStr=m.flapChar?\"Showing: <b>\"+m.flapChar+\"</b>\":\"\";var delBtn=legacy?(\"<button class='micon del dis' title='Not available on legacy (v7) modules' onclick=\\\"event.stopPropagation()\\\">&#x1f5d1;</button>\"):(\"<button class='micon del' title='Destructive actions' onclick=\\\"openDel(\"+m.id+\")\\\">&#x1f5d1;</button>\");var icons=m.provisioned?(\"<div class='micons'>\"+\"<button class='micon' title='Home' onclick=\\\"modHome(\"+m.id+\")\\\">&#x2302;</button>\"+\"<button class='micon' title='Info / EEPROM' onclick=\\\"openInfo(\"+m.id+\")\\\">&#x2139;</button>\"+delBtn+\"</div>\"):\"\";var snStr=legacy?\"SN: <span style='color:var(--dim)'>n/a (legacy)</span>\":(\"SN: \"+m.sn);var fwStr=legacy?\"<br><span class='mc'>FW: v7 or earlier</span>\":(m.fwVersion?\"<br><span class='mc'>FW: v\"+m.fwVersion+\"</span>\":\"\");h+=\"<div class='\"+cls+\"' data-mid='\"+m.id+\"'>\"+icons+idStr+\"<br><span class='mc'>\"+snStr+\"</span>\"+(charStr?\"<br><span class='mc'>\"+charStr+\"</span>\":\"\")+fwStr+\"</div>\";});g.innerHTML=h;}).catch(function(){document.getElementById(\"modGrid\").innerHTML=\"Error loading modules\";});}");
  server.sendContent("loadModules();setInterval(loadModules,5000);");
  server.sendContent("function loadUnprovisioned(){fetch(\"/api/flap/modules\").then(function(r){return r.json();}).then(function(arr){var el=document.getElementById(\"unprovList\");var up=arr.filter(function(m){return !m.provisioned;});if(!up.length){el.innerHTML=\"<p style=\\\"color:var(--dim)\\\">No unprovisioned modules seen yet.</p>\";return;}var h=\"\";up.forEach(function(m){h+=\"<div style=\\\"display:flex;align-items:center;gap:8px;margin-bottom:8px;flex-wrap:wrap\\\">\"+\"<code style=\\\"color:var(--ylw);flex:1;min-width:160px\\\">\"+m.sn+\"</code>\"+\"<button class=\\\"sec\\\" style=\\\"margin:0;padding:4px 10px;font-size:.78rem\\\" onclick=\\\"doHomeSN('\"+m.sn+\"')\\\" title=\\\"Home this module to identify it\\\">Home</button>\"+\"</div>\";});el.innerHTML=h;}).catch(function(){});}");
  server.sendContent("setInterval(loadUnprovisioned,10000);loadUnprovisioned();");
  server.sendContent("function modHome(id){apiFlapCmd(\"/api/flap/home\",{id:id}).then(function(j){loadModules();});}var _infoId=-1;var _delId=-1;function openInfo(id){_infoId=id;document.getElementById(\"modModalTitle\").textContent=\"Module #\"+id;document.getElementById(\"modModal\").style.display=\"flex\";fetchInfo(id);}function refreshDump(){if(_infoId>=0)fetchInfo(_infoId);}function closeModal(){document.getElementById(\"modModal\").style.display=\"none\";_infoId=-1;}");
  server.sendContent("function fetchInfo(id){var b=document.getElementById(\"modModalBody\");b.innerHTML=\"<p style='color:var(--dim)'>Reading module #\"+id+\" ...</p>\";document.getElementById(\"modModalStatus\").textContent=\"\";fetch(\"/api/flap/modules\").then(function(r){return r.json();}).then(function(arr){var m=null;arr.forEach(function(x){if(x.id===id)m=x;});fetch(\"/api/flap/dump\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:id})}).then(function(r){return r.json();}).then(function(d){b.innerHTML=renderInfo(m,d);document.getElementById(\"modModalStatus\").textContent=d.stale?\"EEPROM is cached -- click Refresh for a fresh read\":\"EEPROM read fresh from module\";}).catch(function(e){b.innerHTML=renderInfo(m,{ok:false,error:String(e)});});}).catch(function(e){b.innerHTML=\"<p style='color:var(--hi)'>Error: \"+e+\"</p>\";});}");
  server.sendContent("function mrow(k,v){return \"<div class='mfield'><span class='mk'>\"+k+\"</span><span class='mv'>\"+v+\"</span></div>\";}function renderInfo(m,d){var h=\"\";if(m){var legacy=m.provisioned&&m.lastSeen>0&&!m.fwVersion;h+=mrow(\"Module ID\",m.id)+mrow(\"Serial Number\",legacy?\"n/a (legacy module)\":(m.sn||\"-\"))+mrow(\"Provisioned\",m.provisioned?(legacy?\"yes (hardcoded ID)\":\"yes\"):\"no\")+mrow(\"Firmware\",m.fwVersion?(\"v\"+m.fwVersion):(legacy?\"v7 or earlier\":\"unknown\"))+mrow(\"Last Char\",m.flapChar?m.flapChar:\"-\")+mrow(\"Last Seen\",m.lastSeenEpoch?new Date(m.lastSeenEpoch*1000).toLocaleString():(m.lastSeen?\"seen (clock not set)\":\"-\"));}else{h+=mrow(\"Module ID\",\"(not in registry)\");}h+=\"<div class='sgh' style='margin-top:12px'>EEPROM</div>\";if(!d||!d.ok){h+=\"<p style='color:var(--hi)'>\"+((d&&d.error)?d.error:\"No EEPROM data\")+\"</p>\";return h;}var p=parseDump(d.dump||\"\");if(p.error){h+=\"<p style='color:var(--hi)'>\"+p.error+\"</p>\";return h;}h+=mrow(\"Home Offset\",p.homeOffset+\" steps\")+mrow(\"Steps / Rev\",p.totalSteps);var keys=Object.keys(p.map);h+=mrow(\"Calibrated Flaps\",keys.length+\" / 64\");if(keys.length){h+=\"<div class='mmap'>\";keys.forEach(function(k){h+=\"[\"+k+\"]=\"+p.map[k]+\" \";});h+=\"</div>\";}h+=mrow(\"Raw\",\"<span style='word-break:break-all;color:var(--dim);font-size:.72rem'>\"+d.dump+\"</span>\");return h;}");
  server.sendContent("function openDel(id){_delId=id;document.getElementById(\"delModalTitle\").textContent=\"Module #\"+id+\" -- Destructive Actions\";document.getElementById(\"delModalStatus\").textContent=\"\";document.getElementById(\"delModal\").style.display=\"flex\";}function closeDelModal(){document.getElementById(\"delModal\").style.display=\"none\";_delId=-1;}function delAction(kind){if(_delId<0)return;var names={erase:\"Erase EEPROM\",factoryreset:\"Factory Reset\",deprovision:\"De-provision\"};if(!confirm(names[kind]+\" on module #\"+_delId+\"? This cannot be undone.\"))return;var st=document.getElementById(\"delModalStatus\");st.textContent=names[kind]+\" in progress...\";apiFlapCmd(\"/api/flap/\"+kind,{id:_delId}).then(function(j){st.textContent=j.ok?(names[kind]+\" sent.\"):(\"Error: \"+(j.error||\"failed\"));if(j.ok){setTimeout(function(){closeDelModal();loadModules();},900);}}).catch(function(e){st.textContent=\"Error: \"+e;});}");
  server.sendContent("function sendChar(){var id=parseInt(document.getElementById(\"scId\").value);var ch=document.getElementById(\"scChar\").value.toUpperCase().slice(0,1);var r=document.getElementById(\"scr\");if(!ch){r.textContent=\"Enter a character\";return;}apiFlapCmd(\"/api/flap/char\",{id:id,\"char\":ch}).then(function(j){r.textContent=j.ok?(\"Sent '\"+ch+\"' to \"+(id<0?\"all modules\":\"module #\"+id)):(\"Error: \"+(j.error||\"failed\"));}).catch(function(e){r.textContent=\"Error: \"+e;});}");
  server.sendContent("function sendText(){var text=document.getElementById(\"dispText\").value.toUpperCase();var start=parseInt(document.getElementById(\"dispStart\").value);apiFlapCmd(\"/api/flap/text\",{text:text,start:start}).then(function(j){document.getElementById(\"dr\").textContent=j.ok?\"Sent \"+j.chars+\" chars\":\"Error: \"+j.error;});}");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("function sendIndex(){var id=parseInt(document.getElementById(\"idxId\").value);var idx=parseInt(document.getElementById(\"idxVal\").value);apiFlapCmd(\"/api/flap/index\",{id:id,index:idx}).then(function(j){document.getElementById(\"dr\").textContent=j.ok?\"Sent\":\"Error: \"+j.error;});}");
  server.sendContent("function doHomeSN(sn){document.getElementById(\"provSN\").value=sn;apiFlapCmd(\"/api/flap/homebysn\",{sn:sn}).then(function(j){var el=document.getElementById(\"provR\");if(el)el.textContent=j.ok?\"Homing SN: \"+sn+\" - check which module moves\":\"Error: \"+j.error;});}");
  server.sendContent("function doProvision(){var sn=document.getElementById(\"provSN\").value;var id=parseInt(document.getElementById(\"provId\").value);apiFlapCmd(\"/api/flap/provision\",{sn:sn,id:id}).then(function(j){document.getElementById(\"provR\").textContent=j.ok?\"Provisioning sent\":\"Error: \"+j.error;});}");
  server.sendContent("function doDeprovision(){var id=parseInt(document.getElementById(\"deprovId\").value);apiFlapCmd(\"/api/flap/deprovision\",{id:id}).then(function(j){document.getElementById(\"deprovR\").textContent=j.ok?(\"De-provisioned \"+(id<0?\"all modules\":\"module \"+id)):\"Error: \"+j.error;});}");
  server.sendContent("function saveWifi(){fetch(\"/api/config/wifi\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({ssid:document.getElementById(\"wSSID\").value,pass:document.getElementById(\"wPASS\").value})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"wr\").textContent=j.ok?\"WiFi saved - reconnecting...\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"wr\").textContent=\"Error: \"+e;});}");
  server.sendContent("function testMqtt(){var el=document.getElementById(\"mr\");el.textContent=\"Testing...\";fetch(\"/api/mqtt/test\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({host:document.getElementById(\"mqH\").value,port:parseInt(document.getElementById(\"mqP\").value)||1883,user:document.getElementById(\"mqU\").value,pass:document.getElementById(\"mqPw\").value})}).then(function(r){return r.json();}).then(function(j){el.textContent=j.ok?\"Broker OK - connected and authenticated\":\"Failed: \"+(j.detail||j.error||\"unknown\");}).catch(function(e){el.textContent=\"Error: \"+e;});}");
  server.sendContent("function saveMqtt(){fetch(\"/api/config/mqtt\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({host:document.getElementById(\"mqH\").value,port:parseInt(document.getElementById(\"mqP\").value),user:document.getElementById(\"mqU\").value,pass:document.getElementById(\"mqPw\").value,prefix:document.getElementById(\"mqPfx\").value})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"mr\").textContent=j.ok?\"MQTT saved - reconnecting...\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"mr\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveTz(){var sel=document.getElementById(\"tzSel\");var ntp=document.getElementById(\"ntpSrv\").value.trim();fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({posixTZ:sel.value,ntpServer:ntp})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"tzR\").textContent=j.ok?\"Time settings saved\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"tzR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveGrid(){var rows=parseInt(document.getElementById(\"gRows\").value)||1;var cols=parseInt(document.getElementById(\"gCols\").value)||1;fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({gridRows:rows,gridCols:cols})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"gridR\").textContent=j.ok?\"Layout saved\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"gridR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveOTA(){fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({otaPassword:document.getElementById(\"otaPw\").value})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"otaR\").textContent=j.ok?\"OTA password saved\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"otaR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveDebug(){var v=document.getElementById(\"dbgChk\").checked;fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({serialDebug:v})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"dbgR\").textContent=j.ok?(v?\"Debug enabled\":\"Debug disabled\"):\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"dbgR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function setMaintUI(on){var chk=document.getElementById(\"maintChk\");if(chk)chk.checked=!!on;var lbl=document.querySelector(\".maint-toggle\");if(lbl)lbl.classList.toggle(\"active\",!!on);document.body.classList.toggle(\"maint-on\",!!on);}function toggleMaint(){var chk=document.getElementById(\"maintChk\");var on=chk.checked;fetch(\"/api/maintenance\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({on:on})}).then(function(r){return r.json();}).then(function(j){setMaintUI(j.on);}).catch(function(){chk.checked=!on;setMaintUI(!on);});}");
  server.sendContent("function pollStatus(){fetch(\"/api/status\").then(function(r){return r.json();}).then(function(s){var up=s.uptime,ud=Math.floor(up/86400),uh=Math.floor(up%86400/3600),um=Math.floor(up%3600/60),us=up%60;document.getElementById(\"s-up\").textContent=(ud?ud+\"d \":\"\")+(uh||ud?uh+\"h \":\"\")+um+\"m \"+us+\"s\";document.getElementById(\"s-rx\").textContent=s.rx;document.getElementById(\"s-tx\").textContent=s.tx;document.getElementById(\"s-ip\").textContent=s.ip;document.getElementById(\"s-ap\").textContent=s.apip;var hp=document.getElementById(\"s-hp\");hp.textContent=Math.round(s.heap/1024)+\" KB\";hp.className=\"v \"+(s.heap>=40000?\"vok\":(s.heap>=25000?\"vwarn\":\"vbad\"));var mq=document.getElementById(\"s-mq\");mq.textContent=s.mqtt?\"Connected\":\"Off\";mq.className=\"v \"+(s.mqtt?\"vok\":\"\");document.getElementById(\"s-mod\").textContent=s.modules;if(s.minheap){var mh=document.getElementById(\"s-mh\");mh.textContent=Math.round(s.minheap/1024)+\" KB\";mh.className=\"v \"+(s.minheap>=40000?\"vok\":(s.minheap>=25000?\"vwarn\":\"vbad\"));}if(s.stk){var sm=null,sn=\"\";for(var k in s.stk){if(sm===null||s.stk[k]<sm){sm=s.stk[k];sn=k;}}var st=document.getElementById(\"s-stk\");st.textContent=sm+\" B (\"+sn+\")\";st.className=\"v \"+(sm>=800?\"vok\":(sm>=400?\"vwarn\":\"vbad\"));}if(document.getElementById(\"s-rtc\"))document.getElementById(\"s-rtc\").textContent=s.time||\"--\";var ntp=document.getElementById(\"s-ntp\");if(ntp){ntp.textContent=s.ntpSynced?\"Synced\":\"Pending\";ntp.className=\"v \"+(s.ntpSynced?\"vok\":\"vwarn\");}var b=document.getElementById(\"badge\");b.textContent=s.wifi?\"WiFi: \"+s.ip:\"AP only\";b.className=s.wifi?\"ok\":\"\";if(typeof s.maint!==\"undefined\")setMaintUI(s.maint);}).catch(function(){});}setInterval(pollStatus,3000);pollStatus();");
  server.sendContent("fetch(\"/api/config\").then(function(r){return r.json();}).then(function(c){document.getElementById(\"wSSID\").value=c.wSSID||\"\";document.getElementById(\"mqH\").value=c.mqHost||\"\";document.getElementById(\"mqP\").value=c.mqPort||1883;document.getElementById(\"mqU\").value=c.mqUser||\"\";document.getElementById(\"mqPfx\").value=c.mqPfx||\"splitflap\";if(c.posixTZ){var sel=document.getElementById(\"tzSel\");for(var j=0;j<sel.options.length;j++){if(sel.options[j].value===c.posixTZ){sel.selectedIndex=j;break;}}}if(c.ntpServer){document.getElementById(\"ntpSrv\").value=c.ntpServer;}if(c.gridRows){document.getElementById(\"gRows\").value=c.gridRows;}if(c.gridCols){document.getElementById(\"gCols\").value=c.gridCols;}});");
  server.sendContent("var CAL_CHARS=\" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw\";var CAL_COLORS={r:\"#e23b3b\",o:\"#ff9f0a\",y:\"#ffd60a\",g:\"#2fb84a\",b:\"#3b82f6\",p:\"#a855f7\",w:\"#e8e8e8\"};");
  server.sendContent("var _calId=-1,_calTotal=4096,_calHo=0,_calMap={},_tuneIdx=-1;var CAL_UNSET=65535,CAL_DEF_HO=2832,CAL_DEF_TS=4096;");
  server.sendContent("function calDefault(i){return i*64;}");
  server.sendContent("function calParseDump(raw){var r={homeOffset:0,totalSteps:4096,map:{}};if(!raw)return r;var parts=raw.split(\":\");r.homeOffset=parseInt(parts[0]);r.totalSteps=parseInt(parts[1]);if(isNaN(r.totalSteps))r.totalSteps=4096;if(parts.length>2){var rest=parts.slice(2).join(\":\");rest.split(\",\").forEach(function(e){var kv=e.split(\"=\");if(kv.length===2&&kv[0]!==\"\"){var k=parseInt(kv[0]),v=parseInt(kv[1]);if(!isNaN(k)&&!isNaN(v)&&v!==CAL_UNSET)r.map[k]=v;}});}return r;}");
  server.sendContent("function calLoadModules(){var el=document.getElementById(\"calMods\");el.textContent=\"Loading layout...\";Promise.all([fetch(\"/api/display/state\").then(function(r){return r.json();}),fetch(\"/api/flap/modules\").then(function(r){return r.json();})]).then(function(res){var st=res[0]||{},arr=res[1]||[];var rows=st.rows||1,cols=st.cols||16;var count=rows*cols;el.classList.remove(\"single\");el.style.gridTemplateColumns=\"repeat(\"+cols+\",minmax(0,1fr))\";var byId={};arr.forEach(function(m){if(m.provisioned)byId[m.id]=m;});var h=\"\";for(var id=0;id<count;id++){var m=byId[id];var legacy=m&&m.lastSeen>0&&!m.fwVersion;var known=m&&m.fwVersion;var cls=\"cmod\";if(id===_calId)cls+=\" sel\";if(legacy)cls+=\" legacy\";else if(known)cls+=\" known\";else if(!m)cls+=\" unknown\";var sub=legacy?\"<span class='csn lg'>v7</span>\":(m&&m.sn?\"<span class='csn'>\"+m.sn.slice(-4)+\"</span>\":\"<span class='csn'>--</span>\");h+=\"<div class='\"+cls+\"' data-id='\"+id+\"' onclick='calSelect(\"+id+\")'>\"+id+sub+\"</div>\";}el.innerHTML=h;}).catch(function(){el.innerHTML=\"<span style='color:var(--hi)'>Error loading layout</span>\";});}");
  server.sendContent("function calSelectAny(){var v=parseInt(document.getElementById(\"calAnyId\").value);if(isNaN(v)||v<0||v>254){alert(\"Enter an ID from 0 to 254.\");return;}calSelect(v);}");
  server.sendContent("function calSelect(id){_calId=id;document.querySelectorAll(\"#calMods .cmod\").forEach(function(el){el.classList.toggle(\"sel\",parseInt(el.dataset.id)===id);});var cell=document.querySelector(\"#calMods .cmod[data-id='\"+id+\"']\");var tag=\"\";if(cell){if(cell.classList.contains(\"legacy\"))tag=\" (legacy v7)\";else if(cell.classList.contains(\"unknown\"))tag=\" (not yet seen)\";}document.getElementById(\"calDetail\").style.display=\"block\";document.getElementById(\"calTitle\").textContent=\"Module \"+id+tag;calRefresh();}");
  server.sendContent("function calRefresh(){if(_calId<0)return;var st=document.getElementById(\"calStatus\");st.textContent=\"Reading EEPROM from module \"+_calId+\"...\";document.getElementById(\"calHoIn\").value=\"\";document.getElementById(\"calTsIn\").value=\"\";document.getElementById(\"calMap\").innerHTML=\"\";fetch(\"/api/flap/dump\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:_calId})}).then(function(r){return r.json();}).then(function(d){if(!d.ok){st.textContent=\"Error: \"+(d.error||\"no response from module\");return;}var p=calParseDump(d.dump||\"\");_calHo=p.homeOffset;_calTotal=p.totalSteps;_calMap=p.map;document.getElementById(\"calHoIn\").value=isNaN(p.homeOffset)?\"\":p.homeOffset;document.getElementById(\"calTsIn\").value=p.totalSteps;st.textContent=d.stale?\"Cached EEPROM -- click Re-read for a fresh value\":\"EEPROM read fresh from module\";calRenderMap();}).catch(function(e){st.textContent=\"Error: \"+e;});}");
  server.sendContent("function calSwatch(ch){var c=CAL_COLORS[ch];return c?\"<span class='sw' style='background:\"+c+\"'></span>\":ch;}");
  server.sendContent("function calRenderMap(){var el=document.getElementById(\"calMap\");var h=\"\";for(var i=0;i<CAL_CHARS.length;i++){var ch=CAL_CHARS[i];var has=_calMap.hasOwnProperty(i);var val=has?_calMap[i]:calDefault(i);var disp=(ch===\" \")?\"&#9632;\":(CAL_COLORS[ch]?calSwatch(ch):ch);h+=\"<div class='cc\"+(has?\" custom\":\"\")+\"' onclick='calOpenTune(\"+i+\")'><div class='cch'>\"+disp+\"</div><div class='ccv'>\"+val+\"</div></div>\";}el.innerHTML=h;}");
  server.sendContent("function calApi(path,body,cb){fetch(path,{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify(body)}).then(function(r){return r.json();}).then(cb).catch(function(e){cb({ok:false,error:String(e)});});}");
  server.sendContent("function calSaveHo(){var v=parseInt(document.getElementById(\"calHoIn\").value);if(isNaN(v)||v<0){document.getElementById(\"calStatus\").textContent=\"Enter a valid home offset.\";return;}calApi(\"/api/flap/homeoffset\",{id:_calId,steps:v},function(j){document.getElementById(\"calStatus\").textContent=j.ok?\"Home offset saved (\"+v+\"). Click Home Motor to verify.\":\"Error: \"+j.error;if(j.ok)_calHo=v;});}");
  server.sendContent("function calRevertHo(){document.getElementById(\"calHoIn\").value=CAL_DEF_HO;calApi(\"/api/flap/homeoffset\",{id:_calId,steps:CAL_DEF_HO},function(j){document.getElementById(\"calStatus\").textContent=j.ok?\"Home offset reset to default (\"+CAL_DEF_HO+\").\":\"Error: \"+j.error;if(j.ok)_calHo=CAL_DEF_HO;});}");
  server.sendContent("function calSaveTs(){var v=parseInt(document.getElementById(\"calTsIn\").value);if(isNaN(v)||v<1){document.getElementById(\"calStatus\").textContent=\"Enter a valid total steps.\";return;}calApi(\"/api/flap/totalsteps\",{id:_calId,steps:v},function(j){document.getElementById(\"calStatus\").textContent=j.ok?\"Total steps saved (\"+v+\").\":\"Error: \"+j.error;if(j.ok){_calTotal=v;calRenderMap();}});}");
  server.sendContent("function calRevertTs(){document.getElementById(\"calTsIn\").value=CAL_DEF_TS;calApi(\"/api/flap/totalsteps\",{id:_calId,steps:CAL_DEF_TS},function(j){document.getElementById(\"calStatus\").textContent=j.ok?\"Total steps reset to default (\"+CAL_DEF_TS+\").\":\"Error: \"+j.error;if(j.ok){_calTotal=CAL_DEF_TS;calRenderMap();}});}");
  server.sendContent("function calCountSteps(){if(_calId<0)return;var btn=document.getElementById(\"calCountBtn\");var st=document.getElementById(\"calStatus\");if(!confirm(\"Count steps on module \"+_calId+\"? The reel will spin a full revolution to measure its steps per revolution.\"))return;btn.disabled=true;btn.textContent=\"Counting...\";st.textContent=\"Calibrating module \"+_calId+\" -- the reel is spinning, please wait (up to ~15s)...\";calApi(\"/api/flap/calibrate\",{id:_calId},function(j){if(j.ok&&typeof j.stepsPerRev===\"number\"){var n=j.stepsPerRev;document.getElementById(\"calTsIn\").value=n;_calTotal=n;calRenderMap();st.textContent=\"Measured \"+n+\" steps/rev. Saving to module...\";calApi(\"/api/flap/totalsteps\",{id:_calId,steps:n},function(j2){st.textContent=j2.ok?(\"Total steps measured and saved: \"+n+\".\"):(\"Measured \"+n+\" but save failed: \"+j2.error);btn.disabled=false;btn.textContent=\"Count Steps\";});}else{st.textContent=\"Error: \"+(j.error||\"no calibration response\");btn.disabled=false;btn.textContent=\"Count Steps\";}});}");
  server.sendContent("function calNudge(d){if(_calId<0)return;calApi(\"/api/flap/nudge\",{id:_calId,steps:d},function(j){if(j.ok){_calHo+=d;document.getElementById(\"calHoIn\").value=_calHo;document.getElementById(\"calStatus\").textContent=\"Nudged \"+(d>0?\"+\":\"\")+d+\" (offset now \"+_calHo+\"). Saved instantly.\";}else{document.getElementById(\"calStatus\").textContent=\"Error: \"+j.error;}});}");
  server.sendContent("function calHomeMotor(){if(_calId<0)return;calApi(\"/api/flap/home\",{id:_calId},function(j){document.getElementById(\"calStatus\").textContent=j.ok?\"Homing module \"+_calId+\"...\":\"Error: \"+j.error;});}");
  server.sendContent("function calOpenTune(i){_tuneIdx=i;var ch=CAL_CHARS[i];var has=_calMap.hasOwnProperty(i);var cur=has?_calMap[i]:calDefault(i);var label=(ch===\" \")?\"(blank)\":(CAL_COLORS[ch]?ch.toUpperCase()+\" (color)\":ch);document.getElementById(\"tuneTitle\").textContent=\"Tune: \"+label;document.getElementById(\"tuneExp\").textContent=\"Default: \"+calDefault(i)+(has?\"  |  Current EEPROM: \"+cur:\"  (using default)\");document.getElementById(\"tuneVal\").value=cur;document.getElementById(\"tuneStatus\").textContent=\"\";document.getElementById(\"tuneModal\").style.display=\"flex\";}");
  server.sendContent("function calCloseTune(){document.getElementById(\"tuneModal\").style.display=\"none\";_tuneIdx=-1;}");
  server.sendContent("function calTuneNudge(d){var el=document.getElementById(\"tuneVal\");var v=(parseInt(el.value)||0)+d;if(v<0)v=0;el.value=v;}");
  server.sendContent("function calTuneGoto(){if(_tuneIdx<0)return;var v=parseInt(document.getElementById(\"tuneVal\").value);if(isNaN(v)||v<0){document.getElementById(\"tuneStatus\").textContent=\"Enter a valid step.\";return;}calApi(\"/api/flap/goto\",{id:_calId,step:v},function(j){document.getElementById(\"tuneStatus\").textContent=j.ok?\"Moving to step \"+v+\"... watch the reel.\":\"Error: \"+j.error;});}");
  server.sendContent("function calTuneLock(){if(_tuneIdx<0)return;var v=parseInt(document.getElementById(\"tuneVal\").value);if(isNaN(v)||v<0){document.getElementById(\"tuneStatus\").textContent=\"Enter a valid step.\";return;}calApi(\"/api/flap/writepos\",{id:_calId,idx:_tuneIdx,pos:v},function(j){if(j.ok){_calMap[_tuneIdx]=v;document.getElementById(\"tuneStatus\").textContent=\"Locked to EEPROM at step \"+v+\".\";calRenderMap();setTimeout(calCloseTune,700);}else{document.getElementById(\"tuneStatus\").textContent=\"Error: \"+j.error;}});}");
  server.sendContent("function calTuneRevert(){if(_tuneIdx<0)return;var def=calDefault(_tuneIdx);calApi(\"/api/flap/writepos\",{id:_calId,idx:_tuneIdx,pos:CAL_UNSET},function(j){if(j.ok){delete _calMap[_tuneIdx];document.getElementById(\"tuneVal\").value=def;document.getElementById(\"tuneStatus\").textContent=\"Unset in EEPROM -- now uses default (\"+def+\").\";calRenderMap();setTimeout(calCloseTune,800);}else{document.getElementById(\"tuneStatus\").textContent=\"Error: \"+j.error;}});}");
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
  uint8_t outBuf[TX_MAX_BYTES];
  size_t  outLen = min(strlen(d), (size_t)TX_MAX_BYTES);
  memcpy(outBuf, d, outLen);
  if (!outLen) { sendJsonError(400, "Empty data"); return; }
  rs485Send(outBuf, outLen);
  char resp[48];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%zu}", outLen);
  server.send(200, "application/json", resp);
}

// GET /api/flap/modules
void handleApiModules() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  String out;
  out.reserve(sfModuleCount * 220);
  out = "[";
  for (int i = 0; i < sfModuleCount; i++) {
    const SFModule& m = sfModules[i];
    if (i) out += ',';
    out += "{\"id\":";      out += (int)m.id;
    out += ",\"sn\":\"";    out += m.serialNum;  out += '"';
    out += ",\"provisioned\":"; out += m.provisioned ? "true" : "false";
    out += ",\"flapIndex\":"; out += m.flapIndex;
    out += ",\"flapChar\":\"";
    if (m.flapChar >= 32 && m.flapChar <= 126 && m.flapChar != '"') out += m.flapChar;
    out += '"';
    out += ",\"fwVersion\":\""; out += m.fwVersion; out += '"';
    out += ",\"lastSeen\":"; out += m.lastSeen;
    out += ",\"lastSeenEpoch\":"; out += m.lastSeenEpoch;
    out += '}';
  }
  out += ']';
  xSemaphoreGive(sfMutex);
  server.send(200, "application/json", out);
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

  String out;
  out.reserve(cells * 6 + 64);
  out = "{\"rows\":"; out += rows;
  out += ",\"cols\":"; out += cols;
  out += ",\"cells\":[";
  for (int i = 0; i < cells; i++) {
    if (i) out += ',';
    char c = cellChar[i];
    if (c == 0)            out += "null";              // no module at this id
    else if (c == 1)       out += "\"?\"";             // module present, char unknown
    else if (c == '"' || c == '\\') { out += '"'; out += '\\'; out += c; out += '"'; }
    else { out += '"'; out += c; out += '"'; }
  }
  out += "]}";
  server.send(200, "application/json", out);
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
// Sends m<id>c, then waits for the module's m<id>:<steps> reply (the reel
// physically measures a full revolution, which takes several seconds). On
// success returns {ok,id,stepsPerRev}; the module saves the measured value to
// its own EEPROM as part of calibration. Broadcast (id<0) is fire-and-forget.
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

  // Arm the capture slot for this id, then send the calibrate command.
  sfCalibSteps     = 0;
  sfCalibCaptureTs = 0;
  sfCalibWaitId    = id;
  sfCalibrate(id);

  // The reel turns a full revolution plus a homing pass; the sample log shows
  // ~6.5s end to end, so a 15s window gives generous margin for slower reels.
  // The wait loop touches wdgWebMs each iteration, so it is watchdog-safe.
  int  steps    = 0;
  bool gotReply = false;
  unsigned long deadline = millis() + 15000;
  while (millis() < deadline) {
    wdgWebMs = millis();
    vTaskDelay(pdMS_TO_TICKS(20));
    if (sfCalibCaptureTs != 0) {
      steps    = sfCalibSteps;
      gotReply = true;
      break;
    }
  }
  sfCalibWaitId = -1;  // disarm capture

  if (!gotReply) {
    sendJsonError(504, "No calibration response from module");
    return;
  }
  char out[64];
  snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%d,\"stepsPerRev\":%d}", id, steps);
  server.send(200, "application/json", out);
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

  // Record the lastSeen timestamp before sending so we can detect a fresh reply
  unsigned long seenBefore = 0;
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* m = sfFindById((uint8_t)id);
  if (m) seenBefore = m->lastSeen;
  xSemaphoreGive(sfMutex);

  // Send the version query onto the bus
  sfQueryVersion(id);

  // Wait for a fresh version response (lastSeen advances when sfParseResponse
  // processes the reply and writes fwVersion). v18+ modules STAGGER their
  // version responses by ~100ms per ID slot (observed train: id5 @ +100ms,
  // id9 @ +200ms, id10 @ +300ms after the first), so a fixed 500ms window can
  // never catch high-ID modules even when they are healthy. Scale the window
  // with the queried ID, capped so the handler stays bounded.
  char          fwVer[8]     = "";
  char          sn[21]       = "";
  int           repId        = -1;
  bool          gotReply     = false;
  unsigned long repLastSeen  = 0;
  unsigned long waitMs   = 500UL + (unsigned long)(id > 25 ? 25 : id) * 100UL;
  unsigned long deadline = millis() + waitMs;
  while (millis() < deadline) {
    wdgWebMs = millis();  // keep watchdog alive during version wait
    vTaskDelay(pdMS_TO_TICKS(10));
    xSemaphoreTake(sfMutex, portMAX_DELAY);
    SFModule* mx = sfFindById((uint8_t)id);
    if (mx && mx->lastSeen != seenBefore && mx->fwVersion[0]) {
      strlcpy(fwVer, mx->fwVersion,  sizeof(fwVer));
      strlcpy(sn,    mx->serialNum,  sizeof(sn));
      repId        = mx->id;
      repLastSeen  = mx->lastSeen;
      gotReply     = true;
    }
    xSemaphoreGive(sfMutex);
    if (gotReply) break;
  }

  if (gotReply) {
    char out[128];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"id\":%d,\"ver\":\"%s\",\"sn\":\"%s\",\"stale\":false,\"lastSeen\":%lu}",
             repId, fwVer, sn, repLastSeen);
    DBG("[API] version response: id=%d ver=%s sn=%s\n", repId, fwVer, sn);
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
    "\"time\":\"%s\",\"ntpSynced\":%s,\"maint\":%s}",
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
    gMaintenanceMode?"true":"false");
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
    "'<span style=\"color:rgb(76,175,80)\">Upload successful! Rebooting...</span>';"  
    "}else{"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(233,69,96)\">Error: '+xhr.responseText+'</span>';}"
    "};"
    "xhr.onerror=function(){"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(233,69,96)\">Upload failed.</span>';"  
    "};"
    "xhr.open('POST','/api/ota/upload');"
    "xhr.setRequestHeader('X-OTA-Password',document.getElementById('fw').name);"
    "xhr.send(f);"
    "}"
    "</script></body></html>";
  server.send(200, "text/html", html);
}

void handleOTAUpload() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  // Optional password check
  if (strlen(cfg.otaPassword) > 0) {
    if (!server.hasHeader("X-OTA-Password") ||
        strcmp(server.header("X-OTA-Password").c_str(), cfg.otaPassword) != 0) {
      // Password check is advisory from web UI; ArduinoOTA handles its own auth
    }
  }

  if (!server.hasArg("plain") && server.method() != HTTP_POST) {
    server.send(400, "text/plain", "POST firmware binary");
    return;
  }

  // The firmware binary arrives as the raw POST body
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    printf("[OTA] Web upload start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      printf("[OTA] Begin failed\n");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      printf("[OTA] Write error\n");
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      printf("[OTA] Web upload complete (%u bytes) -- rebooting\n", upload.totalSize);
      server.send(200, "text/plain", "OK");
      delay(500);
      ESP.restart();
    } else {
      printf("[OTA] Update.end failed\n");
      server.send(500, "text/plain", "Update failed");
    }
  }
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
  // Build mXW command -- map can be large; use String to avoid buffer overflow
  String cmd = String("mXW") + sn + ":" + homeOffset + ":" + totalSteps
               + ":" + map + "\n";
  DBG("[API] restore by SN %s\n", sn);
  rs485SendStr(cmd.c_str());
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

  // Arm the shared dump capture slot for this id, then send the request.
  sfDumpCapture[0] = 0;
  sfDumpCaptureTs  = 0;
  sfDumpWaitId     = id;

  // Prefer the serial-number dump command mXD<sn> for fw >= 15 with a known SN.
  // A full 64-flap dump is ~565 bytes -- ~590ms just to transmit at 9600 baud,
  // plus the module's EEPROM-read time -- so the wait MUST be well over 500ms.
  // The previous 500ms window expired mid-response and triggered the m<id>d
  // fallback, whose request collided on the half-duplex bus with the late mXD
  // response (the cause of the dropped bytes / no-response). A 1200ms window
  // lets mXD fully respond, so no premature fallback and no collision.
  // The wait loop touches wdgWebMs each iteration, so a longer wait is
  // watchdog-safe.
  bool triedSN = false;
  if (fwVerNum >= 15 && sn[0]) {
    DBG("[API] dump module %d via SN %s (fw=%d)\n", id, sn, fwVerNum);
    sfDumpBySN(sn);
    triedSN = true;
  } else {
    DBG("[API] dump module %d via ID (fw=%d)\n", id, fwVerNum);
    char buf[16]; snprintf(buf, sizeof(buf), "m%dd\n", id); rs485SendStr(buf);
  }

  char rawDump[TX_MAX_BYTES] = "";
  bool gotReply = false;
  unsigned long deadline = millis() + 1200;
  while (millis() < deadline) {
    wdgWebMs = millis();
    vTaskDelay(pdMS_TO_TICKS(10));
    if (sfDumpCaptureTs != 0) {
      strlcpy(rawDump, sfDumpCapture, sizeof(rawDump));
      gotReply = true;
      break;
    }
  }

  // Only if the SN command got NO response within the full window do we fall
  // back to m<id>d. After a complete 1200ms with silence, the module clearly
  // isn't answering mXD, so there is no late response left to collide with.
  if (!gotReply && triedSN) {
    DBG("[API] SN dump timed out, retrying module %d via ID\n", id);
    char buf[16]; snprintf(buf, sizeof(buf), "m%dd\n", id); rs485SendStr(buf);
    deadline = millis() + 1200;
    while (millis() < deadline) {
      wdgWebMs = millis();
      vTaskDelay(pdMS_TO_TICKS(10));
      if (sfDumpCaptureTs != 0) {
        strlcpy(rawDump, sfDumpCapture, sizeof(rawDump));
        gotReply = true;
        break;
      }
    }
  }

  sfDumpWaitId = -1;  // disarm capture

  if (gotReply) {
    // Build JSON reply (escape the dump string for JSON safety)
    String reply = "{\"ok\":true,\"id\":";
    reply += id;
    reply += ",\"sn\":\""; reply += sn; reply += "\",";
    reply += "\"dump\":";
    reply += '"';
    for (const char* p2 = rawDump; *p2; p2++) {
      if (*p2 == '"' || *p2 == '\\') reply += '\\';
      reply += *p2;
    }
    reply += "\",\"stale\":false}";
    server.send(200, "application/json", reply);
    reply = "";
  } else {
    server.send(200, "application/json",
      "{\"ok\":false,\"error\":\"no response from module\"}");
  }
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
  }
  char out[40];
  snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s}",
           gMaintenanceMode ? "true" : "false");
  server.send(200, "application/json", out);
}

void webInit() {
  server.on("/",                     HTTP_GET,     handleRoot);
  server.on("/ota",                  HTTP_GET,     handleOTAPage);
  server.on("/api/ota/upload",       HTTP_POST,    [](){}, handleOTAUpload);
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

void taskNetwork(void* pv) {
  // WiFi init done in setup() - this task only polls and reconnects
  while (true) {
    bool staUp = (WiFi.status() == WL_CONNECTED);
    if (staUp && !staWasUp) {
      staWasUp = true;
      if (!ntpSynced) ntpSynced = rtcNTPSync();
      { IPAddress _a = WiFi.localIP();
  printf("[WiFi] Connected IP=%d.%d.%d.%d\n", _a[0],_a[1],_a[2],_a[3]); }
    } else if (!staUp && staWasUp) {
      staWasUp = false;
      printf("[WiFi] Disconnected\n");
    }
    if (!staUp && strlen(cfg.wifiSSID) && millis() - wifiRetryMs > 15000UL) {
      wifiRetryMs = millis();
      WiFi.disconnect();
      WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    }
    if (staUp && strlen(cfg.mqttHost)) {
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
        if (mqttQMutex && xSemaphoreTake(mqttQMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          while (mqttQTail != mqttQHead) {
            MqttQItem& item = mqttQueue[mqttQTail];
            mqtt.publish(item.topic, (uint8_t*)item.payload, item.len, false);
            mqttQTail = (mqttQTail + 1) % MQTT_Q_SIZE;
          }
          xSemaphoreGive(mqttQMutex);
        }
      }
    }
    if (millis() - lastStatusMs > STATUS_INTERVAL_MS) {
      lastStatusMs = millis();
      mqttPublishStatus();
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
  memset(sfModules, 0, sizeof(sfModules));
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

  // 5. WiFi - MUST be initialised here on the main Arduino task
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
  { IPAddress _b = WiFi.softAPIP();
    printf("[WiFi] AP: %s  %s  %d.%d.%d.%d\n",
           DEFAULT_AP_SSID, DEFAULT_AP_PASS, _b[0],_b[1],_b[2],_b[3]); }
  if (strlen(cfg.wifiSSID)) {
    WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    printf("[WiFi] STA connecting to %s...\n", cfg.wifiSSID);
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
           "rx=%lu tx=%lu rej=%lu wifi=%d rssi=%d mqtt=%d mods=%d\n",
           now/1000, freeHeap, minHeap, maxBlk,
           freeHeap ? (unsigned)(100 - (maxBlk * 100UL / freeHeap)) : 0,
           s485, sWeb, sNet, sOta, sRtc,
           rxCount, txCount, sfParseRejects,
           (int)(WiFi.status()==WL_CONNECTED),
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
