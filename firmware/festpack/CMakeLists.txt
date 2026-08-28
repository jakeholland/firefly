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

    # S20 — the authored demo festpack (firmware/assets/demo/) parses to
    # the v0.1 schema. Read from the assets dir at test time.
    add_executable(test_demo_pack
        tests/test_demo_pack.c
    )
    target_link_libraries(test_demo_pack PRIVATE ff-festpack unity)
    target_compile_definitions(test_demo_pack PRIVATE
        FF_DEMO_PACK_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/demo/firefly-fields.festpack.json"
    )
    ff_apply_warnings(test_demo_pack)

    add_test(NAME test_demo_pack COMMAND test_demo_pack)
endif()

# S07 — schedule engine (now-playing/next-starred/day-lineup/alarm), slices
# a + c. See firmware/festpack/include/ff_sched.h for why this lives here
# instead of core/ (module-placement rationale in the header's top
# comment) — its entire public surface is fp_pack_t/fp_set_t, so it is
# built into ff-festpack itself rather than as a separate library.
target_sources(ff-festpack PRIVATE src/ff_sched.c)

# S08 predictive-T9 addendum — festpack->words bridge (fp_t9words). Turns a
# loaded fp_pack_t's artist/stage/landmark names into the caller-supplied
# supplementary word array that core/t9pred's ff_t9pred_match_ex ranks above
# its static dictionary. Lives HERE (not core/t9pred) so the engine stays
# festpack-free — see fp_t9words.h's placement note. Depends only on fp_pack.h;
# it does NOT link or include the engine (it just produces C strings).
target_sources(ff-festpack PRIVATE src/fp_t9words.c)

if(FF_BUILD_TESTS)
    # test_t9words exercises the bridge AND the pure engine together: it links
    # ff-core directly (ff-festpack links ff-core PRIVATE, so its include dir
    # is not propagated) to reach ff_t9pred.h / the dictionary.
    add_executable(test_t9words
        tests/test_t9words.c
    )
    target_link_libraries(test_t9words PRIVATE ff-festpack ff-core unity)
    ff_apply_warnings(test_t9words)

    add_test(NAME test_t9words COMMAND test_t9words)
endif()

if(FF_BUILD_TESTS)
    add_executable(test_sched
        tests/test_sched.c
    )
    target_link_libraries(test_sched PRIVATE ff-festpack unity)
    # test_sched's one integration case (S07_AC5, real Lost Lands fixture)
    # reads fixtures/ from disk at test time, same fixture dir as
    # test_festpack above — no need for a separate test_sched_integration
    # executable: ff_sched already lives inside ff-festpack, so linking
    # fp_parse alongside the rest of test_sched's fixture-literal cases
    # has no layering cost (see ff_sched.h's placement note).
    target_compile_definitions(test_sched PRIVATE
        FP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/"
    )
    ff_apply_warnings(test_sched)

    add_test(NAME test_sched COMMAND test_sched)
endif()
