#ifndef SFGW_COMMON_H
#define SFGW_COMMON_H

/*
 * Split-Flap Gateway
 * Firmware for the Waveshare ESP32-S3-RS485-CAN board.
 *
 * Multi-file PlatformIO project. This header (common.h) is the shared kernel:
 * it pulls in the libraries, defines the board configuration macros and the
 * data types used across subsystems, and declares the cross-cutting globals
 * (defined once in globals.cpp). Every .cpp includes it via gateway.h. See
 * gateway.h for a map of the source files.
 *
 * Copyright (c) 2026 Alex Van de Putte
 *
 * Licensed under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0
 * International License (CC BY-NC-SA 4.0):
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 *
 * You are free to share and adapt this project for non-commercial purposes, as
 * long as you give appropriate credit and distribute any derivatives under the
 * same license.
 *
 * Split-flap module hardware and the initial protocol by Adam G Makes
 * (YouTube: https://www.youtube.com/@AdamGMakes).
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
 *   REST API (see the reference block at the bottom of main.cpp)
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
#include <esp_mac.h>

/* ---- This board's unique id -------------------------------------------------------
   Derived from the NIC-specific half of the MAC (bytes 3..5) -- the only part that
   actually differs between two boards.

   NOT ESP.getEfuseMac() masked, which is the trap this exists to close.
   esp_efuse_mac_get_default() writes the six MAC bytes in NETWORK order, and Arduino's
   EspClass reads that same buffer straight back as a LITTLE-endian uint64 -- so the LOW
   bytes of the value it returns are the OUI (48:27:e2), which is identical on every
   Espressif chip ever made. Masking the low 24 or 32 bits therefore hands EVERY board
   the same "unique" id.

   That is not theoretical. Two of these gateways on one LAN both derived the hostname
   splitflap-gw-e22748 and both connected to MQTT as splitflap-20E22748 -- and a broker
   evicts the client already holding a duplicate id, so the pair knocked each other
   offline in a loop, forever.                                                          */
static inline uint32_t boardId24() {          // 6 hex digits -- hostname suffix
  uint8_t m[6] = {0};
  esp_efuse_mac_get_default(m);
  return ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | (uint32_t)m[5];
}
static inline uint32_t boardId32() {          // 8 hex digits -- MQTT client id, HA node id
  uint8_t m[6] = {0};
  esp_efuse_mac_get_default(m);
  return ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16)
       | ((uint32_t)m[4] << 8)  | (uint32_t)m[5];
}
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
// The companion reads this back as "version" from GET /api/config and stores its
// settings on the gateway only when that version parses >= 3.1 (a 3.0 gateway
// omits the field, so it keeps settings local). This firmware clears that floor.
#define FW_VERSION           "3.7.3"         // gateway firmware version (UI + boot log)
// What this gateway IS, and which gateway API it speaks. GET /api/capabilities reports both, so
// a client can tell a real split-flap wall from the Matrix Portal emulation of one without
// sniffing the firmware version -- they answer the same URLs with the same shape, and the
// differences that matter (a fixed reel per module vs one shared reel; colours as flaps vs
// colours as names) are exactly what capabilities describes.
#define PRODUCT_NAME         "Split-Flap Gateway"
#define API_VERSION          "3.1.0"

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
#define MQTT_BUF_SIZE        1024  // MQTT packet buffer + queue slot: holds a full
                                   // restore command AND the worst-case rx/tx monitor
                                   // JSON (256 wire bytes -> 3-byte UTF-8 glyphs =
                                   // ~830B payload + topic/header) without truncation
#define MQTT_Q_SIZE          32    // outbound MQTT publish queue depth

/* ---- Flap set sizing ----
   Compile-time MAXIMUM number of physical flaps per module = capacity of the
   module firmware's EEPROM flap map and char buffers (firmware MAX_FLAPS). The
   runtime-configurable flap set ('N' command, firmware v31+) sets the ACTIVE
   count and ordered character set within this bound; both default to 64. Sized
   to match the firmware so a full char set round-trips through dump/restore. */
#define SF_MAX_FLAPS         64

