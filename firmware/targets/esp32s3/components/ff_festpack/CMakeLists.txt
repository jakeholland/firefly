# ff_festpack — ESP-IDF wrapper for firmware/festpack. Same source list
# as firmware/festpack/CMakeLists.txt's ff-festpack target, compiled
# UNCHANGED.
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(FF_FP_DIR ${FF_FW_DIR}/festpack)

idf_component_register(
    SRCS
        "${FF_FP_DIR}/src/fp_pack.c"
        "${FF_FP_DIR}/src/ff_sched.c"
    INCLUDE_DIRS "${FF_FP_DIR}/include"
    REQUIRES ff_platform
    PRIV_REQUIRES ff_jsmn ff_core
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
