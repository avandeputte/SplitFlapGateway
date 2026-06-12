#!/usr/bin/env bash
# sfgw_setup.sh -- Split-Flap Gateway bridge setup and service installer
# Installs sfgw_serial_bridge.py and creates a systemd service to run it
# automatically on boot.
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

ask() {
    # ask <VAR> <prompt> <default>
    local var="$1" prompt="$2" default="$3"
    local val
    printf "  ${BOLD}%s${RST} [%s]: " "$prompt" "$default"
    read -r val
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
    read -r val
    val="${val:-$default}"
    [[ "$val" =~ ^[Yy] ]]
}

# ---------------------------------------------------------------------------
# Locate the bridge script (next to this setup script, or in CWD)
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRIDGE_SRC="${SCRIPT_DIR}/sfgw_serial_bridge.py"
[ -f "$BRIDGE_SRC" ] || die "sfgw_serial_bridge.py not found next to this script (looked in $SCRIPT_DIR)"

# ---------------------------------------------------------------------------
# Welcome
# ---------------------------------------------------------------------------
clear
hdr "Split-Flap Gateway -- Bridge Service Setup"
echo "  This script will:"
echo "    1. Install sfgw_serial_bridge.py to a directory you choose"
echo "    2. Install paho-mqtt if not already present"
echo "    3. Write a config file with your MQTT settings"
echo "    4. Create and enable a systemd service"
echo
echo "  Run with sudo if you want to install system-wide."
echo "  No sudo needed for a user-level install."
echo
if ! ask_yn "Continue?" y; then
    echo "Aborted."; exit 0
fi

# ---------------------------------------------------------------------------
# Check systemd
# ---------------------------------------------------------------------------
hdr "Checking prerequisites"

if ! command -v systemctl &>/dev/null; then
    die "systemd not found. This setup script requires a systemd-based Linux distro."
fi
ok "systemd found"

if ! command -v python3 &>/dev/null; then
    die "python3 not found. Please install Python 3.7+ first."
fi
PYTHON=$(command -v python3)
ok "python3 found: $PYTHON"

# Check / install paho-mqtt
if python3 -c "import paho.mqtt" 2>/dev/null; then
    ok "paho-mqtt already installed"
else
    warn "paho-mqtt not installed"
    if ask_yn "Install paho-mqtt now?" y; then
        pip3 install paho-mqtt || die "pip3 install paho-mqtt failed"
        ok "paho-mqtt installed"
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
ask INSTALL_DIR "Install directory" "$HOME/sfgw"
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
echo "  These settings will be saved to a config file."
echo "  You can query the gateway's web UI (Settings page) if unsure."
echo

ask MQTT_BROKER   "MQTT broker host or IP"  "localhost"
ask MQTT_PORT     "MQTT broker port"         "1883"
ask MQTT_PREFIX   "MQTT topic prefix"        "splitflap"
ask MQTT_USER     "MQTT username (leave blank if none)" ""
if [ -n "$MQTT_USER" ]; then
    printf "  ${BOLD}MQTT password${RST}: "
    read -rs MQTT_PASS; echo
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
    RUN_AS_USER="$USER"
else
    CFG_DIR="/etc"
    CFG_FILE="/etc/sfgw-bridge.conf"
    SYSTEMD_DIR="/etc/systemd/system"
    SYSTEMD_USER_FLAG=""
    RUN_AS_USER="$USER"
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

cat > "$SERVICE_FILE" << SVCEOF
[Unit]
Description=Split-Flap Gateway RS485 Serial Bridge
Documentation=https://github.com/your-repo/split-flap-gateway
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${RUN_AS_USER}
ExecStart=${PYTHON} ${BRIDGE_DEST} --config ${CFG_FILE}
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
WantedBy=default.target
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
    if ask_yn "Start the service now?" y; then
        systemctl --user start "$SERVICE_NAME"
        sleep 2
        if systemctl --user is-active --quiet "$SERVICE_NAME"; then
            ok "Service is running"
        else
            warn "Service did not start cleanly"
            echo
            echo "  Check logs with:"
            echo "    journalctl --user -u $SERVICE_NAME -n 40"
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
            warn "Service did not start cleanly"
            echo
            echo "  Check logs with:"
            echo "    journalctl -u $SERVICE_NAME -n 40"
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
