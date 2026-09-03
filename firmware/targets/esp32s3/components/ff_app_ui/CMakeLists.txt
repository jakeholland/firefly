# ff_app_ui — ESP-IDF wrapper for firmware/app's LVGL screen code.
#
# The device analogue of the sim build's ff-app-ui library (plus the pure
# helper libs it links: ff-layout, ff-radar-layout, ff-lineup-layout,
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
#
# debt/shared-face-dispatch: ff_face_dispatch.c (app/ff_face_dispatch.c —
# the ONE face-dispatch chain shared by targets/sim/face_dispatch.c and
# targets/esp32s3/main/ff_face.c) is added to THIS component's SRCS, not
# ff_app's — even though the file lives directly under app/, same as the
# four .c files ff_app's own CMakeLists builds. ff_face_dispatch.c calls
# ff_scr_nav_build/ff_scr_compose_build/ff_scr_power_menu_build/
# ff_scr_launcher_build/ff_scr_flare_build_takeover/ff_scr_settings_*,
# every one of which is a screen builder in THIS component's SRCS list
# above — ff_app's REQUIRES (ff_core ff_meshclient ff_festpack ff_platform,
# below) never picked up lvgl or the screen builders in the first place
# (S15a's ff_app component was deliberately screen-free: app_main only
# needed the shell at that point, see that component's own header
# comment). Building ff_face_dispatch.c as part of ff_app would need a
# NEW dependency edge from ff_app onto ff_app_ui/lvgl that does not
# otherwise exist and inverts the layering every other file in ff_app's
# SRCS keeps (data-plumbing, zero LVGL) — ff_app_ui is simply the
# component whose REQUIRES already covers what this file needs, so it
# lives here instead. The sim build makes the identical choice
# (app/CMakeLists.txt's new `ff-face-dispatch` library links ff-app-ui,
# not just ff-app).
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(FF_APP_DIR ${FF_FW_DIR}/app)

idf_component_register(
    SRCS
        # Pure (zero-LVGL) layout/format helpers the screens link against.
        "${FF_APP_DIR}/screens/ff_layout.c"
        "${FF_APP_DIR}/screens/radar_layout.c"
        "${FF_APP_DIR}/screens/lineup_layout.c"
        "${FF_APP_DIR}/screens/flare_fmt.c"
        # Real LVGL screen builders (the sim's ff-app-ui source set).
        "${FF_APP_DIR}/screens/scr_radar.c"
        "${FF_APP_DIR}/screens/scr_nav.c"
        "${FF_APP_DIR}/screens/scr_widgets.c" # S17 debt cleanup — shared pill factory (flare/power_menu/settings)
        "${FF_APP_DIR}/screens/scr_lineup.c"
        "${FF_APP_DIR}/screens/scr_inbox.c"
        "${FF_APP_DIR}/screens/scr_compose.c"
        "${FF_APP_DIR}/screens/scr_flare.c"
        "${FF_APP_DIR}/screens/scr_settings.c"
        "${FF_APP_DIR}/screens/scr_map.c"
        "${FF_APP_DIR}/screens/scr_power_menu.c" # S26 slice b — the PWR-button power menu modal
        "${FF_APP_DIR}/screens/scr_banner.c" # S26 slice d — the ff_notify message banner overlay
        "${FF_APP_DIR}/screens/scr_launcher.c" # S26 slice e — the BOOT-button launcher, carousel retired
        "${FF_APP_DIR}/ff_face_dispatch.c" # debt/shared-face-dispatch — the shared face-dispatch chain (see above)
    INCLUDE_DIRS
        "${FF_APP_DIR}/screens"  # scr_*.h, *_layout.h
        "${FF_APP_DIR}/theme"    # ff_theme.h (header-only)
    # ff_app: ff_app_state.h / ff_intent.h include paths (also publishes
    #         ${FF_APP_DIR} itself, so "ff_face_dispatch.h" resolves here
    #         and for any component — e.g. main — that REQUIRES ff_app)
    #         + the ff_intent_emit symbol the screens emit through.
    # ff_core: ff_app_state.h transitively needs ff_radar.h.
    # lvgl: every scr_*.c includes lvgl.h.
    REQUIRES ff_app ff_core lvgl
)

# libm (sinf/cosf/fmodf in radar/flare geometry) is part of the xtensa
# newlib toolchain — no explicit link needed, unlike the sim's `m` on
# glibc. Warnings-as-errors to match ff_apply_warnings() in the sim.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
