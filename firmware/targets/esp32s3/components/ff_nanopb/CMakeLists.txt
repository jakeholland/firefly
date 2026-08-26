# ff_nanopb — nanopb runtime (pb_common/pb_decode/pb_encode), fetched at
# the SAME pinned commit as the sim build's ff-nanopb target
# (firmware/CMakeLists.txt) — v0.4.9.1. Kept in lockstep deliberately: a
# device build linking a different nanopb runtime than the sim/tests
# would defeat the point of both targets sharing meshclient's wire
# handling.
#
# Only the runtime, not nanopb's own CMakeLists.txt/generator — same
# reasoning as firmware/CMakeLists.txt's ff-nanopb target: nanopb's own
# CMakeLists.txt unconditionally find_program()s protoc and FATAL_ERRORs
# if it's missing, even though only pb.h/pb_common.c/pb_decode.c/
# pb_encode.c (the runtime) are needed here — the generated protobuf C
# sources under meshclient/proto/ are already committed (see
# meshclient/tools/gen_nanopb.sh), so a normal build never needs protoc.
# FetchContent_Populate (not FetchContent_MakeAvailable, which would
# add_subdirectory() nanopb's own CMakeLists.txt) mirrors that exactly.
#
# REAL PORTABILITY FINDING (S15a): ESP-IDF's component_get_requirements.cmake
# runs EVERY component CMakeLists.txt a first time in a restricted "early
# expansion" pass (CMAKE_BUILD_EARLY_EXPANSION=1) just to harvest
# idf_component_register()'s REQUIRES/PRIV_REQUIRES, before the real
# project() context exists. FetchContent's define_property() call is not
# scriptable in that context ("CMake Error ... define_property command is
# not scriptable"), so calling FetchContent unconditionally at file scope
# — which works fine in the sim's plain-CMake project — hard-fails
# `idf.py build` here. Guarding on CMAKE_BUILD_EARLY_EXPANSION is the
# documented ESP-IDF way to skip heavy/non-scriptable logic during that
# pass; idf_component_register() below still runs unconditionally, as it
# must in both passes.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(FetchContent)

    FetchContent_Declare(
        ff_nanopb_src
        GIT_REPOSITORY https://github.com/nanopb/nanopb.git
        GIT_TAG        cad3c18ef15a663e30e3e43e3a752b66378adec1 # v0.4.9.1 — must match firmware/CMakeLists.txt's nanopb pin
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ff_nanopb_src)
    if(NOT ff_nanopb_src_POPULATED)
        if(POLICY CMP0169)
            cmake_policy(SET CMP0169 OLD)
        endif()
        FetchContent_Populate(ff_nanopb_src)
    endif()
endif()

idf_component_register(
    SRCS
        "${ff_nanopb_src_SOURCE_DIR}/pb_common.c"
        "${ff_nanopb_src_SOURCE_DIR}/pb_decode.c"
        "${ff_nanopb_src_SOURCE_DIR}/pb_encode.c"
    INCLUDE_DIRS "${ff_nanopb_src_SOURCE_DIR}"
)

# Vendored code: not ours, no -Wall -Wextra -Werror (matches the sim
# build's treatment of the same sources).
