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

add_executable(ffsim
    main.c
)

# Land the binary at build/ffsim regardless of source nesting — that's the
# path CLAUDE.md/AGENTS.md and CI both invoke.
set_target_properties(ffsim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

target_link_libraries(ffsim PRIVATE
    # ff-core: not listed explicitly — main.c's ff_version.h needs it, but
    # ff-sim-fixture already brings it in transitively (ff-sim-fixture
    # PUBLIC ff-app INTERFACE ff-core, see app/CMakeLists.txt's S06 note);
    # listing it again here just duplicated the static-library entry on
    # the final link line (harmless on Linux/GNU ld, but Apple's ld warns
    # "ignoring duplicate libraries" for it).
    ff-sim-fixture       # S13b — fixture.h loader (fixture.c above)
    ff-sim-face-dispatch # face_dispatch.h (this file, above)
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
