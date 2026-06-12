# Split-Flap Gateway

> [!NOTE]
> **New to this project?** Read the [blog post](BLOG.md) for the full story — why it exists, how it works, and how it fits into the split-flap display ecosystem.

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
- **Module management** — discover, provision, home, calibrate, and deprovision split-flap modules (up to 200)
- **Sticky module list** — the known-module registry persists across reboots in flash; entries not seen for 6 hours are pruned automatically
- **EEPROM inspector** — click any module card to fetch and display its EEPROM configuration on demand
- **Real-time bus monitor** — live RS-485 traffic with protocol decoding and local timestamps
- **NTP time sync** — RTC backed by PCF85063; syncs from `pool.ntp.org` on WiFi connect, persists through power loss
- **DST-aware timezone** — POSIX TZ string support covering 20 regions, takes effect immediately
- **MQTT integration** — publishes bus frames and module events; subscribes to control topics
- **REST API** — full JSON API with 31 endpoints for programmatic control (OpenAPI spec included)
- **OTA updates** — flash new firmware over WiFi from a browser or Arduino IDE; no USB cable required after first flash
- **AP fallback** — creates its own access point when WiFi is unavailable
- **Configurable serial debug** — verbose logging toggled from the web UI Settings tab
- **Watchdog** — detects stalled FreeRTOS tasks and auto-reboots; emergency reboot on low heap

---

## Installation

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
[Boot] Split-Flap Gateway
[WiFi] Connected IP=192.168.1.105
[MQTT] Connecting to broker:1883...
[WDG] up=30s heap=187432 rx=0 tx=0 wifi=1 mqtt=1
```

**Debug messages** (enable in Settings → Serial Debug):
```
[RX] m5v:12:5:AABBCCDD  (2026-06-10 14:32:01)
[TX] m5-A
[MQTT->BUS] m9h
[API] show char 'A' on module 5
[API] home module 9
[API] provision SN AABBCCDD -> ID 5
[CFG] WiFi SSID set to 'MyNetwork'
[CFG] MQTT broker set to 192.168.1.50:1883  prefix=splitflap
[CFG] Timezone set to EST5EDT,M3.2.0,M11.1.0
[CFG] Serial debug enabled
```

---

## First Boot

On first boot the gateway starts in Access Point mode:

| Setting | Value |
|---|---|
| SSID | `Split-Flap-GW` |
| Password | `12345678` |
| Web UI | http://192.168.4.1 |

Connect to the AP, open the web UI, go to **Settings → WiFi**, enter your network credentials, and click **Save WiFi**. The gateway reconnects and displays its IP in the status badge. NTP syncs automatically on first WiFi connection.

---

## Web UI

Navigate to the gateway's IP address in any browser.

| Tab | Description |
|---|---|
| **Modules** | Grid of all known modules — ID, serial number, current character, firmware version. The list is sticky: it persists across reboots and only drops modules not seen for 6 hours. Click any module to fetch and inspect its EEPROM configuration (fetched fresh each time). **↻ Identify All** clears the list (memory and saved file) and broadcasts `m*v` so every module re-announces itself. |
| **Display** | Send a text string across sequential modules by start ID, or send a specific flap index to a module |
| **Provision** | Discover unprovisioned modules, home by serial number to identify them physically, assign IDs, de-provision individually or all at once |
| **Bus Monitor** | Live RS-485 traffic with protocol-decoded descriptions and local-time timestamps |
| **Settings** | WiFi credentials, MQTT broker, timezone, serial debug toggle, OTA firmware update |
| **Status** | Uptime, frame counters, IP addresses, heap, MQTT state, RTC time, NTP sync |

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
- Entries not seen for **6 hours** are pruned automatically (checked at boot and
  once a minute). The 6-hour window uses the battery-backed RTC clock, so it
  measures real elapsed time across reboots.
- Saves are written only when the list actually changes (a module appears, is
  provisioned, or is deprovisioned) and are debounced to limit flash wear.
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

All bus frames are newline-terminated ASCII starting with `m`.

### Commands (gateway → modules)

| Frame | Description |
|---|---|
| `m<id>-<char>\n` | Show character (e.g. `m5-A\n`) |
| `m<id>+<index>\n` | Show flap by index 0–63 (firmware v7+) |
| `m<id>h\n` | Home the module |
| `m<id>c\n` | Calibrate (measure steps/rev) |
| `m<id>v\n` | Query firmware version |
| `m<id>d\n` | Dump EEPROM configuration |
| `m<id>o<n>\n` | Set home offset (steps past Hall trigger to flap 0) |
| `m<id>t<n>\n` | Set total steps per revolution |
| `m<id>s<n>\n` | Nudge forward n steps and add to home offset |
| `m<id>g<n>\n` | Go to raw step position n |
| `m<id>w<i>:<p>\n` | Write calibrated position p for flap index i |
| `m<id>a<n>\n` | Set auto-home flag (1=home on boot, 0=restore saved position) |
| `m<id>e\n` | Erase calibrated flap position map |
| `m<id>i<n>\n` | Set module ID to n |
| `m<id>R\n` | Reset — erase stored ID, return to unprovisioned (firmware v9+) |
| `m<id>F\n` | Factory reset EEPROM defaults (preserves module ID) |
| `m*h\n` | Broadcast home (all modules) |
| `m*v\n` | Broadcast version query (all modules) |
| `mXI<sn>:<id>\n` | Assign ID to module by serial number |
| `mXH<sn>\n` | Home module by serial number |
| `mXD<sn>\n` | Dump EEPROM by serial number (firmware v15+) |
| `mXF<sn>\n` | Factory reset by serial number |
| `mXW<sn>:<offset>:<steps>:<map>\n` | Restore EEPROM to module by serial number |

### Responses (modules → gateway)

| Frame | Description |
|---|---|
| `m<id>v:<version>:<moduleId>:<serialNumber>\n` | Firmware version |
| `m<id>:<steps>\n` | Calibration result |
| `m<id>d:<homeOffset>:<stepsPerRev>:<map>\n` | EEPROM dump |
| `mXadv:<serialNumber>\n` | Unprovisioned module advertisement |
| `mXack:<serialNumber>:<assignedId>\n` | Provisioning acknowledgement |

### Character set (64 flaps, index 0–63)

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

---

## REST API

Base URL: `http://<gateway-ip>`
All `POST` endpoints accept `Content-Type: application/json`.

