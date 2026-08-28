# ff_core — ESP-IDF wrapper for firmware/core. Same source list as
# firmware/core/CMakeLists.txt's ff-core target, compiled UNCHANGED (no
# core/ file needed a single line of edits for this port — see the S15a
# PR body's portability findings). -Wall -Wextra -Werror applied to match
# ff_apply_warnings()'s treatment of ff-core in the sim build.
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(FF_CORE_DIR ${FF_FW_DIR}/core)

idf_component_register(
    SRCS
        "${FF_CORE_DIR}/src/ff_version.c"
        "${FF_CORE_DIR}/src/ff_settings.c"
        "${FF_CORE_DIR}/src/ff_touchcal.c"
        "${FF_CORE_DIR}/src/ff_geo.c"
        "${FF_CORE_DIR}/src/ff_proto.c"
        "${FF_CORE_DIR}/src/ff_crew.c"
        "${FF_CORE_DIR}/src/ff_radar.c"
        "${FF_CORE_DIR}/src/ff_t9.c"
        "${FF_CORE_DIR}/src/ff_flare.c"
        "${FF_CORE_DIR}/src/ff_feed.c"
        "${FF_CORE_DIR}/src/ff_heard.c"
        "${FF_CORE_DIR}/src/ff_wall.c"
        "${FF_CORE_DIR}/src/ff_map.c"
    INCLUDE_DIRS "${FF_CORE_DIR}/include"
    REQUIRES ff_platform
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
