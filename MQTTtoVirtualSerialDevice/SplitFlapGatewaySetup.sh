#!/usr/bin/env bash
# SplitFlapGatewaySetup.sh -- Split-Flap Gateway bridge setup and service installer
# Installs sfgw_serial_bridge.py (downloading it from GitHub if needed) and
# creates a systemd service to run it automatically on boot.
# Supports being run directly (bash SplitFlapGatewaySetup.sh) or piped from a
# downloader (curl -fsSL <url> | bash).
set -euo pipefail

# ---------------------------------------------------------------------------
# Colour helpers (disabled if not a terminal)
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'
    BLU='\033[0;34m'; CYN='\033[0;36m'; BOLD='\033[1m'; RST='\033[0m'
else
    RED=''; GRN=''; YLW=''; BLU=''; CYN=''; BOLD=''; RST=''
fi

hr()  { printf "${BLU}%s${RST}\n" "------------------------------------------------------------"; }
hdr() { echo; hr; printf "${BOLD}  %s${RST}\n" "$1"; hr; echo; }
ok()  { printf "  ${GRN}OK${RST}  %s\n" "$1"; }
info(){ printf "  ${CYN}--${RST}  %s\n" "$1"; }
warn(){ printf "  ${YLW}!!${RST}  %s\n" "$1"; }
die() { printf "\n${RED}ERROR:${RST} %s\n\n" "$1" >&2; exit 1; }

# Read a line of user input. When this script is piped into bash
# (curl ... | bash), stdin is the script text, not the keyboard -- so read from
# the controlling terminal (/dev/tty) instead. Falls back to stdin if no tty.
read_tty() {
    # $1 = variable name to populate
    if [ -r /dev/tty ]; then
        read -r "$1" < /dev/tty
    else
        read -r "$1"
    fi
}

# Read a secret (no echo) from the terminal, same tty-aware logic as read_tty.
read_secret() {
    # $1 = variable name to populate
    if [ -r /dev/tty ]; then
        read -rs "$1" < /dev/tty
    else
        read -rs "$1"
    fi
}

# If we were piped in with no terminal at all, interactive prompts can't work.
# Detect that early and tell the user how to run it properly.
if [ ! -t 0 ] && [ ! -r /dev/tty ]; then
    printf "${RED}ERROR:${RST} This installer is interactive but has no terminal to read from.\n" >&2
    printf "Download it first, then run it:\n" >&2
    printf "  curl -fsSL <url> -o SplitFlapGatewaySetup.sh && bash SplitFlapGatewaySetup.sh\n" >&2
    exit 1
fi

ask() {
    # ask <VAR> <prompt> <default>
    local var="$1" prompt="$2" default="$3"
    local val
    printf "  ${BOLD}%s${RST} [%s]: " "$prompt" "$default"
    read_tty val
    val="${val:-$default}"
    eval "$var=\"\$val\""
}

ask_yn() {
    # ask_yn <prompt> <default y|n>  -> returns 0=yes 1=no
    local prompt="$1" default="${2:-y}"
    local val
    if [ "$default" = "y" ]; then
        printf "  ${BOLD}%s${RST} [Y/n]: " "$prompt"
    else
        printf "  ${BOLD}%s${RST} [y/N]: " "$prompt"
    fi
    read_tty val
    val="${val:-$default}"
    [[ "$val" =~ ^[Yy] ]]
}

# Detect the system package manager (apt / dnf / yum / pacman / zypper).
# Echoes the manager name, or empty string if none recognized.
detect_pkg_mgr() {
    if   command -v apt-get &>/dev/null; then echo "apt"
    elif command -v dnf     &>/dev/null; then echo "dnf"
    elif command -v yum     &>/dev/null; then echo "yum"
    elif command -v pacman  &>/dev/null; then echo "pacman"
    elif command -v zypper  &>/dev/null; then echo "zypper"
    else echo ""
    fi
}

# Run a command with sudo when not already root. Echoes a note the first time.
SUDO=""
need_sudo() {
    if [ "$EUID" -ne 0 ]; then SUDO="sudo"; else SUDO=""; fi
}

# Best-effort detection of this machine's primary LAN IPv4 address. This is the
# address the Split-Flap gateway (a separate device on the network) must use to
# reach the broker -- NOT localhost/127.0.0.1, which would only work for clients
# on this same machine. Tries several methods and falls back gracefully.
detect_lan_ip() {
    local ip=""
    # Method 1: the source IP the kernel would use to reach a public address
    # (works without actually sending anything; most reliable on Linux).
    if command -v ip &>/dev/null; then
        ip="$(ip -4 route get 1.1.1.1 2>/dev/null | grep -oE 'src [0-9.]+' | awk '{print $2}' | head -n1)"
    fi
    # Method 2: first non-loopback address from hostname -I.
    if [ -z "$ip" ] && command -v hostname &>/dev/null; then
        ip="$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+\.' | grep -v '^127\.' | head -n1)"
    fi
    # Method 3: parse ifconfig as a last resort.
    if [ -z "$ip" ] && command -v ifconfig &>/dev/null; then
        ip="$(ifconfig 2>/dev/null | grep -oE 'inet (addr:)?[0-9.]+' | awk '{print $NF}' | sed 's/addr://' | grep -v '^127\.' | head -n1)"
    fi
    echo "$ip"
}

