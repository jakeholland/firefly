# festpack — festpack.json subset parser (spec: fest-almanac v0.1).

# S05
add_library(ff-festpack STATIC
    src/fp_pack.c
)

target_include_directories(ff-festpack PUBLIC include)
target_compile_features(ff-festpack PUBLIC c_std_11)
# fp_pack.h's ff_latlon_t comes from platform/ (see
# firmware/platform/include/ff_latlon.h) — PUBLIC so consumers of
# fp_pack.h resolve that include without also pulling in core/.
# ff-core is PRIVATE: fp_pack.c calls ff_geo_project() (ff_geo.h) to do
# the lat/lon -> east/north projection (was a local TODO(S01) stub, now
# the real thing — see docs/specs/S01-core-geo.md), but that dependency
# is an implementation detail, not part of fp_pack.h's public surface.
target_link_libraries(ff-festpack
    PUBLIC ff-platform
    PRIVATE ff-jsmn ff-core
)
ff_apply_warnings(ff-festpack)

if(FF_BUILD_TESTS)
    add_executable(test_festpack
        tests/test_festpack.c
    )
    target_link_libraries(test_festpack PRIVATE ff-festpack unity)
    # Fixtures are read from disk at test time (not embedded) — point the
    # test at its own source-tree fixtures/ dir regardless of build dir.
    target_compile_definitions(test_festpack PRIVATE
        FP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/"
    )
    ff_apply_warnings(test_festpack)

    add_test(NAME test_festpack COMMAND test_festpack)
endif()
