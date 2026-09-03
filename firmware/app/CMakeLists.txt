# app — LVGL screens: render core state, forward input. No domain logic.
#
# ff-app itself stays INTERFACE (header-only: ff_app_state.h, no domain
# logic per CLAUDE.md). Real screens are a separate library, ff-app-ui
# (added under the # S06b marker below) — see that section for why.
#
# Source lists live in sources.cmake (this directory's target-agnostic
# libraries) and screens/sources.cmake (the LVGL screen libraries),
# shared verbatim with the esp32s3 IDF components ff_app and ff_app_ui
# (firmware/targets/esp32s3/components/) — see those files' comments and
# each sources.cmake's own header for why.
include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/screens/sources.cmake)

# Sim-only completeness guards (see ff_check_sources_complete()'s comment
# in the top-level firmware/CMakeLists.txt) — every app/*.c and
# app/screens/*.c must be named in one of these lists, or this fails
# configure naming the missing file.
ff_check_sources_complete(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake
    FF_ROUTE_SOURCES FF_INTENT_SOURCES FF_WALL_WINDOW_SOURCES
    FF_WIRING_SOURCES FF_SHELL_SOURCES FF_DEMO_SOURCES)
ff_check_sources_complete(${CMAKE_CURRENT_SOURCE_DIR}/screens ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/screens/sources.cmake
    FF_LAYOUT_SOURCES FF_RADAR_LAYOUT_SOURCES FF_LINEUP_LAYOUT_SOURCES
    FF_FLARE_FMT_SOURCES FF_APP_UI_SOURCES)

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
# shape as ff-layout/ff-radar-layout/ff-lineup-layout/ff-flare-fmt above. It
# links ff-app only for ff_app_state.h's ff_app_face_t, which routing
# extends rather than mirroring in a second enum.
# ---------------------------------------------------------------------------
add_library(ff-route STATIC ${FF_ROUTE_SOURCES})
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

# S16c1 (app/ff_intent.c + app/include/ff_intent.h — the intent seam;
# docs/specs/S16-app-shell.md slice c1)
# ---------------------------------------------------------------------------
# The contract screens emit through (`ff_intent_t`) and its one moving
# part (the process-global emit sink). Pure C11, zero LVGL, zero
# meshclient — deliberately: screens link this to BUILD intents, and this
# library pulling mc_client.h in would hand every screen the
# core+meshclient+app inclusion only ff_wiring.c/ff_shell.c are allowed
# (see ff_intent.h's header comment; ff_wiring_canned_reply_t moved into
# ff_intent.h for the same reason). The RECEIVING end, ff_shell_intent,
# lives in ff-shell below and does not link this — the target binds the
# two together at startup via ff_intent_emit_bind(ff_shell_intent_sink,..).
# ---------------------------------------------------------------------------
add_library(ff-intent STATIC ${FF_INTENT_SOURCES})
target_link_libraries(ff-intent PUBLIC ff-app) # app/include/ff_intent.h's include path
target_compile_features(ff-intent PUBLIC c_std_11)
ff_apply_warnings(ff-intent)

# S18c (app/ff_wall_window.c — pack -> wall-clock plausibility window;
# docs/specs/S18-wall-clock-trust.md slice c)
# ---------------------------------------------------------------------------
# The festpack->window derivation seam. Pure C11, zero LVGL. Lives in app/
# (not core/) because it needs fp_pack_t, and core deliberately does not
# depend on festpack (docs/ARCHITECTURE.md's one-way edge; ff_wall.h's
# placement note). It links ff-core (for ff_wall_unix_from_doy, the shared
# civil-date math the window is computed against) and ff-festpack (for
# fp_pack_t). The shell calls it on pack load and hands the result to
# ff_wall_set_window; core takes the window as plain values and never sees
# the pack. Unit-testable with a plain fp_pack_t and no shell — see
# tests/test_wall_window.c.
# ---------------------------------------------------------------------------
add_library(ff-wall-window STATIC ${FF_WALL_WINDOW_SOURCES})
target_include_directories(ff-wall-window PUBLIC include)
target_link_libraries(ff-wall-window PUBLIC ff-core ff-festpack)
target_compile_features(ff-wall-window PUBLIC c_std_11)
ff_apply_warnings(ff-wall-window)

if(FF_BUILD_TESTS)
    add_executable(test_wall_window
        tests/test_wall_window.c
    )
    target_link_libraries(test_wall_window PRIVATE ff-wall-window unity)
    ff_apply_warnings(test_wall_window)

    add_test(NAME test_wall_window COMMAND test_wall_window)
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
add_library(ff-layout STATIC ${FF_LAYOUT_SOURCES})
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
add_library(ff-radar-layout STATIC ${FF_RADAR_LAYOUT_SOURCES})
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
add_library(ff-flare-fmt STATIC ${FF_FLARE_FMT_SOURCES})
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