# ---------------------------------------------------------------------------
# Locate the bridge script (next to this setup script, in CWD, or download it).
# Definitions only here -- the actual locate/download runs after the welcome
# prompt below so the user sees what the script is first.
# ---------------------------------------------------------------------------
# Raw (not "blob") URL -- blob is the HTML page; raw is the file content.
BRIDGE_RAW_URL="https://raw.githubusercontent.com/avandeputte/SplitFlapGateway/main/MQTTtoVirtualSerialDevice/sfgw_serial_bridge.py"

# When run as a normal file, SCRIPT_DIR is the script's directory. When piped
# into bash (curl ... | bash) there is no source file, so BASH_SOURCE points at
# "bash" or a pipe -- fall back to the current working directory. The bridge
# will then simply not be found locally and the download path kicks in.
if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]}" ]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
else
    SCRIPT_DIR="$(pwd)"
fi
BRIDGE_SRC="${SCRIPT_DIR}/sfgw_serial_bridge.py"

# Download the bridge script to a writable path. Tries curl then wget. Verifies
# the result looks like the real Python file (GitHub can return an HTML error
# page with a 200, so a content sanity check matters). Echoes nothing; returns
# non-zero on failure (2 = no downloader, 3 = bad content).
download_bridge() {
    local url="$1" dest="$2"
    if command -v curl &>/dev/null; then
        curl -fLso "$dest" "$url" || return 1
    elif command -v wget &>/dev/null; then
        wget -qO "$dest" "$url" || return 1
    else
        return 2   # no downloader available
    fi
    # Sanity-check: must be a non-trivial file that mentions the bridge name,
    # not an HTML error page.
    if [ ! -s "$dest" ] || ! head -n 5 "$dest" | grep -q "sfgw_serial_bridge"; then
        rm -f "$dest"
        return 3
    fi
    return 0
}

# Resolve BRIDGE_SRC: prefer a local copy, else offer to download. Called after
# the welcome confirmation.
locate_or_download_bridge() {
    if [ -f "$BRIDGE_SRC" ]; then
        return 0
    fi
    # Not next to the setup script -- try the current directory.
    if [ -f "./sfgw_serial_bridge.py" ]; then
        BRIDGE_SRC="$(cd "$(dirname "./sfgw_serial_bridge.py")" && pwd)/sfgw_serial_bridge.py"
        return 0
    fi

    hdr "Bridge script"
    warn "sfgw_serial_bridge.py was not found next to this script."
    echo "  Looked in: $SCRIPT_DIR"
    echo
    if ask_yn "Download sfgw_serial_bridge.py from GitHub now?" y; then
        local dl_dest
        if [ -w "$SCRIPT_DIR" ]; then
            dl_dest="${SCRIPT_DIR}/sfgw_serial_bridge.py"
        else
            dl_dest="$(mktemp /tmp/sfgw_serial_bridge.XXXXXX.py)"
        fi
        info "Downloading from:"
        info "  $BRIDGE_RAW_URL"
        if download_bridge "$BRIDGE_RAW_URL" "$dl_dest"; then
            BRIDGE_SRC="$dl_dest"
            ok "Downloaded sfgw_serial_bridge.py -> $BRIDGE_SRC"
        else
            local rc=$?
            if [ "$rc" -eq 2 ]; then
                die "Neither curl nor wget is installed -- cannot download. Install one, or place sfgw_serial_bridge.py next to this script."
            else
                die "Download failed. Check your internet connection, or download sfgw_serial_bridge.py manually from:
    https://github.com/avandeputte/SplitFlapGateway/tree/main/MQTTtoVirtualSerialDevice
  and place it next to this script."
            fi
        fi
    else
        die "sfgw_serial_bridge.py is required. Place it next to this script, or re-run and allow the download."
    fi
}

# ---------------------------------------------------------------------------
# Welcome
# ---------------------------------------------------------------------------
clear
hdr "Split-Flap Gateway -- Bridge Service Setup"
echo "  This script will:"
echo "    1. Install Python 3 and pip if they are missing"
echo "    2. Install sfgw_serial_bridge.py to a directory you choose"
echo "    3. Optionally install and configure an MQTT broker (Mosquitto),"
echo "       including an optional username/password and auto-start on boot"
echo "    4. Install paho-mqtt if not already present"
echo "    5. Write a config file with your MQTT settings"
echo "    6. Create and enable a systemd service"
echo "    7. Tell you exactly what to enter on the gateway's Settings page"
echo "    8. Optionally install the splitflap-os web app and point it at the"
echo "       virtual serial port (network setup skipped via --skip-network)"
echo
echo "  Run with sudo if you want to install system-wide."
echo "  No sudo needed for a user-level install."
echo
if ! ask_yn "Continue?" y; then
    echo "Aborted."; exit 0
