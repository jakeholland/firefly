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

# CONFIG_FF_DEV_TRUST_CHANNEL (Kconfig.projbuild, default n) gates the
# device-side dev/field auto-pair stopgap in ff_shell.c/.h — see
# ff_shell.h's ff_shell_dev_trust_all doc comment.
#
# ff_shell.c currently DOES see this macro without any help from here:
# its own `#include <stdio.h>` resolves (esp-idf's newlib platform
# include path wins over the toolchain's own copy) into a chain that
# reaches components/newlib/platform_include/sys/lock.h, which
# unconditionally `#include "sdkconfig.h"` — and once any header in a
# translation unit pulls that in, every CONFIG_* macro is visible for
# the rest of the file, ff_shell.c's own `#if defined(CONFIG_FF_
# DEV_TRUST_CHANNEL)` guards included. Verified with `-H` against this
# component's own compile_commands.json entry (see this option's PR body
# for the full include trace).
#
# That is an ACCIDENT of ff_shell.c happening to include <stdio.h> for
# an unrelated reason, not a documented guarantee — ESP-IDF does NOT
# force-include sdkconfig.h for every component (confirmed against
# tools/cmake/kconfig.cmake: SDKCONFIG_HEADER is recorded as a build
# property and handed to whichever files #include it themselves, never
# injected as a global -include/-imacros flag). A future edit that drops
# or reorders that #include would silently revert this option to
# "compiled out" with no build error — exactly the silent-regression
# failure mode this repo's own FF_TARGET_SIM discipline exists to avoid
# (firmware/CMakeLists.txt's own comment on that flag, and targets/sim/
# main.c's #error guard for the mirror case). So the definition is
# ALSO supplied explicitly and unconditionally here, PRIVATE to this
# component only (ff_shell.c is the sole file in it that reads the
# macro), making the intent a property of the build graph instead of an
# incidental header side effect:
if(CONFIG_FF_DEV_TRUST_CHANNEL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_FF_DEV_TRUST_CHANNEL=1)
endif()
