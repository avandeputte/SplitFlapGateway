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
#define FW_VERSION           "2.0"           // gateway firmware version (UI + boot log)

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

/* ---- Flap set sizing ----
   Compile-time MAXIMUM number of physical flaps per module = capacity of the
   module firmware's EEPROM flap map and char buffers (firmware MAX_FLAPS). The
   runtime-configurable flap set ('N' command, firmware v31+) sets the ACTIVE
   count and ordered character set within this bound; both default to 64. Sized
   to match the firmware so a full char set round-trips through dump/restore. */
#define SF_MAX_FLAPS         64

/* ---- Housekeeping cadences ---- */
#define STATUS_INTERVAL_MS      60000UL   // MQTT status publish cadence (1/min)
#define MODULE_STALE_SECS       21600UL   // 6h: prune modules not seen in this long
#define MODULE_PROBE_GRACE_MS   3000UL    // wait this long for a stale module's version reply before dropping it
#define MODULE_PROBE_SPACING_MS 150UL     // gap between consecutive stale probes: a module answers a bare 'v' INSTANTLY, so back-to-back queries make each reply collide with the next probe -- space them so every reply is received (and clears the probe) before the next query goes out
#define MODULE_PROBE_BATCH      8         // max stale modules probed per prune cycle; the rest are probed on later cycles (bounds the net task's spaced-probe burst)
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
#define DBG(...) do { if (gSerialDebug) printf(__VA_ARGS__); } while(0)

/* ============================================================================
 *  SHARED INFRASTRUCTURE  (cross-cutting; defined once in globals.cpp)
 * ==========================================================================*/
extern volatile bool gSerialDebug;
extern volatile bool gMaintenanceMode;
extern volatile bool gQuietTime;
extern volatile bool gDisplayDirty;
extern volatile bool gOtaInProgress;
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