fi

# Resolve the bridge script now (download from GitHub if it isn't present).
locate_or_download_bridge

# ---------------------------------------------------------------------------
# Check systemd
# ---------------------------------------------------------------------------
hdr "Checking prerequisites"

if ! command -v systemctl &>/dev/null; then
    die "systemd not found. This setup script requires a systemd-based Linux distro."
fi
ok "systemd found"

# PKG_MGR is detected later in the broker section; detect it now too so we can
# install Python/pip if they are missing (it is cheap and idempotent).
PKG_MGR="$(detect_pkg_mgr)"

# Python 3 -- install it if missing.
if ! command -v python3 &>/dev/null; then
    warn "python3 not found."
    if [ -n "$PKG_MGR" ] && ask_yn "Install Python 3 now?" y; then
        need_sudo
        case "$PKG_MGR" in
            apt)    $SUDO apt-get update && $SUDO apt-get install -y python3 || true ;;
            dnf)    $SUDO dnf install -y python3 || true ;;
            yum)    $SUDO yum install -y python3 || true ;;
            pacman) $SUDO pacman -Sy --noconfirm python || true ;;
            zypper) $SUDO zypper install -y python3 || true ;;
        esac
    fi
fi
if ! command -v python3 &>/dev/null; then
    die "python3 is required but could not be installed. Install Python 3.7+ manually and re-run."
fi
PYTHON=$(command -v python3)
ok "python3 found: $PYTHON"

# pip3 -- install it if missing. Some minimal images ship python3 without pip.
if ! command -v pip3 &>/dev/null && ! python3 -m pip --version &>/dev/null; then
    warn "pip (Python package installer) not found."
    if [ -n "$PKG_MGR" ] && ask_yn "Install pip now?" y; then
        need_sudo
        case "$PKG_MGR" in
            apt)    $SUDO apt-get install -y python3-pip || true ;;
            dnf)    $SUDO dnf install -y python3-pip || true ;;
            yum)    $SUDO yum install -y python3-pip || true ;;
            pacman) $SUDO pacman -Sy --noconfirm python-pip || true ;;
            zypper) $SUDO zypper install -y python3-pip || true ;;
        esac
    fi
fi
if command -v pip3 &>/dev/null || python3 -m pip --version &>/dev/null; then
    ok "pip is available"
else
    warn "pip is not available. If paho-mqtt can be installed from your distro"
    warn "package manager (python3-paho-mqtt) this is fine; otherwise install"
    warn "pip manually so the MQTT library can be installed."
fi

# ---------------------------------------------------------------------------
# Optional: install AND configure an MQTT broker (Mosquitto)
# ---------------------------------------------------------------------------
hdr "MQTT broker (Mosquitto)"

# PKG_MGR was detected earlier in the prerequisites section.

# These get populated if we install/configure Mosquitto here, and are then
# offered as defaults when we write the bridge's own MQTT config further down.
BROKER_INSTALLED=""     # "yes" if we configured a local broker this run
BROKER_LAN_IP=""        # LAN IP the gateway should use to reach this broker
BROKER_SET_USER=""      # username we configured (blank = anonymous)
BROKER_SET_PASS=""      # password we configured

configure_mosquitto() {
    # Writes a Split-Flap-specific Mosquitto config that (a) opens a network
    # listener on 1883 so the gateway can connect over the LAN, and (b) either
    # enables password auth or explicitly allows anonymous access. A default
    # Mosquitto install on recent versions binds to localhost only and denies
    # anonymous clients, so without this the ESP32 gateway cannot connect.
    need_sudo

    local conf_dir="/etc/mosquitto/conf.d"
    local conf_file="${conf_dir}/splitflap.conf"
    local pass_file="/etc/mosquitto/passwd"

    # The conf.d directory exists on apt-based installs; create it elsewhere and
    # make sure the main mosquitto.conf includes it.
    if [ ! -d "$conf_dir" ]; then
        $SUDO mkdir -p "$conf_dir" 2>/dev/null || true
        if [ -f /etc/mosquitto/mosquitto.conf ] && \
           ! grep -q "conf.d" /etc/mosquitto/mosquitto.conf 2>/dev/null; then
            echo "include_dir /etc/mosquitto/conf.d" | $SUDO tee -a /etc/mosquitto/mosquitto.conf >/dev/null
        fi
    fi

    echo
    info "Configuring the broker so the gateway can connect over the network."
    echo
    echo "  You can optionally require a username and password. If you leave the"
    echo "  username blank, the broker will allow anonymous connections (simplest"
    echo "  for a private home network)."
    echo
    ask BROKER_SET_USER "MQTT username (blank = anonymous, no password)" ""

    local conf_auth=""
    if [ -n "$BROKER_SET_USER" ]; then
        printf "  ${BOLD}MQTT password for '%s'${RST}: " "$BROKER_SET_USER"
        read_secret BROKER_SET_PASS; echo
        while [ -z "$BROKER_SET_PASS" ]; do
            warn "Password cannot be empty when a username is set."
            printf "  ${BOLD}MQTT password for '%s'${RST}: " "$BROKER_SET_USER"
            read_secret BROKER_SET_PASS; echo
        done
        # Create/update the password file.
        if command -v mosquitto_passwd &>/dev/null; then
            # -b sets user+password in batch; -c creates (overwrites) the file.
            $SUDO mosquitto_passwd -b -c "$pass_file" "$BROKER_SET_USER" "$BROKER_SET_PASS" \
                && ok "Password file created: $pass_file" \
                || warn "Could not create password file -- check mosquitto_passwd"
            $SUDO chmod 600 "$pass_file" 2>/dev/null || true
            # mosquitto must be able to read it (runs as the 'mosquitto' user on
            # most distros). chown if that user exists.
            if id mosquitto &>/dev/null; then
                $SUDO chown mosquitto: "$pass_file" 2>/dev/null || true
            fi
        else
            warn "mosquitto_passwd not found; cannot create password file."
            warn "Falling back to anonymous access."
            BROKER_SET_USER=""; BROKER_SET_PASS=""
        fi
    fi

    if [ -n "$BROKER_SET_USER" ]; then
        conf_auth="allow_anonymous false
password_file ${pass_file}"
    else
        conf_auth="allow_anonymous true"
    fi

    # Write the listener + auth config.
    $SUDO tee "$conf_file" >/dev/null << MQTTCONF
# Split-Flap Gateway broker configuration (created by sfgw_setup.sh)
# Listen on all interfaces so the ESP32 gateway can connect over the LAN.
listener 1883 0.0.0.0
${conf_auth}
MQTTCONF
    ok "Broker config written: $conf_file"
}

