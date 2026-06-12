#!/usr/bin/env python3
"""
sfgw_serial_bridge.py -- Split-Flap Gateway Virtual Serial Port Bridge

Creates a PTY that behaves exactly like a USB RS-485 serial adapter,
bridging it to the Split-Flap Gateway MQTT topics:

  User writes to PTY  ->  published to  <prefix>/send
  <prefix>/rx arrives ->  written to PTY

Config file locations (searched in order):
  /etc/sfgw-bridge.conf
  ~/.config/sfgw-bridge/config.ini

Config file format:
  [mqtt]
  broker   = 192.168.1.50
  port     = 1883
  prefix   = splitflap
  user     =
  password =

  [bridge]
  link    = /tmp/ttyVSF0
  verbose = false

Usage:
  sfgw_serial_bridge.py [--config FILE] [--broker HOST] [--port N]
    [--prefix STR] [--link PATH] [--user U] [--password P] [-v]
"""

import argparse
import configparser
import json
import os
import select
import signal
import sys
import termios
import threading
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("ERROR: paho-mqtt not installed.  Run:  pip3 install paho-mqtt")

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
CONFIG_SEARCH = [
    "/etc/sfgw-bridge.conf",
    os.path.expanduser("~/.config/sfgw-bridge/config.ini"),
]

def load_config(path=None):
    cp = configparser.ConfigParser()
    cp.read_dict({
        "mqtt":   {"broker": "localhost", "port": "1883",
                   "prefix": "splitflap", "user": "", "password": ""},
        "bridge": {"link": "/tmp/ttyVSF0", "verbose": "false"},
    })
    candidates = [path] if path else CONFIG_SEARCH
    read = cp.read([c for c in candidates if c])
    if read:
        print(f"[config] Loaded {read[0]}")
    return cp

# ---------------------------------------------------------------------------
# PTY helpers
# ---------------------------------------------------------------------------

def configure_pty_slave(slave_fd):
    """
    Raw mode on the slave: no line buffering, no echo, no output mangling.
    This makes the PTY behave like a raw serial port.
    """
    attrs = termios.tcgetattr(slave_fd)
    attrs[0] = 0   # iflag: no input processing
    attrs[1] = 0   # oflag: no output processing (critical - prevents \n->\r\n)
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0   # lflag: no echo, no canonical mode
    attrs[6][termios.VMIN]  = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

_verbose = False

def log(msg, verbose=False, force=False):
    if force or verbose or _verbose:
        print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

# ---------------------------------------------------------------------------
# MQTT client factory - works with paho 1.x and 2.x
# ---------------------------------------------------------------------------

def make_mqtt_client(client_id):
    """
    Create a paho MQTT client compatible with both paho 1.x and 2.x.
    paho 2.x introduced CallbackAPIVersion; we use VERSION1 explicitly when
    available so callbacks receive (client, userdata, flags, rc) as integers,
    not as paho 2.x objects.
    """
    try:
        # paho 2.x: use VERSION1 to keep the classic integer rc in callbacks
        return mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION1,
            client_id=client_id,
        )
    except AttributeError:
        # paho 1.x: no CallbackAPIVersion, use plain constructor
        return mqtt.Client(client_id=client_id)

# ---------------------------------------------------------------------------
# Bridge
# ---------------------------------------------------------------------------

