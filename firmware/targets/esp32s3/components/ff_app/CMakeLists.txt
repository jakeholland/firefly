# ff_app — ESP-IDF wrapper for the target-agnostic slice of firmware/app:
# ff_route.c, ff_intent.c, ff_wall_window.c, ff_wiring.c, ff_shell.c,
# ff_demo.c/ff_demoapply.c — the same files firmware/app/CMakeLists.txt
# builds into ff-route/ff-intent/ff-wall-window/ff-wiring/ff-shell/ff-demo
# for the sim, now both read from firmware/app/sources.cmake instead of
# hand-copying the list (see that file's header). LVGL screen code
# (app/screens/, app/theme/) is deliberately NOT part of this component:
# ff_shell.c and everything it links have zero LVGL dependency
# (data-plumbing + projection only — see app/include/ff_shell.h's top
# comment), and S15 slice a's app_main.c only needs the shell, not a
# renderer. Screens live in the ff_app_ui component (slice b).
#
# Compiled as ONE component rather than six (unlike the sim build's six
# separate CMake targets) — S15a's brief allows "one component with the
# right srcs/includes" and there is no cross-target reuse need here that
# would justify the extra indirection ESP-IDF's component boundaries add.
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(FF_APP_DIR ${FF_FW_DIR}/app)

include(${FF_APP_DIR}/sources.cmake)

set(FF_APP_COMPONENT_SOURCES
    ${FF_ROUTE_SOURCES}
    ${FF_INTENT_SOURCES}
    ${FF_WALL_WINDOW_SOURCES}
    ${FF_WIRING_SOURCES}
    ${FF_SHELL_SOURCES}
    ${FF_DEMO_SOURCES}
)
list(TRANSFORM FF_APP_COMPONENT_SOURCES PREPEND "${FF_APP_DIR}/")

idf_component_register(
    SRCS ${FF_APP_COMPONENT_SOURCES}
    INCLUDE_DIRS
        "${FF_APP_DIR}"         # ff_wiring.h lives here, not under include/
        "${FF_APP_DIR}/include" # ff_app_state.h, ff_route.h, ff_intent.h, ff_shell.h
    REQUIRES ff_core ff_meshclient ff_festpack ff_platform
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