### RS-485 Bus

| Method | Endpoint | Body | Description |
|---|---|---|---|
| `GET` | `/api/rs485/messages` | — | Drain buffered frames (up to 64) |
| `POST` | `/api/rs485/send` | `{"data":"m5-A\n"}` | Send raw ASCII frame |

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
| `POST` | `/api/flap/identify` | — | Clear the list (memory + saved) and broadcast `m*v` to re-discover all modules |

### Advanced Module Commands

| Method | Endpoint | Body | Description |
|---|---|---|---|
| `POST` | `/api/flap/calibrate` | `{"id":5}` | Calibrate steps/rev |
| `POST` | `/api/flap/homeoffset` | `{"id":5,"steps":42}` | Set home offset |
| `POST` | `/api/flap/totalsteps` | `{"id":5,"steps":4096}` | Set total steps per revolution |
| `POST` | `/api/flap/nudge` | `{"id":5,"steps":10}` | Nudge forward n steps |
| `POST` | `/api/flap/goto` | `{"id":5,"step":100}` | Go to raw step position |
| `POST` | `/api/flap/writepos` | `{"id":5,"idx":1,"pos":210}` | Write calibrated position for flap |
| `POST` | `/api/flap/autohome` | `{"id":5,"enable":1}` | Set auto-home on boot |
| `POST` | `/api/flap/erase` | `{"id":5}` | Erase calibration map |
| `POST` | `/api/flap/factoryreset` | `{"id":5}` | Factory reset EEPROM |

### Provisioning

