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

# ff_layout.c is a SEPARATE, face-agnostic library from ff-app-ui, and
# deliberately has zero LVGL dependency (plain C11 + <math.h>): PR #25 UX
# review (S08 slice d, blocking finding) — Compose shipped chrome
# positioned against the puck's square bounding box instead of the round
# glass. This is the shared "is this rect inside the circle" / "how wide
# fits at this height" primitive every face needs so that bug class can't
# repeat (see ff_layout.h's header comment for why this is a DIFFERENT
# kind of sharing from issue #24's deferred widget-library extraction,
# and doesn't conflict with that deferral).
add_library(ff-layout STATIC
    screens/ff_layout.c
)
target_include_directories(ff-layout PUBLIC screens)
if(UNIX)
    target_link_libraries(ff-layout PRIVATE m)
endif()
ff_apply_warnings(ff-layout)

if(FF_BUILD_TESTS)
    add_executable(test_ff_layout
        screens/tests/test_ff_layout.c
    )
    target_link_libraries(test_ff_layout PRIVATE ff-layout unity)
    ff_apply_warnings(test_ff_layout)

    add_test(NAME test_ff_layout COMMAND test_ff_layout)
endif()

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
    screens/scr_signals.c # S08c
    screens/scr_compose.c # S08d
    screens/scr_flare.c # S10b
)
target_include_directories(ff-app-ui PUBLIC theme screens)
target_link_libraries(ff-app-ui PUBLIC ff-app ff-radar-layout ff-layout ff-flare-fmt lvgl::lvgl)
# scr_radar.c's arrow/dot trigonometry (sinf/cosf) needs libm — same
# rationale as ff-core's CMakeLists.txt note on why this is missed
# locally (AppleClang resolves it via libSystem) but required on
# glibc/Linux CI.
if(UNIX)
    target_link_libraries(ff-app-ui PRIVATE m)
endif()
ff_apply_warnings(ff-app-ui)

# S08b (app/ff_wiring.c — crew-filtered feed wiring;
# docs/specs/S08-signals-t9.md slice b). Deliberately the ONE library that
# links core + meshclient together (docs/ARCHITECTURE.md) — everything
# else in this repo keeps core/meshclient/app strictly unaware of each
# other. Not part of ff-app-ui: this is data-plumbing, no LVGL.
add_library(ff-wiring STATIC
    ff_wiring.c
)
target_include_directories(ff-wiring PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ff-wiring PUBLIC ff-core ff-meshclient)
ff_apply_warnings(ff-wiring)

if(FF_BUILD_TESTS)
    add_executable(test_wiring
        tests/test_wiring.c
    )
    target_link_libraries(test_wiring PRIVATE ff-wiring ff-core ff-meshclient unity)
    ff_apply_warnings(test_wiring)

    add_test(NAME test_wiring COMMAND test_wiring)
endif()

# S10b (continued) — headless LVGL INTERACTION tests for the flare UI's
# button bindings (PR #20 independent code review, HIGH finding: goldens
# never fire a click, so nothing proved GO calls ff_flare_go and not
# ff_flare_dismiss_takeover, or vice versa — exactly the class of
# regression Amendment 3 exists to prevent). Declared here, after
# ff-app-ui, since it needs the real screen builders (scr_flare.c/
# scr_radar.c) linked in — see test_scr_flare.c's header comment for the
# reusable "build against a real ff_flare_t, locate the button, synthesize
# a click, assert on the CORE struct" pattern future faces' input tests
# should follow.
if(FF_BUILD_TESTS)
    add_executable(test_scr_flare
        screens/tests/test_scr_flare.c
    )
    target_link_libraries(test_scr_flare PRIVATE ff-app-ui unity)
    ff_apply_warnings(test_scr_flare)

    add_test(NAME test_scr_flare COMMAND test_scr_flare)
endif()
