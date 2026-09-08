/**
 * fuzz_campaign_main.c — deterministic-by-seed, coverage-guided mutational
 * fuzz driver, linked against the same fuzz_*.c harnesses (which only
 * supply LLVMFuzzerTestOneInput, see this directory's README.md) to make
 * a `fuzz_<name>_campaign` executable for real, timed fuzzing sessions.
 *
 * NOT wired into ctest (see CMakeLists.txt — only built when
 * FF_BUILD_FUZZERS=ON, alongside the real libFuzzer target when clang's
 * compiler-rt fuzzer archive is available). This exists because this
 * hardening pass's build machine has Apple Clang without the bundled
 * `libclang_rt.fuzzer_osx.a` archive (verified absent from the active
 * Xcode toolchain; only present under unrelated third-party SDKs on this
 * machine, not something CI can depend on) — real libFuzzer could not
 * link. Apple Clang DOES support `-fsanitize-coverage=trace-pc-guard`
 * (verified locally), which is the actual feedback primitive libFuzzer
 * itself is built on, so this is a small, honest from-not-around-the-
 * documented-alternative: "a libFuzzer or a deterministic random
 * harness" per this task's own instructions. It is deliberately NOT a
 * from-scratch reimplementation of libFuzzer's corpus minimization,
 * value-profile tracing, or crash deduplication — just: track which
 * edges (SanitizerCoverage guards) a run newly hits, keep the input that
 * hit them, mutate kept inputs to find more. That is enough to make the
 * campaign meaningfully better than pure fresh-random-every-time (this
 * directory's `_smoke` driver), which is the property that matters for
 * the "run each for at least 10 minutes" requirement.
 *
 * Crash capture: the current input is written (raw `write(2)`, not
 * buffered stdio, so it is durable even if the process dies via signal
 * immediately afterward) to `<crash_path>` before every single call to
 * LLVMFuzzerTestOneInput. On a crash, that file holds the exact input —
 * copy it into tests/fuzz/regressions/ and write a Unity regression test
 * that replays it through the real public API (see README.md).
 *
 * Usage: fuzz_<name>_campaign <seconds> <crash_path>
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size);

/* -------------------------------------------------------------------- */
/* Minimal SanitizerCoverage feedback (see LLVM's trace-pc-guard docs for
 * this exact init-assigns-sequential-ids pattern). MAX_EDGES comfortably
 * exceeds this whole codebase's total instrumented edge count; an
 * overflow just stops assigning new ids (those callsites report as
 * id 0, which trace_pc_guard() below then ignores) rather than
 * overflowing anything. */
/* -------------------------------------------------------------------- */
#define MAX_EDGES 400000u

static uint8_t g_ever_hit[MAX_EDGES];
static uint8_t g_run_hit[MAX_EDGES];
static uint32_t g_next_id = 1; /* 0 is reserved: "this callsite has no id yet / overflowed" */

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop)
{
    if (start == stop) {
        return;
    }
    for (uint32_t *p = start; p < stop; p++) {
        if (*p == 0) {
            *p = (g_next_id < MAX_EDGES) ? g_next_id++ : 0u;
        }
    }
}

void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{
    uint32_t id = *guard;
    if (id > 0 && id < MAX_EDGES) {
        g_run_hit[id] = 1u;
    }
}

/* -------------------------------------------------------------------- */
/* Corpus + mutation                                                     */
/* -------------------------------------------------------------------- */
#define MAX_INPUT 4096u
#define MAX_CORPUS 4000u

typedef struct {
    uint8_t data[MAX_INPUT];
    size_t len;
} entry_t;

static entry_t *g_corpus; /* heap-allocated: MAX_CORPUS*sizeof(entry_t) is a few MB */
static size_t g_corpus_n = 0;

static uint32_t rng_state;

