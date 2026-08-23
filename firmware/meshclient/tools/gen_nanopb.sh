#!/usr/bin/env bash
#
# gen_nanopb.sh — regenerate the nanopb C sources under meshclient/proto/
# from meshtastic/protobufs, via nanopb's protoc plugin.
#
# The output IS committed to the repo (meshclient/proto/meshtastic/*.pb.[ch])
# specifically so that a normal `cmake --build` never needs protoc, python,
# or network access — only this script does, and only when a maintainer
# deliberately wants to move the protobuf pin. See docs/specs/S03-meshclient.md.
#
# Requirements to RUN this script (not to build the project):
#   - protoc (the protobuf compiler)   e.g. `brew install protobuf`
#   - python3 with the `protobuf` pip package installed
#   - network access to clone the two pinned repos below
#
# Usage:
#   firmware/meshclient/tools/gen_nanopb.sh [output_dir]
#
# output_dir defaults to firmware/meshclient/proto (relative to repo root).

set -euo pipefail

# ---------------------------------------------------------------------------
# Pins — bump deliberately, note the new commits in the PR that changes them.
# ---------------------------------------------------------------------------
NANOPB_COMMIT="cad3c18ef15a663e30e3e43e3a752b66378adec1"        # tag 0.4.9.1
MESHTASTIC_PROTOBUFS_COMMIT="1b4cb00f3d6b0d620354a11fdd1e0b592f3cb7f5"

NANOPB_REPO="https://github.com/nanopb/nanopb.git"
MESHTASTIC_PROTOBUFS_REPO="https://github.com/meshtastic/protobufs.git"

# The specific .proto files we generate C for. This is mesh.proto's full
# transitive import closure (not the whole meshtastic/protobufs repo —
# admin.proto, atak's siblings under module_config, etc. that mesh.proto
# doesn't reach are left out). Decode scope v1 only touches MyNodeInfo,
# NodeInfo, Position and MeshPacket TEXT/POSITION/PRIVATE, but nanopb still
# needs the full oneof member types to compile FromRadio/ToRadio.
PROTO_FILES=(
    meshtastic/mesh.proto
    meshtastic/channel.proto
    meshtastic/config.proto
    meshtastic/device_ui.proto
    meshtastic/module_config.proto
    meshtastic/atak.proto
    meshtastic/portnums.proto
    meshtastic/telemetry.proto
    meshtastic/xmodem.proto
)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MESHCLIENT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${1:-${MESHCLIENT_DIR}/proto}"
OPTIONS_FILE="${SCRIPT_DIR}/mc_nanopb.options"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

command -v protoc >/dev/null 2>&1 || { echo "error: protoc not found (brew install protobuf)" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "error: python3 not found" >&2; exit 1; }
python3 -c "import google.protobuf" 2>/dev/null || {
    echo "error: python3 is missing the 'protobuf' package (pip install protobuf)" >&2
    exit 1
}

echo "==> cloning nanopb @ ${NANOPB_COMMIT}"
git clone --quiet "${NANOPB_REPO}" "${WORK_DIR}/nanopb"
git -C "${WORK_DIR}/nanopb" checkout --quiet "${NANOPB_COMMIT}"

echo "==> cloning meshtastic/protobufs @ ${MESHTASTIC_PROTOBUFS_COMMIT}"
git clone --quiet "${MESHTASTIC_PROTOBUFS_REPO}" "${WORK_DIR}/protobufs"
git -C "${WORK_DIR}/protobufs" checkout --quiet "${MESHTASTIC_PROTOBUFS_COMMIT}"

echo "==> generating into ${OUT_DIR}"
rm -rf "${OUT_DIR}/meshtastic"
mkdir -p "${OUT_DIR}"

(
    cd "${WORK_DIR}/protobufs"
    protoc \
        -I . \
        -I "${WORK_DIR}/nanopb/generator/proto" \
        --plugin="protoc-gen-nanopb=${WORK_DIR}/nanopb/generator/protoc-gen-nanopb" \
        --nanopb_out="-f${OPTIONS_FILE}:${OUT_DIR}" \
        "${PROTO_FILES[@]}"
)

echo "==> done. Generated files:"
find "${OUT_DIR}/meshtastic" -type f -name '*.pb.*' | sort
