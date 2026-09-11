#!/usr/bin/env bash
# bench_friend.sh — send a text on the Firefly channel FROM Firefly 1,
# over its serial console, so the phone app's Inbox can be exercised by
# hand without a second radio.
#
#   app/tools/bench_friend.sh ["message text"]
#
# Firefly 1 is the ONLY board this script (or anything else) is allowed
# to touch over serial (docs/hardware/heltec-v3.md's bench table;
# Firefly 2 is slice A's, over Bluetooth). Defaults to
# /dev/cu.usbserial-4; override with FIREFLY_SERIAL_PORT. Uses the
# `meshtastic` CLI (MESHTASTIC_BIN, default
# /Users/jakeholland/.local/bin/meshtastic) — the only OTHER permitted
# user of this port, and never at the same time as `swift test`'s
# HardwareTests: a Meshtastic serial port is single-client
# (docs/specs/A01-companion-app.md, "Serial — macOS only" > "Contention
# warning"), so this script refuses to run while something else already
# has the port open rather than hanging.
#
# This sends a plain broadcast text (`--sendtext`, no --ch-index: the
# Firefly channel is Firefly 1's primary, index 0) — nothing here
# changes any device setting.
set -euo pipefail

MESHTASTIC="${MESHTASTIC_BIN:-/Users/jakeholland/.local/bin/meshtastic}"
PORT="${FIREFLY_SERIAL_PORT:-/dev/cu.usbserial-4}"

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  sed -n '2,20p' "$0"
  exit 0
fi

MESSAGE="${1:-friend compass check $(date +%H:%M:%S)}"

command -v "$MESHTASTIC" >/dev/null 2>&1 || {
  echo "error: meshtastic CLI not found at $MESHTASTIC (set MESHTASTIC_BIN)" >&2
  exit 2
}

[ -e "$PORT" ] || {
  echo "error: $PORT does not exist — is Firefly 1 plugged in?" >&2
  exit 2
}

# Best-effort contention check. lsof cannot see every possible holder
# (an app-hosted process can obscure its own open descriptors from a
# plain lsof depending on sandboxing), so this is a courtesy that
# catches the common case — a `swift test` HardwareTests run or a
# console session — not a guarantee. If this fires, the underlying
# `open()` would have failed anyway; this just gives a clear message
# instead of `meshtastic`'s own timeout-shaped one.
if command -v lsof >/dev/null 2>&1 && lsof "$PORT" >/dev/null 2>&1; then
  echo "error: $PORT is already open by another process." >&2
  echo "       Never run this while FIREFLY_HARDWARE=1 swift test --filter Hardware holds the port." >&2
  lsof "$PORT" >&2 || true
  exit 1
fi

echo "== sending on Firefly 1's primary channel ($PORT): \"$MESSAGE\""
"$MESHTASTIC" --port "$PORT" --sendtext "$MESSAGE"
echo "== sent. Watch the phone app's Inbox (CREW/broadcast) for it."
