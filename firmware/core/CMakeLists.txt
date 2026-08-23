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

# S06 (core/radar — radar-face view compute; docs/specs/S06-radar-face.md)
target_sources(ff-core PRIVATE src/ff_radar.c)

if(FF_BUILD_TESTS)
    add_executable(test_radar
        tests/test_radar.c
    )
    target_link_libraries(test_radar PRIVATE ff-core unity)
    ff_apply_warnings(test_radar)

    add_test(NAME test_radar COMMAND test_radar)
endif()

# S08a (core/t9 — multi-tap T9 text-entry engine; docs/specs/S08-signals-t9.md,
# slice a. UI + feed wiring are separate slices, not part of this build unit.)
target_sources(ff-core PRIVATE src/ff_t9.c)

if(FF_BUILD_TESTS)
    add_executable(test_t9
        tests/test_t9.c
    )
    target_link_libraries(test_t9 PRIVATE ff-core unity)
    ff_apply_warnings(test_t9)

    add_test(NAME test_t9 COMMAND test_t9)
endif()

# S10a (flare state machine — IDLE/SENDING/RECEIVED/LOCKED;
# docs/specs/S10-flare.md, slice a. Slice b — takeover + sender UI +
# goldens — needs the S06 app shell and is not part of this slice.)
target_sources(ff-core PRIVATE src/ff_flare.c)

if(FF_BUILD_TESTS)
    add_executable(test_flare
        tests/test_flare.c
    )
    target_link_libraries(test_flare PRIVATE ff-core unity)
    ff_apply_warnings(test_flare)

    add_test(NAME test_flare COMMAND test_flare)
endif()

# S08b (core/feed — Signals-face event ring buffer; docs/specs/S08-signals-t9.md,
# slice b. Wiring — mc events -> feed pushes, crew-paired-sender filter —
# lives in app/ff_wiring.c, not here: core stays free of meshclient.)
target_sources(ff-core PRIVATE src/ff_feed.c)

if(FF_BUILD_TESTS)
    add_executable(test_feed
        tests/test_feed.c
    )
    target_link_libraries(test_feed PRIVATE ff-core unity)
    ff_apply_warnings(test_feed)

    add_test(NAME test_feed COMMAND test_feed)
endif()

# S08b follow-up (PR #25 code review, MEDIUM finding — roster exhaustion
# from untrusted RF) — core/heard: bounded LRU-evictable heard-node list,
# separate from ff_crew's protected paired roster. See ff_heard.h.
target_sources(ff-core PRIVATE src/ff_heard.c)

if(FF_BUILD_TESTS)
    add_executable(test_heard
        tests/test_heard.c
    )
    target_link_libraries(test_heard PRIVATE ff-core unity)
    ff_apply_warnings(test_heard)

    add_test(NAME test_heard COMMAND test_heard)
endif()