static uint32_t xnext(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void corpus_add(uint8_t const *data, size_t len)
{
    size_t idx;
    if (g_corpus_n < MAX_CORPUS) {
        idx = g_corpus_n++;
    } else {
        idx = xnext() % MAX_CORPUS; /* bounded memory: evict a random existing entry */
    }
    memcpy(g_corpus[idx].data, data, len);
    g_corpus[idx].len = len;
}

/* A handful of protocol-shaped byte hints so mutation can splice in a
 * plausible magic/punctuation byte instead of only ever drawing uniform
 * random bytes — this is the one piece of target-specific help in an
 * otherwise fully generic mutator, and it's cheap: worst case (wrong
 * target) it's just another random byte value. */
static const uint8_t kHints[] = {
    0x94, 0xC3, 0x00, 0x01, /* mc_framing magic + short lengths */
    '{', '}', '"', ':', ',', '[', ']', /* JSON */
    '2', '3', '4', '5', '6', '7', '8', '9', /* T9 digits */
};

static size_t mutate(uint8_t *buf, size_t len)
{
    uint32_t strat = xnext() % 6u;
    switch (strat) {
    case 0: /* bit flip */
        if (len > 0) {
            buf[xnext() % len] ^= (uint8_t)(1u << (xnext() % 8u));
        }
        break;
    case 1: /* byte overwrite */
        if (len > 0) {
            buf[xnext() % len] = (uint8_t)xnext();
        }
        break;
    case 2: /* insert one random byte */
        if (len < MAX_INPUT) {
            size_t pos = len ? (xnext() % len) : 0;
            memmove(buf + pos + 1, buf + pos, len - pos);
            buf[pos] = (uint8_t)xnext();
            len++;
        }
        break;
    case 3: /* delete one byte */
        if (len > 0) {
            size_t pos = xnext() % len;
            memmove(buf + pos, buf + pos + 1, len - pos - 1);
            len--;
        }
        break;
    case 4: /* overwrite a short run with a repeated byte (bit-boundary/length-field stress) */
        if (len > 0) {
            size_t pos = xnext() % len;
            size_t n = 1 + (xnext() % 8u);
            uint8_t v = (uint8_t)xnext();
            for (size_t i = 0; i < n && pos + i < len; i++) {
                buf[pos + i] = v;
            }
        }
        break;
    default: /* splice in a protocol-shaped hint byte */
        if (len < MAX_INPUT) {
            size_t pos = len ? (xnext() % (len + 1u)) : 0;
            uint8_t v = kHints[xnext() % (sizeof(kHints) / sizeof(kHints[0]))];
            if (pos == len) {
                buf[pos] = v;
                len++;
            } else {
                buf[pos] = v;
            }
        }
        break;
    }
    return len;
}

/* -------------------------------------------------------------------- */
/* Crash capture                                                         */
/* -------------------------------------------------------------------- */
static int g_crash_fd = -1;

static void save_current_input(uint8_t const *data, size_t len)
{
    if (g_crash_fd < 0) {
        return;
    }
    if (lseek(g_crash_fd, 0, SEEK_SET) != 0) {
        return;
    }
    if (ftruncate(g_crash_fd, 0) != 0) {
        return;
    }
    ssize_t n = write(g_crash_fd, data, len);
    (void)n; /* best-effort: a failed save just means a crash loses repro bytes, not a functional bug */
}

int main(int argc, char **argv)
{
    long seconds = 600; /* 10 minutes, this hardening pass's own requirement */
    char const *crash_path = "fuzz_campaign_crash.bin";

    if (argc > 1) {
        long v = strtol(argv[1], NULL, 10);
        if (v > 0) {
            seconds = v;
        }
    }
    if (argc > 2) {
        crash_path = argv[2];
    }

    g_crash_fd = open(crash_path, O_CREAT | O_RDWR, 0644);
    if (g_crash_fd < 0) {
        fprintf(stderr, "fuzz_campaign: warning: could not open crash_path '%s' (%s) — "
                         "a crash during this run will not save its input.\n",
                crash_path, strerror(errno));
    }

    g_corpus = (entry_t *)calloc(MAX_CORPUS, sizeof(entry_t));
    if (!g_corpus) {
        fprintf(stderr, "fuzz_campaign: calloc failed\n");
        return 1;
    }

    rng_state = 0xC0FFEE01u;

    /* Seed the corpus with a few generic starting points; mutation
     * (including the hint-splice strategy above) grows real structure
     * from here. */
    static uint8_t const seeds[][8] = {
        {0}, /* empty handled separately (len 0) */
        {0x00}, {0xFF}, {0x94, 0xC3, 0x00, 0x00}, {'{', '}'},
    };
    corpus_add(seeds[0], 0);
    for (size_t i = 1; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        size_t slen = (i == 3) ? 4 : (i == 4) ? 2 : 1;
        corpus_add(seeds[i], slen);
    }

    time_t start = time(NULL);
    uint64_t iters = 0, new_coverage_finds = 0;

    uint8_t work[MAX_INPUT];

    for (;;) {
        if ((iters & 0xFFu) == 0u) { /* check wall clock every 256 iters, not every iter */
            if (time(NULL) - start >= seconds) {
                break;
            }
        }
        iters++;

        entry_t *base = &g_corpus[xnext() % g_corpus_n];
        size_t len = base->len;
        memcpy(work, base->data, len);

        uint32_t n_mut = 1u + (xnext() % 3u);
        for (uint32_t m = 0; m < n_mut; m++) {
            len = mutate(work, len);
        }

        memset(g_run_hit, 0, g_next_id);
        save_current_input(work, len);
        (void)LLVMFuzzerTestOneInput(work, len);

        bool found_new = false;
        for (uint32_t e = 1; e < g_next_id; e++) {
            if (g_run_hit[e] && !g_ever_hit[e]) {
                g_ever_hit[e] = 1u;
                found_new = true;
            }
        }
        if (found_new) {
            corpus_add(work, len);
            new_coverage_finds++;
        }
    }

    uint32_t edges_hit = 0;
    for (uint32_t e = 1; e < g_next_id; e++) {
        edges_hit += g_ever_hit[e];
    }

    printf("fuzz_campaign: %llu iterations in %lds, corpus grew to %zu entries "
           "(%llu new-coverage finds), %u/%u edges covered\n",
           (unsigned long long)iters, (long)(time(NULL) - start), g_corpus_n,
           (unsigned long long)new_coverage_finds, edges_hit, g_next_id - 1);

    if (g_crash_fd >= 0) {
        close(g_crash_fd);
        unlink(crash_path); /* clean exit: nothing to repro, don't leave a stale empty file */
    }

    return 0;
}
