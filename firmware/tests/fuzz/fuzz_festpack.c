/**
 * fuzz_festpack.c — fuzzes fp_parse(), the festpack.json (fest-almanac
 * schema v0.1) loader. This is untrusted-input-adjacent even though a
 * festpack usually arrives over USB/SD rather than RF (S05): a corrupt or
 * hostile pack (bad SD card, mismatched schema version, a deliberately
 * poisoned file dropped at a festival) must fail cleanly, never crash or
 * read out of bounds — fp_skip_depth()'s recursion cap exists for exactly
 * this reason (see its doc comment in fp_pack.c).
 *
 * jsmn token scratch is `static` (reused every call, like
 * festpack/tests/test_festpack.c's own s_toks — fp_parse() fully
 * re-tokenizes it each call, nothing carries over) so this harness never
 * pays FP_MAX_TOKENS * sizeof(jsmntok_t) as a per-iteration stack frame.
 */
#include <stdint.h>
#include <stddef.h>

#include "fp_pack.h"

static jsmntok_t s_toks[FP_MAX_TOKENS];
static fp_pack_t s_out;

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    if (size == 0) {
        return 0; /* fp_parse() itself rejects len==0 as FP_ERR_JSON; nothing to gain re-proving that every call */
    }

    fp_result_t r = fp_parse((char const *)data, size, &s_out, s_toks, FP_MAX_TOKENS);

    if (r == FP_OK) {
        /* A handful of the struct's own documented invariants — cheap to
         * check here and exactly the kind of "parsed OK but produced a
         * struct that violates its own contract" bug pure crash-hunting
         * would miss. */
        if (s_out.n_stages > FP_MAX_STAGES || s_out.n_sets > FP_MAX_SETS ||
            s_out.n_features > FP_MAX_FEATURES || s_out.n_landmarks > FP_MAX_LANDMARKS) {
            __builtin_trap();
        }
        for (uint8_t i = 0; i < s_out.n_sets; i++) {
            if (s_out.sets[i].stage_idx >= (int8_t)s_out.n_stages && s_out.sets[i].stage_idx != -1) {
                __builtin_trap();
            }
        }
        for (uint8_t i = 0; i < s_out.n_features; i++) {
            if (s_out.features[i].n_pts > FP_MAX_POLY_PTS) {
                __builtin_trap();
            }
        }
    }

    return 0;
}
