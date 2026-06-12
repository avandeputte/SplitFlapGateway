# Split-Flap Gateway — Virtual Serial Port Bridge

---

> [!IMPORTANT]
> **This project is designed to work with the Split-Flap display from [Adam G Makes](https://www.youtube.com/@AdamGMakes).**
> See [this video](https://youtu.be/-C8_AtxEEQc?si=Gym5wikeFH2vUNRm) for additional information.
>
> - **An MQTT broker is required** for the bridge to function. [Eclipse Mosquitto](https://mosquitto.org) is recommended — it is free, lightweight, and runs on Linux, macOS, Windows, and Docker (`docker run -it -p 1883:1883 eclipse-mosquitto`).

---

---


`sfgw_serial_bridge.py` creates a virtual serial port (PTY) on Linux that behaves exactly like a physical USB RS-485 adapter connected to the split-flap bus. Any application that can open a serial port can use it transparently.

```
Application  <-->  /tmp/ttyVSF0  <-->  sfgw_serial_bridge  <-->  MQTT  <-->  Gateway  <-->  RS-485 bus
```

**Write** to the virtual port → frame published to `splitflap/send` → gateway transmits on bus  
**Bus frame received** → gateway publishes to `splitflap/rx` → written to virtual port → application reads it

---

## Requirements

- Linux (uses POSIX PTY — not available on macOS or Windows)
- Python 3.7+
- paho-mqtt

```bash
pip3 install paho-mqtt
```

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

Use the provided setup script to install the bridge as a service that starts automatically on boot:

```bash
chmod +x sfgw_setup.sh
./sfgw_setup.sh
```

The setup script will:

1. Check that Python 3 and systemd are present
2. Offer to install `paho-mqtt` if missing
3. Ask where to install the bridge script
4. Prompt for MQTT broker settings and save them to a config file
5. Ask for the virtual port symlink path
6. Create and enable a systemd service
7. Optionally start the service immediately

### Install locations

| Location | Service type | Requires sudo |
|---|---|---|
| `~/sfgw` or anywhere under `$HOME` | User service (`systemctl --user`) | No |
| `/usr/local/bin`, `/opt/sfgw`, etc. | System service (`systemctl`) | Yes |

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
