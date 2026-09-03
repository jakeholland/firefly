# ff_core — ESP-IDF wrapper for firmware/core. Same source list as
# firmware/core/CMakeLists.txt's ff-core target, compiled UNCHANGED (no
# core/ file needed a single line of edits for this port — see the S15a
# PR body's portability findings) — both now read
# firmware/core/sources.cmake's FF_CORE_SOURCES instead of hand-copying
# the list (see that file's header for why). -Wall -Wextra -Werror
# applied to match ff_apply_warnings()'s treatment of ff-core in the sim
# build.
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(FF_CORE_DIR ${FF_FW_DIR}/core)

include(${FF_CORE_DIR}/sources.cmake)

# idf_component_register()'s SRCS wants paths relative to the component
# dir or absolute — FF_CORE_SOURCES' entries are relative to
# firmware/core/, so prepend the absolute path to that directory.
set(FF_CORE_SOURCES_ABS ${FF_CORE_SOURCES})
list(TRANSFORM FF_CORE_SOURCES_ABS PREPEND "${FF_CORE_DIR}/")

idf_component_register(
    SRCS ${FF_CORE_SOURCES_ABS}
    INCLUDE_DIRS "${FF_CORE_DIR}/include"
    REQUIRES ff_platform
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