class SplitFlapBridge:

    def __init__(self, broker, port, prefix, link_path,
                 username=None, password=None, verbose=False):
        self.broker    = broker
        self.port      = port
        self.prefix    = prefix.rstrip("/")
        self.link_path = link_path
        self.username  = username
        self.password  = password
        self.verbose   = verbose

        global _verbose
        _verbose = verbose

        self.master_fd  = None
        self.slave_fd   = None
        self.slave_path = None
        self.running    = False

        self.mq = make_mqtt_client(f"sfgw-bridge-{os.getpid()}")
        self.mq.on_connect    = self._on_connect
        self.mq.on_disconnect = self._on_disconnect
        self.mq.on_message    = self._on_message
        self.mq.on_subscribe  = self._on_subscribe

        if username:
            self.mq.username_pw_set(username, password)

    # -- PTY ------------------------------------------------------------------

    def _open_pty(self):
        master_fd, slave_fd = os.openpty()
        slave_path = os.ttyname(slave_fd)
        configure_pty_slave(slave_fd)

        # Keep slave_fd open so the PTY stays alive even when no user
        # app has it open (prevents EIO on master writes).
        self.master_fd  = master_fd
        self.slave_fd   = slave_fd
        self.slave_path = slave_path

        log(f"PTY created: {slave_path}", force=True)

        if os.path.lexists(self.link_path):
            os.remove(self.link_path)
        os.symlink(slave_path, self.link_path)
        log(f"Symlink:     {self.link_path} -> {slave_path}", force=True)

    def _close_pty(self):
        for fd in (self.slave_fd, self.master_fd):
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        self.master_fd = None
        self.slave_fd  = None
        if os.path.lexists(self.link_path):
            try:
                os.remove(self.link_path)
            except OSError:
                pass

    # -- MQTT callbacks -------------------------------------------------------

    def _on_connect(self, client, userdata, flags, rc):
        if rc != 0:
            log(f"MQTT connect failed: rc={rc}", force=True)
            return
        log(f"MQTT connected to {self.broker}:{self.port}", force=True)
        rx_topic = f"{self.prefix}/rx"
        log(f"Subscribing to: {rx_topic}", force=True)
        client.subscribe(rx_topic, qos=0)

    def _on_subscribe(self, client, userdata, mid, granted_qos):
        rx_topic = f"{self.prefix}/rx"
        log(f"Subscribed OK: {rx_topic} (mid={mid} qos={granted_qos})",
            force=True)

    def _on_disconnect(self, client, userdata, rc):
        if rc != 0:
            log(f"MQTT disconnected unexpectedly (rc={rc}), "
                f"will reconnect...", force=True)

    def _on_message(self, client, userdata, msg):
        """
        MQTT rx message -> PTY master.
        Payload JSON: {"ts":...,"wt":"...","command":"m5-A"}
        Firmware strips the trailing newline; we re-add it.
        """
        log(f"MQTT message on [{msg.topic}]: {repr(bytes(msg.payload)[:80])}",
            force=True)
        try:
            try:
                data = json.loads(msg.payload)
            except (json.JSONDecodeError, ValueError) as e:
                log(f"JSON error: {e} payload={repr(bytes(msg.payload)[:60])}",
                    force=True)
                return

            frame = data.get("command", "")
            if not frame:
                log("'command' field empty or missing", force=True)
                return

            if not frame.endswith("\n"):
                frame += "\n"

            raw = frame.encode("latin-1", errors="replace")

            if self.master_fd is not None:
                os.write(self.master_fd, raw)
                log(f"RX -> PTY: {repr(raw)}", force=True)
            else:
                log("master_fd is None - PTY not open", force=True)

        except OSError as e:
            log(f"PTY write error: {e}", force=True)
        except Exception as e:
            log(f"on_message error: {e}", force=True)

    # -- PTY reader thread ----------------------------------------------------

    def _pty_reader(self):
        """
        Bytes written to PTY slave by user app appear on master_fd.
        Accumulate into newline-terminated lines; publish each to MQTT.
        """
        buf   = b""
        topic = f"{self.prefix}/send"

        while self.running:
            try:
                r, _, _ = select.select([self.master_fd], [], [], 0.2)
            except (select.error, ValueError, OSError):
                break
            if not r:
                continue
            try:
                chunk = os.read(self.master_fd, 512)
            except OSError:
                break
            if not chunk:
                break

            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line += b"\n"
                try:
                    frame_str = line.decode("latin-1")
                    self.mq.publish(topic, payload=frame_str, qos=0)
                    log(f"TX -> MQTT ({topic}): {repr(frame_str)}", force=True)
                except Exception as e:
                    log(f"Publish error: {e}", force=True)

    # -- Run / stop -----------------------------------------------------------

    def run(self):
        self._open_pty()
        self.running = True

        log(f"Connecting to MQTT broker {self.broker}:{self.port}...",
            force=True)
        try:
            self.mq.connect(self.broker, self.port, keepalive=60)
        except Exception as e:
            self._close_pty()
            sys.exit(f"ERROR: Cannot connect to MQTT broker: {e}")

        self.mq.loop_start()

        reader = threading.Thread(target=self._pty_reader, daemon=True)
        reader.start()

        print()
        log("Bridge running.  Press Ctrl-C to stop.", force=True)
        log(f"  Virtual port : {self.link_path}", force=True)
        log(f"  MQTT broker  : {self.broker}:{self.port}", force=True)
        log(f"  Topic prefix : {self.prefix}", force=True)
        print()
        log(f"  cat {self.link_path} &", force=True)
        log(f"  printf 'm9h\\n' > {self.link_path}", force=True)
        print()

        try:
            while self.running:
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass

        self.stop()

    def stop(self):
        log("\nShutting down...", force=True)
        self.running = False
        try:
            self.mq.loop_stop()
            self.mq.disconnect()
        except Exception:
            pass
        self._close_pty()
        log("Stopped.", force=True)

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Split-Flap Gateway virtual serial port bridge",
    )
    parser.add_argument("--config",   default=None,
                        help="Path to config file")
    parser.add_argument("--broker",   default=None,
                        help="MQTT broker host")
    parser.add_argument("--port",     type=int,
                        help="MQTT broker port")
    parser.add_argument("--prefix",   default=None,
                        help="MQTT topic prefix")
    parser.add_argument("--link",     default=None,
                        help="Virtual port symlink path")
    parser.add_argument("--user",     default=None,
                        help="MQTT username")
    parser.add_argument("--password", default=None,
                        help="MQTT password")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Print each bridged frame")
    args = parser.parse_args()

    cp = load_config(args.config)

    broker   = args.broker   or cp.get("mqtt",   "broker")
    port     = args.port     or cp.getint("mqtt", "port")
    prefix   = args.prefix   or cp.get("mqtt",   "prefix")
    user     = args.user     or cp.get("mqtt",   "user")     or None
    password = args.password or cp.get("mqtt",   "password") or None
    link     = args.link     or cp.get("bridge", "link")
    verbose  = args.verbose  or cp.getboolean("bridge", "verbose")

    bridge = SplitFlapBridge(
        broker=broker, port=port, prefix=prefix,
        link_path=link, username=user, password=password, verbose=verbose,
    )
    signal.signal(signal.SIGTERM, lambda s, f: bridge.stop())
    bridge.run()


if __name__ == "__main__":
    main()
