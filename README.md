# Split-Flap Gateway

**Firmware version: 2.0**

> [!NOTE]
> **New to this project?** Read the [blog post](BLOG.md) for the full story — why it exists, how it works, and how it fits into the split-flap display ecosystem. Calibrating a module? See the **[Module Calibration Guide](CALIBRATION_GUIDE.md)**.

---

> [!IMPORTANT]
> **This project is designed to work with the Split-Flap display from [Adam G Makes](https://www.youtube.com/@AdamGMakes).**
> See [this video](https://youtu.be/-C8_AtxEEQc?si=Gym5wikeFH2vUNRm) for additional information.
>
> - **Basic functionality** (sending characters and text to the display) works with Adam's original firmware using hardcoded module IDs — no provisioning required.
> - **Full functionality** (module discovery, dynamic ID assignment, and provisioning) requires the universal firmware: [avandeputte/SplitFlapUniversalFirmware](https://github.com/avandeputte/SplitFlapUniversalFirmware).

---

## Table of Contents

- [Hardware](#hardware)
- [Features](#features)
- [Installation](#installation)
  - [Prerequisites](#prerequisites)
  - [Step 1 — Install ESP32 board support](#step-1--install-esp32-board-support)
  - [Step 2 — Install required libraries](#step-2--install-required-libraries)
  - [Step 3 — Configure the board](#step-3--configure-the-board)
  - [Step 4 — Upload](#step-4--upload)
- [Serial Debug Output](#serial-debug-output)
- [First Boot](#first-boot)
- [Web UI](#web-ui)
- [Split-Flap Protocol](#split-flap-protocol)
- [REST API](#rest-api)
  - [RS-485 Bus](#rs-485-bus)
  - [Module Control](#module-control)
  - [Advanced Module Commands](#advanced-module-commands)
  - [Provisioning](#provisioning)
  - [Status and Configuration](#status-and-configuration)
  - [OTA Firmware Update](#ota-firmware-update)
- [OpenAPI Specification](#openapi-specification)
- [MQTT](#mqtt)
- [Timezone Configuration](#timezone-configuration)
- [OTA Firmware Updates](#ota-firmware-updates)
- [Default Settings](#default-settings)
- [License](#license)

---

## Hardware

**Target board:** [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN) — [Buy on Amazon](https://www.amazon.com/dp/B0FNCWZ3D1?ref=ppx_yo2ov_dt_b_fed_asin_title)

| Function | Pin |
|---|---|
| RS-485 TX | GPIO 17 |
| RS-485 RX | GPIO 18 |
| RS-485 DE/RE | GPIO 21 |
| I²C SDA (RTC) | GPIO 39 |
| I²C SCL (RTC) | GPIO 38 |
| RTC chip | PCF85063 @ 0x51 |
| Debug serial | Native USB CDC (`/dev/cu.usbmodem*` on macOS, `COMx` on Windows, `/dev/ttyACM0` on Linux) |

The RS-485 bus runs at **9600 baud, 8N1**.

---

## Features

- **Web UI** — single-page dashboard accessible from any browser
- **Module management** — discover, provision, home, calibrate, and deprovision split-flap modules (up to 255: IDs 0-254)
- **Sticky module list** — the known-module registry persists across reboots in flash; a stale module is version-probed before being dropped (so a merely-quiet module isn't lost), and a gateway that boots with an empty list broadcasts `m*v` to rediscover modules automatically
- **Per-module actions** — each module card has Home, Info, and destructive-action icons. The Info dialog shows all known module data plus a parsed view of its EEPROM (home offset, steps/rev, and the calibrated flap map). The destructive-actions dialog (red trash icon) offers Erase EEPROM, Factory Reset, and De-provision, each with a confirmation prompt
- **Backup & restore** — download all module calibration to a JSON file and restore it later by serial number, with an option to also reassign module IDs (a v31+ module's configurable flap set is captured and restored too)
- **Configurable flap set** — modules on firmware v31+ let you set the active flap count (1–64) and the ordered character set per module from the Info dialog, or push one set to a whole panel with a broadcast; the live values are read back from the `A` dump and persist in module EEPROM
- **Module self-diagnostics** — modules on firmware v26+ gain a 🩺 icon that runs three built-in tests and interprets the results: an instant **stats snapshot** (reset cause, boot count, supply voltage, EEPROM verify, current flap index), a **Hall sensor self-test** (home-sensor health), and a motor-driven **mechanical self-test** that detects intermittent missed steps (drag / weak supply / failing driver) or a stalled reel — with a selectable rotation count (5–20, firmware v29+) for deeper runs
- **Connection test** — a "Test Connection" button on the Settings tab verifies MQTT broker reachability and credentials before saving
- **Health metrics** — the Status tab shows minimum-ever free heap and per-task stack high-water marks, surfacing memory pressure before it causes a crash
- **Diagnostic boot/telemetry log** — on boot the gateway prints its reset reason (POWERON / PANIC / TASK_WDT / BROWNOUT, etc.), a chip/heap snapshot, and where its large buffers were allocated (`[MEM] … in PSRAM`); the periodic `[WDG]` line reports heap, min-ever heap, largest free block, heap-fragmentation %, per-task stack watermarks, RX/TX/parse-reject counters, WiFi station and fallback-AP state, WiFi RSSI, and MQTT state
- **PSRAM-backed buffers** — the bus-monitor ring, MQTT publish queue, and module registry (~60 KB combined) live in the board's PSRAM, keeping internal RAM free for the WiFi/TCP stack (which makes large over-the-air updates reliable); falls back to internal RAM if PSRAM is unavailable
- **mDNS** — the web UI is reachable at `http://splitflap-gw.local` (no IP lookup needed)
- **Maintenance mode** — a header toggle that makes the gateway ignore all externally-originated MQTT commands (the web UI keeps working), so calibration and backup work isn't disturbed by home-automation traffic; always resets to off on reboot
- **Quiet time** — a header toggle (and REST/MQTT/Home Assistant control) that keeps the gateway responsive to commands but stops it from moving any flaps for normal display updates; deliberate calibration moves still work. When turned off, the reels resync to the last requested display. Always resets to off on reboot
- **Home Assistant integration** — optional MQTT auto-discovery (opt-in on the Settings tab) that exposes the gateway as a Home Assistant device: a display text control, Maintenance and Quiet Time switches, an availability sensor (via MQTT LWT), and the full `[WDG]` diagnostic set as sensors, plus a clickable Gateway URL
- **Real-time bus monitor** — live RS-485 traffic with protocol decoding and local timestamps
- **NTP time sync** — RTC backed by PCF85063; syncs on WiFi connect and persists through power loss. The NTP server is configurable in Settings (default `pool.ntp.org`)
- **DST-aware timezone** — POSIX TZ string support covering 20 regions, takes effect immediately
- **MQTT integration** — publishes bus frames and module events; subscribes to control topics
- **REST API** — full JSON API for programmatic control (OpenAPI spec included)
- **OTA updates** — flash new firmware over WiFi from a browser or Arduino IDE; no USB cable required after first flash
- **Fallback AP** — when no network is configured (or the WiFi connection is lost for ~20 s), the gateway brings up its own access point for setup; once connected to your WiFi it runs station-only and shuts the AP down automatically
- **Configurable serial debug** — verbose logging toggled from the web UI Settings tab
- **Watchdog** — detects stalled FreeRTOS tasks and auto-reboots; emergency reboot on low heap

---

## Installation

> **Don't want to build at all?** A precompiled binary is published on the
> [Releases](https://github.com/avandeputte/SplitFlapGateway/releases) page and can
> be flashed directly — no build environment required. See
> [SETUP.md ▸ Flash a prebuilt binary](SETUP.md#flash-a-prebuilt-binary-no-build-environment-needed).
>
> **Using VS Code + PlatformIO?** This repo is a PlatformIO project (`platformio.ini`),
> and **[SETUP.md](SETUP.md)** is a step-by-step guide covering everything from
> installing VS Code through building, flashing over USB, and OTA updates — with the
> board settings already encoded in the project (no manual library install). The
> section below is the **Arduino IDE** alternative; use one path or the other.

### Prerequisites

- [Arduino IDE 2.x](https://www.arduino.cc/en/software)
- ESP32 board support package
- Two Arduino libraries

### Step 1 — Install ESP32 board support

1. Open Arduino IDE → **File → Preferences**
2. Add to *Additional boards manager URLs*:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Open **Tools → Board → Boards Manager**, search for **esp32** by Espressif Systems, install version 3.x

### Step 2 — Install required libraries

Open **Tools → Manage Libraries** and install:

| Library | Author | Purpose |
|---|---|---|
| **PubSubClient** | Nick O'Leary | MQTT client |
| **ArduinoJson** | Benoit Blanchon | JSON serialisation |

`WiFi`, `WebServer`, `Preferences`, `HardwareSerial`, `Wire`, `ArduinoOTA`, `Update`, and `time` are bundled with the ESP32 core.

### Step 3 — Configure the board

Select **ESP32S3 Dev Module** and set:

| Setting | Value |
|---|---|
| USB CDC On Boot | **Enabled** |
| CPU Frequency | 240MHz (WiFi) |
| Core Debug Level | None |
| USB DFU On Boot | Disabled |
| Erase All Flash Before Sketch Upload | Disabled |
| Events Run On | Core 1 |
| Flash Mode | QIO 80MHz |
| Flash Size | **16MB (128Mb)** |
| JTAG Adapter | Disabled |
| Arduino Runs On | Core 1 |
| USB Firmware MSC On Boot | Enabled (Requires USB-OTG Mode) |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** |
| PSRAM | OPI PSRAM |
| Upload Mode | UART0 / Hardware CDC |
| Upload Speed | 921600 |
| USB Mode | Hardware CDC and JTAG |
| Zigbee Mode | Disabled |

### Step 4 — Upload

1. Open `SplitFlapGateway.ino` (must be in a folder named `SplitFlapGateway`)
2. Connect board via USB, select the correct COM port
3. Click **Upload**

After the first USB flash, all subsequent firmware updates can be done [over WiFi](#ota-firmware-updates).

---

## Serial Debug Output

The firmware outputs diagnostic messages at **115200 baud** via the native USB CDC port (requires USB CDC On Boot: Enabled).

| OS | Port |
|---|---|
| macOS | `/dev/cu.usbmodem*` (e.g. `/dev/cu.usbmodem114101`) |
| Windows | `COMx` — listed as "USB Serial Device" in Device Manager |
| Linux | `/dev/ttyACM0` |

**Always-on messages:**
```
[Boot] Split-Flap Gateway v2.0
[Boot] reset=PANIC heap=261540 psram=8388608 flash=16384KB sdk=v5.1.4
[MOD] Loaded 11 modules from FATFS (0 pruned as stale)
[WiFi] Connected IP=192.168.1.105
[MEM] monitor ring in PSRAM (18944 bytes)
[MEM] MQTT queue in PSRAM (26240 bytes)
[MEM] module registry in PSRAM (15300 bytes)
[MQTT] Connecting to broker:1883...
[WDG] up=30s heap=164484 min=84632 maxblk=118772 frag=28% stk(485/web/net/ota/rtc)=4724/4756/2684/2812/748 rx=12 tx=9 rej=0 wifi=1 ap=0 rssi=-58 mqtt=1 mods=11
```

The boot line's `reset=` field is the first thing to check after an unexpected reboot:

| reset reason | meaning |
|---|---|
| `POWERON` | normal power-up |
| `SW` | software reset (e.g. after an OTA flash or config change) |
| `PANIC` | firmware crash (stack overflow, null deref, assert) — check the backtrace above it |
| `TASK_WDT` / `INT_WDT` | a task or interrupt blocked too long |
| `BROWNOUT` | supply voltage dipped — a power problem, not firmware |

The `[MEM]` lines report where the three large runtime buffers landed — PSRAM when available (preferred, to keep internal RAM free for the WiFi/TCP stack during OTA), or internal RAM as a fallback.

The periodic `[WDG]` fields: `heap` (free now), `min` (lowest free heap ever — catches transient dips), `maxblk` (largest allocatable block), `frag` (heap fragmentation %; a high value with adequate `heap` warns of fragmentation before an allocation fails), `stk(...)` (per-task minimum-ever free stack in bytes — a value trending toward 0 predicts a stack-overflow crash), `rx`/`tx` (bus frame counters), `rej` (corrupt/garbled frames rejected at parse — a rising count signals bus collisions or wiring noise), `wifi` (station connected to your network), `ap` (fallback access point currently up — should be `0` whenever `wifi=1`), `rssi` (WiFi signal), and `mqtt` (broker connected). A stall reboot logs which task stalled and its age; a low-heap reboot logs the heap value.

**Debug messages** (enable in Settings → Serial Debug):
```
[RX] m5v:12:5:AABBCCDD  (2026-06-10 14:32:01)
[TX] m5-A
[MQTT->BUS] m9h
[NTP] Syncing (UTC) via pool.ntp.org...
[SF] rejecting corrupt version response for module 5 (sn:...)
[API] home module 9
[API] provision SN AABBCCDD -> ID 5
[CFG] MQTT broker set to 192.168.1.50:1883  prefix=splitflap
[CFG] NTP server set to time.google.com
[CFG] Timezone set to EST5EDT,M3.2.0,M11.1.0
```

---

## First Boot

With no WiFi configured, the gateway brings up a fallback Access Point so you can reach the setup page:

| Setting | Value |
|---|---|
| SSID | `Split-Flap-GW` |
| Password | `12345678` |
| Web UI | http://192.168.4.1 |

Connect to the AP, open the web UI, go to **Settings → WiFi**, enter your network credentials, and click **Save WiFi**. The gateway connects to your network and displays its IP in the status badge. NTP syncs automatically on first WiFi connection.

The Access Point is **fallback-only**: once the gateway is connected to your WiFi it runs station-only and the AP is shut down (so it can't be joined by mistake, and its resources stay free). The AP only comes back if the station connection is lost for more than ~20 seconds, and it drops again automatically when the connection returns. You can confirm the AP is off in the `[WDG]` serial log — `ap=0` whenever `wifi=1`.

---

## Web UI

Navigate to the gateway's IP address in any browser.

<p align="center">
<a href="screenshots/modules.png"><img src="screenshots/modules.png" width="260" alt="Modules tab"></a>
<a href="screenshots/modules_info.png"><img src="screenshots/modules_info.png" width="260" alt="Module info dialog with parsed EEPROM"></a>
<a href="screenshots/modules_manage.png"><img src="screenshots/modules_manage.png" width="260" alt="Module destructive-actions dialog"></a>
<a href="screenshots/display.png"><img src="screenshots/display.png" width="260" alt="Display tab"></a>
<a href="screenshots/provision.png"><img src="screenshots/provision.png" width="260" alt="Provision tab"></a>
<a href="screenshots/calibration.png"><img src="screenshots/calibration.png" width="260" alt="Calibration tab — module grid, in-place home offset and total steps with Count Steps, and the per-character map"></a>
<a href="screenshots/bus_monitor.png"><img src="screenshots/bus_monitor.png" width="260" alt="Bus Monitor tab"></a>
<a href="screenshots/settings.png"><img src="screenshots/settings.png" width="260" alt="Settings tab"></a>
<a href="screenshots/backups.png"><img src="screenshots/backups.png" width="260" alt="Backup tab"></a>
<a href="screenshots/status.png"><img src="screenshots/status.png" width="260" alt="Status tab"></a>
</p>


| Tab | Description |
|---|---|
| **Modules** | Grid of all known modules — ID, serial number, current character, firmware version, sorted by ID (unprovisioned modules last). The list is sticky: it persists across reboots and only drops modules not seen for 6 hours. Each provisioned module card carries three action icons: **⌂ Home** (homes the module), **ℹ Info** (opens a dialog that queries the module live — refreshing its firmware version and reading its EEPROM — and shows all known module data plus a parsed view of the EEPROM: home offset, steps/rev, and the calibrated flap map — and, for a module on firmware v31+, an editable **Flap Set** section for the active flap count and character set), and a red **🗑 trash** icon that opens a destructive-actions dialog offering Erase EEPROM, Factory Reset, or De-provision, each behind a confirmation prompt. Modules running firmware v7 or earlier are flagged **LEGACY** (they have no serial number, no provisioning, and no factory reset, but full homing and calibration support); their unsupported destructive actions are greyed out. **↻ Identify All** clears the list (memory and saved file) and broadcasts `m*v` so every module re-announces itself. |
| **Display** | A **Live Display** at the top renders what the wall is currently showing as a grid of split-flap cells, updating as characters change. Below it: send a text string across sequential modules by start ID, send a single character to one module (or broadcast to all), or send a specific flap index to a module |
| **Provision** | Discover unprovisioned modules, home by serial number to identify them physically, assign IDs, de-provision individually or all at once |
| **Calibration** | Fine-tune any module on the bus — known or not. The module picker is a grid matching your display layout (every position, IDs 0 to rows×cols−1), color-coded green (known), yellow (legacy v7), or dim (not yet seen); you can also type any ID directly. For the selected module you can edit and save **Home Offset** and **Total Steps** in place (each with Save and Revert-to-default), nudge the home offset live, and **Count Steps** (runs the calibrate command — the reel spins one revolution to measure steps/rev, then the measured value is written back). The character map shows every flap's current step position (custom EEPROM values in green, firmware defaults in grey); clicking one opens a tune dialog to GOTO-test a target step, fine-adjust it with nudge buttons, then Lock to EEPROM or Revert (which unsets the entry so the flap uses its default again). A guided **Calibration Wizard** steps through all 64 flaps one at a time. See the **[Module Calibration Guide](CALIBRATION_GUIDE.md)** for a full walkthrough. |
| **Backup** | Download a JSON backup of every module's EEPROM calibration (keyed by serial number), and restore calibration from a backup file. Restore matches modules by serial number; an option lets you also reassign module IDs from the backup. |
| **Bus Monitor** | Live decoded RS-485 traffic with timestamps shown in your browser's local timezone. Pause and auto-scroll preferences persist across visits, and **Download Log** saves the captured frames (up to 5000 lines) as a text file. A **Send Frame** box transmits an arbitrary frame; the gateway normalizes framing and trims trailing junk by default, with a **Raw** checkbox to send bytes verbatim for debugging. |
| **Settings** | WiFi credentials, MQTT broker, timezone, NTP server, display layout (rows x columns for the Live Display), serial debug toggle, OTA firmware update |
| **Status** | Grouped into Network, System Health, RS-485 Bus, and Clock sections. Shows uptime, frame counters, IP addresses, free heap, minimum-ever heap, lowest per-task stack headroom, MQTT state, RTC time, and NTP sync — with color-coded health indicators |

---

## Module List Persistence

The known-module list is **sticky**: it survives reboots and power cycles so the
dashboard isn't empty after a restart.

- Stored as a small file (`/modules.dat`) in the FATFS partition that already
  exists in the default partition scheme — no custom partition table or special
  configuration is needed.
- Only durable fields are saved (ID, serial number, provisioned flag, firmware
  version, and a last-seen wall-clock timestamp). Transient display state and
  EEPROM dumps are not persisted.
- Entries not seen for **6 hours** are checked at boot and once a minute. Rather
  than dropping a stale module outright, the gateway first sends it a version
  query (`m<id>v`) and waits a few seconds — modules only speak when addressed,
  so "not seen" usually just means "nothing talked to it." Only if the module
  fails to answer the probe is it removed. The 6-hour window uses the
  battery-backed RTC clock, so it measures real elapsed time across reboots.
- If the gateway boots with an **empty registry** (first boot, or after Identify
  All), it broadcasts `m*v` once at startup so every module on the bus reports
  its version and serial and repopulates the list automatically.
- Saves are written only when the list actually changes (a module appears, is
  provisioned, gets its firmware version recorded, or is deprovisioned) and are
  debounced to limit flash wear.
- The **↻ Identify All** button (or `POST /api/flap/identify`) wipes both the
  in-memory list and the saved file, then broadcasts `m*v` to rebuild from
  scratch.

> [!NOTE]
> On the **first boot after flashing**, the firmware formats the FATFS partition
> (a one-time operation that adds a few seconds to boot — you'll see a
> `formatting now` message on the serial console). Every subsequent boot mounts
> instantly.

---

## Split-Flap Protocol

All bus frames are ASCII starting with `m`. On the wire, frames are newline-terminated — but you don't have to manage that yourself: the gateway normalizes framing for every send path (bus monitor, REST, MQTT), adding or stripping the trailing newline as needed and trimming any junk past a complete command. The one frame it intentionally sends *without* a terminator is the direct version query `m<id>v` (a gateway turnaround workaround — the module accepts either form). A **Raw** option bypasses all of this for debugging. See the note under the command table for details.

### Commands (gateway → modules)

| Frame | Description |
|---|---|
| `m<id>-<char>\n` | Show character (e.g. `m5-A\n`) |
| `m<id>+<index>\n` | Show flap by index 0–63 (firmware v7+) |
| `m<id>h\n` | Home the module |
| `m<id>c\n` | Calibrate (measure steps/rev) |
| `m<id>v` | Query firmware version — **sent without a trailing newline** (see note below) |
| `m<id>d\n` | Dump EEPROM configuration |
| `m<id>A\n` | Combined all-fields dump — version + EEPROM in one reply (firmware v25+) |
| `m<id>o<n>\n` | Set home offset (steps past Hall trigger to flap 0) |
| `m<id>t<n>\n` | Set total steps per revolution |
| `m<id>s<n>\n` | Nudge forward n steps and add to home offset |
| `m<id>g<n>\n` | Go to raw step position n |
| `m<id>w<i>:<p>\n` | Write calibrated position p for flap index i |
| `m<id>a<n>\n` | Set auto-home flag (1=home on boot, 0=restore saved position) |
| `m<id>e\n` | Erase calibrated flap position map |
| `m<id>i<n>\n` | Set module ID to n |
| `m<id>N<count>:<chars>\n` | Configure flap count (1–64) and/or character set; both parts optional and independent (firmware v31+) |
| `m<id>R\n` | Reset — erase stored ID, return to unprovisioned (firmware v9+) |
| `m<id>F\n` | Factory reset EEPROM defaults (preserves module ID) |
| `m*h\n` | Broadcast home (all modules) |
| `m*N<count>:<chars>\n` | Broadcast flap-set config — set a whole panel at once (firmware v31+) |
| `m*v\n` / `m*v<lo>-<hi>\n` | Broadcast version query (all modules, or an ID range; firmware v25+ for the range) |
| `m*A\n` / `m*A<lo>-<hi>\n` | Broadcast combined all-fields dump (staggered; prefer ranged batches — each reply is long; firmware v25+) |
| `mXI<sn>:<id>\n` | Assign ID to module by serial number |
| `mXH<sn>\n` | Home module by serial number |
| `mXD<sn>\n` | Dump EEPROM by serial number (firmware v15+) |
| `mXA<sn>\n` | Combined all-fields dump by serial number (firmware v25+) |
| `mXN<sn>:<count>:<chars>\n` | Configure flap set by serial number (firmware v31+) |
| `mXF<sn>\n` | Factory reset by serial number |
| `mXW<sn>:<offset>:<steps>:<map>[:<flapCount>:<flapChars>]\n` | Restore EEPROM to module by serial number; the optional flap-set tail (firmware v31+) round-trips an `A` dump |

> **Framing and sanitization are handled for you.** Whether a frame comes from the bus monitor, `POST /api/rs485/send`, or the MQTT `splitflap/send` topic, the gateway normalizes it at a single transmit choke point. It (1) strips any trailing CR/LF you supplied, (2) **trims anything past a complete, well-formed known command** — so `m4vDSassa` is sent as `m4v` rather than relying on the module to ignore the junk — and (3) re-adds exactly one `\n` terminator, so `m5-A` and `m5-A\n` behave identically and a payload command like `m9o2832` is terminated for you (which also spares it the module's 50 ms idle-timeout wait). By-serial `mX…` frames (e.g. a long restore map) and any command the gateway doesn't model are passed through unchanged, so a legitimate frame is never truncated. The **one exception** to terminating is a direct numeric-id version query `m<id>v`, which the gateway always ships **without** a terminator. This is **not** a module requirement — the module accepts both `m<id>v` and `m<id>v\n` and answers either; it acts on the `v` byte itself. The bare form works around a **gateway-side** limitation in its half-duplex bus turnaround: a module answers a direct version query synchronously, the instant it parses `v` (a dump, by contrast, assembles its EEPROM string first, so its reply is naturally late enough to be safe), but the gateway's hardware driver-enable keeps it driving the line until the whole frame has clocked out, and its receiver is off while it transmits — so a trailing `\n` would keep the gateway transmitting for one extra byte-time (~1 ms at 9600 baud) *after* the module has already started replying, and the reply's leading bytes would be lost. Shipping `m<id>v` bare lets the gateway release the bus and start listening before the module answers. The **broadcast** `m*v` keeps a newline (the gateway adds it): a wildcard query collects an optional `<lo>-<hi>` ID range and the module fires a *staggered, deferred* reply on the newline (or a 50 ms idle timeout), so there's no turnaround race there.
>
> **Raw bypass (debugging).** To send bytes exactly as typed — no trimming, no terminator changes — enable **Raw** in the bus monitor's Send box, or pass `"raw":true` in the `/api/rs485/send` or `splitflap/send` JSON body. This is the escape hatch for deliberately exercising malformed or experimental frames.

### Responses (modules → gateway)

| Frame | Description |
|---|---|
| `m<id>v:<version>:<moduleId>:<serialNumber>\n` | Firmware version |
| `m<id>:<steps>\n` | Calibration result |
| `m<id>d:<homeOffset>:<stepsPerRev>:<map>\n` | EEPROM dump |
| `m<id>A:<version>:<moduleId>:<serialNumber>:<homeOffset>:<totalSteps>:<autoHome>:<curIndex>:<map>[:<flapCount>:<flapChars>]\n` | Combined all-fields dump (v25+): everything from `v` and `d`, plus the auto-home flag and the live current flap index. Firmware **v31+** appends the configured flap count and character set after the map (`flapChars` is the final field and may itself contain `:`/`,`/`=`); older firmware omits the tail and the gateway's parser ignores it on older boards |
| `mXadv:<serialNumber>\n` | Unprovisioned module advertisement |
| `mXack:<serialNumber>:<assignedId>\n` | Provisioning acknowledgement |

> **The gateway uses the combined `A` command automatically when it's more efficient.** When the Info dialog refreshes a module's full state it normally needs both a version query and a dump (two bus transactions). For a module known to be **firmware v25+**, the gateway issues a single `A` instead — one transaction that returns version, serial, and the full EEPROM dump together. Older modules (or a module whose version isn't known yet) fall back to the classic version-then-dump sequence. This is handled internally via the `/api/flap/all` endpoint; no client change is required.

### Character set (64 flaps, index 0–63)

The split-flap character set is owned by the **module firmware**, not the gateway. When you send `m<id>-<char>`, the gateway transmits the character byte verbatim (after uppercasing it and rejecting non-printable/non-ASCII bytes); the module maps it to the correct flap index itself. The gateway only deals in flap indices when you explicitly send `m<id>+<index>`. The table below is the module's **default** flap order, provided for reference when interpreting EEPROM dumps:

```
 0  SPACE    1  A    2  B    3  C    4  D    5  E    6  F    7  G
 8  H        9  I   10  J   11  K   12  L   13  M   14  N   15  O
16  P       17  Q   18  R   19  S   20  T   21  U   22  V   23  W
24  X       25  Y   26  Z   27  0   28  1   29  2   30  3   31  4
32  5       33  6   34  7   35  8   36  9   37  !   38  @   39  #
40  $       41  &   42  (   43  )   44  -   45  +   46  =   47  ;
48  q       49  :   50  %   51  '   52  .   53  ,   54  /   55  ?
56  *       57  r   58  o   59  y   60  g   61  b   62  p   63  w
```

On **firmware v31+** both the active flap count (1–64) and this character order are **configurable per module** at runtime via the `N` command, and persist in the module's EEPROM. For a v31+ module the Module Info dialog shows a **Flap Set** section with the live values and an inline editor (`POST /api/flap/flapconfig`); a whole panel can be set at once with a broadcast (`id:-1`). The section is shown **only** for v31+ modules — on older firmware it is omitted and the fixed 64-flap default set above is used. The configured count and characters are also reported at the end of the `A` dump and are captured in backups so a restore round-trips them.

---

## REST API

Base URL: `http://<gateway-ip>`
All `POST` endpoints accept `Content-Type: application/json`.

### RS-485 Bus

| Method | Endpoint | Body | Description |
|---|---|---|---|
| `GET` | `/api/rs485/messages` | — | Drain buffered frames (up to 64) |
| `POST` | `/api/rs485/send` | `{"data":"m5-A\n"}` (optional `"raw":true`) | Send raw ASCII frame. Framing/junk is normalized by default; `"raw":true` sends bytes verbatim |

### Module Control

| Method | Endpoint | Body | Description |
|---|---|---|---|
| `GET` | `/api/flap/modules` | — | List all known modules |
| `POST` | `/api/flap/char` | `{"id":5,"char":"A"}` | Show character (`id:-1` = all) |
| `POST` | `/api/flap/index` | `{"id":5,"index":1}` | Show flap by index 0–63 |
| `POST` | `/api/flap/text` | `{"text":"HELLO","start":0}` | Send text across sequential modules |
| `POST` | `/api/flap/home` | `{"id":5}` | Home module (`id:-1` = all) |
| `POST` | `/api/flap/version` | `{"id":5}` | Query version — waits up to 500ms, returns `{ok,id,ver,sn,stale,lastSeen}` |
| `POST` | `/api/flap/dump` | `{"id":5}` | Fetch EEPROM fresh — waits up to 500ms (1s if SN fallback needed), returns `{ok,id,sn,dump}` |
| `POST` | `/api/flap/all` | `{"id":5}` | Refresh version **and** EEPROM together, returns `{ok,id,ver,sn,dump,autoHome,curIndex,flapCount,flapChars,stale,mode}`. Uses one `A` transaction for v25+ modules (`mode:"A"`), else falls back to version+dump (`mode:"vd"`). `flapCount`/`flapChars` are populated only by firmware v31+ (`flapCount:-99`, `flapChars:""` otherwise) |
| `POST` | `/api/flap/identify` | — | Clear the list (memory + saved) and broadcast `m*v` to re-discover all modules |

### Advanced Module Commands

| Method | Endpoint | Body | Description |
|---|---|---|---|
| `POST` | `/api/flap/calibrate` | `{"id":5}` | Calibrate steps/rev — for a single module, waits up to ~15s for the reel to measure and returns `{ok,id,stepsPerRev}` (`504` on timeout); a broadcast (`id:-1`) is fire-and-forget |
| `POST` | `/api/flap/homeoffset` | `{"id":5,"steps":42}` | Set home offset |
| `POST` | `/api/flap/totalsteps` | `{"id":5,"steps":4096}` | Set total steps per revolution |
| `POST` | `/api/flap/nudge` | `{"id":5,"steps":10}` | Nudge forward n steps |
| `POST` | `/api/flap/goto` | `{"id":5,"step":100}` | Go to raw step position |
| `POST` | `/api/flap/writepos` | `{"id":5,"idx":1,"pos":210}` | Write calibrated position for flap |
| `POST` | `/api/flap/autohome` | `{"id":5,"enable":1}` | Set auto-home on boot |
| `POST` | `/api/flap/flapconfig` | `{"id":5,"flapCount":40,"charSet":" ABC…"}` | Configure flap count (1–64) and/or character set (firmware v31+); `flapCount` and `charSet` are independent and optional. Target by `id` (`id:-1` broadcasts to the whole panel) or by `sn`. No reply — read back with `/api/flap/all` |
| `POST` | `/api/flap/erase` | `{"id":5}` | Erase calibration map |
| `POST` | `/api/flap/factoryreset` | `{"id":5}` | Factory reset EEPROM |
| `POST` | `/api/flap/diag` | `{"id":5}` | Self-diagnostics (fw v26+): returns the stats snapshot `q` inline and starts the Hall sensor test — poll `/api/flap/diag/status` |
| `POST` | `/api/flap/diag/mech` | `{"id":5,"revs":10}` | Start the mechanical self-test (after the Hall test); optional `revs` 5–20 (fw v29+, default 5) |
| `GET` | `/api/flap/diag/status` | — | Poll the active motor test: `{ok,state,kind,...}` (`kind` hall/mech; `state` idle/pending/done/timeout; Hall `code` 0–4, mech `code` 0 OK/1 inconsistent/2 no motion) |

A module running **firmware v26 or newer** shows a 🩺 diagnostics icon in the Modules grid. Clicking it runs three self-tests in order and opens a results modal with plain-language interpretation. The **stats snapshot** (`Q`, instant, no motor) decodes the last reset cause from the RSTFR bits (power-on / brown-out / external / watchdog / software) and reports the boot counter, supply voltage, an EEPROM write-verify result, and the current flap index — flagging a sagging supply, a failed EEPROM verify, or a brown-out/watchdog reset (the supply-voltage check is rail-aware, so a healthy 3.3 V reading is not mistaken for a low 5 V rail). The **Hall sensor self-test** (`T`) then spins the reel ~2 revolutions to check the home sensor, reporting OK or one of stuck-active, stuck-inactive, multiple active regions, or inverted polarity. Finally the **mechanical self-test** (`M`) spins the motor several revolutions and reports OK, *inconsistent* (revolutions varied by more than 5 %, pointing to drag, a weak supply, or a failing driver), or *no motion* — alongside the per-revolution step counts, average magnet width, and motion-gate detail that distinguish a stalled motor from a dead sensor. On firmware v29+ the mechanical test takes a selectable **rotation count (5–20, default 5)**; more rotations catch intermittent faults that a short run misses. The Hall and mechanical tests are motor-driven, so they run as asynchronous jobs the UI polls; the snapshot is shown immediately.

### Provisioning

| Method | Endpoint | Body | Description |
|---|---|---|---|
| `POST` | `/api/flap/provision` | `{"sn":"AABBCCDD","id":5}` | Assign ID by serial number |
| `POST` | `/api/flap/deprovision` | `{"id":5}` | Reset to unprovisioned (`id:-1` = all) |
| `POST` | `/api/flap/homebysn` | `{"sn":"AABBCCDD"}` | Home by serial number |
| `POST` | `/api/flap/dumpbysn` | `{"sn":"AABBCCDD"}` | Dump EEPROM by serial number (fw v15+) |
| `POST` | `/api/flap/factoryresetbysn` | `{"sn":"AABBCCDD"}` | Factory reset by serial number |
| `POST` | `/api/flap/restorebysn` | `{"sn":"...","homeOffset":42,"totalSteps":4096,"map":"0=210,1=320,...","flapCount":40,"charSet":" ABC…"}` | Restore EEPROM by serial number. The optional `flapCount`/`charSet` (firmware v31+) restore the configured flap set so a backup round-trips; omit them to leave the module's flap set unchanged |

When a module acknowledges provisioning, the gateway marks it **provisioning-confirmed** and, after a brief settling delay, queries its firmware version (`m<id>v`) to record the version in the registry. The short delay matters: a version request sent the instant the ack arrives reaches the module before it has settled on its new ID, so no reply comes back; the query is also retried a few times until the version is read. A provisioning-confirmed module is never shown as **legacy** — legacy (v7) modules have no serial number and never acknowledge provisioning, so an ack is definitive proof the module is modern, regardless of whether its version has been read back yet. The Modules grid is always sorted by ID (unprovisioned modules last), so a newly provisioned module slots into its proper place rather than appearing at the end.

#### `/api/flap/version` and `/api/flap/dump` response format

```json
{ "ok": true,  "id": 5, "ver": "12", "sn": "AABBCCDD", "stale": false, "lastSeen": 45231 }
{ "ok": false, "error": "no response from module" }
```

`/api/flap/dump` returns data fetched fresh from the module each call (no cache). On timeout it returns `{ "ok": false, ... }`. The `stale` field is always `false` and retained only for response-shape compatibility. `/api/flap/version` includes `lastSeen`, the milliseconds-since-boot of the most recent module activity.

#### `/api/flap/calibrate` response format

```json
{ "ok": true, "id": 11, "stepsPerRev": 4097 }
{ "ok": false, "error": "No calibration response from module" }
```

For a single module the call blocks while the reel physically measures one revolution (up to ~15s), then returns the measured `stepsPerRev`. The module saves the value to its own EEPROM during calibration; callers typically follow up with `/api/flap/totalsteps` to confirm it. A broadcast (`id:-1`) returns `{ "ok": true, "broadcast": true }` immediately.

#### Module object and `lastSeenEpoch`

Each entry from `/api/flap/modules` includes `lastSeen` (millis-since-boot, resets on reboot) and `lastSeenEpoch` (RTC wall-clock epoch in seconds, which survives reboot; `0` until the clock is set). The UI uses `lastSeenEpoch` to show a human-readable "Last Seen" time. A provisioned module that has responded but reports no `fwVersion` is treated as a **legacy** (firmware v7 or earlier) module.

### Status and Configuration

| Method | Endpoint | Body | Description |
|---|---|---|---|
| `GET` | `/api/status` | — | Uptime, IP, MQTT, RTC time, NTP, heap, maintenance and quiet flags |
| `GET` | `/api/maintenance` | — | Returns `{ok,on}` — current maintenance-mode state |
| `POST` | `/api/mqtt/test` | `{"host":"...","port":1883,"user":"...","pass":"..."}` (all optional, defaults to saved config) | Test broker reachability + credentials without touching the live connection |
| `POST` | `/api/maintenance` | `{"on":true}` | Enable/disable maintenance mode (ignores external MQTT commands) |
| `GET` | `/api/quiet` | — | Returns `{ok,on}` — current quiet-time state |
| `POST` | `/api/quiet` | `{"on":true}` | Enable/disable quiet time (flaps stop moving for display updates; reels resync when disabled) |
| `GET` | `/api/config` | — | Current configuration (passwords excluded) |
| `POST` | `/api/config/wifi` | `{"ssid":"...","pass":"..."}` | WiFi credentials |
| `POST` | `/api/config/mqtt` | `{"host":"...","port":1883,"user":"...","pass":"...","prefix":"splitflap"}` | MQTT settings |
| `POST` | `/api/config/rs485` | `{"baud":9600,"dataBits":8,"parity":0,"stopBits":1}` | RS-485 bus parameters |
| `POST` | `/api/config/settings` | `{"posixTZ":"EST5EDT,..."}` or `{"serialDebug":true}` or `{"haEnabled":true}` or `{"otaPassword":"..."}` | Timezone, serial debug, Home Assistant integration, or OTA password |

### OTA Firmware Update

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/ota` | Browser-based firmware upload page |
| `POST` | `/api/ota/upload` | Upload the compiled application image `SplitFlapGateway.ino.bin` (multipart). See [OTA Firmware Updates](#ota-firmware-updates) for which file to use |

---

## OpenAPI Specification

The repository includes `openapi.yaml` — a complete [OpenAPI 3.1](https://spec.openapis.org/oas/v3.1.0) description of every REST endpoint.

### Using with Postman

1. Open Postman → click **Import**
2. Drag `openapi.yaml` into the import window
3. Postman creates a collection with all requests pre-configured
4. Set a **Collection Variable** named `baseUrl` to your gateway's IP, e.g. `http://192.168.1.105`

### Using with other tools

| Tool | How to import |
|---|---|
| **Swagger UI** | Paste at [editor.swagger.io](https://editor.swagger.io) for interactive docs with Try it out |
| **Insomnia** | Import → From File → select `openapi.yaml` |
| **VS Code REST Client** | Use the OpenAPI extension to generate `.http` files |

---

## MQTT

Default topic prefix: **`splitflap`** (configurable in Settings → MQTT).

### Published topics

| Topic | Payload | Description |
|---|---|---|
| `splitflap/rx` | `{"ts":...,"wt":"...","command":"m5-A"}` | Frame received from bus |
| `splitflap/tx` | `{"ts":...,"wt":"...","command":"m5-A"}` | Frame transmitted to bus |
| `splitflap/status` | `{"uptime":...,"rx":...,"tx":...,"rej":...,"modules":...,"time":"...","ntpSynced":true,"heap":...,"minheap":...,"maxblk":...,"frag":...,"rssi":...,"wifi":true,"stk485":...,"stkweb":...,"stknet":...,"stkota":...,"stkrtc":...,"ip":"...","url":"http://.../","version":"...","maintenance":false,"quiet":false}` | Heartbeat with the full `[WDG]` diagnostic set, published once per minute |
| `splitflap/flap/adv` | `"AABBCCDD..."` | Unprovisioned module advertisement |
| `splitflap/flap/ack` | `{"id":5,"sn":"..."}` | Provisioning acknowledgement |
| `splitflap/flap/version` | `{"id":5,"ver":"12","reportedId":5,"sn":"..."}` | Version response |
| `splitflap/flap/calibrated` | `{"id":5,"stepsPerRev":4096}` | Calibration result |
| `splitflap/flap/dump` | `{"id":5,"dump":"..."}` | EEPROM dump response |
| `splitflap/availability` | `online` / `offline` | Gateway availability (retained; `offline` set via MQTT Last Will). Used by Home Assistant |
| `splitflap/display/state` | `HELLO   WORLD` | Best-known display contents assembled from tracked flap characters (`?` = unknown). Published on change |
| `splitflap/maintenance/state` | `ON` / `OFF` | Maintenance mode state (Home Assistant switch) |
| `splitflap/quiet/state` | `ON` / `OFF` | Quiet time state (Home Assistant switch) |

> The `availability`, `display/state`, `maintenance/state`, and `quiet/state` topics, plus the Home Assistant discovery configs, are only published when Home Assistant integration is enabled on the Settings tab. The `rx`/`tx`/`status`/`flap/*` topics always publish when MQTT is connected.

### Subscribed topics

| Topic | Payload | Description |
|---|---|---|
| `splitflap/send` | `m9h\n` or `{"data":"m9h\n"}` (optional `"raw":true`) | Send raw ASCII frame to bus. Normalized by default; `"raw":true` sends verbatim |
| `splitflap/flap/set` | `{"id":5,"char":"A"}` | Show character |
| `splitflap/flap/home` | `{"id":5}` | Home module |
| `splitflap/flap/provision` | `{"sn":"AABBCC...","id":5}` | Provision module |
| `splitflap/flap/flapconfig` | `{"id":5,"flapCount":40,"charSet":" ABC…"}` | Configure flap count and/or character set (firmware v31+); target by `id` (`id:-1` broadcasts) or `sn`; both fields independent and optional |
| `splitflap/display/set` | `HELLO` | Show a plain string across the display from module 0 (Home Assistant text entity) |
| `splitflap/maintenance/set` | `ON` / `OFF` / `true` / `1` | Set maintenance mode (reachable even while maintenance is on) |
| `splitflap/quiet/set` | `ON` / `OFF` / `true` / `1` | Set quiet time |

### Home Assistant

Enable **Home Assistant integration** on the Settings tab (MQTT must be configured). On connect the gateway publishes retained [MQTT discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) configs under `homeassistant/<component>/sfgw_<chip-id>/...`, so Home Assistant auto-creates a single device with:

- a **Display** text entity (set/read the display string),
- **Maintenance Mode** and **Quiet Time** switches,
- diagnostic **sensors** mirroring the `[WDG]` heartbeat — modules, uptime, free/min heap, largest free block, fragmentation %, WiFi signal, frames received/sent, parse rejects, the five per-task stack watermarks, IP address, firmware version, and a clickable **Gateway URL**.

Availability is backed by an MQTT Last Will, so entities are marked unavailable if the gateway drops off. Turning the integration off publishes empty discovery payloads to remove the entities cleanly.

---

## Time Configuration

Select your region in **Settings → Timezone**, optionally set a custom **NTP Server**, and click **Save Time Settings**. Both take effect immediately — no reboot required; the clock re-syncs against the configured server on the next network tick.

The **NTP server** defaults to `pool.ntp.org`. You can point it at a LAN time server (e.g. your router) or an alternative public pool (e.g. `time.google.com`, `time.cloudflare.com`) if you prefer. Leaving the field blank restores the default.

| Region | POSIX TZ string |
|---|---|
| US Eastern (UTC-5/-4) | `EST5EDT,M3.2.0,M11.1.0` |
| US Pacific (UTC-8/-7) | `PST8PDT,M3.2.0,M11.1.0` |
| London (UTC+0/+1) | `GMT0BST,M3.5.0/1,M10.5.0` |
| Paris / Berlin (UTC+1/+2) | `CET-1CEST,M3.5.0,M10.5.0/3` |
| Sydney (UTC+10/+11) | `AEST-10AEDT,M10.1.0,M4.1.0/3` |
| Tokyo (UTC+9) | `JST-9` |

---

## OTA Firmware Updates

After the initial USB flash, all subsequent updates can be done over WiFi.

> **Upgrading from a pre-1.6 version?** The firmware *currently running* is what receives and writes the new image, so the OTA-reliability improvements introduced in 1.6 (PSRAM-backed buffers, AP-drop and MQTT-pause during upload) can't help the upload that *first* installs a 1.6-or-later build — they only take effect once that build is running. On a memory-constrained pre-1.6 build the browser upload may drop near the end (heap exhaustion); this is harmless (the image is written to the inactive partition, so a failed upload leaves your old firmware running and fully functional), but for that first jump it's more reliable to use **espota** or a one-time **USB flash**. Once you're on 1.6 or later, the web updater is reliable. No migration is needed: PSRAM is set up at boot, and your saved settings and module registry carry over (the Home Assistant option simply defaults to off).

### Browser upload (recommended)

1. In Arduino IDE: **Sketch → Export Compiled Binary** (Ctrl/Cmd+Alt+S). The files land in a `build/<board-fqbn>/` subfolder next to your sketch.
2. From that folder, choose **`SplitFlapGateway.ino.bin`** — the plain application image. **This is the only file to upload over OTA.**
3. Open the gateway web UI → **Settings** → click **Open Firmware Updater →**
4. Select `SplitFlapGateway.ino.bin` and click **Upload Firmware**
5. The gateway reboots automatically on success. Confirm the new build by checking the version badge in the header (e.g. **v2.0**).

> **Which file?** The compiler emits several files alongside the app image — pick the right one:
>
> | File | Use for OTA? | What it is |
> |---|---|---|
> | `SplitFlapGateway.ino.bin` | ✅ **Yes** | The application image — what the OTA updater writes to the app partition |
> | `SplitFlapGateway.ino.merged.bin` | ❌ No | Full-flash image (bootloader + partitions + app) for flashing at offset 0x0 with esptool — **not** an OTA image |
> | `SplitFlapGateway.ino.bootloader.bin` | ❌ No | Bootloader; USB flash only |
> | `SplitFlapGateway.ino.partitions.bin` | ❌ No | Partition table; USB flash only |
> | `SplitFlapGateway.ino.elf` / `.map` | ❌ No | Debug symbols / linker map — not firmware |
>
> The common mistake is grabbing `*.merged.bin`. For OTA you always want the plain `*.ino.bin`.

### Arduino IDE / command line

As an alternative to the browser upload, you can flash over the network with `espota`. The gateway advertises as `splitflap-gw`:

```bash
# Arduino IDE: Tools → Port → Network ports → splitflap-gw

# Command line:
python3 -m espota -i 192.168.1.105 -f SplitFlapGateway.ino.bin

# With OTA password set:
python3 -m espota -i 192.168.1.105 -a yourpassword -f SplitFlapGateway.ino.bin
```

---

## Default Settings

| Setting | Value |
|---|---|
| AP SSID | `Split-Flap-GW` |
| AP Password | `12345678` |
| RS-485 baud | 9600 |
| MQTT port | 1883 |
| MQTT topic prefix | `splitflap` |
| NTP server | `pool.ntp.org` |
| Timezone | UTC |
| Serial debug | Disabled |
| OTA password | None |

---

## License

[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

You are free to share and adapt this project for non-commercial purposes, as long as you give appropriate credit and distribute any derivatives under the same license.