if command -v mosquitto &>/dev/null; then
    ok "Mosquitto is already installed"
    if ask_yn "Configure this Mosquitto for the Split-Flap gateway (listener + optional login)?" y; then
        configure_mosquitto
        BROKER_INSTALLED="yes"
    fi
    need_sudo
    # Always make sure it's enabled on boot and (re)started to apply config.
    $SUDO systemctl enable mosquitto >/dev/null 2>&1 \
        && ok "Mosquitto enabled on boot" \
        || warn "Could not enable Mosquitto on boot"
    $SUDO systemctl restart mosquitto \
        && ok "Mosquitto service (re)started" \
        || warn "Could not start Mosquitto -- start it manually later"
    BROKER_LAN_IP="$(detect_lan_ip)"
else
    echo "  The bridge needs an MQTT broker to talk to. Mosquitto is a free,"
    echo "  lightweight broker that can run on this same machine."
    echo
    if ask_yn "Install the Mosquitto MQTT broker now?" y; then
        if [ -z "$PKG_MGR" ]; then
            warn "Could not detect a supported package manager."
            warn "Install Mosquitto manually, then re-run this script."
        else
            need_sudo
            mosq_ok=""
            case "$PKG_MGR" in
                apt)
                    $SUDO apt-get update && \
                    $SUDO apt-get install -y mosquitto mosquitto-clients && mosq_ok="yes"
                    ;;
                dnf)
                    $SUDO dnf install -y mosquitto && mosq_ok="yes"
                    ;;
                yum)
                    $SUDO yum install -y mosquitto && mosq_ok="yes"
                    ;;
                pacman)
                    $SUDO pacman -Sy --noconfirm mosquitto && mosq_ok="yes"
                    ;;
                zypper)
                    $SUDO zypper install -y mosquitto && mosq_ok="yes"
                    ;;
            esac
            if [ -n "$mosq_ok" ] && command -v mosquitto &>/dev/null; then
                ok "Mosquitto installed"
                configure_mosquitto
                BROKER_INSTALLED="yes"
                # Always enable on boot, then start to apply our config.
                $SUDO systemctl enable mosquitto >/dev/null 2>&1 \
                    && ok "Mosquitto enabled on boot" \
                    || warn "Could not enable Mosquitto on boot"
                $SUDO systemctl restart mosquitto \
                    && ok "Mosquitto service started" \
                    || warn "Could not start Mosquitto -- start it manually later"
                BROKER_LAN_IP="$(detect_lan_ip)"
            else
                warn "Mosquitto installation did not complete."
                warn "You can install it manually and re-run this script."
            fi
        fi
    else
        info "Skipping broker install."
        info "Make sure an MQTT broker is reachable before starting the service."
    fi
fi

# Check / install paho-mqtt
hdr "Python MQTT library (paho-mqtt)"
if python3 -c "import paho.mqtt" 2>/dev/null; then
    ok "paho-mqtt already installed"
