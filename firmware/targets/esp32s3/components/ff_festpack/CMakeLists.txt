# ff_festpack — ESP-IDF wrapper for firmware/festpack. Same source list
# as firmware/festpack/CMakeLists.txt's ff-festpack target, compiled
# UNCHANGED.
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(FF_FP_DIR ${FF_FW_DIR}/festpack)

# S26 slice (a) — ff_jsmn moved from PRIV_REQUIRES to REQUIRES: fp_pack.h
# now exposes jsmntok_t (JSMN_HEADER — declarations only) so callers can
# declare the caller-supplied token scratch fp_parse() takes, which means
# jsmn.h's include dir must propagate to fp_pack.h's own consumers (mirrors
# the sim build's firmware/festpack/CMakeLists.txt PUBLIC ff-jsmn change).
idf_component_register(
    SRCS
        "${FF_FP_DIR}/src/fp_pack.c"
        "${FF_FP_DIR}/src/ff_sched.c"
        "${FF_FP_DIR}/src/fp_t9words.c"
    INCLUDE_DIRS "${FF_FP_DIR}/include"
    REQUIRES ff_platform ff_jsmn
    PRIV_REQUIRES ff_core
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
