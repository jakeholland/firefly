#!/usr/bin/env bash
#
# link_core_sources.sh — (re)create the symlink farm that lets the
# FireflyCore SwiftPM target compile firmware/core's C11 sources IN PLACE.
#
# Why symlinks and not a copy: the puck and the phone must run the SAME
# crew/presence/radar/inbox/find logic, byte for byte. A copy is a fork
# with a grace period. A symlink farm cannot drift — but it CAN go stale
# (a new firmware/core/src/*.c that nobody linked), which is why
# app/FireflyKit/Tests/FireflyCoreTests/CoreSourceLinkTests.swift asserts
# the farm and firmware/core/{src,include} agree, and fails naming the
# missing file. That mirrors ff_check_sources_complete() in
# firmware/CMakeLists.txt, which does the same job for the CMake builds.
#
# Why per-FILE links and not two directory links:
#   - SwiftPM's source scanner is not guaranteed to descend into a
#     symlinked *directory*; a symlinked .c file is just a file to it.
#   - firmware/core/include and firmware/platform/include have to end up
#     in ONE flat directory, because core headers include "ff_latlon.h"
#     and "ff_clock.h" (firmware/platform/include) with quoted includes
#     that resolve relative to the including header's own directory.
#     See firmware/platform/include/ff_latlon.h's own top comment for why
#     those two types live in platform/ rather than core/.
#
# Usage:  app/tools/link_core_sources.sh          (from anywhere)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

TARGET_DIR="${REPO_ROOT}/app/FireflyKit/Sources/FireflyCore"
SRC_LINK_DIR="${TARGET_DIR}/src"
INC_LINK_DIR="${TARGET_DIR}/include"

# Depth from SRC_LINK_DIR / INC_LINK_DIR back up to the repo root:
#   src -> FireflyCore -> Sources -> FireflyKit -> app -> <root>
UP="../../../../.."

rm -rf "${SRC_LINK_DIR}" "${INC_LINK_DIR}"
mkdir -p "${SRC_LINK_DIR}" "${INC_LINK_DIR}"

n_src=0
for f in "${REPO_ROOT}"/firmware/core/src/*.c; do
    b="$(basename "$f")"
    ln -s "${UP}/firmware/core/src/${b}" "${SRC_LINK_DIR}/${b}"
    n_src=$((n_src + 1))
done

n_inc=0
for d in firmware/core/include firmware/platform/include; do
    for f in "${REPO_ROOT}/${d}"/*.h; do
        b="$(basename "$f")"
        ln -s "${UP}/${d}/${b}" "${INC_LINK_DIR}/${b}"
        n_inc=$((n_inc + 1))
    done
done

echo "linked ${n_src} sources -> ${SRC_LINK_DIR}"
echo "linked ${n_inc} headers -> ${INC_LINK_DIR}"
