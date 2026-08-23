# festpack — festpack.json subset parser (spec: fest-almanac v0.1).

# S05
add_library(ff-festpack STATIC
    src/fp_pack.c
)

target_include_directories(ff-festpack PUBLIC include)
target_compile_features(ff-festpack PUBLIC c_std_11)
target_link_libraries(ff-festpack PRIVATE ff-jsmn)
# fp_project() calls cos() (math.h). On macOS/AppleClang that resolves via
# libSystem with no extra flag, which is why this was missed locally; glibc
# (Linux CI) needs an explicit -lm or the final link fails with "undefined
# reference to `cos'". CMake propagates a STATIC library's PRIVATE link
# libraries to whatever links this one, so this is enough for test_festpack
# and any future consumer.
if(UNIX)
    target_link_libraries(ff-festpack PRIVATE m)
endif()
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