/* ---- Housekeeping cadences ---- */
#define STATUS_INTERVAL_MS      60000UL   // MQTT status publish cadence (1/min)
#define MODULE_STALE_SECS       86400UL   // 24h: prune modules not seen in this long. Modules only speak when addressed, so a module that's merely displaying static content ages toward staleness normally -- keep the window generous so a long-lived message doesn't churn the whole wall through the stale-probe path.
#define MODULE_PROBE_GRACE_MS   3000UL    // wait this long for a stale module's version reply before the next probe attempt
#define MODULE_PROBE_SPACING_MS 150UL     // gap between consecutive stale probes: a module answers a bare 'v' INSTANTLY, so back-to-back queries make each reply collide with the next probe -- space them so every reply is received (and clears the probe) before the next query goes out
#define MODULE_PROBE_BATCH      8         // max stale modules probed per prune cycle; the rest are probed on later cycles (bounds the net task's spaced-probe burst)
#define MODULE_PROBE_MAX_TRIES  4         // stale-probe attempts before a module is actually dropped. A single reply is easily lost to a bus collision while the host is streaming display frames, so one probe is not proof of absence -- retry across several prune cycles (~1/min) before evicting a module that may well be alive and displaying.
#define MODULE_POSTPROV_VER_MS  4000UL    // delay after a provisioning ack before the first version query. A freshly-provisioned module writes its new ID to EEPROM and then runs a staggered startup (~150ms x new-ID), so it can be unresponsive for several seconds; wait well past that before the first query.
#define MODULE_VER_RETRY_MS     2500UL    // gap between post-provision version-query retries
#define MODULE_VER_MAX_TRIES    6         // give up after this many version-query attempts (covers ~4s + 5x2.5s ~= 16s)
// Longer than a companion heartbeat (~30 s) ON PURPOSE. Each change RESTARTS this
// clock, so two companions flipping the URL between them never hold still long enough
// to be written -- which is the point. A single, real change persists after two quiet
// minutes.
#define COMPANION_SAVE_DEBOUNCE_MS 120000UL   // companion URL: persist once it settles
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

/* ---- The flap set: what each module can actually show ----
   A module owns its own reel, and since firmware v31 it can be TOLD a different one ('N'), so
   two modules on the same bus need not agree. GET /api/capabilities answers "what can this wall
   show?", and to answer it the gateway has to know each module's set.

   It learns one from an 'A' reply, whose v31+ tail carries :<flapCount>:<flapChars>. That reply
   is ~200 bytes; at 9600 baud, asking 45 modules costs about NINETY SECONDS of bus time. So it
   is asked once, cached in the registry, and PERSISTED -- paying that on every reboot would be
   absurd, and a wall-wide m*A at boot would flood the bus for a minute and a half. Unknown sets
   are filled by a slow background trickle (one module per FLAPSET_QUERY_MS), and a set is
   re-read only when something invalidates it: an 'N' that changes it, or a module the gateway
   has never asked. */
#define FLAPSET_QUERY_MS      2000UL   // at most one 'A' flap-set query per this interval
#define FLAPSET_RETRY_MS     20000UL   // a module that didn't answer: wait this long, try again
#define FLAPSET_MAX_TRIES         3    // then give up; it reports as "unknown", not as a lie
#define FLAPSET_FW_MIN           31    // the firmware that first reports its set at all

// The reel a module firmware is BUILT with (FLAP_CHARS in SplitFlapUniversalFirmware). A module
// older than v31 cannot tell the gateway its set, and this is what it almost certainly has --
// but "almost certainly" is not "reported", so /api/capabilities lists those modules under
// "assumed" rather than folding the guess in silently.
//
// 'q' is not the letter q. The classic reel has no lowercase, so the char map borrowed that byte
// for the DOUBLE-QUOTE flap -- and r/o/y/g/b/p/w are the seven COLOUR flaps, not letters. Both
// are protocol, not repertoire, which is why capabilities reports '"' and a colour list rather
// than 'q' and seven stray consonants.
#define FLAPSET_DEFAULT " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw"
#define FLAP_COLOUR_CODES  "roygbpw"
#define MODULES_FILE     "/modules.dat"
#define MODULES_MAGIC    0x53464733UL   // "SFG3" (bumped: PersistedModule gained the flap set)

