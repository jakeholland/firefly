# firmware/app/sources.cmake — the source lists for the .c files that
# live directly under app/ (not app/screens/).
#
# Shared, verbatim, by:
#   - the sim build (firmware/app/CMakeLists.txt), which keeps most of
#     these as SEPARATE CMake targets (ff-route, ff-intent,
#     ff-wall-window, ff-wiring, ff-shell, ff-demo, ff-face-dispatch) —
#     that split matters for this repo's link-isolation tests, so each
#     library gets its own list variable here rather than one flat list.
#   - the esp32s3 IDF component(s): FF_ROUTE_SOURCES through
#     FF_DEMO_SOURCES go into ff_app
#     (firmware/targets/esp32s3/components/ff_app/CMakeLists.txt), which
#     concatenates them into ONE component (no cross-target reuse need
#     justifies six IDF components here — see that file's comment).
#     FF_FACE_DISPATCH_SOURCES goes into ff_app_ui instead — see that
#     component's CMakeLists.txt for why (it needs the screen builders
#     ff_app_ui already REQUIRES lvgl + ff_app for, which plain ff_app
#     deliberately does not pull in).
#
# LVGL screen code (app/screens/) is a SEPARATE list file —
# app/screens/sources.cmake — not part of this one; see that file's
# header for why. FF_FACE_DISPATCH_SOURCES is the one exception living
# HERE despite calling into LVGL screen builders: the .c file itself is
# physically under app/, not app/screens/ (see ff_face_dispatch.h's top
# comment for why it lives there).
#
# Paths are relative to this directory (firmware/app/).

# S16a — app/ff_route.c: face routing (docs/specs/S16-app-shell.md slice a)
set(FF_ROUTE_SOURCES
    ff_route.c
)

# S16c1 — app/ff_intent.c: the intent seam (docs/specs/S16-app-shell.md slice c1)
#
# S27 amendment: ff_sound_emit.c (docs/specs/S27-sounds.md, "Shell seam")
# joins this SAME list/library rather than getting its own — it is the
# screens-level TAP sound seam, mirroring ff_intent.c's process-global
# emit/bind shape exactly, and every consumer that needs one already
# needs the other (ff-app-ui links ff-intent; the esp32s3 ff_app_ui
# component REQUIRES the ff_app component, which is what compiles
# FF_INTENT_SOURCES on device — see that component's CMakeLists.txt,
# which this PR does not touch). Folding it in here, instead of a
# separate FF_SOUND_EMIT_SOURCES/library, is what lets the device build
# pick it up with ZERO changes under targets/esp32s3/ (this PR's own
# scope boundary: core + shell + settings + sim only).
set(FF_INTENT_SOURCES
    ff_intent.c
    ff_sound_emit.c
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

# debt/shared-face-dispatch — app/ff_face_dispatch.c: the one face-dispatch
# chain shared by targets/sim/face_dispatch.c and targets/esp32s3/main/
# ff_face.c (see ff_face_dispatch.h's top comment). Needs the screen
# builders (app/screens/*.c), so on the sim side it links ff-app-ui, not
# just ff-app — and on the esp32s3 side its SRCS entry lives in the
# ff_app_ui component, not ff_app's (see that component's CMakeLists.txt).
set(FF_FACE_DISPATCH_SOURCES
    ff_face_dispatch.c
)
