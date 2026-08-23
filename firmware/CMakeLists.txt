cmake_minimum_required(VERSION 3.20)
project(firefly LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "" FORCE)
endif()

set(FF_TARGET "sim" CACHE STRING "Firefly build target: sim (esp32s3 arrives with S15)")
option(FF_BUILD_TESTS "Build Unity unit tests and wire them into ctest" ON)

if(NOT FF_TARGET STREQUAL "sim")
    message(FATAL_ERROR "FF_TARGET=${FF_TARGET} is not supported yet — only 'sim' exists until S15 adds esp32s3.")
endif()

# ---------------------------------------------------------------------------
# ff_apply_warnings(target) — -Wall -Wextra -Werror for OUR code only.
# Never applied to FetchContent'd or vendored third-party targets.
# ---------------------------------------------------------------------------
function(ff_apply_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror>
    )
endfunction()

# ---------------------------------------------------------------------------
# Third-party dependencies — FetchContent, pinned to commit hashes (never
# floating tags/branches), per AGENTS.md.
# ---------------------------------------------------------------------------
include(FetchContent)

# SDL2: system install, found via find_package. See firmware/README.md for
# the brew/apt install commands. Declared before LVGL so its headers are
# available to LVGL's SDL driver sources (LV_USE_SDL=1, see lv_conf.h).
find_package(SDL2 REQUIRED)

# LVGL v9.5.0
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        85aa60d18b3d5e5588d7b247abf90198f07c8a63 # v9.5.0
    GIT_SHALLOW    TRUE
)

# Unity (ThrowTheSwitch) v2.6.1
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        cbcd08fa7de711053a3deec6339ee89cad5d2697 # v2.6.1
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(lvgl unity)

# S03: meshclient's position tests compare doubles (GPS lat/lon need more
# precision than float32 offers); Unity only compiles its double-precision
# assertion helpers in when this is defined, and it has to be defined for
# unity.c itself (not just the test .c files) or the assertion symbols
# won't exist to link against.
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

# stb_image_write.h is vendored directly under third_party/ (single header,
# no build system of its own) — see firmware/third_party/CMakeLists.txt.

# LVGL's SDL driver (LV_USE_SDL=1) needs SDL2 headers to compile its own
# sources, regardless of whether ffsim ends up linking SDL2 directly.
if(TARGET SDL2::SDL2)
    target_link_libraries(lvgl PUBLIC SDL2::SDL2)
else()
    target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
endif()

# ---------------------------------------------------------------------------
# S03: nanopb runtime (pb_common/pb_decode/pb_encode) — vendored via
# FetchContent, pinned to a commit (tag 0.4.9.1). We deliberately do NOT
# add_subdirectory() nanopb's own CMakeLists.txt: it unconditionally
# find_program()s protoc and FATAL_ERRORs if missing, even when only the
# runtime library (not the generator) is wanted. The generated protobuf C
# sources under meshclient/proto/ are committed to the repo (see
# meshclient/tools/gen_nanopb.sh), so a normal build never needs protoc —
# only pb.h/pb_common.c/pb_decode.c/pb_encode.c from nanopb itself are
# needed, and we build those directly as our own tiny static library.
FetchContent_Declare(
    nanopb
    GIT_REPOSITORY https://github.com/nanopb/nanopb.git
    GIT_TAG        cad3c18ef15a663e30e3e43e3a752b66378adec1 # v0.4.9.1
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(nanopb)
if(NOT nanopb_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(nanopb)
endif()

add_library(ff-nanopb STATIC
    ${nanopb_SOURCE_DIR}/pb_common.c
    ${nanopb_SOURCE_DIR}/pb_decode.c
    ${nanopb_SOURCE_DIR}/pb_encode.c
)
target_include_directories(ff-nanopb PUBLIC ${nanopb_SOURCE_DIR})
# Vendored code: not ours, no -Wall -Wextra -Werror.

enable_testing()

add_subdirectory(third_party)
add_subdirectory(platform)
add_subdirectory(core)
add_subdirectory(meshclient)
add_subdirectory(festpack)
add_subdirectory(app)
add_subdirectory(targets/sim)
