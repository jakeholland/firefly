#!/usr/bin/env bash
# Firefly crew-node setup: flash (optional) + configure a Seeed XIAO ESP32S3 /
# Wio-SX1262 running stock Meshtastic so it speaks the Firefly crew channel.
#
#   tools/mesh/setup_node.sh --port /dev/cu.usbmodemXXXX --name "Taylor" [--short TAY]
#                            [--flash /path/to/firmware-esp32s3-<ver> dir] [--puck] [--url <channel url>]
#
# --flash   : directory unpacked from meshtastic/firmware's firmware-esp32s3-<ver>.zip;
#             runs its device-install.sh for the seeed-xiao-s3 image (full erase + install).
# --puck    : also enable the Serial module in PROTO mode on D3/D1 (GPIO4/GPIO2) for a
#             Firefly puck wired to this node. Crew-only nodes leave it off.
# --url     : the Firefly channel URL (from `meshtastic --qr` on the first node). Defaults to
#             $FIREFLY_CHANNEL_URL or ~/.config/firefly/channel.url. Never commit it.
#
# Needs: meshtastic CLI (`uv tool install "meshtastic[cli]"`), esptool for --flash.
set -euo pipefail

PORT=""; NAME=""; SHORT=""; FLASH_DIR=""; PUCK=0; URL="${FIREFLY_CHANNEL_URL:-}"
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="$2"; shift 2;;
    --name) NAME="$2"; shift 2;;
    --short) SHORT="$2"; shift 2;;
    --flash) FLASH_DIR="$2"; shift 2;;
    --puck) PUCK=1; shift;;
    --url) URL="$2"; shift 2;;
    -h|--help) sed -n '2,16p' "$0"; exit 0;;
    *) echo "unknown arg $1" >&2; exit 2;;
  esac
done
[ -n "$PORT" ] && [ -n "$NAME" ] || { echo "need --port and --name" >&2; exit 2; }
[ -z "$URL" ] && [ -f "$HOME/.config/firefly/channel.url" ] && URL="$(tr -d '[:space:]' < "$HOME/.config/firefly/channel.url")"
[ -n "$URL" ] || { echo "no channel URL: pass --url, set FIREFLY_CHANNEL_URL, or write ~/.config/firefly/channel.url" >&2; exit 2; }
if [ -z "$SHORT" ]; then SHORT="$(printf '%s' "$NAME" | tr '[:lower:]' '[:upper:]' | cut -c1-4)"; fi
command -v meshtastic >/dev/null || { echo "meshtastic CLI not found (uv tool install \"meshtastic[cli]\")" >&2; exit 2; }

M() { meshtastic --port "$PORT" "$@"; }

if [ -n "$FLASH_DIR" ]; then
  BIN="$(ls "$FLASH_DIR"/firmware-seeed-xiao-s3-*.bin 2>/dev/null | grep -v update | head -1)"
  [ -n "$BIN" ] || { echo "no firmware-seeed-xiao-s3-*.bin in $FLASH_DIR" >&2; exit 2; }
  echo "== flashing $(basename "$BIN") to $PORT (full install)"
  ( cd "$FLASH_DIR" && bash ./device-install.sh -p "$PORT" -f "$BIN" )
  echo "== waiting for the node to boot"
  for i in $(seq 1 30); do [ -e "$PORT" ] && break; sleep 1; done
  sleep 8
fi

echo "== identity: $NAME ($SHORT)"
M --set-owner "$NAME" --set-owner-short "$SHORT"
echo "== region + role"
M --set lora.region US --set device.role CLIENT
echo "== Firefly channel (positionPrecision 32 comes with the URL)"
M --seturl "$URL"
echo "== position cadence for the friend compass"
M --set position.position_broadcast_secs 120 --set position.broadcast_smart_minimum_distance 20 \
  --set position.broadcast_smart_minimum_interval_secs 60 --set position.gps_mode ENABLED
echo "== radios: bluetooth on for the phone app, wifi off"
M --set bluetooth.enabled true --set network.wifi_enabled false
if [ "$PUCK" = 1 ]; then
  echo "== serial module for the Firefly puck (PROTO 115200, rxd GPIO4 = D3, txd GPIO2 = D1)"
  M --set serial.enabled true --set serial.mode PROTO --set serial.baud BAUD_115200 --set serial.rxd 4 --set serial.txd 2
fi

echo "== readback"
M --info | grep -E '^Owner|"region"|"modemPreset"|Index 0|"positionBroadcastSecs"|"gpsMode"|"enabled": true' | sed 's/"psk": "[^"]*"/"psk": "<set>"/' | head -12
echo "done: $NAME on the Firefly channel."
