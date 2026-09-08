# Fuzz harnesses (S14 hardening pass)

Each `fuzz_*.c` file here exports exactly one function:

```c
int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size);
```

and nothing else (no `main`) — that's the libFuzzer contract, followed
here even though this hardening pass's real fuzzing sessions did not end
up using real libFuzzer (see below). Three different drivers can link
against the same object; `CMakeLists.txt` builds all three the same way
for every target in `FF_FUZZ_TARGETS`.

1. **`fuzz_<name>_smoke` (ctest, always built, any compiler).** Links the
   harness against `fuzz_smoke_main.c`, a tiny portable driver that seeds
   a fixed xorshift32 PRNG and calls `LLVMFuzzerTestOneInput` a bounded
   number of times (default 20000, less for the more expensive `t9pred`
   target — see the CMakeLists.txt comment) with pseudo-random inputs, plus
   a handful of fixed edge cases (empty, all-zero, all-0xFF at a spread of
   lengths). Fully deterministic (fixed seed) and fast — this is what
   keeps the fuzz targets alive in `ctest`/CI per
   docs/specs/S14-testing-ci.md ("fuzz smokes ... run 10k iters in CI")
   without requiring clang or libFuzzer anywhere in the normal build. A
   smoke check, not a substitute for an actual timed fuzzing run.

2. **`fuzz_<name>_campaign` (opt-in, `-DFF_BUILD_FUZZERS=ON`).** THE TOOL
   THIS HARDENING PASS'S REAL >=10-MINUTE SESSIONS USED. Links the harness
   against `fuzz_campaign_main.c`: a small coverage-guided mutational
   fuzzer built on `-fsanitize-coverage=trace-pc-guard` (which Apple
   Clang supports natively, no extra archive) rather than real libFuzzer.
   Reason: this build machine's Apple Clang has no bundled
   `libclang_rt.fuzzer_osx.a` (`-fsanitize=fuzzer` fails to link —
   verified; the archive only exists under unrelated third-party SDKs on
   this machine, e.g. an ESP-IDF/Fuchsia toolchain checkout, not something
   a CI runner or a contributor's plain Xcode install can be expected to
   have). `-fsanitize-coverage=trace-pc-guard` is the actual feedback
   primitive libFuzzer itself is built on, so `fuzz_campaign_main.c`
   implements the same core loop libFuzzer uses (run an input, note which
   coverage edges it newly hit, keep+mutate inputs that found something
   new, discard ones that didn't) at a fraction of the code — see that
   file's header for the honest scope of what it does and doesn't
   replicate (no corpus minimization, no value-profile tracing, no crash
   deduplication). It saves the exact bytes of whatever input was being
   tested to a crash file (raw `write(2)`, durable even across a signal)
   before every single call, so a crash's repro input is always on disk
   afterward.

   ```sh
   cmake -B build-fuzz -DFF_TARGET=sim -DFF_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang \
       -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fsanitize-coverage=trace-pc-guard -fno-omit-frame-pointer -g -O1" \
       -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined -fsanitize-coverage=trace-pc-guard"
   cmake --build build-fuzz --target fuzz_mc_framing_campaign --parallel
   UBSAN_OPTIONS=halt_on_error=1 ./build-fuzz/tests/fuzz/fuzz_mc_framing_campaign \
       600 tests/fuzz/regressions/_scratch_crash.bin
   ```

   The global `CMAKE_C_FLAGS`/`CMAKE_EXE_LINKER_FLAGS` (same pattern the
   top-level `sanitizers` CI job already uses for ASan/UBSan — see
   `../../.github/workflows/ci.yml`) matter: they instrument the STATICALLY
   LINKED LIBRARY under test (`ff-core`/`ff-meshclient`/`ff-festpack`/...),
   not just the two files each `fuzz_<name>_campaign` executable compiles
   directly — without them the mutator's coverage feedback only ever sees
   the thin harness wrapper and never actually explores the real parser
   code. Only ever build the specific `fuzz_*` targets from this tree
   (`--target fuzz_..._campaign`, never a bare `--build build-fuzz
   --parallel`) — the rest of the project (ffsim, the Unity test
   executables) is not meant to be built sanitizer-instrumented from a
   throwaway `-O1` config, and nothing here needs it to be.

   First arg is the session length in seconds (default 600 = 10 minutes,
   this hardening pass's own requirement), second is where a crashing
   input gets saved. On a crash, copy that file into
   `tests/fuzz/regressions/<target>_<short-description>.bin` and write a
   Unity regression test that replays it through the real public API (see
   `core/tests/test_proto.c`'s `S14_FUZZ_*` cases for the pattern) — the
   regression corpus files themselves are not auto-replayed by anything,
   they exist so a human/PR reviewer can see the exact bytes that used to
   crash.

3. **`fuzz_<name>` (opt-in, `-DFF_BUILD_FUZZERS=ON -DFF_FUZZ_USE_LIBFUZZER=ON`),
   real libFuzzer.** Only builds where the toolchain's compiler-rt fuzzer
   archive actually exists. Left available for a dev machine that has a
   full LLVM install (e.g. `brew install llvm` on macOS, or any stock
   Linux clang, both of which normally DO bundle it) — this hardening
   pass's own sessions did not rely on it, for the reason above.

## Running a real 10-minute session per target (this hardening pass)

```sh
cmake -B build-fuzz -DFF_TARGET=sim -DFF_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fsanitize-coverage=trace-pc-guard -fno-omit-frame-pointer -g -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined -fsanitize-coverage=trace-pc-guard"
for t in mc_framing mc_fromradio ff_proto festpack t9pred; do
    cmake --build build-fuzz --target fuzz_${t}_campaign --parallel
    UBSAN_OPTIONS=halt_on_error=1 ./build-fuzz/tests/fuzz/fuzz_${t}_campaign \
        600 tests/fuzz/regressions/_scratch_${t}.bin
done
```

Each target's own `.c` file documents its input encoding (several
harnesses pack more than one "knob" into the fuzzer's single byte-string
input, e.g. `fuzz_mc_fromradio` uses a length-prefixed garbage span before
a real frame header so a run can explore the boot-garbage resync path and
the FromRadio/MeshPacket protobuf decode path from the same corpus).
