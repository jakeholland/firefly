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

add_executable(ffsim
    main.c
    fixture_view.c  # S13b — placeholder fixture debug face (still used
                     # for every face except radar; see main.c's
                     # ff_build_face_screen). No separate lib (nothing
                     # else links/tests it — rendering is covered by
                     # golden PNGs, not unit tests, per
                     # docs/ARCHITECTURE.md's "compute = unit; render =
                     # golden PNG" split).
    screenshot.c     # S13c — shared XRGB8888->PNG writer (extracted out of
                     # main.c so the ctl socket's "screenshot" command and
                     # the one-shot --headless --screenshot path share one
                     # implementation).
)

# Land the binary at build/ffsim regardless of source nesting — that's the
# path CLAUDE.md/AGENTS.md and CI both invoke.
set_target_properties(ffsim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

target_link_libraries(ffsim PRIVATE
    # ff-core: not listed explicitly — main.c's ff_version.h and
    # fixture_view.c's ff_app_state.h (-> S06's ff_radar.h) both need it,
    # but ff-sim-fixture already brings it in transitively (ff-sim-fixture
    # PUBLIC ff-app INTERFACE ff-core, see app/CMakeLists.txt's S06 note);
    # listing it again here just duplicated the static-library entry on
    # the final link line (harmless on Linux/GNU ld, but Apple's ld warns
    # "ignoring duplicate libraries" for it).
    ff-sim-fixture  # S13b — fixture.h loader (fixture.c above)
    ff-sim-ctl      # S13c — ctl_server.h/.c (ctl_server.c above)
    ff-sim-ctlout   # S13c — ctl_out_path.h/.c: screenshot path confinement (review fixup)
    ff-sim-live     # S13c — live.h/.c: --connect / --pack wiring (live.c above)
    ff-app-ui       # S06b — scr_nav.h/scr_radar.h (app/CMakeLists.txt)
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