| Method | Endpoint | Body | Description |
|---|---|---|---|
| `POST` | `/api/flap/provision` | `{"sn":"AABBCCDD","id":5}` | Assign ID by serial number |
| `POST` | `/api/flap/deprovision` | `{"id":5}` | Reset to unprovisioned (`id:-1` = all) |
| `POST` | `/api/flap/homebysn` | `{"sn":"AABBCCDD"}` | Home by serial number |
| `POST` | `/api/flap/dumpbysn` | `{"sn":"AABBCCDD"}` | Dump EEPROM by serial number (fw v15+) |
| `POST` | `/api/flap/factoryresetbysn` | `{"sn":"AABBCCDD"}` | Factory reset by serial number |
| `POST` | `/api/flap/restorebysn` | `{"sn":"...","homeOffset":42,"totalSteps":4096,"map":"0=210,1=320,..."}` | Restore EEPROM by serial number |

#### `/api/flap/version` and `/api/flap/dump` response format

```json
{ "ok": true,  "id": 5, "ver": "12", "sn": "AABBCCDD", "stale": false, "lastSeen": 45231 }
{ "ok": false, "error": "no response from module" }
```

`/api/flap/dump` returns data fetched fresh from the module each call (no cache). On timeout it returns `{ "ok": false, ... }`. The `stale` field is always `false` and retained only for response-shape compatibility. `/api/flap/version` includes `lastSeen`, the milliseconds-since-boot of the most recent module activity.

### Status and Configuration

| Method | Endpoint | Body | Description |
|---|---|---|---|
| `GET` | `/api/status` | — | Uptime, IP, MQTT, RTC time, NTP, heap |
| `GET` | `/api/config` | — | Current configuration (passwords excluded) |
| `POST` | `/api/config/wifi` | `{"ssid":"...","pass":"..."}` | WiFi credentials |
| `POST` | `/api/config/mqtt` | `{"host":"...","port":1883,"user":"...","pass":"...","prefix":"splitflap"}` | MQTT settings |
| `POST` | `/api/config/rs485` | `{"baud":9600,"dataBits":8,"parity":0,"stopBits":1}` | RS-485 bus parameters |
| `POST` | `/api/config/settings` | `{"posixTZ":"EST5EDT,..."}` or `{"serialDebug":true}` or `{"otaPassword":"..."}` | Timezone, serial debug, or OTA password |

### OTA Firmware Update

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/ota` | Browser-based firmware upload page |
| `POST` | `/api/ota/upload` | Upload compiled `.bin` file (multipart) |

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
| `splitflap/status` | `{"uptime":...,"rx":...,"tx":...,"modules":...,"time":"...","ntpSynced":true,"heap":...}` | Periodic heartbeat |
| `splitflap/flap/adv` | `"AABBCCDD..."` | Unprovisioned module advertisement |
| `splitflap/flap/ack` | `{"id":5,"sn":"..."}` | Provisioning acknowledgement |
| `splitflap/flap/version` | `{"id":5,"ver":"12","reportedId":5,"sn":"..."}` | Version response |
| `splitflap/flap/calibrated` | `{"id":5,"stepsPerRev":4096}` | Calibration result |
| `splitflap/flap/dump` | `{"id":5,"dump":"..."}` | EEPROM dump response |

### Subscribed topics

| Topic | Payload | Description |
|---|---|---|
| `splitflap/send` | `m9h\n` or `{"data":"m9h\n"}` | Send raw ASCII frame to bus |
| `splitflap/flap/set` | `{"id":5,"char":"A"}` | Show character |
| `splitflap/flap/home` | `{"id":5}` | Home module |
| `splitflap/flap/provision` | `{"sn":"AABBCC...","id":5}` | Provision module |

---

## Timezone Configuration

Select your region in **Settings → Timezone** and click **Save Timezone**. Takes effect immediately — no reboot required.

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

### Browser upload (recommended)

1. In Arduino IDE: **Sketch → Export Compiled Binary** → saves `SplitFlapGateway.ino.bin`
2. Open the gateway web UI → **Settings** → click **Open Firmware Updater →**
3. Select the `.bin` file and click **Upload Firmware**
4. The gateway reboots automatically on success

### Arduino IDE / command line

The gateway advertises as `splitflap-gw` on the network:

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
