#!/usr/bin/env bash
#
# CivSwitch Moonlight bridge — FTP port-forward helper.
#
# Opens a single port on the Pi's home-LAN (uplink) interface and DNATs it to
# the Switch's FTP server on the air-gapped AP subnet, so you can manage the
# Switch's SD card from a PC on the home LAN without giving the Switch internet
# access.
#
# Usage:
#   sudo ./setup-ftp-forward.sh                 # Switch 192.168.4.2, port 5000
#   sudo ./setup-ftp-forward.sh 192.168.4.24 1337
#
# The forward is persistent: it installs a systemd oneshot that re-applies the
# rules on every boot (after the air-gap firewall service rebuilds its table).
#
# NOTE: only the FTP *control* port is forwarded. Use a client in ACTIVE
# transfer mode (not passive) — see the README's "FTP to the Switch" section.
# Passive mode advertises a random data port + the Switch's unroutable
# 192.168.4.x address, so directory listings fail over this NAT.

set -euo pipefail

# ── Config ─────────────────────────────────────────────────────────
SWITCH_IP="${1:-192.168.4.2}"   # the Switch's address on the AP
FTP_PORT="${2:-5000}"           # your FTP server's port (sys-ftpd default 5000)
ETH_IF="eth0"                   # uplink interface towards the home LAN
NFT_TABLE="moonlight_bridge"
INSTALL_DIR="/opt/civswitch-ftp"
SERVICE_NAME="civswitch-ftp.service"

if [[ $EUID -ne 0 ]]; then
  echo "Run as root:  sudo $0 [switch-ip] [ftp-port]" >&2
  exit 1
fi

# Light validation.
if [[ ! "$FTP_PORT" =~ ^[0-9]+$ ]] || (( FTP_PORT < 1 || FTP_PORT > 65535 )); then
  echo "ERROR: '$FTP_PORT' is not a valid port." >&2
  exit 1
fi
if [[ -z "$SWITCH_IP" ]]; then
  echo "ERROR: switch IP is empty." >&2
  exit 1
fi

# Auto-detect the uplink interface if the default isn't present.
if ! ip link show "$ETH_IF" >/dev/null 2>&1; then
  DETECTED="$(ip route | awk '/^default/{print $5; exit}')"
  if [ -n "$DETECTED" ]; then
    ETH_IF="$DETECTED"
  else
    echo "ERROR: uplink interface '$ETH_IF' not found." >&2
    echo "List yours with 'ip link', then edit ETH_IF in this script." >&2
    exit 1
  fi
fi

echo "== FTP forward: $ETH_IF:$FTP_PORT -> $SWITCH_IP:$FTP_PORT =="

# The air-gap firewall must already be installed (setup.sh). If its table is
# missing now, we still install the service and it will apply once the bridge
# firewall is up; just flag it.
if ! nft list table ip "$NFT_TABLE" >/dev/null 2>&1; then
  echo "WARN: nft table '$NFT_TABLE' not present — run the bridge setup.sh first." >&2
  echo "      Installing the persistent service anyway; it applies on next boot." >&2
fi

echo "== Installing the idempotent applier at $INSTALL_DIR/apply.sh =="
mkdir -p "$INSTALL_DIR"
cat > "$INSTALL_DIR/apply.sh" <<EOF
#!/bin/bash
# Re-applies the FTP port-forward to the moonlight_bridge nft table.
# Idempotent; exits quietly if the firewall table isn't up yet.
set -euo pipefail

nft list table ip $NFT_TABLE >/dev/null 2>&1 || exit 0

nft list chain ip $NFT_TABLE prerouting >/dev/null 2>&1 || \\
  nft add chain ip $NFT_TABLE prerouting '{ type nat hook prerouting priority dstnat; policy accept; }'

nft list chain ip $NFT_TABLE prerouting 2>/dev/null | grep -q "dport $FTP_PORT" || \\
  nft add rule ip $NFT_TABLE prerouting iifname "$ETH_IF" tcp dport $FTP_PORT dnat to $SWITCH_IP:$FTP_PORT

nft list chain ip $NFT_TABLE forward 2>/dev/null | grep -q "daddr $SWITCH_IP" || \\
  nft add rule ip $NFT_TABLE forward ip daddr $SWITCH_IP tcp dport $FTP_PORT accept
EOF
chmod +x "$INSTALL_DIR/apply.sh"

echo "== Installing $SERVICE_NAME (re-applies on every boot) =="
cat > "/etc/systemd/system/$SERVICE_NAME" <<EOF
[Unit]
Description=CivSwitch Moonlight bridge FTP port-forward (LAN -> Switch)
Wants=network-online.target
After=network-online.target civswitch-moonlight-firewall.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$INSTALL_DIR/apply.sh

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable "$SERVICE_NAME"

echo "== Applying now =="
"$INSTALL_DIR/apply.sh" || true

echo
echo "Done."
echo "  Forward:      $ETH_IF:$FTP_PORT -> $SWITCH_IP:$FTP_PORT"
echo "  Connect from a PC on the LAN to <Pi LAN IP>:$FTP_PORT (ACTIVE mode)."
echo "  Air-gap:      unchanged — the Switch still can't reach the internet."
echo
echo "Undo:"
echo "  sudo systemctl disable --now $SERVICE_NAME"
echo "  sudo systemctl restart civswitch-moonlight-firewall.service"