/* ---- Companion settings blob (FFat) -- v3.1 --------------------------------
 * The companion app can park its settings here so its container stays stateless.
 * The gateway is a dumb blob store: the body is gzip(minified JSON) whose schema
 * the companion owns, stored verbatim and handed back byte-for-byte. Written via
 * a temp file + rename so a crash mid-write cannot corrupt the good copy.
 * Names are 8.3-safe, like MODULES_FILE, so they work without FATFS long-name
 * support. Real blobs are ~1-2 KB; the cap only exists to bound a rogue PUT. */
#define COMPANION_FILE       "/compset.gz"
#define COMPANION_TMP        "/compset.tmp"
#define COMPANION_MAX_BYTES  (64UL * 1024UL)

/* ---- Companion tab advertisement -- v3.4 -----------------------------------
 * At registration the companion may advertise the tabs (deep links) its own UI
 * offers, so the dashboard links exactly the tabs that companion really has
 * instead of a list hard-coded here; the gateway advertises its own tabs back in
 * the same exchange. Both sides fall back to their built-in list when the peer
 * says nothing, so old<->new in either direction keeps working. The stored form
 * is the re-serialised JSON array: bounded by the caps below, and a companion
 * that overruns them simply doesn't get its list advertised (the dashboard then
 * shows its built-in one). */
// Bytes of stored JSON. Realistic tabs ({"id":"playlists","label":"Playlists"} is
// ~38 B) reach the count cap below first; only max-length ids/labels hit this one,
// where it binds at 5 tabs. Either way the list is dropped whole, never truncated.
#define COMPANION_TABS_MAX     384
#define COMPANION_TABS_MAX_N   10    // max tabs accepted from a companion
#define COMPANION_TAB_ID_MAX   24    // max chars of one tab's id (the URL hash)
#define COMPANION_TAB_LBL_MAX  24    // max chars of one tab's label

/* ==========================================================================*/
#define DBG(...) do { if (gSerialDebug) printf(__VA_ARGS__); } while(0)

/* ============================================================================
 *  SHARED INFRASTRUCTURE  (cross-cutting; defined once in globals.cpp)
 * ==========================================================================*/
extern volatile bool gSerialDebug;
extern volatile bool gMaintenanceMode;
extern volatile bool gQuietTime;
extern char gCompanionStatus[80];          // v3.0: companion running-status
extern volatile unsigned long gCompanionSeenMs;
// v3.4: the tab list the companion advertised at registration, held as the JSON
// array we re-serialised from its POST (so it is valid JSON by construction and
// can be splice into a response with serialized()). Runtime-only, like the status:
// a companion re-advertises on every heartbeat. Empty = it never told us (an older
// companion), and the dashboard falls back to its built-in list of companion tabs.
extern char gCompanionTabs[COMPANION_TABS_MAX];
// The companion URL is persisted on a DEBOUNCE, not on every change. Two companions
// pointed at the same gateway will each re-register their own URL on their heartbeat,
// so cfg.companionUrl flips back and forth -- and saving on every change turned that
// into an NVS write every ~30 s, forever. Observed in the wild. The URL is applied to
// RAM immediately (the UI and the companion tabs are live at once); only the flash
// write waits for the value to hold still. A contested URL therefore never reaches
// flash at all, which is the right answer: nothing durable should be written for a
// value two clients are still arguing over. A companion re-registers within a
// heartbeat of any reboot, so nothing is lost by not persisting it.
extern volatile bool          gCompanionUrlDirty;
extern volatile unsigned long gCompanionUrlDirtyMs;  // millis() of last companion post
extern volatile bool gDisplayDirty;
extern volatile bool gOtaInProgress;
// Last flap byte transmitted to each module id (= grid cell, row-major). Lets the
// display wall show EVERY cell that was written -- provisioned or not -- straight
// from the frame stream, independent of the module registry. 0 = nothing sent.
extern char gWallChars[256];
extern bool gApActive;
extern SemaphoreHandle_t timeMutex;
extern StaticSemaphore_t timeMutexBuf;
extern volatile unsigned long wdgRS485Ms;
extern volatile unsigned long gLastRxMs;
extern volatile unsigned long wdgNetMs;
extern volatile unsigned long wdgWebMs;
extern TaskHandle_t hTaskRTC, hTaskRS485, hTaskOTA, hTaskWeb, hTaskNet;
extern bool ntpSynced;
extern unsigned long staDownSince;

#endif // SFGW_COMMON_H