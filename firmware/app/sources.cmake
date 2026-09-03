# firmware/app/sources.cmake — the source lists for app/'s
# target-agnostic (zero-LVGL) libraries.
#
# Shared, verbatim, by:
#   - the sim build (firmware/app/CMakeLists.txt), which keeps these as
#     SEPARATE CMake targets (ff-route, ff-intent, ff-wall-window,
#     ff-wiring, ff-shell, ff-demo) — that split matters for this repo's
#     link-isolation tests, so each library gets its own list variable
#     here rather than one flat list.
#   - the esp32s3 IDF component
#     (firmware/targets/esp32s3/components/ff_app/CMakeLists.txt), which
#     concatenates all of them into ONE component (no cross-target reuse
#     need justifies six IDF components here — see that file's comment).
#
# LVGL screen code (app/screens/) is a SEPARATE list file —
# app/screens/sources.cmake — not part of this one; see that file's
# header for why.
#
# Paths are relative to this directory (firmware/app/).

# S16a — app/ff_route.c: face routing (docs/specs/S16-app-shell.md slice a)
set(FF_ROUTE_SOURCES
    ff_route.c
)

# S16c1 — app/ff_intent.c: the intent seam (docs/specs/S16-app-shell.md slice c1)
set(FF_INTENT_SOURCES
    ff_intent.c
)

# S18c — app/ff_wall_window.c: pack -> wall-clock plausibility window
# (docs/specs/S18-wall-clock-trust.md slice c)
set(FF_WALL_WINDOW_SOURCES
    ff_wall_window.c
)

# S08b — app/ff_wiring.c: crew-filtered feed wiring (docs/specs/S08-signals-t9.md slice b)
set(FF_WIRING_SOURCES
    ff_wiring.c
)

# S16b1 — app/ff_shell.c: the running application (docs/specs/S16-app-shell.md slice b1)
set(FF_SHELL_SOURCES
    ff_shell.c
)

# S20 / S23c — app/ff_demo.c + ff_demoapply.c: demo-mode seeding + live
# apply glue (docs/specs/S20-demo-mode.md; S23c addendum)
set(FF_DEMO_SOURCES
    ff_demo.c
    ff_demoapply.c
)
