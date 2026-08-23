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

add_executable(ffsim
    main.c
    fixture_view.c  # S13b — placeholder fixture debug face; no separate
                     # lib (nothing else links/tests it — rendering is
                     # covered by golden PNGs, not unit tests, per
                     # docs/ARCHITECTURE.md's "compute = unit; render =
                     # golden PNG" split).
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
