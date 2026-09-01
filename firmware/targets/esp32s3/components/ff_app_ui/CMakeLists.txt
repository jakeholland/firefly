# ff_app_ui — ESP-IDF wrapper for firmware/app's LVGL screen code.
#
# The device analogue of the sim build's ff-app-ui library (plus the pure
# helper libs it links: ff-layout, ff-radar-layout, ff-now-layout,
# ff-flare-fmt) — the SAME .c files the sim compiles, so a face renders
# byte-for-byte the same projection on glass that it does in the sim
# window (S15b: "LVGL version MUST match the sim's (v9) so the same
# core/screen render code produces the same pixels").
#
# S15a's ff_app component deliberately left these out (app_main only
# needed the shell, no renderer — see that component's header). Slice b
# adds them: the screens link LVGL (from the component registry via
# ff_display's manifest, propagated here as a REQUIRES on lvgl) and the
# intent seam / app-state headers (via ff_app + ff_core).
#
# Compiled as ONE component (not the six separate CMake targets the sim
# splits into for unit-testability) — the pure helpers' unit tests run in
# the sim build; on device they are just object files this component
# needs. Same "one component" rationale as ff_app/ff_core's CMakeLists.
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(FF_APP_DIR ${FF_FW_DIR}/app)

idf_component_register(
    SRCS
        # Pure (zero-LVGL) layout/format helpers the screens link against.
        "${FF_APP_DIR}/screens/ff_layout.c"
        "${FF_APP_DIR}/screens/radar_layout.c"
        "${FF_APP_DIR}/screens/now_layout.c"
        "${FF_APP_DIR}/screens/flare_fmt.c"
        # Real LVGL screen builders (the sim's ff-app-ui source set).
        "${FF_APP_DIR}/screens/scr_radar.c"
        "${FF_APP_DIR}/screens/scr_nav.c"
        "${FF_APP_DIR}/screens/scr_now.c"
        "${FF_APP_DIR}/screens/scr_signals.c"
        "${FF_APP_DIR}/screens/scr_compose.c"
        "${FF_APP_DIR}/screens/scr_flare.c"
        "${FF_APP_DIR}/screens/scr_settings.c"
        "${FF_APP_DIR}/screens/scr_map.c"
        "${FF_APP_DIR}/screens/scr_power_menu.c" # S26 slice b — the PWR-button power menu modal
    INCLUDE_DIRS
        "${FF_APP_DIR}/screens"  # scr_*.h, *_layout.h
        "${FF_APP_DIR}/theme"    # ff_theme.h (header-only)
    # ff_app: ff_app_state.h / ff_intent.h include paths + the
    #         ff_intent_emit symbol the screens emit through.
    # ff_core: ff_app_state.h transitively needs ff_radar.h.
    # lvgl: every scr_*.c includes lvgl.h.
    REQUIRES ff_app ff_core lvgl
)

# libm (sinf/cosf/fmodf in radar/flare geometry) is part of the xtensa
# newlib toolchain — no explicit link needed, unlike the sim's `m` on
# glibc. Warnings-as-errors to match ff_apply_warnings() in the sim.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
