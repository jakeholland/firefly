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
    ${FF_DEBUG_CONSOLE_SOURCES} # bench/debug console dispatch (CONFIG_FF_DEBUG_CONSOLE) — ff_debug_console.c
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

# A02 slice D: CONFIG_FF_DEV_TRUST_CHANNEL is GONE. Its replacement,
# CONFIG_FF_CREW_AUTO_ON_CHANNEL (Kconfig.projbuild, default y), is read
# by main/app_main.c — which is in the `main` component, not this one —
# and passed to the shell as a runtime bool through
# ff_shell_set_auto_crew(). So there is deliberately nothing to define
# here: ff_shell.c has no `#if` on it at all, which is the point. A
# shipped behaviour is a plain field; only a compiled-out affordance
# needs a macro, and the one that is left (FF_TARGET_SIM) is not a
# device concern.

# CONFIG_FF_DEBUG_CONSOLE (Kconfig.projbuild, default n) gates the
# bench/debug console's debug-only shell API (ff_shell.c/.h's
# `#if defined(FF_TARGET_SIM) || defined(CONFIG_FF_DEBUG_CONSOLE)`
# section) and ff_debug_console.c's whole body. Same accidental-but-
# relied-upon visibility question the deleted CONFIG_FF_DEV_TRUST_CHANNEL
# raised (ff_shell.c's own <stdio.h> include; ff_debug_console.c includes it
# too, for the same snprintf reason) — supplied explicitly here for the
# same "property of the build graph, not an incidental header side
# effect" reason, PRIVATE to this component (both files that read the
# macro live in it).
if(CONFIG_FF_DEBUG_CONSOLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_FF_DEBUG_CONSOLE=1)
endif()

# DIAGNOSTICS — app/include/ff_build_info.h's FF_BUILD_GIT_SHA/FF_BUILD_DATE
# macros. Same git-describe wiring firmware/app/CMakeLists.txt applies to
# the sim's ff-shell target, PRIVATE to this component (ff_shell.c is the
# sole consumer, same reasoning as the two Kconfig defines just above). A
# missing/broken git leaves both macros at ff_build_info.h's own honest
# "unknown"/__DATE__ fallback, silently.
find_package(Git QUIET)
set(FF_BUILD_GIT_SHA_VALUE "")
set(FF_BUILD_DATE_VALUE "")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${FF_FW_DIR}
        OUTPUT_VARIABLE FF_BUILD_GIT_SHA_VALUE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE FF_BUILD_GIT_SHA_RC
    )
    if(NOT FF_BUILD_GIT_SHA_RC EQUAL 0)
        set(FF_BUILD_GIT_SHA_VALUE "")
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} show -s --format=%cd --date=short HEAD
            WORKING_DIRECTORY ${FF_FW_DIR}
            OUTPUT_VARIABLE FF_BUILD_DATE_VALUE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
endif()
if(NOT FF_BUILD_GIT_SHA_VALUE STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FF_BUILD_GIT_SHA="${FF_BUILD_GIT_SHA_VALUE}")
endif()
if(NOT FF_BUILD_DATE_VALUE STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FF_BUILD_DATE="${FF_BUILD_DATE_VALUE}")
endif()
