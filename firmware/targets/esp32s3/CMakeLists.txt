# targets/esp32s3 — ESP-IDF project for the ESP32-S3 UI brain (S15 slice a).
#
# This is a SEPARATE build entry point from firmware/CMakeLists.txt (the
# sim's plain-CMake project) — idf.py drives this one, not
# `cmake -S firmware -B build`. It does not add_subdirectory() the sim
# tree at all; instead every component below points its SRCS/INCLUDE_DIRS
# straight at the existing firmware/{core,festpack,meshclient,app}
# sources (see each component's CMakeLists.txt), so core/festpack/
# meshclient/app compile UNCHANGED — literally the same .c files the sim
# target builds, not a copy (per S15's contract: pure-C11 with zero deps
# should just cross-compile).
#
# Build (after `source ~/esp/esp-idf/export.sh` — see docs/specs/
# S15-esp32s3-target.md and the S15a PR body for the full command):
#   idf.py -C firmware/targets/esp32s3 set-target esp32s3
#   idf.py -C firmware/targets/esp32s3 build
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(firefly_esp32s3)
