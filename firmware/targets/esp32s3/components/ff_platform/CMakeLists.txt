# ff_platform — ESP-IDF wrapper for firmware/platform (ff_clock.h,
# ff_latlon.h). Header-only, same shape as the sim build's ff-platform
# INTERFACE target (firmware/platform/CMakeLists.txt) — no .c files to
# compile, just an include path other components need.
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)

idf_component_register(
    INCLUDE_DIRS "${FF_FW_DIR}/platform/include"
)
