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

# S16a (app/ff_route.c — face routing; docs/specs/S16-app-shell.md slice a)
# ---------------------------------------------------------------------------
# A STATIC library rather than a header-only addition to ff-app: ff_route.c
# is real (if small) state-machine logic that needs compiling. It lives in
# app/ and NOT core/ deliberately — core has zero knowledge that screens
# exist, and a face enum there would make core's contents change whenever a
# face is added (see ff_route.h's header comment, and S16 §"App: routing").
#
# Zero LVGL dependency, same "pure C11, unit-testable without a display"
# shape as ff-layout/ff-radar-layout/ff-now-layout/ff-flare-fmt above. It
# links ff-app only for ff_app_state.h's ff_app_face_t, which routing
# extends rather than mirroring in a second enum.
# ---------------------------------------------------------------------------
add_library(ff-route STATIC
    ff_route.c
)
target_link_libraries(ff-route PUBLIC ff-app) # ff_app_state.h (-> ff-core's ff_radar.h)
target_compile_features(ff-route PUBLIC c_std_11)
ff_apply_warnings(ff-route)

if(FF_BUILD_TESTS)
    add_executable(test_route
        tests/test_route.c
    )
    target_link_libraries(test_route PRIVATE ff-route unity)
    ff_apply_warnings(test_route)

    add_test(NAME test_route COMMAND test_route)
endif()

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
# docs/specs/S08-signals-t9.md slice b). One of the TWO libraries that
# link core + meshclient together (the other is ff-shell, S16 slice b1 —
# see ff_wiring.h's header comment for why that invariant changed);
# everything else in this repo keeps core/meshclient/app strictly unaware
# of each other. Not part of ff-app-ui: this is data-plumbing, no LVGL.
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

# S16b1 (app/ff_shell.c — the running application; docs/specs/S16-app-shell.md
# slice b1)
# ---------------------------------------------------------------------------
# The object that owns a live mc_client_t, the core domain state and the
# ff_app_state_t projection together across time. Target-agnostic: both
# targets/sim and targets/esp32s3 drive this same library.
#
# The SECOND library in this repo that links core + meshclient + app
# together, alongside ff-wiring. That is the design, not a slip: S16's
# "Layering — a correction to a prior claim" retires ff_wiring's old "the
# ONE file" invariant, since ff_shell_cfg_t holds an mc_transport_t by
# construction. ff_wiring.h's header comment is updated to match in this
# same slice.
#
# Links ff-festpack for fp_pack_t (the Now-face projection and the
# wall-clock UTC-offset resolution both need a pack) — note the pack
# STORAGE is the target's, handed in through ff_shell_cfg_t.pack, so the
# ~48 KB never lands inside ff_shell_t. See ff_shell.h.
#
# No LVGL: this is data-plumbing and projection, exactly like ff-wiring.
# The screens read ff_shell_view()'s output; they are not linked here.
# ---------------------------------------------------------------------------
add_library(ff-shell STATIC
    ff_shell.c
)
target_include_directories(ff-shell PUBLIC include)
target_link_libraries(ff-shell PUBLIC ff-app ff-route ff-wiring ff-core ff-meshclient ff-festpack)
target_compile_features(ff-shell PUBLIC c_std_11)
ff_apply_warnings(ff-shell)

if(FF_BUILD_TESTS)
    add_executable(test_shell
        tests/test_shell.c
    )
    # ff-meshclient-proto: S16b2's transport-driven AC6 test hand-encodes
    # synthetic Meshtastic frames (the technique the retired
    # targets/sim/tests/test_live.c used) — it needs the proto message
    # structs and nanopb's encoder directly, not just ff-meshclient's
    # public API (ff-meshclient links proto PRIVATE, so neither the
    # includes nor the archive propagate). ff-nanopb is deliberately NOT
    # listed (proto links it PUBLIC). Apple's ld still warns "ignoring
    # duplicate libraries" for the proto archive — it appears once here
    # and once in ff-meshclient's PRIVATE link closure — and that one is
    # structurally unavoidable without changing meshclient's visibility,
    # which is not this slice's call (S08 PR #25's NIT ruling: harmless,
    # confirmed, documented).
    target_link_libraries(test_shell PRIVATE ff-shell ff-meshclient-proto unity)
    ff_apply_warnings(test_shell)

    add_test(NAME test_shell COMMAND test_shell)
endif()
