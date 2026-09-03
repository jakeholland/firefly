# firefly-core — pure C11 domain logic. No I/O, no LVGL, no ESP-IDF.

add_library(ff-core STATIC
    src/ff_version.c
    src/ff_settings.c # S11
    src/ff_touchcal.c # S15d
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

    # S15d — touch calibration (per-axis affine solve/apply; pure core).
    add_executable(test_touchcal
        tests/test_touchcal.c
    )
    target_link_libraries(test_touchcal PRIVATE ff-core unity)
    ff_apply_warnings(test_touchcal)

    add_test(NAME test_touchcal COMMAND test_touchcal)

    # Tech-debt sprint: ff_time_reached (firmware/platform/include/ff_clock.h)
    # — the single de-duplicated replacement for six identical
    # reached()/due() one-liners core modules used to each define.
    add_executable(test_time_reached
        tests/test_time_reached.c
    )
    target_link_libraries(test_time_reached PRIVATE ff-platform unity)
    ff_apply_warnings(test_time_reached)

    add_test(NAME test_time_reached COMMAND test_time_reached)
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

# S04 (firefly protocol — app-layer flare/rally/status packets;
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

# S08 predictive-T9 addendum (core/t9pred — dictionary + ranking engine;
# docs/specs/S08-signals-t9.md's reserved "Predictive + pack dictionary =
# v1.5"). Pure groundwork ALONGSIDE the multi-tap ff_t9 (which is untouched);
# NOT wired into the composer here. The dictionary table ff_t9dict_data.c is
# GENERATED from core/tools/t9dict_words.txt by core/tools/gen_t9dict.py
# (frequency ranking of public-domain Project Gutenberg text — see
# core/tools/NOTICE-t9dict.md). Regenerate with:
#   (cd core/tools && python3 gen_t9dict.py emit)
target_sources(ff-core PRIVATE
    src/ff_t9pred.c
    src/ff_t9dict_data.c
)

if(FF_BUILD_TESTS)
    add_executable(test_t9pred
        tests/test_t9pred.c
    )
    target_link_libraries(test_t9pred PRIVATE ff-core unity)
    ff_apply_warnings(test_t9pred)

    add_test(NAME test_t9pred COMMAND test_t9pred)
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

# S26g (core/include/ff_flare_mark.h — the flare mark's geometry table,
# shared between app/screens/scr_flare.c's LVGL takeover screen and the
# esp32s3 target's raw pre-LVGL boot splash; docs/specs/S26-device-
# lifecycle.md "(g) Boot animation" amendment, 2026-09-02). Header-only
# (no ff-core source to add — see the header's own top comment), so this
# test #includes it directly; see test_flare_mark.c's top comment for why
# it still links ff-core (libm for sinf/cosf, propagated the same way
# test_geo.c documents).
if(FF_BUILD_TESTS)
    add_executable(test_flare_mark
        tests/test_flare_mark.c
    )
    target_link_libraries(test_flare_mark PRIVATE ff-core unity)
    ff_apply_warnings(test_flare_mark)

    add_test(NAME test_flare_mark COMMAND test_flare_mark)
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

# core/sigview — originally the whole S22 Signals-face view-model
# (docs/specs/S22-signals-rework.md, slice a); the S24 inbox rework moved
# the row-list/target machinery to ff_inbox.c and the shell. What remains
# is the honest presence classifier (ff_sigview_presence, reused by
# ff_inbox.c) and the ff_target_kind_t vocabulary — see ff_sigview.h.
target_sources(ff-core PRIVATE src/ff_sigview.c)

if(FF_BUILD_TESTS)
    add_executable(test_sigview
        tests/test_sigview.c
    )
    target_link_libraries(test_sigview PRIVATE ff-core unity)
    ff_apply_warnings(test_sigview)

    add_test(NAME test_sigview COMMAND test_sigview)
endif()

# S24a (core/inbox — Signals inbox -> thread view-model: conversations
# (CREW + one per paired member) with honest unread/preview/presence,
# thread extraction with direction preserved, per-thread mark-read;
# docs/specs/S24-signals-inbox.md, slice a. Pure C11, no UI — the S24
# inbox/thread screens, slices b/c, only render this.)
target_sources(ff-core PRIVATE src/ff_inbox.c)

if(FF_BUILD_TESTS)
    add_executable(test_inbox
        tests/test_inbox.c
    )
    target_link_libraries(test_inbox PRIVATE ff-core unity)
    ff_apply_warnings(test_inbox)

    add_test(NAME test_inbox COMMAND test_inbox)
endif()

# S16 slice b0 (core/wall — wall-clock derivation: mesh-timestamp offset
# latch/re-latch, the plausibility gate, and unix -> local ->
# (day_doy, now_min). docs/specs/S16-app-shell.md's "Wall clock" section.)
#
# Lives in core/, not app/, even though the spec presents it as
# ff_shell_wall(): it is pure math and policy with no I/O, and it takes
# the UTC offset as plain values rather than an fp_pack_t, so core keeps
# its zero-dependency, no-festpack posture. Full rationale in ff_wall.h.
target_sources(ff-core PRIVATE src/ff_wall.c)

# S09 (core/map — the map-face fixed-fit camera transform;
# docs/specs/S09-map-face.md, slice a)
target_sources(ff-core PRIVATE src/ff_map.c)

if(FF_BUILD_TESTS)
    add_executable(test_map
        tests/test_map.c
    )
    target_link_libraries(test_map PRIVATE ff-core unity)
    ff_apply_warnings(test_map)

    add_test(NAME test_map COMMAND test_map)
endif()

if(FF_BUILD_TESTS)
    add_executable(test_wall
        tests/test_wall.c
    )
    target_link_libraries(test_wall PRIVATE ff-core unity)
    # ff_sched.h is pulled in HEADER-ONLY (its include dir, not the
    # library) so test_wall can static-assert FF_WALL_DAY_START_MIN ==
    # FF_SCHED_FESTIVAL_DAY_START_MIN — the festival-day boundary must
    # not drift between the two. Nothing links ff-festpack here, and
    # ff-core itself deliberately does not (and must not) depend on it:
    # see ff_wall.h's placement note and ff_sched.h's.
    target_include_directories(test_wall PRIVATE
        $<TARGET_PROPERTY:ff-festpack,INTERFACE_INCLUDE_DIRECTORIES>
    )
    ff_apply_warnings(test_wall)

    add_test(NAME test_wall COMMAND test_wall)
endif()

# S23a (core/demofeed — deterministic seeded demo-feed generator;
# docs/specs/S23-demo-feed.md, slice a). Pure C11 event SOURCE only: the live
# demo clock, the CONFIG_FF_DEMO_LIVE switch, the app apply-loop through
# ff_wiring, and the demo festpack string table are slices b/c/d — not here.
target_sources(ff-core PRIVATE src/ff_demofeed.c)

if(FF_BUILD_TESTS)
    add_executable(test_demofeed
        tests/test_demofeed.c
    )
    target_link_libraries(test_demofeed PRIVATE ff-core unity)
    ff_apply_warnings(test_demofeed)

    add_test(NAME test_demofeed COMMAND test_demofeed)
endif()

# S26 slice b / S25 slice b (core/power_fsm — the PWR-button press-timing
# FSM + the reboot BOOT-release guard; docs/specs/S26-device-lifecycle.md
# "(b) Power button -> power menu -> soft power-off"). Pure C11, no I/O:
# the esp32s3 target only samples GPIO6/GPIO0 and drives GPIO7 — the
# press-timing DECISION lives here per the house rule.
target_sources(ff-core PRIVATE src/ff_power_fsm.c)

if(FF_BUILD_TESTS)
    add_executable(test_power_fsm
        tests/test_power_fsm.c
    )
    target_link_libraries(test_power_fsm PRIVATE ff-core unity)
    ff_apply_warnings(test_power_fsm)

    add_test(NAME test_power_fsm COMMAND test_power_fsm)
endif()

# S26 slice c (core/idle — inactivity -> dim -> screen off;
# docs/specs/S26-device-lifecycle.md "(c) Inactivity -> dim -> screen
# off"). Pure C11, no I/O: the esp32s3 target (and the sim's ctl loop,
# for its own AC3 test) only enact the DIM/OFF backlight + rebuild-skip
# decision this FSM makes — see ff_idle.h.
target_sources(ff-core PRIVATE src/ff_idle.c)

if(FF_BUILD_TESTS)
    add_executable(test_idle
        tests/test_idle.c
    )
    target_link_libraries(test_idle PRIVATE ff-core unity)
    ff_apply_warnings(test_idle)

    add_test(NAME test_idle COMMAND test_idle)
endif()

# S26 slice e (core/button — a small, generic debounced push-button, used
# for the BOOT-as-home-button debounce; docs/specs/S26-device-lifecycle.md
# "(e) Home button + launcher"). Pure C11, no I/O: the esp32s3 target only
# samples GPIO0 (ff_power_boot_pressed, already exists for the reboot
# guard) and forwards the debounced press as FF_INTENT_HOME.
target_sources(ff-core PRIVATE src/ff_button.c)

if(FF_BUILD_TESTS)
    add_executable(test_button
        tests/test_button.c
    )
    target_link_libraries(test_button PRIVATE ff-core unity)
    ff_apply_warnings(test_button)

    add_test(NAME test_button COMMAND test_button)
endif()

# S26 slice d (core/notify — the notification queue: fixed-depth FIFO,
# coalescing, expiry; docs/specs/S26-device-lifecycle.md "(d) ff_notify +
# message banner"). Pure C11, no I/O — the shell wires events -> enqueue;
# `scr_banner.c` only renders.
target_sources(ff-core PRIVATE src/ff_notify.c)

if(FF_BUILD_TESTS)
    add_executable(test_notify
        tests/test_notify.c
    )
    target_link_libraries(test_notify PRIVATE ff-core unity)
    ff_apply_warnings(test_notify)

    add_test(NAME test_notify COMMAND test_notify)
endif()
