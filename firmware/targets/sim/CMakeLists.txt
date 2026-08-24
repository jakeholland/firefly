# ffsim — desktop sim target (FF_TARGET=sim). SDL window or headless PNG.

# S13b
# ---------------------------------------------------------------------------
# ff_app_state_t JSON fixture loader (see fixture.h/fixture.c). A STATIC
# library (not a source compiled straight into ffsim) so test_fixture below
# can link the exact same object rather than duplicating the parser logic
# in a second translation unit — same shape as ff-sim-store just below.
# ---------------------------------------------------------------------------
add_library(ff-sim-fixture STATIC
    fixture.c
)
target_include_directories(ff-sim-fixture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ff-sim-fixture PUBLIC ff-app PRIVATE ff-jsmn)
ff_apply_warnings(ff-sim-fixture)

if(FF_BUILD_TESTS)
    add_executable(test_fixture
        tests/test_fixture.c
    )
    target_link_libraries(test_fixture PRIVATE ff-sim-fixture ff-app unity)
    # tests/fixtures/*.json are read from disk at test time (same pattern
    # as firmware/festpack/tests's FP_FIXTURE_DIR) — point the test at the
    # committed fixtures dir regardless of build dir.
    target_compile_definitions(test_fixture PRIVATE
        FF_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/"
    )
    ff_apply_warnings(test_fixture)

    add_test(NAME test_fixture COMMAND test_fixture)
endif()

# S08 (PR #25 UX review follow-up) — face_dispatch.c (which fixture face
# maps to which screen builder) extracted out of main.c into its own
# STATIC library, bundled with fixture_view.c (the S13 placeholder debug
# face face_dispatch falls back to), so BOTH ffsim's own render paths AND
# targets/sim/tests/test_face_hit_targets.c (the cross-face round-glass
# hit-target sweep — needs to build the exact real screen a fixture would
# render) share one dispatch table instead of risking two copies drifting
# apart. Previously fixture_view.c compiled straight into the ffsim
# executable with no separate lib "because nothing else links/tests it" —
# that's no longer true once this sweep test exists.
add_library(ff-sim-face-dispatch STATIC
    face_dispatch.c
    fixture_view.c
)
target_include_directories(ff-sim-face-dispatch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ff-sim-face-dispatch PUBLIC ff-app-ui)
ff_apply_warnings(ff-sim-face-dispatch)

# S13c
# ---------------------------------------------------------------------------
# ff_ctl_server: the --ctl PORT control socket. A STATIC library (not
# sources compiled straight into ffsim) for the same reason as
# ff-sim-fixture above — test_ctl_server links the exact object under
# test rather than duplicating it in a second translation unit. No
# ff-app/LVGL/fixture dependency at all (see ctl_server.h's design note):
# it only knows ff_ctl_handlers_t, a caller-supplied vtable.
# ---------------------------------------------------------------------------
add_library(ff-sim-ctl STATIC
    ctl_server.c
)
target_include_directories(ff-sim-ctl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ff-sim-ctl PRIVATE ff-jsmn)
ff_apply_warnings(ff-sim-ctl)

if(FF_BUILD_TESTS)
    add_executable(test_ctl_server
        tests/test_ctl_server.c
    )
    target_link_libraries(test_ctl_server PRIVATE ff-sim-ctl unity)
    ff_apply_warnings(test_ctl_server)

    add_test(NAME test_ctl_server COMMAND test_ctl_server)
endif()