else
    warn "paho-mqtt not installed"
    if ask_yn "Install paho-mqtt now?" y; then
        installed=""
        # Prefer the distro package on apt/dnf-based systems -- it avoids the
        # PEP 668 "externally-managed-environment" pip error entirely.
        if [ -n "$PKG_MGR" ] && ask_yn "Install via the system package manager (recommended)?" y; then
            need_sudo
            case "$PKG_MGR" in
                apt) $SUDO apt-get install -y python3-paho-mqtt && installed="yes" ;;
                dnf) $SUDO dnf install -y python3-paho-mqtt && installed="yes" ;;
                yum) $SUDO yum install -y python3-paho-mqtt && installed="yes" ;;
                pacman) $SUDO pacman -Sy --noconfirm python-paho-mqtt && installed="yes" ;;
                zypper) $SUDO zypper install -y python3-paho-mqtt && installed="yes" ;;
            esac
        fi
        # Fall back to pip if the distro package route was skipped or failed.
        if [ -z "$installed" ]; then
            if pip3 install paho-mqtt 2>/tmp/sfgw_pip_err; then
                installed="yes"
            elif grep -q "externally-managed-environment" /tmp/sfgw_pip_err; then
                warn "System Python is externally managed (PEP 668)."
                info "Retrying with: pip3 install --user --break-system-packages"
                if pip3 install --user --break-system-packages paho-mqtt; then
                    installed="yes"
                fi
            else
                cat /tmp/sfgw_pip_err >&2
            fi
            rm -f /tmp/sfgw_pip_err
        fi
        if [ -n "$installed" ]; then
            ok "paho-mqtt installed"
        else
            die "Could not install paho-mqtt. Install it manually (see the README) and re-run."
        fi
    else
        warn "paho-mqtt is required -- the service will fail without it"
    fi
fi

# ---------------------------------------------------------------------------
# Install location
# ---------------------------------------------------------------------------
hdr "Installation directory"
echo "  Common choices:"
echo "    /usr/local/bin          -- system-wide (needs sudo)"
echo "    /opt/sfgw               -- dedicated directory (needs sudo)"
echo "    $HOME/sfgw          -- user home (no sudo)"
echo
# When running as root (e.g. via sudo), default to a system-wide location;
# otherwise default to the user's home so no sudo is needed.
if [ "$EUID" -eq 0 ]; then
    DEFAULT_INSTALL_DIR="/usr/local/bin"
else
    DEFAULT_INSTALL_DIR="$HOME/sfgw"
fi
ask INSTALL_DIR "Install directory" "$DEFAULT_INSTALL_DIR"
INSTALL_DIR="${INSTALL_DIR%/}"

if [ ! -d "$INSTALL_DIR" ]; then
    mkdir -p "$INSTALL_DIR" || die "Cannot create $INSTALL_DIR"
    ok "Created $INSTALL_DIR"
fi

BRIDGE_DEST="${INSTALL_DIR}/sfgw_serial_bridge.py"
cp "$BRIDGE_SRC" "$BRIDGE_DEST"
chmod +x "$BRIDGE_DEST"
ok "Installed bridge script: $BRIDGE_DEST"

# Verify the installed script accepts --config
if ! python3 "$BRIDGE_DEST" --help 2>&1 | grep -q -- "--config"; then
    die "Installed script does not support --config. Make sure you are using the latest sfgw_serial_bridge.py."
fi
ok "Bridge script supports --config"

# ---------------------------------------------------------------------------
# Config file
# ---------------------------------------------------------------------------
hdr "MQTT configuration"
echo "  These settings tell the bridge how to reach the broker."
if [ -n "$BROKER_INSTALLED" ]; then
    info "Using the broker you just configured on this machine."
    info "The bridge runs here too, so it connects via localhost."
fi
echo

# If we just configured a local broker, the bridge connects to it on localhost
# (same machine) and reuses the credentials we set. The gateway, being a
# separate device, will use the LAN IP instead -- printed in the summary below.
DEF_BROKER="localhost"
DEF_USER="$BROKER_SET_USER"

ask MQTT_BROKER   "MQTT broker host or IP"  "$DEF_BROKER"
ask MQTT_PORT     "MQTT broker port"         "1883"
ask MQTT_PREFIX   "MQTT topic prefix"        "splitflap"
ask MQTT_USER     "MQTT username (leave blank if none)" "$DEF_USER"
if [ -n "$MQTT_USER" ]; then
    # If it matches the broker user we just set, reuse that password silently.
    if [ -n "$BROKER_INSTALLED" ] && [ "$MQTT_USER" = "$BROKER_SET_USER" ] && [ -n "$BROKER_SET_PASS" ]; then
        MQTT_PASS="$BROKER_SET_PASS"
        info "Reusing the password you set for the broker."
    else
        printf "  ${BOLD}MQTT password${RST}: "
        read_secret MQTT_PASS; echo
    fi
else
    MQTT_PASS=""
fi

hdr "Virtual serial port"
ask LINK_PATH "Symlink path for virtual port" "/tmp/ttyVSF0"

VERBOSE="false"
if ask_yn "Enable verbose logging (prints each frame)?" n; then
    VERBOSE="true"
fi

# Decide config file location
if [[ "$INSTALL_DIR" == "$HOME"* ]]; then
    CFG_DIR="$HOME/.config/sfgw-bridge"
    CFG_FILE="$CFG_DIR/config.ini"
    SYSTEMD_DIR="$HOME/.config/systemd/user"
    SYSTEMD_USER_FLAG="--user"
    SYSTEMD_WANTED_BY="default.target"
    RUN_AS_USER="$USER"
