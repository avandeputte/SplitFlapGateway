# Split-Flap Gateway — Virtual Serial Port Bridge (MQTT)

---

> [!IMPORTANT]
> **This project is designed to work with the Split-Flap display from [Adam G Makes](https://www.youtube.com/@AdamGMakes).**
> See [this video](https://youtu.be/-C8_AtxEEQc?si=Gym5wikeFH2vUNRm) for additional information.
>
> **An MQTT broker is required.** [Eclipse Mosquitto](https://mosquitto.org) is recommended — it is free, lightweight, and runs on Linux, macOS, Windows, and Docker. The setup script in this folder can install and configure Mosquitto for you (see [Installation](#installation-as-a-systemd-service)).
>
> Prefer not to run a broker? There is also a **REST bridge** that talks straight to the gateway's HTTP API with no broker and no Python dependencies. It lives in a separate folder with its own README.

---

`sfgw_serial_bridge.py` creates a virtual serial port (PTY) on Linux that behaves exactly like a physical USB RS-485 adapter connected to the split-flap bus. Any application that can open a serial port can use it transparently.

```
Application  <-->  /tmp/ttyVSF0  <-->  sfgw_serial_bridge  <-->  MQTT  <-->  Gateway  <-->  RS-485 bus
```

**Write** to the virtual port → frame published to `splitflap/send` → gateway transmits on bus  
**Bus frame received** → gateway publishes to `splitflap/rx` → written to virtual port → application reads it

---

## Quick install (one command)

If you just want to get going, this single command downloads the installer and runs it. It will then fetch the bridge program itself and walk you through the rest:

```bash
curl -fsSL https://raw.githubusercontent.com/avandeputte/SplitFlapGateway/main/MQTTtoVirtualSerialDevice/SplitFlapGatewaySetup.sh | bash
```

Or with `wget`:

```bash
wget -qO- https://raw.githubusercontent.com/avandeputte/SplitFlapGateway/main/MQTTtoVirtualSerialDevice/SplitFlapGatewaySetup.sh | bash
```

The installer is interactive (it asks where to install, broker settings, etc.). When piped from `curl`/`wget` like this, it reads your answers from the terminal, so the prompts work normally — **as long as you run it in an interactive terminal** (not from cron or a non-interactive script). If you'd rather inspect the script before running it — always a good habit for `curl | bash` — download it first:

```bash
curl -fsSL https://raw.githubusercontent.com/avandeputte/SplitFlapGateway/main/MQTTtoVirtualSerialDevice/SplitFlapGatewaySetup.sh -o SplitFlapGatewaySetup.sh
less SplitFlapGatewaySetup.sh      # review it
bash SplitFlapGatewaySetup.sh
```

The sections below cover downloading the files manually if you prefer.

---

## Downloading the files

You need two files from this folder of the GitHub repository:

- `sfgw_serial_bridge.py` — the bridge program
- `SplitFlapGatewaySetup.sh` — the installer script

Repository folder: **<https://github.com/avandeputte/SplitFlapGateway/tree/main/MQTTtoVirtualSerialDevice>**

> [!TIP]
> You can get away with downloading just **`SplitFlapGatewaySetup.sh`** — if `sfgw_serial_bridge.py` isn't sitting next to it when you run it, the setup script offers to download the bridge from GitHub automatically. Grabbing both up front still works and is fine for offline machines.

Pick whichever method below you are most comfortable with.

### Method 1 — Download each file in your web browser (easiest)

1. Open the folder link above in your browser.
2. Click the file **`sfgw_serial_bridge.py`**.
3. On the file page, click the **Raw** button (top-right of the file view). The raw file opens.
4. Save the page: press **Ctrl+S** (Windows/Linux) or **Cmd+S** (Mac), and save it as `sfgw_serial_bridge.py`. Make sure your browser does **not** add a `.txt` extension — the name must stay exactly `sfgw_serial_bridge.py`.
5. Go back and repeat steps 2–4 for **`SplitFlapGatewaySetup.sh`**.
6. Move both files into the same folder on the machine that will run the bridge (for example `~/sfgw`).

> Tip: keep both files together in one folder — the setup script expects `sfgw_serial_bridge.py` to sit right next to it.

### Method 2 — Download with `curl` (one command per file)

On the Linux machine that will run the bridge, run:

```bash
# Create and enter a working folder
mkdir -p ~/sfgw && cd ~/sfgw

# Download the two files straight from GitHub (raw content)
curl -LO https://raw.githubusercontent.com/avandeputte/SplitFlapGateway/main/MQTTtoVirtualSerialDevice/sfgw_serial_bridge.py
curl -LO https://raw.githubusercontent.com/avandeputte/SplitFlapGateway/main/MQTTtoVirtualSerialDevice/SplitFlapGatewaySetup.sh

# Make the installer executable
chmod +x SplitFlapGatewaySetup.sh
```

If you have `wget` instead of `curl`, substitute `wget` for `curl -LO`:

```bash
wget https://raw.githubusercontent.com/avandeputte/SplitFlapGateway/main/MQTTtoVirtualSerialDevice/sfgw_serial_bridge.py
wget https://raw.githubusercontent.com/avandeputte/SplitFlapGateway/main/MQTTtoVirtualSerialDevice/SplitFlapGatewaySetup.sh
```

### Method 3 — Clone the whole repository with `git`

If you have `git` installed and want the entire project:

```bash
git clone https://github.com/avandeputte/SplitFlapGateway.git
cd SplitFlapGateway/MQTTtoVirtualSerialDevice
chmod +x SplitFlapGatewaySetup.sh
```

The two files you need are then in `SplitFlapGateway/MQTTtoVirtualSerialDevice/`.

---

## Requirements

- Linux (uses POSIX PTY — not available on macOS or Windows)
- Python 3.7+
- paho-mqtt
- An MQTT broker (e.g. Mosquitto — the setup script can install it for you)

The setup script installs `paho-mqtt` for you. To install it manually:

```bash
pip3 install paho-mqtt
```

> [!NOTE]
> **If you see `error: externally-managed-environment`:** newer Linux distributions (Debian 12+, Ubuntu 23.04+, recent Raspberry Pi OS, Fedora, etc.) mark the system Python as protected (PEP 668), so `pip3 install` into it is blocked by default. Pick one of these:
>
> ```bash
> # Option A (preferred) — install the distro package instead of using pip:
> sudo apt install python3-paho-mqtt        # Debian / Ubuntu / Raspberry Pi OS
> # sudo dnf install python3-paho-mqtt      # Fedora
>
> # Option B — install with pip into your user account, overriding the guard:
> pip3 install --user --break-system-packages paho-mqtt
>
> # Option C — use an isolated virtual environment (no system changes):
> python3 -m venv ~/sfgw-venv
> ~/sfgw-venv/bin/pip install paho-mqtt
> # then run the bridge with that interpreter:
> ~/sfgw-venv/bin/python3 sfgw_serial_bridge.py --broker 192.168.1.50 -v
> ```
>
> The `python3-paho-mqtt` distro package (Option A) is the cleanest choice and avoids the flag entirely. If you do use pip, `--break-system-packages` combined with `--user` does **not** harm your system — it just tells pip you accept managing this package yourself, installed into your home directory rather than the protected system location.

---

## Installing an MQTT broker (Mosquitto)

The bridge needs an MQTT broker to talk to. If you don't already have one, the easiest option is **Mosquitto**, which can run on the same machine as the bridge.

**The setup script (`SplitFlapGatewaySetup.sh`) installs and fully configures Mosquitto for you** — just answer "yes" when it offers. Specifically, it will:

- Install Mosquitto via your system's package manager
- Write a config that opens a **network listener on port 1883** so the gateway (a separate device) can connect over the LAN — a default Mosquitto install only listens on localhost, which the gateway cannot reach
- Optionally set up a **username and password** (leave the username blank for anonymous access on a private home network)
- **Enable it to start automatically on boot**
- Print the exact values to enter on the gateway's **Settings** page — including this machine's **LAN IP address** (not `localhost`, since the gateway connects over the network)

If you'd rather install it yourself:

```bash
# Debian / Ubuntu / Raspberry Pi OS
sudo apt update
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable --now mosquitto

# Fedora
sudo dnf install mosquitto
sudo systemctl enable --now mosquitto
```

A manual install listens on `localhost:1883` only. To let the gateway connect, create `/etc/mosquitto/conf.d/splitflap.conf` with a network listener:

```
listener 1883 0.0.0.0
allow_anonymous true
```

For password protection instead of anonymous access:

```
listener 1883 0.0.0.0
allow_anonymous false
password_file /etc/mosquitto/passwd
```

…and create the password file with `sudo mosquitto_passwd -c /etc/mosquitto/passwd <username>`. Restart with `sudo systemctl restart mosquitto` after any change.

### Pointing the gateway at the broker

The Split-Flap gateway is a separate device on your network, so it must reach the broker by its **LAN IP address — never `localhost`**. On the gateway's web UI **Settings** page, in the MQTT section, enter:

| Field | Value |
|---|---|
| Broker Host / IP | The broker machine's LAN IP (e.g. `192.168.2.50`) — find it with `hostname -I` |
| Port | `1883` |
| Topic Prefix | `splitflap` |
| Username / Password | Only if you configured authentication; otherwise leave blank |

Then click **Save MQTT** and **Test Connection** on the gateway. A successful test confirms the gateway can reach the broker. (Tip: if the broker machine's IP can change, set a DHCP reservation on your router so the address stays valid.)

---

## Quick Start

```bash
python3 sfgw_serial_bridge.py --broker 192.168.1.50 -v
```

Output:
```
[14:32:01] PTY created: /dev/pts/4
[14:32:01] Symlink:     /tmp/ttyVSF0 -> /dev/pts/4
[14:32:01] MQTT connected to 192.168.1.50:1883
[14:32:01] Subscribing to: splitflap/rx
[14:32:01] Subscribed OK: splitflap/rx (mid=1 qos=(0,))

[14:32:01] Bridge running.  Press Ctrl-C to stop.
[14:32:01]   Virtual port : /tmp/ttyVSF0
```

Then in another terminal:

```bash
# Read bus traffic
cat /tmp/ttyVSF0 &

# Send a command
printf 'm9h\n' > /tmp/ttyVSF0
```

---

## Installation as a systemd Service

Use the provided setup script to install the bridge as a service that starts automatically on boot. Run it from the folder where you downloaded the two files:

```bash
chmod +x SplitFlapGatewaySetup.sh
./SplitFlapGatewaySetup.sh
```

The setup script will:

1. **Install Python 3 and pip if they are missing** (useful on minimal images like Raspberry Pi OS Lite)
2. Install `sfgw_serial_bridge.py` to a directory you choose (downloading it from GitHub first if it isn't already next to the setup script)
3. Offer to install and **fully configure an MQTT broker (Mosquitto)** — a network listener so the gateway can connect over the LAN, an optional username/password, and auto-start on boot
4. Offer to install `paho-mqtt` if missing (handling the `externally-managed-environment` case automatically)
5. Ask where to install the bridge script (defaults to `/usr/local/bin` when run with sudo, or `~/sfgw` otherwise)
6. Prompt for MQTT broker settings and save them to a config file
7. Ask for the virtual port symlink path
8. Create and enable a systemd service
9. Optionally start the service immediately
10. Print the exact MQTT settings to enter on the gateway's **Settings** page, including this machine's **LAN IP**
11. Optionally download and install the [splitflap-os](#optional-splitflap-os-web-app) web app, pointed at the virtual serial port (network setup skipped)

### Install locations

| Location | Service type | Requires sudo |
|---|---|---|
| `~/sfgw` or anywhere under `$HOME` | User service (`systemctl --user`) | No |
| `/usr/local/bin`, `/opt/sfgw`, etc. | System service (`systemctl`) | Yes |

---

## Optional: splitflap-os web app

The setup script can also download and install **[splitflap-os](https://github.com/csader/splitflap-os)** by csader — a separate web UI with 40+ apps (weather, stocks, word clock, playlists, and more) that drives the display. When you accept the prompt, the script:

1. Installs `git` if it isn't already present
2. Clones `https://github.com/csader/splitflap-os.git` into a directory you choose (default `~/splitflap-os`)
3. Sets `"serial_port"` in `server/settings.json` to this bridge's virtual port (e.g. `/tmp/ttyVSF0`), so the app drives the display through the bridge — preserving any other settings already in that file
4. Runs the splitflap-os installer with **`--skip-network`**, since this machine is already on your network and doesn't need the WiFi-hotspot fallback

After install, splitflap-os is reachable at `http://<this-machine's-LAN-IP>` and talks to the display over the virtual serial port.

> [!NOTE]
> Make sure the bridge service is running so the virtual port exists when splitflap-os opens it. If splitflap-os ever regenerates its configuration, re-set `"serial_port"` to your virtual port path in `<install-dir>/server/settings.json` and restart it.

To do this manually instead of via the setup script:

```bash
git clone https://github.com/csader/splitflap-os.git
cd splitflap-os
# point it at the bridge's virtual port:
mkdir -p server
python3 - << 'PY'
import json, os
p = "server/settings.json"
d = json.load(open(p)) if os.path.isfile(p) else {}
d["serial_port"] = "/tmp/ttyVSF0"
json.dump(d, open(p, "w"), indent=2)
PY
sudo bash setup/install.sh --skip-network
```

---

## Configuration File

Settings are read from a config file. The setup script creates this automatically, but you can edit it directly.

**Locations searched (in order):**
1. `/etc/sfgw-bridge.conf` (system-wide install)
2. `~/.config/sfgw-bridge/config.ini` (user install)

**Format:**

```ini
[mqtt]
broker   = 192.168.1.50
port     = 1883
prefix   = splitflap
user     =
password =

[bridge]
link    = /tmp/ttyVSF0
verbose = false
```

The file is created with `chmod 600` to protect MQTT credentials.

After editing, restart the service:

```bash
# System service
sudo systemctl restart sfgw-bridge

# User service
systemctl --user restart sfgw-bridge
```

---

## Command-Line Options

Command-line arguments override the config file.

| Option | Description | Default |
|---|---|---|
| `--config FILE` | Path to config file | Auto-detected |
| `--broker HOST` | MQTT broker hostname or IP | `localhost` |
| `--port N` | MQTT broker port | `1883` |
| `--prefix STR` | MQTT topic prefix | `splitflap` |
| `--link PATH` | Symlink path for virtual port | `/tmp/ttyVSF0` |
| `--user STR` | MQTT username | — |
| `--password STR` | MQTT password | — |
| `-v` / `--verbose` | Print each bridged frame | off |

---

## Service Management

### System service

```bash
sudo systemctl status  sfgw-bridge
sudo systemctl start   sfgw-bridge
sudo systemctl stop    sfgw-bridge
sudo systemctl restart sfgw-bridge

# Live logs
journalctl -u sfgw-bridge -f
```

### User service

```bash
systemctl --user status  sfgw-bridge
systemctl --user start   sfgw-bridge
systemctl --user stop    sfgw-bridge
systemctl --user restart sfgw-bridge

journalctl --user -u sfgw-bridge -f
```

---

## Using the Virtual Port

The virtual port at `/tmp/ttyVSF0` behaves like a raw serial port. Use `cat` for monitoring rather than `screen` or `minicom` — terminal emulators reconfigure the PTY and may interfere.

```bash
# Monitor all bus traffic
cat /tmp/ttyVSF0

# Send a single command
printf 'm5-A\n' > /tmp/ttyVSF0

# Home module 9
printf 'm9h\n' > /tmp/ttyVSF0

# Home all modules
printf 'm*h\n' > /tmp/ttyVSF0

# Query version of module 3
printf 'm3v\n' > /tmp/ttyVSF0

# Send text starting at module 0
# (you need to send one char per module individually)
printf 'm0-H\n' > /tmp/ttyVSF0
printf 'm1-E\n' > /tmp/ttyVSF0
printf 'm2-L\n' > /tmp/ttyVSF0
```

From a Python script:

```python
import serial

port = serial.Serial('/tmp/ttyVSF0', baudrate=9600, timeout=1)

# Send a command
port.write(b'm5-A\n')

# Read responses
while True:
    line = port.readline()
    if line:
        print(line.decode().strip())
```

> **Note:** Install pyserial with `pip3 install pyserial`.  
> Set `baudrate=9600` to match the RS-485 bus speed (the PTY ignores the baud rate setting but pyserial requires a value).

---

## MQTT Topics

| Direction | Topic | Format | Description |
|---|---|---|---|
| Publish | `splitflap/send` | `m9h\n` | Send raw frame to bus |
| Subscribe | `splitflap/rx` | `{"ts":...,"wt":"...","command":"m5-A"}` | Frames received from bus |

The bridge subscribes to `splitflap/rx` and publishes to `splitflap/send`. The `command` field in the received JSON contains the frame without the trailing newline; the bridge re-adds it before writing to the PTY.

---

## Diagnostics

If the bridge is running but you are not receiving messages:

**Check the journal for subscription confirmation:**
```bash
journalctl -u sfgw-bridge -n 20
```
You should see:
```
Subscribed OK: splitflap/rx (mid=1 qos=(0,))
```

**Verify the gateway is publishing to the correct topic:**
```bash
# Subscribe to all topics on the broker
mosquitto_sub -h <broker> -t '#' -v
```
You should see `splitflap/rx` messages when the bus is active.

**Test the virtual port directly:**
```bash
cat /tmp/ttyVSF0 &
printf 'm*h\n' > /tmp/ttyVSF0
```
If the send works (you see it in the bus monitor) but nothing comes back on the PTY, the issue is in the MQTT subscription path. Run with `-v` for detailed per-frame logging.

---

## How It Works

The bridge creates a Linux PTY (pseudo-terminal) pair:

- **Master fd** — held by the bridge process. Writing to it delivers bytes to readers of the slave. Reading from it receives bytes written to the slave.
- **Slave fd** — kept open by the bridge to prevent the PTY from being destroyed when no user app has it open. Exposed via symlink at `/tmp/ttyVSF0`.

The slave is configured in raw mode (all termios flags zero) so bytes pass through with no transformation — no echo, no line buffering, no `\n` → `\r\n` translation. This is what makes it behave identically to a physical serial port.

Two concurrent paths run inside the bridge:

- **MQTT → PTY:** `on_message` callback (paho network thread) receives `splitflap/rx`, extracts the `command` field, appends `\n`, and calls `os.write(master_fd, raw)`.
- **PTY → MQTT:** A reader thread calls `select()` on `master_fd`, accumulates bytes until `\n`, then publishes each complete line to `splitflap/send`.

---

## License

[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

You are free to share and adapt this project for non-commercial purposes, as long as you give appropriate credit and distribute any derivatives under the same license.
