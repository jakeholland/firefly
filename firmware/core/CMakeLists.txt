# firefly-core — pure C11 domain logic. No I/O, no LVGL, no ESP-IDF.

add_library(ff-core STATIC
    src/ff_version.c
    src/ff_settings.c # S11
)

target_include_directories(ff-core PUBLIC include)
target_compile_features(ff-core PUBLIC c_std_11)
# ff_geo.h's ff_latlon_t lives in platform/ (docs/ARCHITECTURE.md's shared
# seam, see firmware/platform/include/ff_latlon.h) — PUBLIC so anything
# linking ff-core also sees platform/include for that (and ff_clock.h).
target_link_libraries(ff-core PUBLIC ff-platform)
# ff_geo.c calls cos()/sqrt()/fmodf() etc (math.h). On macOS/AppleClang that
# resolves via libSystem with no extra flag, which is why this was missed
# locally; glibc (Linux CI) needs an explicit -lm or the final link fails
# with "undefined reference to `cos'". CMake propagates a STATIC library's
# PRIVATE link libraries to whatever links this one, so this is enough for
# test_core, test_geo, and any future consumer.
if(UNIX)
    target_link_libraries(ff-core PRIVATE m)
endif()
ff_apply_warnings(ff-core)

if(FF_BUILD_TESTS)
    add_executable(test_core
        tests/test_version.c
    )
    target_link_libraries(test_core PRIVATE ff-core unity)
    ff_apply_warnings(test_core)

    add_test(NAME test_core COMMAND test_core)

    # S11 — settings & persistence (slice a: store seam + settings + logic).
    add_executable(test_settings
        tests/test_settings.c
    )
    target_link_libraries(test_settings PRIVATE ff-core unity)
    ff_apply_warnings(test_settings)

    add_test(NAME test_settings COMMAND test_settings)
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

# S04 (firefly protocol — app-layer pulse/flare/rally/status packets;
# docs/specs/S04-firefly-protocol.md)
target_sources(ff-core PRIVATE src/ff_proto.c)

if(FF_BUILD_TESTS)
    add_executable(test_proto
        tests/test_proto.c
    )
    target_compile_definitions(test_proto PRIVATE
        FF_PROTO_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    target_link_libraries(test_proto PRIVATE ff-core unity)
    ff_apply_warnings(test_proto)

    add_test(NAME test_proto COMMAND test_proto)
endif()

# S02 (core/crew — crew model & freshness; docs/specs/S02-core-crew.md)
target_sources(ff-core PRIVATE src/ff_crew.c)

if(FF_BUILD_TESTS)
    add_executable(test_crew
        tests/test_crew.c
    )
    target_link_libraries(test_crew PRIVATE ff-core unity)
    ff_apply_warnings(test_crew)

    add_test(NAME test_crew COMMAND test_crew)
endif()
