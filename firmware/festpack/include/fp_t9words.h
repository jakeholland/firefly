/**
 * fp_t9words.h — festpack -> predictive-T9 supplementary-word bridge.
 *
 * ## Why this lives in festpack/, not core/t9pred
 * The predictive-T9 engine (core/t9pred) is PURE: it must not know what a
 * festpack is. But it accepts a caller-supplied array of extra words to rank
 * above its static dictionary (ff_t9pred_match_ex / ff_t9pred_session_set_extra
 * — see ff_t9pred.h). This bridge is the piece that turns a loaded fp_pack_t
 * into exactly that array: artist / stage / landmark names.
 *
 * It belongs HERE because it needs to see `fp_pack_t` (festpack owns that
 * type). Placing it in core/t9pred would force core to depend on festpack,
 * violating the zero-dependency rule (CLAUDE.md: "All logic goes in core/ ...
 * no I/O"; and core/t9pred is deliberately festpack-free). Placing it in the
 * app would be fine too, but keeping it beside fp_pack_t lets festpack's own
 * tests exercise it and mirrors ff_sched.c's placement rationale (its public
 * surface is fp_pack_t, so it lives in festpack, not core).
 *
 * The composer (app layer) is the intended caller: it will
 *   fp_t9words_collect(pack, words, N);
 *   ff_t9pred_session_set_extra(&sess, words, n);
 * Wiring it into the compose UI is a later slice; this header only makes the
 * data available.
 *
 * Pure C11, no heap, no I/O. The collected pointers ALIAS into `*pack` (the
 * fixed-size name buffers), so the pack must outlive any use of the array by
 * the engine. fp_pack.c NUL-terminates and truncates every name to its buffer
 * (artist[32], stage/landmark name[28]), so the aliased strings are always
 * safe, bounded, C strings.
 */
#ifndef FP_T9WORDS_H
#define FP_T9WORDS_H

#include "fp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * fp_t9words_collect — gather the festival vocabulary from `pack` into `out`.
 *
 * Collects, in this order, every non-empty:
 *   - set artist name   (pack->sets[i].artist)
 *   - stage name        (pack->stages[i].name)
 *   - landmark name     (pack->landmarks[i].name)
 * skipping empties and de-duplicating case-insensitively (first occurrence
 * wins). Writes up to `max` pointers (each aliasing into `*pack`) and returns
 * the number written. Returns 0 when `pack`/`out` is NULL, `max` <= 0, or the
 * pack has no usable names — the caller then supplies no supplement and the
 * engine runs on its static dictionary alone (unchanged behaviour).
 *
 * The array is ready to hand straight to ff_t9pred_match_ex / _set_extra: the
 * engine matches these case-insensitively and stops a multi-word name at its
 * first non-letter, so "Sullivan King" is a candidate for the keys of
 * "sullivan".
 */
int fp_t9words_collect(fp_pack_t const *pack, char const **out, int max);

#ifdef __cplusplus
}
#endif

#endif /* FP_T9WORDS_H */