# S13c (review fixup, PR #19 finding #2)
# ---------------------------------------------------------------------------
# ff_ctl_out_path: confines the ctl socket's `screenshot` path to a single
# configured output root (--ctl-out DIR). Separate STATIC library from
# ff-sim-ctl above (same "test links the exact object" reasoning) — this
# one needs realpath()/lstat(), ctl_server.c deliberately doesn't.
# ---------------------------------------------------------------------------
add_library(ff-sim-ctlout STATIC
    ctl_out_path.c
)
target_include_directories(ff-sim-ctlout PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
ff_apply_warnings(ff-sim-ctlout)

if(FF_BUILD_TESTS)
    add_executable(test_ctl_out_path
        tests/test_ctl_out_path.c
    )
    target_link_libraries(test_ctl_out_path PRIVATE ff-sim-ctlout unity)
    ff_apply_warnings(test_ctl_out_path)

    add_test(NAME test_ctl_out_path COMMAND test_ctl_out_path)
endif()

# S13c
# ---------------------------------------------------------------------------
# ff_live: minimal mc_client -> ff_app_state_t wiring for `ffsim --connect`
# (S13 slice a's --connect flag, implemented here as slice c/d's
# prerequisite — see the PR body's "deviations" section for why this
# landed in this slice rather than a/b). STATIC library so test_live can
# link it directly.
# ---------------------------------------------------------------------------
add_library(ff-sim-live STATIC
    live.c
)
target_include_directories(ff-sim-live PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ff-sim-live PUBLIC ff-sim-fixture ff-meshclient ff-core PRIVATE ff-festpack)
ff_apply_warnings(ff-sim-live)

if(FF_BUILD_TESTS)
    add_executable(test_live
        tests/test_live.c
    )
    # test_live.c hand-encodes synthetic mc protobuf frames (same technique
    # as meshclient/tests/test_meshclient.c) to drive ff_live_t end to end
    # without a real meshtasticd/docker — needs the proto message structs
    # and nanopb's encoder directly, not just ff-meshclient's public API.
    target_link_libraries(test_live PRIVATE ff-sim-live ff-meshclient-proto ff-nanopb unity)
    target_compile_definitions(test_live PRIVATE
        FF_LIVE_FESTPACK_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../festpack/tests/fixtures"
    )
    ff_apply_warnings(test_live)

    add_test(NAME test_live COMMAND test_live)
endif()

# Issue #3 — ff_screenshot_write now creates its output directory itself
# (`--headless --screenshot DIR` used to fail until the caller mkdir'd
# DIR). screenshot.c is compiled directly into this test binary (a second
# independent executable, so no duplicate-symbol concern with ffsim's own
# copy) rather than split into a static lib — it's one function plus a
# vendored stb implementation, and nothing else links it.
if(FF_BUILD_TESTS)
    add_executable(test_screenshot
        tests/test_screenshot.c
        screenshot.c
    )
    target_include_directories(test_screenshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_screenshot PRIVATE ff-stb-image-write unity)
    ff_apply_warnings(test_screenshot)

    add_test(NAME test_screenshot COMMAND test_screenshot)
endif()

add_executable(ffsim
    main.c
    screenshot.c     # S13c — shared XRGB8888->PNG writer (extracted out of
                     # main.c so the ctl socket's "screenshot" command and
                     # the one-shot --headless --screenshot path share one
                     # implementation). fixture_view.c is NOT listed here —
                     # it's built into ff-sim-face-dispatch above (S08's
                     # extraction); listing it again here would link its
                     # symbols twice.
)

# Land the binary at build/ffsim regardless of source nesting — that's the
# path CLAUDE.md/AGENTS.md and CI both invoke.
set_target_properties(ffsim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

target_link_libraries(ffsim PRIVATE
    # ff-core and ff-sim-fixture: NEITHER is listed explicitly here, even
    # though main.c uses both directly (ff_version.h, fixture.h's
    # ff_fixture_load_file) — both are already brought in transitively
    # through ff-sim-live (`ff-sim-live PUBLIC ff-sim-fixture ff-core ...`,
    # see the S13c library declaration above), which itself is listed
    # below. Listing either of them AGAIN here just duplicates the
    # static-library entry on the final link line: harmless (CMake/the
    # linker still resolve every symbol correctly — confirmed by a clean
    # from-scratch build), but Apple's ld warns "ignoring duplicate
    # libraries" for it, which is pure noise once you've confirmed it's
    # harmless (S08 PR #25 code review, NIT finding — this used to list
    # ff-sim-fixture explicitly, which is exactly the ff-sim-live-vs-
    # direct duplicate the warning was flagging; ff-core was already
    # correctly omitted for the identical reason, see the note this one
    # replaces).
    ff-sim-face-dispatch # S08 — face_dispatch.h (this file, above)
    ff-sim-ctl           # S13c — ctl_server.h/.c (ctl_server.c above)
    ff-sim-ctlout        # S13c — ctl_out_path.h/.c: screenshot path confinement (review fixup)
    ff-sim-live          # S13c — live.h/.c: --connect / --pack wiring (live.c above); also the
                         # transitive source of ff-sim-fixture/ff-core above
    ff-app-ui            # S06b — scr_nav.h/scr_radar.h (app/CMakeLists.txt)
    lvgl::lvgl
    ff-stb-image-write
)

if(TARGET SDL2::SDL2)
    target_link_libraries(ffsim PRIVATE SDL2::SDL2)
else()
    target_include_directories(ffsim PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(ffsim PRIVATE ${SDL2_LIBRARIES})
endif()

ff_apply_warnings(ffsim)

# ---------------------------------------------------------------------------
# S11 — trivial file-backed ff_store_t for the sim target (slice a).
# ---------------------------------------------------------------------------
add_library(ff-sim-store STATIC
    store_file.c
)
target_include_directories(ff-sim-store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ff-sim-store PUBLIC ff-core)
ff_apply_warnings(ff-sim-store)

if(FF_BUILD_TESTS)
    add_executable(test_store_file
        tests/test_store_file.c
    )
    target_link_libraries(test_store_file PRIVATE ff-sim-store ff-core unity)
    ff_apply_warnings(test_store_file)

    add_test(NAME test_store_file COMMAND test_store_file)
endif()

# ---------------------------------------------------------------------------
# S08 (PR #25 UX review follow-up, blocking finding #3 — "add an automated
# assertion that makes this class impossible"). For every committed
# tests/fixtures/*.json, builds the exact real screen ffsim would (via
# ff-sim-face-dispatch, the same dispatch table main.c uses — not a
# reimplementation) and walks the resulting LVGL object tree asserting
# every CLICKABLE element's hit-rect lies entirely within the round glass
# (ff-layout's ff_layout_rect_in_circle) and is >= FF_THEME_MIN_HIT_PX in
# both dimensions. Modeled on app/screens/tests/test_radar_layout.c's
# "assertion-level, not visual" discipline: goldens are a pixel-diff
# against themselves, so a control that's wrong-but-stable in every
# render passes them forever — exactly how the Compose placement bug
# shipped in the first place.
# ---------------------------------------------------------------------------
if(FF_BUILD_TESTS)
    add_executable(test_face_hit_targets
        tests/test_face_hit_targets.c
    )
    target_link_libraries(test_face_hit_targets PRIVATE
        ff-sim-fixture
        ff-sim-face-dispatch
        ff-app-ui
        ff-layout
        lvgl::lvgl
        unity
    )
    target_compile_definitions(test_face_hit_targets PRIVATE
        FF_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/"
    )
    ff_apply_warnings(test_face_hit_targets)

    add_test(NAME test_face_hit_targets COMMAND test_face_hit_targets)
endif()
