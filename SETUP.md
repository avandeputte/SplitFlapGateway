# Build & Flash Guide — VS Code + PlatformIO

This guide walks you through everything needed to build the Split-Flap **Gateway**
firmware and load it onto the board, starting from a clean computer:

1. [Install Visual Studio Code](#1-install-visual-studio-code)
2. [Install the PlatformIO IDE extension](#2-install-the-platformio-ide-extension)
3. [Download (get) the project](#3-download-get-the-project)
4. [Open the project in VS Code](#4-open-the-project-in-vs-code)
5. [Build (compile) the firmware](#5-build-compile-the-firmware)
6. [Connect the board and find its port](#6-connect-the-board-and-find-its-port)
7. [Upload (flash) over USB](#7-upload-flash-over-usb)
8. [Open the serial monitor](#8-open-the-serial-monitor)
9. [First-time configuration](#9-first-time-configuration)
10. [Later updates over Wi-Fi (OTA)](#10-later-updates-over-wi-fi-ota)
11. [Command-line quick reference](#11-command-line-quick-reference)
12. [Troubleshooting](#12-troubleshooting)

> **Just want to flash a prebuilt binary?** You don't need any of this — a
> precompiled image is on the Releases page. Jump to
> **[Flash a prebuilt binary (no build environment needed)](#flash-a-prebuilt-binary-no-build-environment-needed)**.

> **What is this?** The gateway is the ESP32-S3 controller that drives the RS-485
> split-flap bus and serves the web UI. This is a [PlatformIO](https://platformio.org/)
> project (see `platformio.ini`). PlatformIO downloads the ESP32 toolchain and every
> required library **automatically** on the first build — you do not install board
> packages or libraries by hand.
>
> Building from the **Arduino IDE is no longer supported** — there is no `.ino` sketch,
> and all board settings live in `platformio.ini`. This guide is the supported path.

> **Target hardware:** Waveshare **ESP32-S3-RS485-CAN** (ESP32-S3, 16 MB flash,
> OPI PSRAM), connected to your computer with a **USB-C data cable** (some cables
> are charge-only — if the board never appears as a serial port, try another cable).

---

## Flash a prebuilt binary (no build environment needed)

If you just want to run the firmware and don't care about compiling from source, you
**don't need VS Code, PlatformIO, or any of the steps below**. A precompiled image is
published on the project's **[Releases](https://github.com/avandeputte/SplitFlapGateway/releases)**
page — download it and flash it directly. Pick the case that matches your board:

- **The board already runs this gateway firmware (just updating)** — download the
  **app image** (e.g. `firmware.bin`) and upload it through the gateway's built-in
  web updater: web UI ▸ **Settings ▸ Open Firmware Updater →** (or browse to
  `http://<gateway-ip>/ota`). No tools to install at all. See
  [Later updates over Wi-Fi (OTA)](#10-later-updates-over-wi-fi-ota).

- **A blank / brand-new board (first flash, over USB)** — download the **full-flash
  image** (e.g. `firmware.factory.bin`, which bundles the bootloader + partition
  table + app) and write it with a **browser-based flasher** — nothing to install:
  1. Open the Espressif **ESP web flasher**: <https://espressif.github.io/esptool-js/>
  2. Plug the board in with a USB-C **data** cable and click **Connect**, then choose
     its serial port.
  3. Add the file at **flash address `0x0`**, select `firmware.factory.bin`, and click
     **Program**.
  4. If it won't start programming, put the board in download mode first: hold
     **BOOT**, tap **RESET**, release **BOOT**, then retry.

  Prefer a terminal? [`esptool`](https://github.com/espressif/esptool) does the same:
  ```bash
  esptool --chip esp32s3 write_flash 0x0 firmware.factory.bin
  ```
  > Use `firmware.factory.bin` (full flash) at `0x0` for a blank board. The plain
  > `firmware.bin` is the **app-only** image — that one is for the OTA updater, not a
  > bare-board USB flash.

After flashing, continue from [§9 First-time configuration](#9-first-time-configuration).

**Everything below is only needed if you want to build from source.**

---

## 1. Install Visual Studio Code

Download and install VS Code for your operating system:

- **All platforms:** <https://code.visualstudio.com/Download>
  - **Windows:** run the `VSCodeUserSetup-x64-*.exe` installer.
  - **macOS:** download the `.zip`, unzip, and drag **Visual Studio Code.app** into
    `/Applications`. (Or `brew install --cask visual-studio-code`.)
  - **Linux:** install the `.deb`/`.rpm`, or `sudo snap install code --classic`.

Launch VS Code once to confirm it opens.

---

## 2. Install the PlatformIO IDE extension

1. In VS Code, open the **Extensions** view: click the squares icon in the left
   Activity Bar, or press **Ctrl+Shift+X** (**Cmd+Shift+X** on macOS).
2. Search for **PlatformIO IDE**.
3. Click **Install** on the entry published by **PlatformIO**.
4. Wait for it to finish — the first install downloads a small Python core and can
   take a few minutes. When prompted, allow it to install and **reload the window**.
5. You'll know it's ready when a small **PlatformIO** (alien-head 👽) icon appears in
   the left Activity Bar and a row of icons (✓ build, → upload, 🔌 monitor, 🗑 clean)
   appears in the bottom status bar.

> PlatformIO needs internet access on the first build to download the ESP32
> platform and libraries. After that, builds work offline.

---

## 3. Download (get) the project

Pick **one** of the following.

### Option A — Clone with Git (recommended; makes updates easy)

1. Install Git if you don't have it: <https://git-scm.com/downloads>.
2. Open a terminal and run:
   ```bash
   git clone https://github.com/avandeputte/SplitFlapGateway.git
   ```
3. The gateway firmware lives in the versioned subfolder **`SplitFlapGateway/2.0`**.

### Option B — Download a ZIP

1. On the project's GitHub page, click **Code ▸ Download ZIP**.
2. Unzip it somewhere permanent (not your Downloads folder).

> **Avoid OneDrive / iCloud / Dropbox synced folders.** Cloud sync can lock files
> mid-build and cause random compile errors. Use a local path such as
> `~/Projects/SplitFlapGateway` or `C:\dev\SplitFlapGateway`.

---

## 4. Open the project in VS Code

PlatformIO identifies a project by the folder that **directly contains
`platformio.ini`**. For this repo that is the **`2.1`** folder.

1. In VS Code: **File ▸ Open Folder…**
2. Select the **`SplitFlapGateway/2.0`** folder (the one with `platformio.ini`,
   `src/`, and this `SETUP.md` inside it).
   - ⚠️ Do **not** open the parent `SplitFlapGateway` folder — PlatformIO won't find
     `platformio.ini` there and the toolbar will be inactive.
3. If VS Code asks "Do you trust the authors of the files in this folder?", choose
   **Yes, I trust the authors**.
4. The first time you open it, PlatformIO indexes the project (bottom-right shows
   "Configuring project…"). Let it finish.

---

## 5. Build (compile) the firmware

This compiles the code and, on the first run, downloads the ESP32 toolchain and the
two external libraries (`PubSubClient`, `ArduinoJson`) declared in `platformio.ini`.

- Click the **✓ (Build)** icon in the bottom status bar, **or**
- Open the PlatformIO sidebar (👽) ▸ **esp32s3_devmodule ▸ General ▸ Build**, **or**
- Press **Ctrl+Alt+B**.

The first build can take several minutes (toolchain + library download). A successful
build ends with a memory-usage table and:

```
========================= [SUCCESS] Took ... seconds =========================
```

The compiled images are written to `.pio/build/esp32s3_devmodule/` — most importantly
`firmware.bin` (the app image, used later for OTA).

> You do **not** need to change any board settings — every board option
> (PSRAM, 16 MB flash, the 3 MB-app/9.9 MB-FATFS partition scheme, USB mode,
> CPU speed, etc.) is already encoded in `platformio.ini`.

---

## 6. Connect the board and find its port

1. Plug the board into your computer with a **USB-C data cable**.
2. Identify the serial port:

   | OS | Port looks like | How to list |
   |---|---|---|
   | **macOS** | `/dev/cu.usbmodem*` (e.g. `/dev/cu.usbmodem114101`) | `ls /dev/cu.*` or `pio device list` |
   | **Linux** | `/dev/ttyACM0` or `/dev/ttyUSB0` | `pio device list` |
   | **Windows** | `COMx` (e.g. `COM7`), shown as "USB Serial Device" | Device Manager ▸ Ports, or `pio device list` |

   In VS Code you can run **PlatformIO ▸ Devices** (👽 sidebar) or, from the PlatformIO
   terminal, `pio device list`.

3. **Set the port.** `platformio.ini` currently pins a macOS port:
   ```ini
   upload_port  = /dev/cu.usbmodem114101
   monitor_port = /dev/cu.usbmodem114101
   ```
   Do one of the following so it matches **your** machine:
   - **Easiest / most portable:** comment both lines out (add a `;` at the start of
     each). PlatformIO then **auto-detects** the board:
     ```ini
     ; upload_port  = /dev/cu.usbmodem114101
     ; monitor_port = /dev/cu.usbmodem114101
     ```
   - **Or** replace the value with your actual port (e.g. `COM7` on Windows).

   > Tip: to keep these machine-specific tweaks out of the tracked file, you can put
   > them in a local, git-ignored `platformio_override.ini` instead.

4. **Linux only — serial permissions.** If uploads fail with "permission denied",
   add yourself to the `dialout` group once and log back in:
   ```bash
   sudo usermod -a -G dialout $USER
   ```

> **Drivers:** the ESP32-S3 uses **native USB** (it appears as a USB CDC serial
> device), so no CH340/CP210x driver is normally required on modern macOS, Linux, or
> Windows 10/11.

---

## 7. Upload (flash) over USB

This builds (if needed) and writes the firmware to the board.

- Click the **→ (Upload)** icon in the bottom status bar, **or**
- PlatformIO sidebar (👽) ▸ **esp32s3_devmodule ▸ General ▸ Upload**, **or**
- Press **Ctrl+Alt+U**.

PlatformIO compiles, then `esptool` connects at 921600 baud, erases the app region,
writes the image, and the board reboots into the new firmware. Success ends with:

```
Hard resetting via RTS pin...
========================= [SUCCESS] ...
```

### If the upload can't connect to the board

Most ESP32-S3 boards auto-enter the bootloader, but if `esptool` reports
*"Failed to connect / No serial data received / wrong boot mode"*, force it into
download mode manually:

1. Press and **hold** the **BOOT** button.
2. Tap (press and release) the **RESET / EN** button.
3. **Release BOOT.**
4. Start the Upload again.

After flashing, press **RESET** (or unplug/replug) to run the firmware normally.

> **First flash must be over USB.** Once a build is running, you can do every later
> update [over Wi-Fi](#10-later-updates-over-wi-fi-ota) — no cable needed.

---

## 8. Open the serial monitor

To watch boot logs and the periodic `[WDG]` diagnostics (115200 baud):

- Click the **🔌 (Serial Monitor)** icon in the bottom status bar, **or**
- PlatformIO sidebar (👽) ▸ **esp32s3_devmodule ▸ General ▸ Monitor**, **or**
- run `pio device monitor` in the PlatformIO terminal.

You should see something like:

```
[Boot] Split-Flap Gateway v3.2.0
[Boot] reset=... heap=... psram=8388608 flash=16384KB sdk=...
[WDG] up=30s heap=... mqtt=1 mods=11 ...
```

Press **Ctrl+C** to stop the monitor. (Only one program can hold the port at a time —
close the monitor before uploading, or just use Upload, which handles the port for you.)

---

## 9. First-time configuration

The board has no Wi-Fi credentials yet, so it starts its own access point.

1. On a phone or laptop, join the Wi-Fi network **`Split-Flap-GW`** (password
   **`12345678`**).
2. Open **<http://192.168.4.1>** in a browser.
3. Go to **Settings ▸ Wi-Fi**, enter your network's SSID and password, and **Save Wi-Fi**.
4. The gateway joins your network; its IP appears on the **Status** page (and in
   the serial log). After that it's reachable on your LAN at that IP or at
   **<http://splitflap-gw.local>** (mDNS).

For provisioning modules, calibration, MQTT, etc., see the [README](README.md) and
[CALIBRATION_GUIDE](CALIBRATION_GUIDE.md).

---

## 10. Later updates over Wi-Fi (OTA)

After the first USB flash you can update without a cable.

### A. Browser upload (simplest)

1. **Build** the firmware in VS Code (step 5).
2. The image you upload is **`.pio/build/esp32s3_devmodule/firmware.bin`**.
   - This is the plain **application image** — what the OTA updater writes to the app
     partition.
   - ⚠️ **Do not** upload `firmware.factory.bin` — that's the full-flash
     (bootloader + partitions + app) image for USB/esptool only, **not** an OTA image.
3. In the gateway web UI: **Settings ▸ Open Firmware Updater →** (or browse to
   `http://<gateway-ip>/ota`).
4. Select `firmware.bin`, click **Upload Firmware**.
5. The gateway reboots on success — confirm the version badge in the header.

> The image is written to the *inactive* partition, so a failed/interrupted OTA
> leaves your current firmware running and intact.

### B. OTA from the command line / PlatformIO (optional)

The board advertises over mDNS as `splitflap-gw`. You can push `firmware.bin` with
`espota`:

```bash
# from .pio/build/esp32s3_devmodule/
python3 -m espota -i splitflap-gw.local -f firmware.bin
# with an OTA password set in Settings:
python3 -m espota -i 192.168.1.105 -a yourpassword -f firmware.bin
```

To make PlatformIO's **Upload** target the board over Wi-Fi instead of USB, set in
`platformio.ini` (or a local override):

```ini
upload_protocol = espota
upload_port     = splitflap-gw.local   ; or the gateway IP
; upload_flags  = --auth=yourpassword  ; if an OTA password is set
```

---

## 11. Command-line quick reference

PlatformIO Core (`pio`) is installed with the extension. Open the PlatformIO
terminal (👽 sidebar ▸ **Quick Access ▸ Miscellaneous ▸ New Terminal**) and run these
from the `2.1` folder:

```bash
pio run                  # build (compile) the firmware
pio run -t upload        # build + flash over USB
pio run -t monitor       # open the serial monitor (115200)
pio run -t upload -t monitor   # flash, then immediately monitor
pio device list          # list serial ports / connected boards
pio run -t clean         # remove build artifacts
pio pkg update           # update platform + libraries
```

> If `pio` isn't on your PATH in a plain system terminal, use the bundled copy:
> macOS/Linux `~/.platformio/penv/bin/pio`, Windows
> `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`.

---

## 12. Troubleshooting

| Symptom | Fix |
|---|---|
| PlatformIO toolbar/icons are missing or greyed out | You opened the wrong folder. Open the **`2.1`** folder that contains `platformio.ini` (step 4). |
| First build fails downloading the platform/toolchain | Check internet access/proxy/VPN and rebuild; PlatformIO needs the network only on the first build. |
| Upload: *"could not open port" / "port is busy"* | Close the **Serial Monitor** (it holds the port), then upload. Only one program can use the port at a time. |
| Upload: *"Failed to connect to ESP32-S3"* | Force bootloader mode: hold **BOOT**, tap **RESET**, release **BOOT**, upload again (step 7). |
| No serial port appears at all | Use a **data** USB-C cable (not charge-only), try another port/cable, and confirm the board has power. |
| Wrong/auto port chosen | Set `upload_port`/`monitor_port` in `platformio.ini` to your port, or comment them out for auto-detect (step 6). |
| Linux: *"permission denied"* on the port | `sudo usermod -a -G dialout $USER`, then log out and back in (step 6). |
| Monitor shows nothing / garbage after boot | Ensure the monitor is at **115200** baud (it's preset in `platformio.ini`); press **RESET** to re-print the boot banner. |
| OTA upload rejected or bricks behavior | Make sure you uploaded **`firmware.bin`**, not `firmware.factory.bin` (step 10). |

---

Need the bigger picture (architecture, REST API, MQTT, module provisioning,
calibration)? See [README.md](README.md), [ARCHITECTURE.md](ARCHITECTURE.md), and
[CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md).
