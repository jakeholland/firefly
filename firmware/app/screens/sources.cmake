# firmware/app/screens/sources.cmake — the source lists for app/screens'
# libraries: the pure (zero-LVGL) layout/format helpers, and the real
# LVGL screen builders.
#
# Shared, verbatim, by:
#   - the sim build (firmware/app/CMakeLists.txt), which keeps these as
#     FIVE separate CMake targets (ff-layout, ff-radar-layout,
#     ff-lineup-layout, ff-flare-fmt, ff-app-ui) — each pure helper is
#     unit-tested directly (see e.g. ff-radar-layout's comment in
#     firmware/app/CMakeLists.txt for why), which is why each keeps its
#     own list variable here rather than one flat list.
#   - the esp32s3 IDF component
#     (firmware/targets/esp32s3/components/ff_app_ui/CMakeLists.txt),
#     which concatenates all of them into ONE component (the pure
#     helpers' unit tests only run in the sim build; on device they are
#     just object files this component needs — see that file's comment).
#
# Paths are relative to firmware/app/ (the parent directory, NOT this
# one) — same "src/"-style prefix convention firmware/core/sources.cmake
# uses relative to firmware/core/, chosen here because both consumers
# (firmware/app/CMakeLists.txt's add_library() calls, and the
# ff_app_ui component's prepend base) are rooted at firmware/app/, not
# firmware/app/screens/.

# S06b — face-agnostic "is this rect inside the round glass" / "how wide
# fits at this height" primitive every face needs (see ff_layout.h).
set(FF_LAYOUT_SOURCES
    screens/ff_layout.c
)

# S06b — radar face's collision-free layout math (arrow shortening,
# ring-dot placement + clustering).
set(FF_RADAR_LAYOUT_SOURCES
    screens/radar_layout.c
)

# S07b — Now face's format/geometry helpers (docs/specs/S07-now-face.md slice b).
set(FF_LINEUP_LAYOUT_SOURCES
    screens/lineup_layout.c
)

# S10b — flare UI's headline/compass/countdown string formatting
# (docs/specs/S10-flare.md slice b).
set(FF_FLARE_FMT_SOURCES
    screens/flare_fmt.c
)

# S06b/S08c/S08d/S09/S10b/S11b/S26 — the real LVGL screen builders (the
# device analogue of the sim's ff-app-ui library).
set(FF_APP_UI_SOURCES
    screens/scr_radar.c
    screens/scr_nav.c
    screens/scr_widgets.c   # S17 debt cleanup — shared pill factory (flare/power_menu/settings)
    screens/scr_lineup.c    # S07b — Now face
    screens/scr_inbox.c     # S08c
    screens/scr_compose.c   # S08d
    screens/scr_flare.c     # S10b
    screens/scr_settings.c  # S11b — the Settings modal face
    screens/scr_map.c       # S09 — the Map face (Radar's alternate view)
    screens/scr_power_menu.c # S26 slice b — the PWR-button power menu modal
    screens/scr_banner.c    # S26 slice d — the ff_notify message banner overlay
    screens/scr_launcher.c  # S26 slice e — the BOOT-button launcher, carousel retired
)
