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

# S10b (app/screens — flare UI: takeover, sender overlay, lock chip;
# docs/specs/S10-flare.md slice b)
# ---------------------------------------------------------------------------
# flare_fmt.c is a SEPARATE library from ff-app-ui, same rationale as
# ff-radar-layout above: plain C11, zero LVGL dependency, so its output
# (headline/compass/countdown strings) is unit-testable with direct string
# assertions (test_flare_fmt, below) rather than only inferable from a
# golden PNG diff.
add_library(ff-flare-fmt STATIC
    screens/flare_fmt.c
)
target_include_directories(ff-flare-fmt PUBLIC screens)
if(UNIX)
    target_link_libraries(ff-flare-fmt PRIVATE m) # fmodf
endif()
ff_apply_warnings(ff-flare-fmt)

if(FF_BUILD_TESTS)
    add_executable(test_flare_fmt
        screens/tests/test_flare_fmt.c
    )
    target_link_libraries(test_flare_fmt PRIVATE ff-flare-fmt unity)
    ff_apply_warnings(test_flare_fmt)

    add_test(NAME test_flare_fmt COMMAND test_flare_fmt)
endif()
# ---------------------------------------------------------------------------

add_library(ff-app-ui STATIC
    screens/scr_radar.c
    screens/scr_nav.c
    screens/scr_flare.c # S10b
)
target_include_directories(ff-app-ui PUBLIC theme screens)
target_link_libraries(ff-app-ui PUBLIC ff-app ff-radar-layout ff-flare-fmt lvgl::lvgl)
# scr_radar.c's arrow/dot trigonometry (sinf/cosf) needs libm — same
# rationale as ff-core's CMakeLists.txt note on why this is missed
# locally (AppleClang resolves it via libSystem) but required on
# glibc/Linux CI.
if(UNIX)
    target_link_libraries(ff-app-ui PRIVATE m)
endif()
ff_apply_warnings(ff-app-ui)
