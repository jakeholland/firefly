# firefly-core — pure C11 domain logic. No I/O, no LVGL, no ESP-IDF.

add_library(ff-core STATIC
    src/ff_version.c
)

target_include_directories(ff-core PUBLIC include)
target_compile_features(ff-core PUBLIC c_std_11)
ff_apply_warnings(ff-core)

if(FF_BUILD_TESTS)
    add_executable(test_core
        tests/test_version.c
    )
    target_link_libraries(test_core PRIVATE ff-core unity)
    ff_apply_warnings(test_core)

    add_test(NAME test_core COMMAND test_core)
endif()

# S01 (core/geo — bearing, distance, heading fusion; docs/specs/S01-core-geo.md)
target_sources(ff-core PRIVATE src/ff_geo.c)

if(FF_BUILD_TESTS)
    add_executable(test_geo
        tests/test_geo.c
    )
    target_link_libraries(test_geo PRIVATE ff-core unity)
    ff_apply_warnings(test_geo)

    add_test(NAME test_geo COMMAND test_geo)
endif()
