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

# S16b2: live.{c,h} (S13c's interim mc_client -> ff_app_state_t wiring,
# `ff-sim-live`) and its tests are RETIRED — `ffsim --connect` drives the
# app shell (app/ff_shell.h, linked below) instead, closing live.h's
# self-documented spec-gap deviation. See docs/specs/S13-sim-target.md's
# Amendments and docs/specs/S16-app-shell.md slice b2.

# S16 slice d
# ---------------------------------------------------------------------------
# ff-sim-live-setup: the "bring a live ff_shell_t up" sequence (transport,
# --dev-trust-all, --pack, heading) shared between ffsim's live window mode
# and the --ctl loop (live_setup.h's top comment explains why one
# implementation, not two). STATIC so both ffsim and test_ctl_flare_sequence
# below link the exact same object.
# ---------------------------------------------------------------------------
add_library(ff-sim-live-setup STATIC
    live_setup.c
)
target_include_directories(ff-sim-live-setup PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ff-sim-live-setup PUBLIC ff-shell)
ff_apply_warnings(ff-sim-live-setup)

# ff-sim-ctl-loop: the --ctl session itself (ctl_loop.h/.c), extracted out
# of main.c so it can be driven in-process by test_ctl_flare_sequence (AC10
# — a full draft-typed/flare-injected/takeover/cleared sequence through the
# ctl socket's REAL command-processing layer, no docker, no real TCP
# client) as well as by ffsim's own main().
# ---------------------------------------------------------------------------
add_library(ff-sim-ctl-loop STATIC
    ctl_loop.c
)
target_include_directories(ff-sim-ctl-loop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ff-sim-ctl-loop
    PUBLIC
        ff-sim-live-setup
        ff-sim-ctl
        ff-shell
        lvgl::lvgl
    PRIVATE
        ff-sim-ctlout
        ff-sim-face-dispatch
        ff-sim-fixture
)
ff_apply_warnings(ff-sim-ctl-loop)

if(FF_BUILD_TESTS)
    # S16 slice d, AC10: draft typed -> flare injected -> takeover renders
    # -> cleared -> composer returns with draft intact, driven through
    # ff_ctl_process_line (the ctl socket's pure, socket-free command layer
    # — ctl_server.h's own documented split) against a real ff-sim-ctl-loop
    # session. screenshot.c is compiled directly into this test binary (a
    # second independent executable, same shape as test_screenshot below)
    # since ff-sim-ctl-loop's screenshot handler needs ff_screenshot_write
    # but nothing else here links a copy of it.
    add_executable(test_ctl_flare_sequence
        tests/test_ctl_flare_sequence.c
        screenshot.c
    )
    target_link_libraries(test_ctl_flare_sequence PRIVATE
        ff-sim-ctl-loop
        ff-sim-ctl
        ff-shell
        ff-app-ui
        ff-stb-image-write
        lvgl::lvgl
        unity
    )
    ff_apply_warnings(test_ctl_flare_sequence)

    add_test(NAME test_ctl_flare_sequence COMMAND test_ctl_flare_sequence)

    # S26 slice c, AC3: "OFF skips face rebuilds" — asserts the render
    # loop's rebuild count is 0 across an OFF window, against real dirty
    # ticks (not "nothing happened anyway" — see the test file's top
    # comment), driven through the same ff-sim-ctl-loop session as
    # test_ctl_flare_sequence above.
    add_executable(test_idle_render_skip
        tests/test_idle_render_skip.c
        screenshot.c
    )
    target_link_libraries(test_idle_render_skip PRIVATE
        ff-sim-ctl-loop
        ff-sim-ctl
        ff-shell
        ff-app-ui
        ff-stb-image-write
        lvgl::lvgl
        unity
    )
    ff_apply_warnings(test_idle_render_skip)

    add_test(NAME test_idle_render_skip COMMAND test_idle_render_skip)

    # S26 wake-only-touch amendment (docs/specs/S26-device-lifecycle.md
    # "(c) Inactivity -> dim -> screen off", 2026-09-02): a real ctl tap
    # that BEGINS while idle is DIM/OFF must wake ACTIVE and deliver
    # NOTHING to LVGL (no CLICKED, no PRESSED style) — driven through the
    # same real ff-sim-ctl-loop session as the tests above.
    add_executable(test_wakeonly_touch
        tests/test_wakeonly_touch.c
        screenshot.c
    )
    target_link_libraries(test_wakeonly_touch PRIVATE
        ff-sim-ctl-loop
        ff-sim-ctl
        ff-shell
        ff-app-ui
        ff-stb-image-write
        lvgl::lvgl
        unity
    )
    ff_apply_warnings(test_wakeonly_touch)

    add_test(NAME test_wakeonly_touch COMMAND test_wakeonly_touch)
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

# S20 — embed the Firefly Fields demo festpack as a C byte array for
# --demo, GENERATED from the JSON asset at configure time (the JSON under
# firmware/assets/demo/ is the single source of truth; the array is never
# hand-edited). A stale cached configure would only miss a JSON edit until
# the next `cmake` reconfigure — CI configures fresh, and the demo asset is
# stable. On device the same bytes come from ESP-IDF EMBED_FILES instead.
set(FF_DEMO_ASSET ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/demo/firefly-fields.festpack.json)
set(FF_DEMO_GEN ${CMAKE_CURRENT_BINARY_DIR}/ff_demo_festpack_gen.c)
file(READ ${FF_DEMO_ASSET} _ff_demo_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _ff_demo_bytes "${_ff_demo_hex}")
file(WRITE ${FF_DEMO_GEN}
    "/* GENERATED from ${FF_DEMO_ASSET} at configure time — do not edit. */\n"
    "#include \"ff_demo_festpack.h\"\n"
    "const unsigned char ff_demo_festpack_json[] = { ${_ff_demo_bytes} };\n"
    "const unsigned int ff_demo_festpack_json_len = sizeof(ff_demo_festpack_json);\n")