else
    CFG_DIR="/etc"
    CFG_FILE="/etc/sfgw-bridge.conf"
    SYSTEMD_DIR="/etc/systemd/system"
    SYSTEMD_USER_FLAG=""
    SYSTEMD_WANTED_BY="multi-user.target"
    # For a system service, run as the human who invoked sudo (if any) rather
    # than root, so the bridge has a normal user's permissions. Fall back to
    # the current user when not under sudo.
    RUN_AS_USER="${SUDO_USER:-$USER}"
fi

mkdir -p "$CFG_DIR"
cat > "$CFG_FILE" << CFGEOF
[mqtt]
broker   = ${MQTT_BROKER}
port     = ${MQTT_PORT}
prefix   = ${MQTT_PREFIX}
user     = ${MQTT_USER}
password = ${MQTT_PASS}

[bridge]
link    = ${LINK_PATH}
verbose = ${VERBOSE}
CFGEOF
chmod 600 "$CFG_FILE"   # protect credentials
ok "Config file written: $CFG_FILE"

# ---------------------------------------------------------------------------
# systemd service
# ---------------------------------------------------------------------------
hdr "Creating systemd service"

SERVICE_NAME="sfgw-bridge"
SERVICE_FILE="${SYSTEMD_DIR}/${SERVICE_NAME}.service"
mkdir -p "$SYSTEMD_DIR"

# A user service (systemctl --user) must NOT contain a User= directive -- it
# already runs as the invoking user, and including User= makes systemd reject
# the unit ("control process exited with error code"). Only set User= for a
# system-level service. The trailing newline is embedded so that, when omitted,
# no blank line is left in the unit file.
if [ "$SYSTEMD_USER_FLAG" = "--user" ]; then
    SVC_USER_LINE=""
else
    SVC_USER_LINE="User=${RUN_AS_USER}"$'\n'
fi

cat > "$SERVICE_FILE" << SVCEOF
[Unit]
Description=Split-Flap Gateway RS485 Serial Bridge
Documentation=https://github.com/avandeputte/SplitFlapGateway
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
${SVC_USER_LINE}ExecStart=${PYTHON} ${BRIDGE_DEST} --config ${CFG_FILE}
Restart=on-failure
RestartSec=10
# Give the virtual port time to settle before dependent services start
ExecStartPost=/bin/sleep 2

# Ensure the symlink is cleaned up if the service crashes
ExecStopPost=/bin/sh -c 'rm -f ${LINK_PATH}'

StandardOutput=journal
StandardError=journal
SyslogIdentifier=sfgw-bridge

[Install]
WantedBy=${SYSTEMD_WANTED_BY}
SVCEOF

ok "Service file written: $SERVICE_FILE"

# ---------------------------------------------------------------------------
# Enable and start
# ---------------------------------------------------------------------------
hdr "Enabling and starting service"

if [ "$SYSTEMD_USER_FLAG" = "--user" ]; then
    # User-level service
    systemctl --user daemon-reload
    systemctl --user enable "$SERVICE_NAME"
    # A --user service only runs while the user has an active login session
    # unless "lingering" is enabled. Enable it so the bridge starts at boot.
    if command -v loginctl &>/dev/null; then
        if loginctl enable-linger "$USER" 2>/dev/null; then
            ok "Enabled lingering so the service starts at boot"
        else
            warn "Could not enable lingering (needs sudo). To start at boot, run:"
            warn "    sudo loginctl enable-linger $USER"
        fi
    fi
    if ask_yn "Start the service now?" y; then
        systemctl --user start "$SERVICE_NAME"
        sleep 2
        if systemctl --user is-active --quiet "$SERVICE_NAME"; then
            ok "Service is running"
        else
            warn "Service did not start cleanly. Recent log output:"
            echo
            systemctl --user status "$SERVICE_NAME" --no-pager -l 2>&1 | sed 's/^/    /' | head -n 20 || true
            journalctl --user -u "$SERVICE_NAME" -n 20 --no-pager 2>&1 | sed 's/^/    /' || true
            echo
            echo "  Full logs:  journalctl --user -u $SERVICE_NAME -n 40"
        fi
    fi
    echo
    info "Service management commands:"
    info "  systemctl --user status  $SERVICE_NAME"
    info "  systemctl --user stop    $SERVICE_NAME"
    info "  systemctl --user start   $SERVICE_NAME"
    info "  systemctl --user restart $SERVICE_NAME"
    info "  journalctl --user -u $SERVICE_NAME -f"
