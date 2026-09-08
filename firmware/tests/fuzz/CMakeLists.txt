# tests/fuzz — fuzz harnesses for the wire-facing/untrusted-input parsers
# (S14 hardening pass, docs/specs/S14-testing-ci.md: "sim/unit builds
# compile with ASan+UBSan in CI; fuzz smokes ... run 10k iters in CI").
# See this directory's README.md for the three-driver design.
#
# Three executables per target, all built from the SAME fuzz_<name>.c
# harness (which supplies only LLVMFuzzerTestOneInput, no main):
#
#   fuzz_<name>_smoke     — always built (any compiler), always in ctest.
#                            Fixed-seed bounded random driver; this is
#                            what keeps the target alive in ordinary CI.
#   fuzz_<name>_campaign   — opt-in (FF_BUILD_FUZZERS=ON). Coverage-guided
#                            mutational driver (-fsanitize-coverage=
#                            trace-pc-guard, no external libFuzzer archive
#                            needed — see fuzz_campaign_main.c's header for
#                            why: this build machine's Apple Clang has no
#                            bundled libclang_rt.fuzzer_osx.a). THIS is the
#                            tool this hardening pass's real >=10-minute
#                            sessions used.
#   fuzz_<name>            — opt-in (FF_BUILD_FUZZERS=ON AND
#                            FF_FUZZ_USE_LIBFUZZER=ON), real libFuzzer.
#                            Only builds where the toolchain's compiler-rt
#                            fuzzer archive actually exists; left available
#                            for a dev machine that has one, but this
#                            hardening pass did not rely on it.
#
# None of the opt-in targets are required for the clang-vs-gcc-14
# zero-warnings gate or for ctest.
option(FF_BUILD_FUZZERS "Build the coverage-guided fuzz campaign executables" OFF)
option(FF_FUZZ_USE_LIBFUZZER "Also build real libFuzzer executables (needs compiler-rt's fuzzer archive)" OFF)

set(FF_FUZZ_TARGETS
    mc_framing
    mc_fromradio
    ff_proto
    festpack
    t9pred
)

# libname per target — everything but mc_framing/mc_fromradio only needs
# its own module's static library; the meshclient ones need the nanopb
# proto lib too (mirrors meshclient/CMakeLists.txt's own test_meshclient).
set(_ff_fuzz_libs_mc_framing ff-meshclient ff-meshclient-proto)
set(_ff_fuzz_libs_mc_fromradio ff-meshclient ff-meshclient-proto ff-platform)
set(_ff_fuzz_libs_ff_proto ff-core)
set(_ff_fuzz_libs_festpack ff-festpack)
set(_ff_fuzz_libs_t9pred ff-core)

# Per-target smoke iteration override. t9pred's per-iteration cost is
# dominated by the O(dictionary size ~3.4k words) linear scans inside
# ff_t9pred_count_ex()/nth_match_ex() — the harness's session-op loop
# (tests/fuzz/fuzz_t9pred.c) calls those up to 32x per iteration, so the
# smoke driver's usual 20000-iteration default (fine for every other,
# O(1)-ish target here) took ~60s under ASan — legal under the 120s
# default ctest TIMEOUT but an uncomfortably thin margin on a slower CI
# runner. 4000 iterations still exercises the same code paths (fixed
# edge cases run regardless of this count) in a few seconds.
set(_ff_fuzz_iters_t9pred 4000)

foreach(_ff_fuzz_name IN LISTS FF_FUZZ_TARGETS)
    set(_ff_fuzz_src ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_${_ff_fuzz_name}.c)
    set(_ff_fuzz_libs ${_ff_fuzz_libs_${_ff_fuzz_name}})

    # --- Coverage-guided campaign runner (opt-in) ------------------------
    #
    # Deliberately NO target_compile_options()/target_link_options() here
    # for the sanitizer/coverage flags: those would only instrument
    # fuzz_campaign_main.c and fuzz_${_ff_fuzz_name}.c themselves (the two
    # sources THIS add_executable() compiles) and leave the statically
    # linked library under test (ff-core/ff-meshclient/ff-festpack/...)
    # uninstrumented — coverage feedback would then only ever see the thin
    # harness wrapper, never the actual parser code being fuzzed, which
    # defeats the entire point of a coverage-guided mutator. Instead this
    # follows the SAME pattern the top-level `sanitizers` CI job already
    # uses for ASan/UBSan (see ci.yml / README.md): the whole build-fuzz
    # tree must be *configured* with global
    # -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fsanitize-coverage=trace-pc-guard ..."
    # so every library in the link (including FetchContent'd deps, though
    # only ff-core/ff-meshclient(-proto)/ff-festpack/ff-platform actually
    # end up linked into these particular executables) is instrumented
    # identically. See this directory's README.md for the exact recipe.
    if(FF_BUILD_FUZZERS)
        add_executable(fuzz_${_ff_fuzz_name}_campaign
            ${_ff_fuzz_src}
            fuzz_campaign_main.c
        )
        target_link_libraries(fuzz_${_ff_fuzz_name}_campaign PRIVATE ${_ff_fuzz_libs})
        # Deliberately NOT ff_apply_warnings()'d with -Werror: a fuzz
        # session is a local diagnostic tool, not a build gate.
    endif()

    # --- Real libFuzzer target (opt-in, needs the compiler-rt archive) --
    #
    # Same instrumentation caveat as the campaign runner above: for
    # coverage feedback to see the library under test (not just this
    # harness file), configure this tree's CMAKE_C_FLAGS with
    # `-fsanitize=fuzzer-no-link,address,undefined` globally (so
    # ff-core/ff-meshclient/ff-festpack get instrumented too) and add only
    # the `fuzzer` component (which pulls in libFuzzer's own main) here,
    # at the final executable link. Untested in this hardening pass — see
    # README.md for why (no libFuzzer archive on this build machine) —
    # left available for a toolchain that has one.
    if(FF_BUILD_FUZZERS AND FF_FUZZ_USE_LIBFUZZER)
        add_executable(fuzz_${_ff_fuzz_name} ${_ff_fuzz_src})
        target_link_libraries(fuzz_${_ff_fuzz_name} PRIVATE ${_ff_fuzz_libs})
        target_compile_options(fuzz_${_ff_fuzz_name} PRIVATE
            -fsanitize=fuzzer -fno-omit-frame-pointer -g -O1
        )
        target_link_options(fuzz_${_ff_fuzz_name} PRIVATE
            -fsanitize=fuzzer
        )
    endif()

    # --- Deterministic smoke target (always built, always in ctest) -----
    if(FF_BUILD_TESTS)
        add_executable(fuzz_${_ff_fuzz_name}_smoke
            ${_ff_fuzz_src}
            fuzz_smoke_main.c
        )
        target_link_libraries(fuzz_${_ff_fuzz_name}_smoke PRIVATE ${_ff_fuzz_libs})
        if(DEFINED _ff_fuzz_iters_${_ff_fuzz_name})
            target_compile_definitions(fuzz_${_ff_fuzz_name}_smoke PRIVATE
                FF_FUZZ_SMOKE_DEFAULT_ITERS=${_ff_fuzz_iters_${_ff_fuzz_name}}
            )
        endif()
        ff_apply_warnings(fuzz_${_ff_fuzz_name}_smoke)

        add_test(NAME fuzz_${_ff_fuzz_name}_smoke COMMAND fuzz_${_ff_fuzz_name}_smoke)
    endif()
endforeach()
