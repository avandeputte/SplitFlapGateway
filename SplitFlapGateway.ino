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

// Early-declared debug flag so DBG() works before cfg is constructed.
// Kept in sync with cfg.serialDebug in loadConfig() and handleApiConfigSettings().
static volatile bool gSerialDebug = false;
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
#define NTP_SERVER        "pool.ntp.org"
#define NTP_TIMEOUT_MS    8000UL

struct RtcTime {
  uint16_t year;
  uint8_t  month, day, hour, minute, second;
  bool     valid;
};
static volatile RtcTime rtcNow = {2000,1,1,0,0,0,false};
// POSIX TZ string -- declared here (before cfg) so rtcFormatTime can use it.
static char gPosixTZ[64] = "UTC0";
// Mutex protecting setenv/tzset/localtime (not thread-safe in newlib)
static SemaphoreHandle_t     timeMutex     = NULL;
static StaticSemaphore_t     timeMutexBuf;
// Watchdog timestamps -- each task writes millis() here every iteration
static volatile unsigned long wdgRS485Ms   = 0;
static int                    mqttFailCount = 0;  // consecutive MQTT connect failures
static volatile unsigned long wdgNetMs     = 0;
static volatile unsigned long wdgWebMs     = 0;
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
  DBG("[NTP] Syncing (UTC)...\n");
  configTime(0, 0, NTP_SERVER);  // always fetch UTC
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
#define MSG_RING_SIZE        64
#define MSG_MAX_BYTES        256
#define MQTT_BUF_SIZE        512
#define MQTT_Q_SIZE 32
struct MqttQItem { char topic[48]; char payload[MQTT_BUF_SIZE]; size_t len; };
static MqttQItem             mqttQueue[MQTT_Q_SIZE];
static volatile int          mqttQHead     = 0;
static volatile int          mqttQTail     = 0;
static SemaphoreHandle_t     mqttQMutex    = NULL;
static StaticSemaphore_t     mqttQMutexBuf;
#define STATUS_INTERVAL_MS   10000UL
#define MODULE_STALE_SECS    21600UL   // 6h: prune modules not seen in this long
#define MODULE_SAVE_DEBOUNCE_MS 5000UL // coalesce NVS writes

/* ----------------------------------------------------------
   Split-flap character set (must match module firmware)
---------------------------------------------------------- */
static const char FLAP_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw";
#define FLAP_CHAR_COUNT  64   // length of FLAP_CHARS

/* ----------------------------------------------------------
   Module registry  (tracks known modules on the bus)
---------------------------------------------------------- */
#define MAX_MODULES         200   // supports up to ~200 modules

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
static char                   sfDumpCapture[MSG_MAX_BYTES] = "";  // raw dump after 'd:'
static volatile unsigned long sfDumpCaptureTs = 0;   // millis() when captured (0=none)
static volatile bool          sfModulesDirty   = false;  // pending NVS save
static volatile unsigned long sfModulesDirtyMs = 0;      // millis() when first dirtied
static bool          ntpSynced   = false; // declared early; also set in taskNetwork

/* ----------------------------------------------------------
   Persistent configuration stored in NVS
---------------------------------------------------------- */
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
  bool          serialDebug;   // enable verbose serial output
  char          otaPassword[32]; // OTA update password (blank = no auth)
};

GwConfig cfg;
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
  char          wallTime[24];
};

// Explicit prototypes - must appear after struct, before function bodies,
// to prevent the Arduino IDE preprocessor inserting them above the struct.
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
  out.reserve(512);  // pre-allocate to reduce realloc churn
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
  rs485.begin(cfg.rs485Baud, buildSerialConfig(), RS485_RX_PIN, RS485_TX_PIN);
  rs485.setPins(-1, -1, -1, RS485_EN_PIN);
  rs485.setMode(UART_MODE_RS485_HALF_DUPLEX);
  DBG("[RS485] baud=%lu\n", cfg.rs485Baud);
}

