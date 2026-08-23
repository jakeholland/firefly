# app — LVGL screens: render core state, forward input. No domain logic.
#
# ff-app itself stays INTERFACE (header-only: ff_app_state.h, no domain
# logic per CLAUDE.md). Real screens are a separate library, ff-app-ui
# (added under the # S06b marker below) — see that section for why.

add_library(ff-app INTERFACE)

# S13b
# Publishes ff_app_state.h — the plain view-state struct screens will
# render (docs/ARCHITECTURE.md principle 3: "the UI is a projection").
# Header-only: no .c sources belong here until a real screen lands (no
# domain logic allowed in app/, per CLAUDE.md).
target_include_directories(ff-app INTERFACE include)

# S06 — ff_app_state.h's `radar` field is the real core/include/ff_radar.h
# ff_radar_view_t (DRIFT GUARD resolution, see that header's comment), so
# anything consuming ff_app_state.h now needs ff-core's include path too.
# INTERFACE, not PRIVATE: ff-app itself has no sources to compile, only
# this header to propagate.
target_link_libraries(ff-app INTERFACE ff-core)

# S06b (app/theme, app/screens — shell/nav + radar-face rendering;
# docs/specs/S06-radar-face.md slices b/c/d)
# ---------------------------------------------------------------------------
# The first real screen code in this repo: a genuine STATIC library (not
# header-only like ff-app above), since scr_nav.c/scr_radar.c are .c
# sources that need compiling and linking, not just a header to publish.
# ---------------------------------------------------------------------------

# radar_layout.c is a SEPARATE library from ff-app-ui, and deliberately
# has zero LVGL dependency (plain C11 + <math.h>): it's the radar face's
# collision-free layout math (arrow shortening, ring-dot placement +
# clustering), pulled out specifically so it's unit-testable with real
# geometric assertions (test_radar_layout, below) instead of only via a
# golden PNG a human has to notice overlapping pixels in — PR #16 UX
# review round 3 / code review round 2.
add_library(ff-radar-layout STATIC
    screens/radar_layout.c
)
target_include_directories(ff-radar-layout PUBLIC screens)
target_link_libraries(ff-radar-layout PUBLIC ff-app)
if(UNIX)
    target_link_libraries(ff-radar-layout PRIVATE m)
endif()
ff_apply_warnings(ff-radar-layout)

if(FF_BUILD_TESTS)
    add_executable(test_radar_layout
        screens/tests/test_radar_layout.c
    )
    target_link_libraries(test_radar_layout PRIVATE ff-radar-layout unity)
    ff_apply_warnings(test_radar_layout)

    add_test(NAME test_radar_layout COMMAND test_radar_layout)
endif()

add_library(ff-app-ui STATIC
    screens/scr_radar.c
    screens/scr_nav.c
)
target_include_directories(ff-app-ui PUBLIC theme screens)
target_link_libraries(ff-app-ui PUBLIC ff-app ff-radar-layout lvgl::lvgl)
# scr_radar.c's arrow/dot trigonometry (sinf/cosf) needs libm — same
# rationale as ff-core's CMakeLists.txt note on why this is missed
# locally (AppleClang resolves it via libSystem) but required on
# glibc/Linux CI.
if(UNIX)
    target_link_libraries(ff-app-ui PRIVATE m)
endif()
ff_apply_warnings(ff-app-ui)

# S07b (app/screens/now_layout.c, scr_now.c — Now face render;
# docs/specs/S07-now-face.md slice b)
# ---------------------------------------------------------------------------
# ff-now-layout: same "pure C11, zero LVGL, unit-tested directly" split as
# ff-radar-layout above (format/geometry helpers only — see
# app/screens/now_layout.h's top comment for what it reuses from
# radar_layout.h and what it doesn't). Even lighter-weight than
# ff-radar-layout: no ff_radar.h/FF_CREW_MAX dependency at all, so this
# links only ff_apply_warnings + libm, not ff-app.
# ---------------------------------------------------------------------------
add_library(ff-now-layout STATIC
    screens/now_layout.c
)
target_include_directories(ff-now-layout PUBLIC screens)
target_compile_features(ff-now-layout PUBLIC c_std_11)
if(UNIX)
    target_link_libraries(ff-now-layout PRIVATE m)
endif()
ff_apply_warnings(ff-now-layout)

if(FF_BUILD_TESTS)
    add_executable(test_now_layout
        screens/tests/test_now_layout.c
    )
    target_link_libraries(test_now_layout PRIVATE ff-now-layout unity)
    ff_apply_warnings(test_now_layout)

    add_test(NAME test_now_layout COMMAND test_now_layout)
endif()

# scr_now.c joins ff-app-ui (declared above) rather than a new library:
# same shape as scr_radar.c/scr_nav.c — real LVGL screen code, not a
# separately-tested pure module. Appended via target_sources (not folded
# into the add_library() call above) so this stays a purely additive diff
# — two other face agents (Signals/Flare UI) are touching this same file
# in parallel for their own tiles.
target_sources(ff-app-ui PRIVATE
    screens/scr_now.c
)
target_link_libraries(ff-app-ui PUBLIC ff-now-layout)