# S17 slice a (app/theme/ff_theme.h — the crew palette; docs/specs/S17-usability-hardening.md)
# ---------------------------------------------------------------------------
# ff_theme.h is header-only (no .c file — see its own top comment), so
# there is no ff-theme library to link; this test #includes the header
# directly and needs only its own include path plus lvgl (the header
# pulls in lvgl.h for its font/color macros elsewhere in the file, even
# though the palette function under test here touches none of them —
# same reasoning ff-app-ui's own lvgl::lvgl link comment gives). No LVGL
# runtime state is touched (no lv_init(), no widgets), matching this
# repo's "pure C11, unit-tested directly" convention for anything
# testable without a display.
if(FF_BUILD_TESTS)
    add_executable(test_theme
        theme/tests/test_theme.c
    )
    target_include_directories(test_theme PRIVATE theme)
    target_link_libraries(test_theme PRIVATE lvgl::lvgl unity)
    ff_apply_warnings(test_theme)

    add_test(NAME test_theme COMMAND test_theme)
endif()
# ---------------------------------------------------------------------------

add_library(ff-app-ui STATIC ${FF_APP_UI_SOURCES})
target_include_directories(ff-app-ui PUBLIC theme screens)
# ff-intent (S16c1): scr_nav/scr_inbox/scr_compose emit navigation
# intents through the seam — see ff_intent.h for why this dependency is
# safe for screens (no meshclient, no shell, no core state).
target_link_libraries(ff-app-ui PUBLIC ff-app ff-radar-layout ff-layout ff-flare-fmt ff-intent lvgl::lvgl)
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
add_library(ff-wiring STATIC ${FF_WIRING_SOURCES})
target_include_directories(ff-wiring PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# ff-app: ff_wiring.h now includes app/include/ff_intent.h — the
# ff_wiring_canned_reply_t definition moved there in S16c1 so screens can
# build intents without transitively including mc_client.h. ff-app is the
# INTERFACE library that publishes that include path.
target_link_libraries(ff-wiring PUBLIC ff-core ff-meshclient ff-app)
ff_apply_warnings(ff-wiring)

if(FF_BUILD_TESTS)
    add_executable(test_wiring
        tests/test_wiring.c
    )
    target_link_libraries(test_wiring PRIVATE ff-wiring ff-core ff-meshclient unity)
    ff_apply_warnings(test_wiring)

    add_test(NAME test_wiring COMMAND test_wiring)
endif()

# S07b (app/screens/lineup_layout.c, scr_lineup.c — Now face render;
# docs/specs/S07-now-face.md slice b)
# ---------------------------------------------------------------------------
# ff-lineup-layout: same "pure C11, zero LVGL, unit-tested directly" split as
# ff-radar-layout above (format/geometry helpers only — see
# app/screens/lineup_layout.h's top comment for what it reuses from
# radar_layout.h and what it doesn't). Even lighter-weight than
# ff-radar-layout: no ff_radar.h/FF_CREW_MAX dependency at all, so this
# links only ff_apply_warnings + libm, not ff-app.
# ---------------------------------------------------------------------------
add_library(ff-lineup-layout STATIC ${FF_LINEUP_LAYOUT_SOURCES})
target_include_directories(ff-lineup-layout PUBLIC screens)
target_compile_features(ff-lineup-layout PUBLIC c_std_11)
if(UNIX)
    target_link_libraries(ff-lineup-layout PRIVATE m)
endif()
ff_apply_warnings(ff-lineup-layout)

if(FF_BUILD_TESTS)
    add_executable(test_lineup_layout
        screens/tests/test_lineup_layout.c
    )
    target_link_libraries(test_lineup_layout PRIVATE ff-lineup-layout unity)
    ff_apply_warnings(test_lineup_layout)

    add_test(NAME test_lineup_layout COMMAND test_lineup_layout)
endif()

# scr_lineup.c (S07b — Now face) is part of ff-app-ui's FF_APP_UI_SOURCES
# (screens/sources.cmake) alongside scr_radar.c/scr_nav.c — real LVGL
# screen code, not a separately-tested pure module.
target_link_libraries(ff-app-ui PUBLIC ff-lineup-layout)

# S10b (continued) — headless LVGL geometry tests for the flare UI (the
# lock-disclosure chip's round-glass clamp, issue #27). Declared here,
# after ff-app-ui, since it needs the real screen builder (scr_flare.c)
# linked in. The button-binding proofs this test used to carry (PR #20
# independent code review, HIGH finding: goldens never fire a click) moved
# to test_scr_intent.c in S16 slice c2, once GO/DISMISS/CANCEL/FLARE
# stopped mutating a live ff_flare_t directly and started emitting
# through the intent seam instead — see that file's header comment for
# the reusable "build the real screen, synthesize a click, assert on what
# came out of the seam" pattern this repo's input tests now follow.
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
add_library(ff-shell STATIC ${FF_SHELL_SOURCES})
target_include_directories(ff-shell PUBLIC include)
target_link_libraries(ff-shell PUBLIC ff-app ff-route ff-wiring ff-core ff-meshclient ff-festpack ff-wall-window)
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

    # S16c1 — ff_shell_intent dispatch + the emit seam, driven with no
    # transport/LVGL (same injection harness as test_shell). Separate
    # executable rather than more test_shell.c so the intent slice's
    # coverage reads (and reviews) as one unit.
    add_executable(test_intent
        tests/test_intent.c
    )
    target_link_libraries(test_intent PRIVATE ff-shell ff-intent unity)
    ff_apply_warnings(test_intent)

    add_test(NAME test_intent COMMAND test_intent)

    # S16c1/c2 — headless LVGL INTERACTION tests for every wired emit
    # site: c1's three navigation-only stubs (nav long-press, Compose
    # back, Signals "+") plus c2's core-mutating ones (Compose SEND,
    # Signals rally tap + canned-reply chips, radar FLARE, and the S10
    # takeover/sender-overlay buttons GO/DISMISS/CANCEL, moved here from
    # test_scr_flare.c once they stopped mutating a live ff_flare_t
    # directly) — "build the real screen, synthesize the event, assert on
    # what came out the seam". A golden can't prove a click emits the
    # right intent, or any intent at all.
    add_executable(test_scr_intent
        screens/tests/test_scr_intent.c
    )
    target_link_libraries(test_scr_intent PRIVATE ff-app-ui ff-intent unity)
    ff_apply_warnings(test_scr_intent)

    add_test(NAME test_scr_intent COMMAND test_scr_intent)

    # S26 slice d (maintainer decision B, 2026-09-02) — the banner strip's
    # move to cover the status bar: geometry (disjoint from Radar's name/
    # distance stack and a thread's first bubble, covers the status row,
    # every corner clears the glass by 10px) plus the #145 PRESS_LOCK tap/
    # drag proof, same "build the real screens, measure the real rects"
    # discipline test_scr_flare.c/test_radar_layout.c already established
    # for this exact class of bug (a pixel-stable golden can't catch a
    # wrong-but-stable placement).
    add_executable(test_scr_banner
        screens/tests/test_scr_banner.c
    )
    target_link_libraries(test_scr_banner PRIVATE ff-app-ui ff-intent unity)
    ff_apply_warnings(test_scr_banner)

    add_test(NAME test_scr_banner COMMAND test_scr_banner)
endif()

# S20 (app/ff_demo.c — demo-mode seeding; docs/specs/S20-demo-mode.md)
# ---------------------------------------------------------------------------
# Seeds the shell into the fictional "Firefly Fields" festival through the
# real shell entry points (ff_shell_events/pair/load_pack/set_my_pos), so
# every face shows honest, real-looking data with no mesh. A THIRD file
# that legitimately links core + meshclient + app together (alongside
# ff-wiring and ff-shell — see ff_demo.h's Layering note): it constructs
# mc_nodeinfo_t/mc_position_t/mc_rx_meta_t to drive the shell's inbound
# seam. No LVGL. Target-agnostic — both targets/sim and targets/esp32s3
# link it. The festpack BYTES are provided by the target (a generated C
# array in the sim, EMBED_FILES on device); ff_demo_seed only parses them.
#
# S23c (app/ff_demoapply.c) joins ff_demo.c here: the live demo's per-event
# apply glue + the pure idx->node_id / text_ref->string / kind->dispatch
# mapping (ff_demoapply.h). Same demo-content gating as ff_demo.c — on
# device reachable only from app_main.c's CONFIG_FF_DEMO_MODE/_LIVE block,
# dead-stripped from a field build.
add_library(ff-demo STATIC ${FF_DEMO_SOURCES})
target_include_directories(ff-demo PUBLIC include)
target_link_libraries(ff-demo PUBLIC ff-shell ff-core ff-meshclient)
target_compile_features(ff-demo PUBLIC c_std_11)
ff_apply_warnings(ff-demo)

if(FF_BUILD_TESTS)
    # Seeds a real (no-transport) shell and asserts the four Radar states,
    # the feed, and the latched wall clock — the demo world computed
    # honestly through core, not asserted by hand.
    add_executable(test_demo
        tests/test_demo.c
    )
    target_link_libraries(test_demo PRIVATE ff-demo unity)
    # The demo festpack asset, read from the source tree at test time.
    target_compile_definitions(test_demo PRIVATE
        FF_DEMO_PACK_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/demo/firefly-fields.festpack.json"
    )
    ff_apply_warnings(test_demo)

    add_test(NAME test_demo COMMAND test_demo)

    # S23c — the pure apply-mapping helper (idx->node_id, text_ref bounds,
    # kind->dispatch) unit-tested with no shell/clock/radio, plus an
    # integration pass that drives ff_demo_apply_event into a real seeded
    # shell and asserts a synthetic signal lands in the feed and a poke
    # refreshes presence — through the real projection, not hand-poked.
    add_executable(test_demoapply
        tests/test_demoapply.c
    )
    target_link_libraries(test_demoapply PRIVATE ff-demo unity)
    target_compile_definitions(test_demoapply PRIVATE
        FF_DEMO_PACK_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/demo/firefly-fields.festpack.json"
    )
    ff_apply_warnings(test_demoapply)

    add_test(NAME test_demoapply COMMAND test_demoapply)
endif()