void rs485Send(const uint8_t* data, size_t len) {
  if (!len || len > MSG_MAX_BYTES) return;
  rs485.write(data, len);
  rs485.flush();
  txCount++;
  // Log the transmitted frame (strip trailing newline for readability)
  { char dbg[MSG_MAX_BYTES]; size_t dlen = (len > 0 && data[len-1] == '\n') ? len-1 : len;
    memcpy(dbg, data, dlen); dbg[dlen] = '\0';
    DBG("[TX] %s\n", dbg); }
  RS485Msg m;
  m.timestamp = millis();
  m.dir = 'T';
  m.len = len;
  memcpy(m.data, data, len);
  rtcFormatTime(m.wallTime, sizeof(m.wallTime));
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

// Return index in FLAP_CHARS for a character, or -1
static int flapCharIndex(char c) {
  const char* p = strchr(FLAP_CHARS, c);
  return p ? (int)(p - FLAP_CHARS) : -1;
}

/* ----------------------------------------------------------
   Send split-flap commands
   All generate the ASCII bus protocol and call rs485SendStr()
---------------------------------------------------------- */

// Display a character on one module.  addr=-1 = broadcast.
void sfSendChar(int addr, char c) {
  char buf[24];
  if (addr < 0)
    snprintf(buf, sizeof(buf), "m*-%c\n", c);
  else
    snprintf(buf, sizeof(buf), "m%d-%c\n", addr, c);
  rs485SendStr(buf);
  // Update local state
  if (addr >= 0) {
    SFModule* m = sfFindById((uint8_t)addr);
    if (m) { m->flapChar = c; m->flapIndex = flapCharIndex(c); }
  }
}

// Display by flap index.  addr=-1 = broadcast.
void sfSendIndex(int addr, int idx) {
  char buf[24];
  if (addr < 0)
    snprintf(buf, sizeof(buf), "m*+%d\n", idx);
  else
    snprintf(buf, sizeof(buf), "m%d+%d\n", addr, idx);
  rs485SendStr(buf);
  if (addr >= 0) {
    SFModule* m = sfFindById((uint8_t)addr);
    if (m) {
      m->flapIndex = idx;
      m->flapChar  = (idx >= 0 && idx < FLAP_CHAR_COUNT) ? FLAP_CHARS[idx] : 0;
    }
  }
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
    char c = text[i];
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A'; // lowercase -> uppercase
    if (flapCharIndex(c) == -1) c = ' ';           // unsupported -> blank
    sfSendChar((int)(startAddr + i), c);
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

void sfParseResponse(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'm') return;

  // Convert to null-terminated string for easier parsing
  char buf[MSG_MAX_BYTES + 1];
  size_t copyLen = (len < MSG_MAX_BYTES) ? len : MSG_MAX_BYTES;
  memcpy(buf, data, copyLen);
  buf[copyLen] = 0;
  // Strip trailing \r\n
  for (int i = (int)copyLen - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r'); i--)
    buf[i] = 0;

  // -- Provisioning advertisement: mXadv:<serialNumber>
  if (strncmp(buf, "mXadv:", 6) == 0) {
    const char* sn = buf + 6;
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
    strlcpy(m->fwVersion, field[0] ? field[0] : "?", sizeof(m->fwVersion));
    if (field[2] && field[2][0]) {
      strlcpy(m->serialNum, field[2], sizeof(m->serialNum));
    }
    int reportedId = (field[1] && field[1][0]) ? atoi(field[1]) : -1;
    DBG("[SF] Module %d fw:%s reportedId:%d sn:%s\n",
                  id, m->fwVersion, reportedId, field[2] ? field[2] : "");
    char payload[96];
    snprintf(payload, sizeof(payload),
      "{\"id\":%d,\"ver\":\"%s\",\"reportedId\":%d,\"sn\":\"%s\"}",
      id, m->fwVersion, reportedId, field[2] ? field[2] : "");
    mqttPublishSFEvent("version", payload);
  }
  // Calibration result: m<id>:<steps>
  else if (cmd == ':') {
    int steps = atoi(p);
    DBG("[SF] Module %d calibrated: %d steps/rev\n", id, steps);
    char payload[48];
    snprintf(payload, sizeof(payload), "{\"id\":%d,\"stepsPerRev\":%d}", id, steps);
    mqttPublishSFEvent("calibrated", payload);
  }
  // EEPROM dump: m<id>d:<homeOffset>:<totalSteps>:<map>
  else if (cmd == 'd' && *p == ':') {
    DBG("[SF] Module %d dump: %s\n", id, p + 1);
    // Capture into the shared single-slot buffer IF a dump request is
    // waiting for this module id (set by handleApiDump). No per-module cache.
    char clean[MSG_MAX_BYTES];
    strlcpy(clean, p + 1, sizeof(clean));
    size_t dl = strlen(clean);
    while (dl > 0 && (clean[dl-1] == '\n' || clean[dl-1] == '\r')) clean[--dl] = 0;
    if (sfDumpWaitId == (int)id) {
      strlcpy(sfDumpCapture, clean, sizeof(sfDumpCapture));
      sfDumpCaptureTs = millis();
    }
    char payload[MSG_MAX_BYTES];
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
  // Build JSON with snprintf -- avoids JsonDocument heap allocation in hot path
  char buf[MQTT_BUF_SIZE];
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
  if (length >= MQTT_BUF_SIZE) return;
  char buf[MQTT_BUF_SIZE + 1];
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
    char plainBuf[MSG_MAX_BYTES + 1];
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
      uint8_t outBuf[MSG_MAX_BYTES];
      size_t  outLen = min(strlen(d), (size_t)MSG_MAX_BYTES);
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
    printf("[MQTT] Failed rc=%d\n", mqtt.state());
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
  server.sendContent(".stat{background:#0d1b2a;border-radius:6px;padding:8px;text-align:center}");
  server.sendContent(".stat .v{font-size:1.2rem;font-weight:bold;color:var(--hi)}.stat .k{font-size:.7rem;color:var(--dim)}");
  server.sendContent(".mod{background:#0d1b2a;border-radius:6px;padding:8px;border:1px solid var(--acc);font-size:.8rem}");
  server.sendContent(".mod .mid{font-size:1rem;font-weight:bold;color:var(--ylw)}.mod .mc{font-size:.75rem;color:var(--dim)}");
  server.sendContent(".mod{cursor:pointer;transition:border-color .15s}.mod:hover{border-color:var(--hi)}.modal-overlay{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.7);z-index:100;align-items:center;justify-content:center}.modal-overlay.on{display:flex}.modal{background:var(--card);border:1px solid var(--acc);border-radius:10px;padding:20px;max-width:560px;width:90%;max-height:80vh;overflow-y:auto}.modal h3{color:var(--hi);margin-bottom:12px}.modal .mfield{display:flex;gap:8px;padding:5px 0;border-bottom:1px solid #0d1b2a;font-size:.85rem}.modal .mfield .mk{color:var(--dim);min-width:120px}.modal .mfield .mv{color:var(--txt);font-family:monospace}.modal .mmap{margin-top:10px;font-size:.78rem;font-family:monospace;background:#080818;padding:8px;border-radius:4px;max-height:160px;overflow-y:auto}");
  server.sendContent(".unprovisioned{border-color:var(--hi)}");
  server.sendContent("#sr{font-size:.8rem;color:var(--grn);min-height:16px;margin-top:5px}");
  server.sendContent("</style></head><body>");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("<header><h1>Split-Flap Gateway</h1><span id=\"badge\">...</span></header>");
  server.sendContent("<nav>");
  server.sendContent("<a class=\"on\" onclick=\"show('modules',this)\">Modules</a>");
  server.sendContent("<a onclick=\"show('display',this)\">Display</a>");
  server.sendContent("<a onclick=\"show('provision',this)\">Provision</a>");
  server.sendContent("<a onclick=\"show('monitor',this)\">Bus Monitor</a>");
  server.sendContent("<a onclick=\"show('settings',this)\">Settings</a>");
  server.sendContent("<a onclick=\"show('statusp',this)\">Status</a>");
  server.sendContent("</nav>");
  server.sendContent("<div id=\"pane-modules\" class=\"pane on\">");
  server.sendContent("<div class=\"card\"><h2>Known Modules</h2><div id=\"modGrid\" class=\"grid2\">Loading...</div><button class=\"sec\" onclick=\"refreshModules()\" title=\"Broadcasts m*v to all modules so they report their version and serial number\">&#x21bb; Identify All</button><span id=\"refreshR\" style=\"margin-left:10px;font-size:.8rem;color:var(--grn)\"></span></div>");
  server.sendContent("<div class=\"card\"><h2>Quick Commands</h2><div class=\"row\"><div><label>Module ID (-1=all)</label><input id=\"qId\" type=\"number\" value=\"-1\" min=\"-1\" max=\"254\"></div><div><label>Character</label><input id=\"qChar\" type=\"text\" maxlength=\"1\" value=\"A\"></div></div>");
  server.sendContent("<button onclick=\"qSendChar()\">Send Char</button> <button class=\"sec\" onclick=\"qHome()\">Home</button> <button class=\"sec\" onclick=\"qVersion()\">Get Version</button>");
  server.sendContent("<div id=\"qr\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div></div>");
  server.sendContent("<div id=\"modModal\" style=\"display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.75);z-index:200;align-items:center;justify-content:center;\" onclick=\"if(event.target===this)closeModal()\"><div style=\"background:var(--card);border:1px solid var(--acc);border-radius:10px;padding:22px;max-width:560px;width:90%;max-height:82vh;overflow-y:auto;\"><div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:10px\"><span style=\"font-size:1.05rem;font-weight:600;color:var(--hi)\" id=\"modModalTitle\">Module EEPROM</span><button class=\"sec\" onclick=\"closeModal()\" style=\"margin:0;padding:3px 10px;font-size:.8rem\">&#x2715;</button></div><div id=\"modModalBody\" style=\"font-size:.85rem\"></div><div style=\"margin-top:14px;display:flex;gap:8px;align-items:center;flex-wrap:wrap\"><button class=\"sec\" onclick=\"refreshDump()\">&#x21bb; Refresh</button><span id=\"modModalStatus\" style=\"font-size:.78rem;color:var(--ylw)\"></span></div></div></div>");
  server.sendContent("<div id=\"pane-display\" class=\"pane\"><div class=\"card\"><h2>Send Text to Display</h2>");
  server.sendContent("<label>Text</label><input id=\"dispText\" type=\"text\" placeholder=\"HELLO WORLD\" style=\"text-transform:uppercase\">");
  server.sendContent("<label>Start Module ID</label><input id=\"dispStart\" type=\"number\" value=\"0\" min=\"0\" max=\"253\">");
  server.sendContent("<button onclick=\"sendText()\">Send to Display</button><div id=\"dr\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
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
  server.sendContent("<div id=\"pane-monitor\" class=\"pane\"><div class=\"card\"><h2>RS485 Bus Monitor</h2><div id=\"log\"></div>");
  server.sendContent("<div style=\"display:flex;gap:10px;margin-top:8px;align-items:center;flex-wrap:wrap\">");
  server.sendContent("<button class=\"sec\" onclick=\"clearLog()\">Clear</button>");
  server.sendContent("<label style=\"margin:0;display:flex;align-items:center;gap:5px;color:var(--txt)\"><input type=\"checkbox\" id=\"asc\" checked style=\"width:auto\"> Auto-scroll</label>");
  server.sendContent("<span id=\"logCount\" style=\"font-size:.74rem;color:var(--dim)\">0 frames</span></div></div>");
  server.sendContent("<div class=\"card\"><h2>Send Frame</h2><label>Data (ASCII)</label><textarea id=\"sdata\" rows=\"2\" placeholder=\"m5-A\"></textarea><div id=\"sr\"></div><button onclick=\"doSend()\">Send</button></div></div>");
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
  server.sendContent("<button onclick=\"saveMqtt()\">Save MQTT</button>");
  server.sendContent("<div id=\"mr\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div>");
  server.sendContent("</div>");
  server.sendContent("<div class=\"card\"><h2>Timezone</h2>");
  server.sendContent("<label>Timezone</label><select id=\"tzSel\"><option value=\"UTC0\">UTC</option><option value=\"PST8PDT,M3.2.0,M11.1.0\">US Pacific (UTC-8/-7)</option><option value=\"MST7MDT,M3.2.0,M11.1.0\">US Mountain (UTC-7/-6)</option><option value=\"MST7\">US Mountain AZ (UTC-7 no DST)</option><option value=\"CST6CDT,M3.2.0,M11.1.0\">US Central (UTC-6/-5)</option><option value=\"EST5EDT,M3.2.0,M11.1.0\">US Eastern (UTC-5/-4)</option><option value=\"BRT3BRST,M10.3.0,M2.3.0\">Sao Paulo (UTC-3/-2)</option><option value=\"AZOT1AZOST,M3.5.0/0,M10.5.0/1\">Azores (UTC-1/0)</option><option value=\"GMT0BST,M3.5.0/1,M10.5.0\">London (UTC+0/+1)</option><option value=\"CET-1CEST,M3.5.0,M10.5.0/3\">Paris/Berlin (UTC+1/+2)</option><option value=\"EET-2EEST,M3.5.0/3,M10.5.0/4\">Helsinki/Athens (UTC+2/+3)</option><option value=\"MSK-3\">Moscow (UTC+3 no DST)</option><option value=\"GST-4\">Dubai (UTC+4 no DST)</option><option value=\"PKT-5\">Karachi (UTC+5 no DST)</option><option value=\"BST-6\">Dhaka (UTC+6 no DST)</option><option value=\"ICT-7\">Bangkok (UTC+7 no DST)</option><option value=\"CST-8\">Shanghai/HK/Singapore (UTC+8)</option><option value=\"JST-9\">Tokyo (UTC+9 no DST)</option><option value=\"AEST-10AEDT,M10.1.0,M4.1.0/3\">Sydney (UTC+10/+11)</option><option value=\"NZST-12NZDT,M9.5.0,M4.1.0/3\">Auckland (UTC+12/+13)</option></select>");

  server.sendContent("<p style=\"font-size:.82rem;color:var(--dim);margin-top:4px\">Used for bus monitor timestamps. Takes effect immediately after saving.</p>");
  server.sendContent("<button onclick=\"saveTz()\">Save Timezone</button>");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("<div id=\"tzR\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div>");
  server.sendContent("</div>");
  server.sendContent("<div class=\"card\"><h2>Serial Debug</h2><p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">Enable verbose serial output on the native USB serial port (115200 baud). Shows every RX/TX frame, MQTT events, and module activity.</p><label style=\"display:flex;align-items:center;gap:10px;cursor:pointer;color:var(--txt)\"><input type=\"checkbox\" id=\"dbgChk\" style=\"width:auto\">Enable Serial Debug Output</label><button onclick=\"saveDebug()\" style=\"margin-top:10px\">Save</button><div id=\"dbgR\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
  server.sendContent("<div class=\"card\"><h2>OTA Firmware Update</h2><p style=\"font-size:.82rem;color:var(--dim);margin-bottom:8px\">Upload new firmware directly from your browser ? no USB cable or Arduino IDE required.</p><a href=\"/ota\" target=\"_blank\" style=\"display:inline-block;margin-bottom:12px;padding:7px 16px;background:var(--hi);color:#fff;border-radius:4px;text-decoration:none;font-size:.9rem\">Open Firmware Updater &rarr;</a><label style=\"margin-top:8px\">OTA Password</label><input id=\"otaPw\" type=\"password\" placeholder=\"Leave blank for no password\"><p style=\"font-size:.77rem;color:var(--dim);margin-top:4px\">Protects ArduinoOTA (IDE/command-line) uploads. The web updater above is always accessible.</p><button onclick=\"saveOTA()\" style=\"margin-top:6px\">Save OTA Password</button><div id=\"otaR\" style=\"margin-top:6px;font-size:.82rem;color:var(--grn)\"></div></div>");
  server.sendContent("</div>");
  server.sendContent("<div id=\"pane-statusp\" class=\"pane\"><div class=\"card\"><h2>System Status</h2><div class=\"grid3\">");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-up\">-</div><div class=\"k\">Uptime (s)</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-rx\">-</div><div class=\"k\">Frames RX</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-tx\">-</div><div class=\"k\">Frames TX</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-bd\">-</div><div class=\"k\">Baud Rate</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-ip\">-</div><div class=\"k\">STA IP</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-ap\">-</div><div class=\"k\">AP IP</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-hp\">-</div><div class=\"k\">Free Heap</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-mq\">-</div><div class=\"k\">MQTT</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-mod\">-</div><div class=\"k\">Modules</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-rtc\">-</div><div class=\"k\">RTC Time</div></div>");
  server.sendContent("<div class=\"stat\"><div class=\"v\" id=\"s-ntp\">-</div><div class=\"k\">NTP Sync</div></div>");
  server.sendContent("</div></div></div>");
  server.sendContent("<script>");
  server.sendContent("function show(id,el){document.querySelectorAll(\".pane\").forEach(function(p){p.classList.remove(\"on\");});document.querySelectorAll(\"nav a\").forEach(function(a){a.classList.remove(\"on\");});document.getElementById(\"pane-\"+id).classList.add(\"on\");el.classList.add(\"on\");}");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("var FC=\" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw\";");
  server.sendContent("(function(){var el=document.getElementById(\"charMap\");if(!el)return;var s=\"\";for(var i=0;i<FC.length;i++){s+='<span style=\"margin-right:8px;color:var(--txt)\">'+i+':<strong>'+FC[i]+\"</strong></span>\";}el.innerHTML=s;})();");
  server.sendContent("function sfDecode(raw){var s=raw.replace(/[\\r\\n]+$/,\"\");if(s.length<2||s[0]!==\"m\")return \"\";if(s[1]===\"X\"){if(s.indexOf(\"mXadv:\")==0)return \"ADV  unprovisioned SN: \"+s.slice(6);if(s.indexOf(\"mXack:\")==0){var r=s.slice(6),ci=r.lastIndexOf(\":\");return ci>=0?\"ACK  SN \"+r.slice(0,ci)+\" -> ID \"+r.slice(ci+1):\"ACK \"+r;}if(s[2]===\"I\"){var ci2=s.indexOf(\":\",3);return ci2>=0?\"PROVISION  SN \"+s.slice(3,ci2)+\" -> ID \"+s.slice(ci2+1):\"PROVISION \"+s.slice(3);}if(s[2]===\"H\")return \"PROVISION  home SN \"+s.slice(3);if(s[2]===\"D\")return \"DUMP       SN \"+s.slice(3);if(s[2]===\"F\")return \"FACTORY RST SN \"+s.slice(3);if(s[2]===\"W\")return \"RESTORE    \"+s.slice(3);return \"PROVISIONING \"+s.slice(2);}var p=1,id=\"\",bc=false;while(p<s.length&&(s[p]===\"*\"||s[p]>=\"0\"&&s[p]<=\"9\")){if(s[p]===\"*\")bc=true;else id+=s[p];p++;}var who=bc?\"ALL\":\"#\"+id;if(p>=s.length)return who+\" (incomplete)\";var cmd=s[p],rest=s.slice(p+1);if(cmd===\"-\"){var fi=FC.indexOf(rest[0]||\"\");var sfx=fi>=0?\" (idx \"+fi+\")\":\"\";return \"SHOW CHAR    \"+who+\" -> [\"+(rest[0]||\"?\")+\"]\"+sfx;}if(cmd===\"+\"){var n=parseInt(rest);var ch=isNaN(n)?\"?\":(FC[n]||\"?\");return \"SHOW INDEX   \"+who+\" -> \"+n+\" [\"+ch+\"]\";}if(cmd===\"h\")return \"HOME         \"+who;if(cmd===\"c\")return rest?\"CALIB RESP   \"+who+\" \"+rest+\" steps/rev\":\"CALIBRATE    \"+who;if(cmd===\"o\")return \"HOME OFFSET  \"+who+\" = \"+rest+\" steps\";if(cmd===\"t\")return \"TOTAL STEPS  \"+who+\" = \"+rest;if(cmd===\"s\")return \"NUDGE        \"+who+\" \"+rest+\" steps\";if(cmd===\"g\")return \"GOTO STEP    \"+who+\" -> step \"+rest;if(cmd===\"w\"){var wci=rest.indexOf(\":\");return wci>=0?\"WRITE POS    \"+who+\" idx \"+rest.slice(0,wci)+\" -> \"+rest.slice(wci+1)+\" steps\":\"WRITE POS    \"+who+\" \"+rest;}if(cmd===\"i\")return \"SET ID       \"+who+\" -> ID \"+rest;if(cmd===\"a\")return \"AUTO-HOME    \"+who+(rest===\"1\"?\" ON\":\" OFF\");if(cmd===\"d\")return rest&&rest[0]===\":\"?\"DUMP RESP    \"+who+\" \"+rest.slice(1):\"DUMP?        \"+who;if(cmd===\"e\")return \"ERASE MAP    \"+who;if(cmd===\":\")return \"CALIB RESP   \"+who+\" \"+rest+\" steps/rev\";if(cmd===\"v\"){if(rest&&rest[0]===\":\"){var vp=rest.slice(1).split(\":\");var vs=\"VERSION RESP \"+who+\" fw:\"+vp[0];if(vp.length>1&&vp[1]!==\"\")vs+=\" id:\"+vp[1];if(vp.length>2&&vp[2]!==\"\")vs+=\" sn:\"+vp[2];return vs;}return \"VERSION?     \"+who;}if(cmd===\"R\")return \"RESET PROV   \"+who;if(cmd===\"F\")return \"FACTORY RST  \"+who;return \"CMD [\"+cmd+\"] \"+who+(rest?\" \"+rest:\"\");}");
  server.sendContent("var _lfc=0;");
  server.sendContent("function clearLog(){document.getElementById(\"log\").innerHTML=\"\";_lfc=0;document.getElementById(\"logCount\").textContent=\"0 frames\";}");
  server.sendContent("function appendLogRow(m){var log=document.getElementById(\"log\");var ascii=(m.command||\"\").replace(/[\\r\\n]/g,\"\");var desc=sfDecode(ascii);var safeR=ascii.replace(/&/g,\"&amp;\").replace(/</g,\"&lt;\").replace(/>/g,\"&gt;\");var safeD=desc.replace(/&/g,\"&amp;\").replace(/</g,\"&lt;\").replace(/>/g,\"&gt;\");var ts=m.wt?m.wt:(m.ts<60000?m.ts+\"ms\":Math.floor(m.ts/1000)+\"s\");var row=document.createElement(\"div\");row.className=\"logrow \"+(m.dir===\"R\"?\"rx\":\"tx\");row.innerHTML=\"<span class=\\\"lts\\\">\"+ts+\"</span>\"+\"<span class=\\\"ldir\\\">\"+(m.dir===\"R\"?\"RX\":\"TX\")+\"</span>\"+\"<span class=\\\"lraw\\\">\"+safeR+\"</span>\";if(desc)row.innerHTML+=\"<span class=\\\"ldesc\\\">\"+safeD+\"</span>\";log.appendChild(row);_lfc++;document.getElementById(\"logCount\").textContent=_lfc+\" frame\"+(_lfc===1?\"\":\"s\");if(document.getElementById(\"asc\").checked)log.scrollTop=log.scrollHeight;}");
  server.sendContent("setInterval(function(){fetch(\"/api/rs485/messages\").then(function(r){return r.json();}).then(function(arr){arr.forEach(appendLogRow);}).catch(function(){});},600);");
  server.sendContent("function doSend(){var data=document.getElementById(\"sdata\").value.trim();if(!data){document.getElementById(\"sr\").textContent=\"Nothing to send.\";return;}fetch(\"/api/rs485/send\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({data:data})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"sr\").textContent=j.ok?\"Sent \"+j.bytes+\" bytes\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"sr\").textContent=\"Error: \"+e;});}");
  server.sendContent("function apiFlapCmd(path,body){return fetch(path,{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify(body)}).then(function(r){return r.json();});}");
  server.sendContent("var _currentModId=-1;function parseDump(raw){if(!raw)return{error:\"No data\"};var parts=raw.split(\":\");if(parts.length<2)return{error:\"Invalid format\",raw:raw};var r={homeOffset:parseInt(parts[0]),totalSteps:parseInt(parts[1]),map:{}};if(parts[2])parts[2].split(\",\").forEach(function(e){var kv=e.split(\"=\");if(kv.length===2&&kv[0]!==\"\")r.map[parseInt(kv[0])]=parseInt(kv[1]);});return r;}function renderDump(d){if(!d||!d.ok)return\"<p style=\\\"color:var(--hi)\\\">\"+(d&&d.error?d.error:\"No data\")+\"</p>\";var p=parseDump(d.dump||\"\");if(p.error)return\"<p style=\\\"color:var(--hi)\\\">\"+p.error+\"</p>\";function row(k,v){return\"<div class=\\\"mfield\\\"><span class=\\\"mk\\\">\"+k+\" : </span><span class=\\\"mv\\\">\"+v+\"</span></div>\";}var html=\"\"+row(\"Module ID\",d.id)+row(\"Serial Number\",d.sn)+row(\"Home Offset\",p.homeOffset+\" steps\")+row(\"Steps / Rev\",p.totalSteps);var keys=Object.keys(p.map);html+=row(\"Calibrated\",keys.length+\" / 64 flaps\");if(keys.length){html+=\"<div class=\\\"mmap\\\">\";keys.forEach(function(k){html+=\"[\"+k+\"]=\"+p.map[k]+\" \";});html+=\"</div>\";}html+=row(\"Raw EEPROM\",\"<span style=\\\"word-break:break-all;color:var(--dim)\\\">\"+d.dump+\"</span>\");return html;}function fetchDump(id){document.getElementById(\"modModalBody\").innerHTML=\"<p style=\\\"color:var(--dim)\\\">Fetching EEPROM from module #\"+id+\"...</p>\";document.getElementById(\"modModalStatus\").textContent=\"\";fetch(\"/api/flap/dump\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({id:id})}).then(function(r){return r.json();}).then(function(d){document.getElementById(\"modModalBody\").innerHTML=renderDump(d);document.getElementById(\"modModalStatus\").textContent=d.stale?\"Cached data -- click Refresh for latest\":\"Fresh from module\";}).catch(function(e){document.getElementById(\"modModalBody\").innerHTML=\"<p style=\\\"color:var(--hi)\\\">Error: \"+e+\"</p>\";});}function openModModal(id){_currentModId=id;document.getElementById(\"modModalTitle\").textContent=\"Module #\"+id+\" - EEPROM\";var mo=document.getElementById(\"modModal\");mo.style.display=\"flex\";fetchDump(id);}function refreshDump(){if(_currentModId>=0)fetchDump(_currentModId);}function closeModal(){document.getElementById(\"modModal\").style.display=\"none\";_currentModId=-1;}");
  server.sendContent("function refreshModules(){var el=document.getElementById(\"refreshR\");el.textContent=\"Identifying...\";fetch(\"/api/flap/identify\",{method:\"POST\"}).then(function(r){return r.json();}).then(function(j){el.textContent=j.ok?\"List cleared, identifying all modules -- refreshing in 2s\":\"Error: \"+j.error;if(j.ok)setTimeout(function(){loadModules();},2000);setTimeout(function(){loadModules();el.textContent=\"\";},7000);}).catch(function(e){el.textContent=\"Error: \"+e;});}");
  server.sendContent("function loadModules(){fetch(\"/api/flap/modules\").then(function(r){return r.json();}).then(function(arr){var g=document.getElementById(\"modGrid\");if(!arr.length){g.innerHTML=\"<p style='color:var(--dim)'>No modules detected yet.</p>\";return;}var h=\"\";arr.forEach(function(m){var cls=m.provisioned?\"mod\":\"mod unprovisioned\";var idStr=m.provisioned?\"ID: <span class='mid'>\"+m.id+\"</span>\":\"<span style='color:var(--hi)'>Unprovisioned</span>\";var charStr=m.flapChar?\"Showing: <b>\"+m.flapChar+\"</b> (idx \"+m.flapIndex+\")\":\"\";var badge=\"\";h+=\"<div class='\"+cls+\"' data-mid='\"+m.id+\"'>\"+idStr+badge+\"<br><span class='mc'>SN: \"+m.sn+\"</span>\"+(charStr?\"<br><span class='mc'>\"+charStr+\"</span>\":\"\")+(m.fwVersion?\"<br><span class='mc'>FW: v\"+m.fwVersion+\"</span>\":\"\")+\"</div>\";});g.innerHTML=h;g.querySelectorAll(\".mod\").forEach(function(el){el.addEventListener(\"click\",function(){openModModal(parseInt(this.dataset.mid));});});}).catch(function(){document.getElementById(\"modGrid\").innerHTML=\"Error loading modules\";});}");
  server.sendContent("loadModules();setInterval(loadModules,5000);");
  server.sendContent("function loadUnprovisioned(){fetch(\"/api/flap/modules\").then(function(r){return r.json();}).then(function(arr){var el=document.getElementById(\"unprovList\");var up=arr.filter(function(m){return !m.provisioned;});if(!up.length){el.innerHTML=\"<p style=\\\"color:var(--dim)\\\">No unprovisioned modules seen yet.</p>\";return;}var h=\"\";up.forEach(function(m){h+=\"<div style=\\\"display:flex;align-items:center;gap:8px;margin-bottom:8px;flex-wrap:wrap\\\">\"+\"<code style=\\\"color:var(--ylw);flex:1;min-width:160px\\\">\"+m.sn+\"</code>\"+\"<button class=\\\"sec\\\" style=\\\"margin:0;padding:4px 10px;font-size:.78rem\\\" onclick=\\\"doHomeSN('\"+m.sn+\"')\\\" title=\\\"Home this module to identify it\\\">Home</button>\"+\"</div>\";});el.innerHTML=h;}).catch(function(){});}");
  server.sendContent("setInterval(loadUnprovisioned,10000);loadUnprovisioned();");
  server.sendContent("function qSendChar(){var id=parseInt(document.getElementById(\"qId\").value);var ch=document.getElementById(\"qChar\").value;apiFlapCmd(\"/api/flap/char\",{id:id,\"char\":ch}).then(function(j){document.getElementById(\"qr\").textContent=j.ok?\"Sent\":\"Error: \"+j.error;});}");
  server.sendContent("function qHome(){var id=parseInt(document.getElementById(\"qId\").value);apiFlapCmd(\"/api/flap/home\",{id:id}).then(function(j){document.getElementById(\"qr\").textContent=j.ok?\"Homing\":\"Error: \"+j.error;});}");
  server.sendContent("function qCalibrate(){var id=parseInt(document.getElementById(\"qId\").value);apiFlapCmd(\"/api/flap/calibrate\",{id:id}).then(function(j){document.getElementById(\"qr\").textContent=j.ok?\"Calibrating\":\"Error: \"+j.error;});}");
  server.sendContent("function qVersion(){var id=parseInt(document.getElementById(\"qId\").value);apiFlapCmd(\"/api/flap/version\",{id:id}).then(function(j){document.getElementById(\"qr\").textContent=j.ok?\"Query sent\":\"Error: \"+j.error;});}");
  server.sendContent("function sendText(){var text=document.getElementById(\"dispText\").value.toUpperCase();var start=parseInt(document.getElementById(\"dispStart\").value);apiFlapCmd(\"/api/flap/text\",{text:text,start:start}).then(function(j){document.getElementById(\"dr\").textContent=j.ok?\"Sent \"+j.chars+\" chars\":\"Error: \"+j.error;});}");
  wdgWebMs = millis();  // touch WDG during long page send
  server.sendContent("function sendIndex(){var id=parseInt(document.getElementById(\"idxId\").value);var idx=parseInt(document.getElementById(\"idxVal\").value);apiFlapCmd(\"/api/flap/index\",{id:id,index:idx}).then(function(j){document.getElementById(\"dr\").textContent=j.ok?\"Sent\":\"Error: \"+j.error;});}");
  server.sendContent("function doHomeSN(sn){document.getElementById(\"provSN\").value=sn;apiFlapCmd(\"/api/flap/homebysn\",{sn:sn}).then(function(j){var el=document.getElementById(\"provR\");if(el)el.textContent=j.ok?\"Homing SN: \"+sn+\" - check which module moves\":\"Error: \"+j.error;});}");
  server.sendContent("function doProvision(){var sn=document.getElementById(\"provSN\").value;var id=parseInt(document.getElementById(\"provId\").value);apiFlapCmd(\"/api/flap/provision\",{sn:sn,id:id}).then(function(j){document.getElementById(\"provR\").textContent=j.ok?\"Provisioning sent\":\"Error: \"+j.error;});}");
  server.sendContent("function doDeprovision(){var id=parseInt(document.getElementById(\"deprovId\").value);apiFlapCmd(\"/api/flap/deprovision\",{id:id}).then(function(j){document.getElementById(\"deprovR\").textContent=j.ok?(\"De-provisioned \"+(id<0?\"all modules\":\"module \"+id)):\"Error: \"+j.error;});}");
  server.sendContent("function saveWifi(){fetch(\"/api/config/wifi\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({ssid:document.getElementById(\"wSSID\").value,pass:document.getElementById(\"wPASS\").value})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"wr\").textContent=j.ok?\"WiFi saved - reconnecting...\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"wr\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveMqtt(){fetch(\"/api/config/mqtt\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({host:document.getElementById(\"mqH\").value,port:parseInt(document.getElementById(\"mqP\").value),user:document.getElementById(\"mqU\").value,pass:document.getElementById(\"mqPw\").value,prefix:document.getElementById(\"mqPfx\").value})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"mr\").textContent=j.ok?\"MQTT saved - reconnecting...\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"mr\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveTz(){var sel=document.getElementById(\"tzSel\");fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({posixTZ:sel.value})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"tzR\").textContent=j.ok?\"Timezone saved\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"tzR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveOTA(){fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({otaPassword:document.getElementById(\"otaPw\").value})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"otaR\").textContent=j.ok?\"OTA password saved\":\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"otaR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function saveDebug(){var v=document.getElementById(\"dbgChk\").checked;fetch(\"/api/config/settings\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({serialDebug:v})}).then(function(r){return r.json();}).then(function(j){document.getElementById(\"dbgR\").textContent=j.ok?(v?\"Debug enabled\":\"Debug disabled\"):\"Error: \"+j.error;}).catch(function(e){document.getElementById(\"dbgR\").textContent=\"Error: \"+e;});}");
  server.sendContent("function pollStatus(){fetch(\"/api/status\").then(function(r){return r.json();}).then(function(s){document.getElementById(\"s-up\").textContent=s.uptime;document.getElementById(\"s-rx\").textContent=s.rx;document.getElementById(\"s-tx\").textContent=s.tx;document.getElementById(\"s-bd\").textContent=s.baud;document.getElementById(\"s-ip\").textContent=s.ip;document.getElementById(\"s-ap\").textContent=s.apip;document.getElementById(\"s-hp\").textContent=s.heap;document.getElementById(\"s-mq\").textContent=s.mqtt?\"Connected\":\"Off\";document.getElementById(\"s-mod\").textContent=s.modules;if(document.getElementById(\"s-rtc\"))document.getElementById(\"s-rtc\").textContent=s.time||\"--\";if(document.getElementById(\"s-ntp\"))document.getElementById(\"s-ntp\").textContent=s.ntpSynced?\"OK\":\"Pending\";var b=document.getElementById(\"badge\");b.textContent=s.wifi?\"WiFi: \"+s.ip:\"AP only\";b.className=s.wifi?\"ok\":\"\";}).catch(function(){});}setInterval(pollStatus,3000);pollStatus();");
  server.sendContent("fetch(\"/api/config\").then(function(r){return r.json();}).then(function(c){document.getElementById(\"wSSID\").value=c.wSSID||\"\";document.getElementById(\"mqH\").value=c.mqHost||\"\";document.getElementById(\"mqP\").value=c.mqPort||1883;document.getElementById(\"mqU\").value=c.mqUser||\"\";document.getElementById(\"mqPfx\").value=c.mqPfx||\"splitflap\";if(c.posixTZ){var sel=document.getElementById(\"tzSel\");for(var j=0;j<sel.options.length;j++){if(sel.options[j].value===c.posixTZ){sel.selectedIndex=j;break;}}}});");
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
  uint8_t outBuf[MSG_MAX_BYTES];
  size_t  outLen = min(strlen(d), (size_t)MSG_MAX_BYTES);
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
    out += '}';
  }
  out += ']';
  xSemaphoreGive(sfMutex);
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
  if (idx < 0 || idx >= FLAP_CHAR_COUNT) { sendJsonError(400, "Invalid index"); return; }
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
void handleApiCalibrate() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  DBG("[API] calibrate module %d\n", id);
  sfCalibrate(id);
  server.send(200, "application/json", "{\"ok\":true}");
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

  // Wait up to 500 ms for a fresh version response (lastSeen advances when
  // sfParseResponse processes the reply and writes fwVersion)
  char          fwVer[8]     = "";
  char          sn[21]       = "";
  int           repId        = -1;
  bool          gotReply     = false;
  unsigned long repLastSeen  = 0;
  unsigned long deadline = millis() + 500;
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
  char out[320];
  snprintf(out, sizeof(out),
    "{\"uptime\":%lu,\"rx\":%lu,\"tx\":%lu,\"baud\":%lu,"
    "\"wifi\":%s,\"ip\":\"%d.%d.%d.%d\",\"apip\":\"%d.%d.%d.%d\","
    "\"heap\":%u,\"mqtt\":%s,\"modules\":%d,"
    "\"time\":\"%s\",\"ntpSynced\":%s}",
    millis()/1000, rxCount, txCount, cfg.rs485Baud,
    (WiFi.status()==WL_CONNECTED)?"true":"false",
    lip[0],lip[1],lip[2],lip[3],
    aip[0],aip[1],aip[2],aip[3],
    ESP.getFreeHeap(),
    mqtt.connected()?"true":"false",
    sfModuleCount, rtcBuf,
    ntpSynced?"true":"false");
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
  printf("[OTA] Ready (hostname: splitflap-gw)\n");
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

  // Try mXD<sn> first for fw >= 15; fall back to m<id>d otherwise.
  bool triedSN = false;
  if (fwVerNum >= 15 && sn[0]) {
    DBG("[API] dump module %d via SN %s (fw=%d)\n", id, sn, fwVerNum);
    sfDumpBySN(sn);
    triedSN = true;
  } else {
    DBG("[API] dump module %d via ID (fw=%d)\n", id, fwVerNum);
    char buf[16]; snprintf(buf, sizeof(buf), "m%dd\n", id); rs485SendStr(buf);
  }

  // Wait up to 500 ms for a fresh response (captured by sfParseResponse).
  char rawDump[MSG_MAX_BYTES] = "";
  bool gotReply = false;
  unsigned long deadline = millis() + 500;
  while (millis() < deadline) {
    wdgWebMs = millis();
    vTaskDelay(pdMS_TO_TICKS(10));
    if (sfDumpCaptureTs != 0) {
      strlcpy(rawDump, sfDumpCapture, sizeof(rawDump));
      gotReply = true;
      break;
    }
  }

  // SN-based command got no response -- fall back to ID-based.
  if (!gotReply && triedSN) {
    DBG("[API] SN dump timed out, retrying module %d via ID\n", id);
    char buf[16]; snprintf(buf, sizeof(buf), "m%dd\n", id); rs485SendStr(buf);
    deadline = millis() + 500;
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

void webInit() {
  server.on("/",                     HTTP_GET,     handleRoot);
  server.on("/ota",                  HTTP_GET,     handleOTAPage);
  server.on("/api/ota/upload",       HTTP_POST,    [](){}, handleOTAUpload);
  server.on("/api/rs485/messages",   HTTP_GET,     handleApiMessages);
  server.on("/api/rs485/send",       HTTP_POST,    handleApiSend);
  server.on("/api/rs485/send",       HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/modules",     HTTP_GET,     handleApiModules);
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
  static uint8_t lineBuf[MSG_MAX_BYTES];
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
      if (lineLen < MSG_MAX_BYTES - 1) {
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
        m.len       = lineLen;
        memcpy(m.data, lineBuf, lineLen);
        rtcFormatTime(m.wallTime, sizeof(m.wallTime));
        // Log the received frame (strip trailing newline for readability)
        { char dbg[MSG_MAX_BYTES]; size_t dlen = lineLen > 0 ? lineLen-1 : 0;
          memcpy(dbg, lineBuf, dlen); dbg[dlen] = '\0';
          DBG("[RX] %s  (%s)\n", dbg, m.wallTime); }
        ringPush(m);
        mqttPublishMsg(m);
        sfParseResponse(lineBuf, lineLen);
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
  printf("\n[Boot] Split-Flap Gateway\n");

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
  xTaskCreatePinnedToCore(taskRTC,     "RTC",     2048, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(taskRS485,   "RS485",   4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(taskOTA,     "OTA",     4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(taskWeb,     "Web",     8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(taskNetwork, "Network", 6144, NULL, 1, NULL, 1);

  printf("[Boot] Ready\n");
}

void loop() {
  static unsigned long lastWdgCheck = 0;
  unsigned long now = millis();
  if (now - lastWdgCheck >= 30000UL) {
    lastWdgCheck = now;
    printf("[WDG] up=%lus heap=%u rx=%lu tx=%lu wifi=%d mqtt=%d\n",
                  now/1000, ESP.getFreeHeap(), rxCount, txCount,
                  (int)(WiFi.status()==WL_CONNECTED), (int)mqtt.connected());

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
        printf("[WDG] STALL: RS485=%d Web=%d Net=%d -- rebooting\n",
                      ok485, okWeb, okNet);
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