else
    # System-level service
    if [ "$EUID" -ne 0 ]; then
        warn "Not running as root -- using sudo for systemctl commands"
        SUDO="sudo"
    else
        SUDO=""
    fi
    $SUDO systemctl daemon-reload
    $SUDO systemctl enable "$SERVICE_NAME"
    if ask_yn "Start the service now?" y; then
        $SUDO systemctl start "$SERVICE_NAME"
        sleep 2
        if $SUDO systemctl is-active --quiet "$SERVICE_NAME"; then
            ok "Service is running"
        else
            warn "Service did not start cleanly. Recent log output:"
            echo
            $SUDO systemctl status "$SERVICE_NAME" --no-pager -l 2>&1 | sed 's/^/    /' | head -n 20 || true
            $SUDO journalctl -u "$SERVICE_NAME" -n 20 --no-pager 2>&1 | sed 's/^/    /' || true
            echo
            echo "  Full logs:  journalctl -u $SERVICE_NAME -n 40"
        fi
    fi
    echo
    info "Service management commands:"
    info "  systemctl status  $SERVICE_NAME"
    info "  systemctl stop    $SERVICE_NAME"
    info "  systemctl start   $SERVICE_NAME"
    info "  systemctl restart $SERVICE_NAME"
    info "  journalctl -u $SERVICE_NAME -f"
fi

# ---------------------------------------------------------------------------
# Optional: download and install the splitflap-os web application
# (https://github.com/csader/splitflap-os) and point it at the virtual port.
# ---------------------------------------------------------------------------
SFOS_INSTALLED=""
SFOS_DIR=""
hdr "splitflap-os web application (optional)"
echo "  splitflap-os (by csader) is a separate web UI with 40+ apps -- weather,"
echo "  stocks, word clock, playlists, and more -- that drives the display."
echo "  It can run on this same machine and talk to the virtual serial port"
echo "  this bridge creates."
echo
echo "  Network setup (WiFi hotspot fallback, etc.) will be SKIPPED via the"
echo "  installer's --skip-network flag, since this machine is already on your"
echo "  network."
echo
if ask_yn "Download and install splitflap-os now?" n; then
    if ! command -v git &>/dev/null; then
        warn "git is not installed -- it is required to download splitflap-os."
        if [ -n "$PKG_MGR" ] && ask_yn "Install git now?" y; then
            need_sudo
            case "$PKG_MGR" in
                apt)    $SUDO apt-get install -y git || true ;;
                dnf)    $SUDO dnf install -y git || true ;;
                yum)    $SUDO yum install -y git || true ;;
                pacman) $SUDO pacman -Sy --noconfirm git || true ;;
                zypper) $SUDO zypper install -y git || true ;;
            esac
        fi
    fi

    if ! command -v git &>/dev/null; then
        warn "git still not available -- skipping splitflap-os install."
    else
        ask SFOS_PARENT "Directory to install splitflap-os into" "$HOME"
        SFOS_PARENT="${SFOS_PARENT%/}"
        SFOS_DIR="${SFOS_PARENT}/splitflap-os"

        clone_ok=""
        if [ -d "$SFOS_DIR/.git" ]; then
            info "splitflap-os already present at $SFOS_DIR -- updating it."
            ( cd "$SFOS_DIR" && git pull --ff-only ) && clone_ok="yes" \
                || warn "git pull failed -- using the existing copy as-is" 
            [ -d "$SFOS_DIR" ] && clone_ok="yes"
        else
            if git clone https://github.com/csader/splitflap-os.git "$SFOS_DIR"; then
                clone_ok="yes"
            else
                warn "git clone failed -- check your internet connection."
            fi
        fi

        if [ -n "$clone_ok" ]; then
            ok "splitflap-os downloaded to $SFOS_DIR"

            # Configure server/settings.json to use the bridge's virtual port
            # BEFORE running the installer, so the app starts already pointed at
            # the right device. We use Python (already verified present) to edit
            # the JSON safely -- it preserves any other keys and creates the file
            # (and parent dir) if the installer hasn't generated it yet.
            SFOS_SETTINGS="${SFOS_DIR}/server/settings.json"
            if "$PYTHON" - "$SFOS_SETTINGS" "$LINK_PATH" << 'PYEOF'
import json, os, sys
path, port = sys.argv[1], sys.argv[2]
data = {}
if os.path.isfile(path):
    try:
        with open(path) as f:
            data = json.load(f)
        if not isinstance(data, dict):
            data = {}
    except Exception:
        data = {}   # malformed/empty -> start fresh but keep a backup
        try:
            os.replace(path, path + ".bak")
        except Exception:
            pass