add_executable(ffsim
    main.c
    ff_demo_run.c    # S20 — the --demo render driver (Firefly Fields)
    ${FF_DEMO_GEN}   # S20 — generated demo festpack bytes (above)
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

# S18 slice c (#40) — this build's date, for main.c's no-pack-window decay
# backstop (ffsim_warn_if_epoch_ceiling_near). Computed at configure time
# as unix seconds; the actual threshold and predicate live in core
# (ff_wall_ceiling_deadline_near / FF_WALL_EPOCH_CEILING), so nothing here
# duplicates the constant and it cannot drift from the gate. A stale
# cached configure only makes the date older (a MORE conservative warning),
# never a false negative near the real deadline.
string(TIMESTAMP FF_WALL_BUILD_UNIX_S "%s" UTC)
target_compile_definitions(ffsim PRIVATE FF_WALL_BUILD_UNIX_S=${FF_WALL_BUILD_UNIX_S})

target_link_libraries(ffsim PRIVATE
    # ff-core and ff-meshclient are NOT listed explicitly, even though
    # main.c uses both directly (ff_version.h, mc_transport_tcp.h) —
    # both arrive transitively through ff-shell (`ff-shell PUBLIC ...
    # ff-core ff-meshclient ff-festpack`, app/CMakeLists.txt). Listing
    # them AGAIN would duplicate the static-library entry on the final
    # link line: harmless, but Apple's ld warns "ignoring duplicate
    # libraries" for it (S08 PR #25 code review, NIT finding — same
    # reasoning as the ff-sim-live-era note this one replaces).
    ff-sim-face-dispatch # S08 — face_dispatch.h (this file, above)
    ff-sim-ctl           # S13c — ctl_server.h/.c (ctl_server.c above)
    ff-sim-ctlout        # S13c — ctl_out_path.h/.c: screenshot path confinement (review fixup)
    ff-sim-fixture       # S13b — fixture.h: --fixture load + the ctl `state` dump
    ff-sim-live-setup    # S16 slice d — live_setup.h: the --connect/--pack/--dev-trust-all
                         # sequence shared between live window mode and the ctl loop
    ff-sim-ctl-loop      # S16 slice d — ctl_loop.h: the --ctl session itself
    ff-shell             # S16b2 — the app shell: --connect / --pack / --dev-trust-all
                         # (replaces ff-sim-live); transitive source of ff-core/ff-meshclient
    ff-demo              # S20 — ff_demo_seed: the Firefly Fields demo world (--demo)
    ff-app-ui            # S06b — scr_nav.h/scr_radar.h (app/CMakeLists.txt)
    lvgl::lvgl
    ff-stb-image-write
)

# S20 — the generated ff_demo_festpack_gen.c #includes "ff_demo_festpack.h"
# from this source dir; the generated file itself lives in the binary dir.
target_include_directories(ffsim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

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

    # S16 slice e, AC8 — "a FF_INTENT_SETTING_SET is persisted: shell
    # closed, re-inited against the same store, value survives", against
    # the REAL file-backed store (not a mock round-trip — see the file's
    # header comment for why that distinction matters here). Lives here
    # rather than app/tests/test_intent.c because store_file.c (ff-sim-store)
    # is a targets/sim-only seam; app/ cannot depend on it.
    add_executable(test_shell_settings_persist
        tests/test_shell_settings_persist.c
    )
    target_link_libraries(test_shell_settings_persist PRIVATE ff-shell ff-sim-store unity)
    ff_apply_warnings(test_shell_settings_persist)

    add_test(NAME test_shell_settings_persist COMMAND test_shell_settings_persist)
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

# ---------------------------------------------------------------------------
# Issue #75/#77 — the map face's circle-containment regression, targeted at
# tests/fixtures/map_clip_stress.json. Same shape as test_face_hit_targets
# above (in-process ff_build_face_screen + a headless display buffer, no
# SDL) but checks LABEL objects specifically (not just CLICKABLE ones —
# scr_map.c's labels are deliberately non-clickable, tap-anywhere-on-puck
# chrome) and asserts the build completes fast, the actual issue #75
# regression. See test_map_circle_containment.c's own header comment for
# why the polygon/line vertex-clamp half of the fix is proven at the
# geometry level instead (core/tests/test_map.c), not here.
# ---------------------------------------------------------------------------
if(FF_BUILD_TESTS)
    add_executable(test_map_circle_containment
        tests/test_map_circle_containment.c
    )
    target_link_libraries(test_map_circle_containment PRIVATE
        ff-sim-fixture
        ff-sim-face-dispatch
        ff-app-ui
        ff-layout
        lvgl::lvgl
        unity
    )
    target_compile_definitions(test_map_circle_containment PRIVATE
        FF_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/"
    )
    ff_apply_warnings(test_map_circle_containment)

    add_test(NAME test_map_circle_containment COMMAND test_map_circle_containment)
endif()
