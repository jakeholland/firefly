#!/usr/bin/env bash
#
# link_core_sources.sh — (re)create the symlink farm that lets the
# FireflyCore SwiftPM target compile firmware/core's (and, since the
# festpack-from-fest-almanac work, firmware/festpack's) C11 sources IN
# PLACE.
#
# Why symlinks and not a copy: the puck and the phone must run the SAME
# crew/presence/radar/inbox/find/festpack-parsing logic, byte for byte. A
# copy is a fork with a grace period. A symlink farm cannot drift — but
# it CAN go stale (a new firmware/core/src/*.c or firmware/festpack/src/*.c
# that nobody linked), which is why
# app/FireflyKit/Tests/FireflyCoreTests/CoreSourceLinkTests.swift asserts
# the farm and firmware/{core,festpack}/{src,include} agree, and fails
# naming the missing file. That mirrors ff_check_sources_complete() in
# firmware/CMakeLists.txt, which does the same job for the CMake builds.
#
# Why festpack lands in the SAME FireflyCore target rather than a new
# one: fp_parse() is the ONE parser both the puck and the phone must run
# (docs/specs/S05-festpack.md) — a second Swift-side JSON schema parser
# would be exactly the drift this whole symlink-farm arrangement exists
# to prevent. festpack/src's only extra dependency is firmware/third_party's
# vendored jsmn.h (header-only — ff-jsmn is a CMake INTERFACE library
# with no .c of its own), so that one header is linked alongside
# core/platform's below.
#
# Why per-FILE links and not directory links:
#   - SwiftPM's source scanner is not guaranteed to descend into a
#     symlinked *directory*; a symlinked .c file is just a file to it.
#   - firmware/core/include, firmware/platform/include and
#     firmware/festpack/include have to end up in ONE flat directory,
#     because core/festpack headers include e.g. "ff_latlon.h"
#     (firmware/platform/include) and "jsmn.h" (firmware/third_party)
#     with quoted includes that resolve relative to the including
#     header's own directory. See firmware/platform/include/ff_latlon.h's
#     own top comment for why lat/lon lives in platform/ rather than
#     core/.
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
for d in firmware/core/src firmware/festpack/src; do
    for f in "${REPO_ROOT}/${d}"/*.c; do
        b="$(basename "$f")"
        ln -s "${UP}/${d}/${b}" "${SRC_LINK_DIR}/${b}"
        n_src=$((n_src + 1))
    done
done

n_inc=0
for d in firmware/core/include firmware/platform/include firmware/festpack/include; do
    for f in "${REPO_ROOT}/${d}"/*.h; do
        b="$(basename "$f")"
        ln -s "${UP}/${d}/${b}" "${INC_LINK_DIR}/${b}"
        n_inc=$((n_inc + 1))
    done
done

# jsmn.h (firmware/third_party) — header-only (JSMN_HEADER in fp_pack.h's
# declaration-only mode, JSMN_STATIC in fp_pack.c's own implementation
# TU), needed because fp_pack.h #include "jsmn.h"s it. Just the one
# vendored header, not the rest of third_party/ (stb_image.h etc. — no
# festpack dependency on those).
ln -s "${UP}/firmware/third_party/jsmn.h" "${INC_LINK_DIR}/jsmn.h"
n_inc=$((n_inc + 1))

echo "linked ${n_src} sources -> ${SRC_LINK_DIR}"
echo "linked ${n_inc} headers -> ${INC_LINK_DIR}"