os.makedirs(os.path.dirname(path), exist_ok=True)
data["serial_port"] = port
with open(path, "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
print(path)
PYEOF
            then
                ok "Configured serial_port = $LINK_PATH in server/settings.json"
            else
                warn "Could not write $SFOS_SETTINGS automatically."
                warn "After install, set \"serial_port\": \"$LINK_PATH\" in that file."
            fi

            # Run the installer with --skip-network.
            if [ -f "$SFOS_DIR/setup/install.sh" ]; then
                echo
                info "Running the splitflap-os installer (sudo, --skip-network)..."
                need_sudo
                if ( cd "$SFOS_DIR" && $SUDO bash setup/install.sh --skip-network ); then
                    ok "splitflap-os installed"
                    SFOS_INSTALLED="yes"
                    # The installer may regenerate settings.json; re-apply our
                    # serial_port afterwards to be safe.
                    "$PYTHON" - "$SFOS_SETTINGS" "$LINK_PATH" << 'PYEOF' || true
import json, os, sys
path, port = sys.argv[1], sys.argv[2]
data = {}
if os.path.isfile(path):
    try:
        with open(path) as f:
            data = json.load(f)
        if not isinstance(data, dict):
            data = {}
    except Exception:
        data = {}
os.makedirs(os.path.dirname(path), exist_ok=True)
data["serial_port"] = port
with open(path, "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
PYEOF
                    ok "Re-applied serial_port = $LINK_PATH after install"
                else
                    warn "The splitflap-os installer reported an error."
                    warn "You can re-run it manually:"
                    warn "    cd $SFOS_DIR && sudo bash setup/install.sh --skip-network"
                fi
            else
                warn "setup/install.sh not found in the cloned repo."
                warn "Check $SFOS_DIR and run its installer manually."
            fi
        fi
    fi
else
    info "Skipping splitflap-os install."
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
hdr "Setup complete"
printf "  ${GRN}Virtual port:${RST}  %s\n"  "$LINK_PATH"
printf "  ${GRN}Bridge script:${RST} %s\n"  "$BRIDGE_DEST"
printf "  ${GRN}Config file:${RST}   %s\n"  "$CFG_FILE"
printf "  ${GRN}Service:${RST}       %s\n"  "$SERVICE_NAME"
echo
info "To change settings, edit $CFG_FILE and restart the service."
info "The virtual port is re-created automatically each time the service starts."
echo

# splitflap-os summary (only if we installed it this run)
if [ -n "$SFOS_INSTALLED" ]; then
    SFOS_LAN_IP="$(detect_lan_ip)"
    [ -z "$SFOS_LAN_IP" ] && SFOS_LAN_IP="<this-computer's-LAN-IP>"
    hr
    printf "${BOLD}  splitflap-os web application${RST}\n"
    hr
    echo
    printf "  ${GRN}Installed at:${RST}   %s\n" "$SFOS_DIR"
    printf "  ${GRN}Serial port:${RST}    %s  (the virtual port from this bridge)\n" "$LINK_PATH"
    printf "  ${GRN}Web UI:${RST}         http://%s\n" "$SFOS_LAN_IP"
    echo
    info "Network setup was skipped (--skip-network); this machine uses its"
    info "existing network connection."
    info "If splitflap-os regenerates its config, set \"serial_port\" back to"
    info "$LINK_PATH in ${SFOS_DIR}/server/settings.json"
    echo
fi

# ---------------------------------------------------------------------------
# Gateway connection instructions -- what to enter in the gateway's web UI.
# The gateway is a SEPARATE device, so it must reach the broker by LAN IP,
# never localhost.
# ---------------------------------------------------------------------------
if [ -n "$BROKER_INSTALLED" ]; then
    # Resolve the LAN IP for display; fall back to a clear placeholder.
    GW_BROKER_IP="$BROKER_LAN_IP"
    if [ -z "$GW_BROKER_IP" ]; then
        GW_BROKER_IP="$(detect_lan_ip)"
    fi
    if [ -z "$GW_BROKER_IP" ]; then
        GW_BROKER_IP="<this-computer's-LAN-IP>"
    fi

    hr
    printf "${BOLD}  Point the Split-Flap gateway at this broker${RST}\n"
    hr
    echo
    echo "  Open the gateway's web UI in a browser, go to the Settings page,"
    echo "  and in the MQTT section enter:"
    echo
    printf "    ${BOLD}Broker Host / IP${RST} : ${GRN}%s${RST}\n" "$GW_BROKER_IP"
    printf "    ${BOLD}Port${RST}             : ${GRN}1883${RST}\n"
    printf "    ${BOLD}Topic Prefix${RST}     : ${GRN}%s${RST}\n" "${MQTT_PREFIX:-splitflap}"
    if [ -n "$BROKER_SET_USER" ]; then
        printf "    ${BOLD}Username${RST}         : ${GRN}%s${RST}\n" "$BROKER_SET_USER"
        printf "    ${BOLD}Password${RST}         : ${GRN}%s${RST}\n" "(the password you just set)"
    else
        printf "    ${BOLD}Username${RST}         : ${GRN}(leave blank)${RST}\n"
        printf "    ${BOLD}Password${RST}         : ${GRN}(leave blank)${RST}\n"
    fi
    echo
    warn "Use the LAN IP above, NOT 'localhost' -- the gateway is a separate"
    warn "device on your network and cannot reach this machine via localhost."
    echo
    info "Then click 'Save MQTT' and 'Test Connection' on the gateway's Settings"
    info "page. A successful test confirms the gateway can reach this broker."
    if [ "$GW_BROKER_IP" = "<this-computer's-LAN-IP>" ]; then
        echo
        warn "Could not auto-detect this machine's LAN IP. Find it with:"
        warn "    hostname -I        (first address)   or   ip -4 addr"
    fi
    echo
    info "Tip: if this machine's IP can change, consider a DHCP reservation on"
    info "your router so the gateway's broker address stays valid."
    echo
fi
