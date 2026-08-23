# ffsim — desktop sim target (FF_TARGET=sim). SDL window or headless PNG.

add_executable(ffsim main.c)

# Land the binary at build/ffsim regardless of source nesting — that's the
# path CLAUDE.md/AGENTS.md and CI both invoke.
set_target_properties(ffsim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

target_link_libraries(ffsim PRIVATE
    ff-core
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
