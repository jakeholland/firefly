/**
 * ff_sched.c — see ff_sched.h for the full contract (module placement
 * rationale, festival-day/now_min semantics, midnight-crossing rule,
 * half-open "now" window — see docs/specs/S07-now-face.md's
 * ## Amendments for the ruling that changed it from inclusive-both-ends
 * to half-open, PR #9).
 */
#include "ff_sched.h"

#include <string.h>

/* Effective end-of-set minute, folding a midnight-crossing set (end_min
 * < start_min) forward by one day so all comparisons against `now_min`
 * (which may itself exceed 1440 for late-night queries) are plain
 * integer comparisons.
 *
 * 2026-09-11: fp_parse() now performs this same fold at parse time
 * (fp_pack.c's fp_parse_set_daytime, for a published end with no
 * `end_day` — see fp_pack.h's fp_set_t.end_min doc comment), so a
 * pack that came from fp_parse() never actually has end_min <
 * start_min here any more. This check is kept as defense in depth for
 * an fp_set_t assembled some other way (this file's own unit tests
 * build one directly with mk_set(), deliberately passing an unfolded
 * end_min, to exercise this exact fold without going through the
 * parser) — not because a parsed pack still needs it. */
static int16_t sched_effective_end(fp_set_t const *s)
{
    if (s->end_min < s->start_min) {
        return (int16_t)(s->end_min + FF_SCHED_DAY_SPAN_MIN);
    }
    return s->end_min;
}

/* 2026-08-24 amendment: "timed" means a known start_min, full stop —
 * end_min is optional and, when null, DERIVED (see sched_derive_end
 * below). See ff_sched.h's "Timed means a known start_min" section for
 * the end-to-end finding (Bass Canyon 2026: 82 real start times, every
 * end_min null) that prompted this. */
static bool sched_has_start(fp_set_t const *s)
{
    return s->start_min >= 0;
}

/* sched_next_stage_start — the smallest known start_min, strictly
 * greater than `s->start_min`, among sets sharing `s`'s stage_idx AND
 * day_doy. Returns -1 if none exists (either `s` is the last known-
 * start set on its stage this day, or no other set on this stage/day
 * has a known start at all).
 *
 * Deliberately a full scan, NOT a `p->sets[]` array-order lookup — see
 * ff_sched.h's "Timed means a known start_min" section: real
 * festpack.json schedules are not guaranteed sorted (the vendored Bass
 * Canyon pack lists each stage's sets headliner-first, i.e. DESCENDING
 * by start_min), so "the next element in the array" is not "the next
 * set chronologically". A tie (two different sets sharing the same
 * start_min, a data anomaly but not one this module should crash or
 * misbehave on) resolves to that shared start_min either way — which
 * set index supplied it doesn't change the derived effective_end.
 *
 * RESOLVED (2026-09-09, S05-festpack.md amendment — was "KNOWN GAP", PR
 * #65 review finding 3): this function compares two different sets' raw
 * start_min values directly, with no per-comparison midnight fold —
 * that was flagged as a gap for a standalone (both start AND end after
 * actual local midnight) set with a numerically SMALLER raw start_min
 * than an earlier evening entry on the same stage/day_doy, which would
 * have sorted BEFORE the evening entry instead of after it.
 *
 * We now have the visibility the old comment asked for: the real Lost
 * Lands 2026 set-time grid is exactly this shape (Wompy Woods Friday
 * night runs 14:30 through a 00:15/01:05/02:00 post-midnight tail).
 * The fix lives one layer down, at parse time: fest-almanac gives every
 * schedule entry an optional `night` (the festival night it is billed
 * under) alongside its plain calendar `day`, and fp_pack.c's
 * fp_parse_set_daytime() folds the two into a single festival-night
 * day_doy with start_min measured from THAT night's midnight — so the
 * post-midnight tail arrives here already at start_min >= 1440, in the
 * same number space as that night's evening sets. See fp_pack.h's
 * fp_set_t doc comment.
 *
 * Given that encoding, this function's plain `o->start_min <=
 * s->start_min` / `o->start_min < best` comparisons need NO change:
 * 00:15 (folded to 1455) correctly compares greater than 23:00 (1380)
 * on the same day_doy. The gap only remains live for a pack that files
 * a post-midnight set under the NEXT calendar day's day_doy at a small
 * raw start_min — the option S05's amendment names and rejects
 * precisely because it composes with neither this comparison nor
 * ff_wall.h's wall-clock resolution, and which fp_parse_set_daytime's
 * fallback fold (start < 06:00 -> previous day's night) also avoids for
 * a pack that predates `night`. See test_sched.c's
 * S07_real_lost_lands_* cases and test_festpack.c's
 * S05_AC1_lost_lands_after_midnight_* / S05_AC1_night_fold_* cases for
 * the regression coverage. */
static int16_t sched_next_stage_start(fp_pack_t const *p, fp_set_t const *s)
{
    int16_t best = -1;
    for (uint16_t i = 0; i < p->n_sets; i++) {
        fp_set_t const *o = &p->sets[i];
        if (o == s) continue;
        if (o->day_doy != s->day_doy) continue;
        if (o->stage_idx != s->stage_idx) continue;
        if (!sched_has_start(o)) continue;
        if (o->start_min <= s->start_min) continue;
        if (best < 0 || o->start_min < best) best = o->start_min;
    }
    return best;
}

/* sched_derive_end — this set's effective end and whether that end is
 * real (vs. a day-window fallback with genuinely unknown duration). See
 * ff_sched.h's "Timed means a known start_min" section and
 * ff_now_row_t.pct_valid's doc comment for the full contract. Caller
 * must already know `s` has a known start_min (sched_has_start). */
static void sched_derive_end(fp_pack_t const *p, fp_set_t const *s, int16_t *out_end, bool *out_pct_valid)
{
    if (s->end_min >= 0) {
        *out_end = sched_effective_end(s);
        *out_pct_valid = true;
        return;
    }

    int16_t next_start = sched_next_stage_start(p, s);
    if (next_start >= 0) {
        /* Published set-time-grid semantics: your set ends when the
         * next one on your stage begins. next_start is another set's
         * start_min, already measured from the SAME festival night's
         * midnight as `s->start_min` (fp_pack.c folds an after-midnight
         * set to >= 1440 at parse time — see fp_pack.h), so no
         * midnight-crossing fold is needed here; that fold only applies
         * to a single set's OWN end_min < its OWN start_min, which is
         * the other, older way a pack can spell "ends after midnight". */
        *out_end = next_start;
        *out_pct_valid = true;
        return;
    }

    /* Last known-start set on this stage this day: live once started,
     * but the true end is unknowable. Do NOT invent a duration — cap at
     * the festival day window's own end, the one boundary this module
     * actually knows, and flag pct as not meaningful. */
    *out_end = (int16_t)(FF_SCHED_FESTIVAL_DAY_START_MIN + FF_SCHED_DAY_SPAN_MIN);
    *out_pct_valid = false;
}

/* ff_sched_alarm_t.fired bit helpers. Declared up top (rather than just
 * above ff_sched_alarm_tick) because ff_sched_toggle_star also needs
 * sched_bit_clear. */
static bool sched_bit_get(uint8_t const *bits, uint16_t idx)
{
    return (bits[idx / 8] >> (idx % 8)) & 1u;
}

static void sched_bit_set(uint8_t *bits, uint16_t idx)
{
    bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
}

static void sched_bit_clear(uint8_t *bits, uint16_t idx)
{
    bits[idx / 8] &= (uint8_t)~(1u << (idx % 8));
}

uint8_t ff_sched_now_playing(fp_pack_t const *p, uint16_t day_doy, int16_t now_min,
                              ff_now_row_t out[], uint8_t max)
{
    uint8_t n = 0;
    for (uint16_t i = 0; i < p->n_sets && n < max; i++) {
        fp_set_t const *s = &p->sets[i];
        if (s->day_doy != day_doy) continue;
        if (!sched_has_start(s)) continue;

        /* 2026-08-25 review fixup (PR #65 finding 1): two sets sharing
         * the exact same (stage_idx, day_doy, start_min) — malformed or
         * duplicate pack data (a copy/paste error, a support-act slot
         * re-announced) — always derive the SAME effective_end from each
         * other's perspective (sched_next_stage_start already excludes a
         * tied sibling as ITS OWN derivation source, so the derived
         * value itself doesn't depend on which one you ask), which means
         * both would otherwise report "now" for the identical window on
         * the SAME stage — violating the one-row-per-stage contract this
         * function's own doc comment promises. Same "ties -> lower set
         * index" rule this file already uses in next_starred/alarm_tick:
         * an earlier-index sibling with the identical tuple wins: this
         * later one is suppressed entirely, not merely deduped in the
         * output. Regression: S07_2026_08_25_duplicate_start_same_stage_dedupes. */
        bool shadowed = false;
        for (uint16_t j = 0; j < i; j++) {
            fp_set_t const *o = &p->sets[j];
            if (o->day_doy == s->day_doy && o->stage_idx == s->stage_idx && o->start_min == s->start_min) {
                shadowed = true;
                break;
            }
        }
        if (shadowed) continue;

        int16_t end;
        bool pct_valid;
        sched_derive_end(p, s, &end, &pct_valid);

        /* Half-open: start_min <= now_min < end. At a zero-gap changeover
         * (set A's effective end == set B's start_min — explicit or
         * derived, doesn't matter which) this is what makes B the sole
         * "now" row on that stage — A's `now_min >= end` excludes it the
         * same minute B's `now_min >= start` includes it. See
         * docs/specs/S07-now-face.md ## Amendments. */
        if (now_min < s->start_min || now_min >= end) continue;

        uint8_t pct = 0; /* placeholder when !pct_valid — see ff_now_row_t */
        if (pct_valid) {
            int16_t dur = (int16_t)(end - s->start_min);
            if (dur <= 0) {
                /* Degenerate zero-length set (start_min == end_min): unreachable
                 * given the half-open range check above (start_min <= now_min <
                 * end_min has no solutions when end_min == start_min), but kept
                 * as a defensive guard against a divide-by-zero if that check
                 * is ever refactored. */
                pct = 0;
            } else {
                /* now_min ranges [start_min, end-1] here (half-open), so this
                 * is always < 100 — the set leaves "now" at end_min itself
                 * rather than lingering for one more tick at pct=100. */
                int32_t scaled = (int32_t)(now_min - s->start_min) * 100;
                int32_t p100 = scaled / dur;
                if (p100 > 99) p100 = 99;   /* defensive clamp; unreachable given the range above */
                if (p100 < 0) p100 = 0;
                pct = (uint8_t)p100;
            }
        }

        out[n].set = s;
        out[n].mins_left = (int16_t)(end - now_min);
        out[n].pct_done = pct;
        out[n].pct_valid = pct_valid;
        n++;
    }
    return n;
}

bool ff_sched_next_starred(fp_pack_t const *p, uint16_t day_doy, int16_t now_min, ff_next_t *out)
{
    fp_set_t const *best = NULL;
    for (uint16_t i = 0; i < p->n_sets; i++) {
        fp_set_t const *s = &p->sets[i];
        if (!s->starred) continue;
        if (s->day_doy != day_doy) continue;
        if (!sched_has_start(s)) continue;
        if (s->start_min <= now_min) continue; /* not strictly future */

        if (best == NULL || s->start_min < best->start_min) {
            best = s;
        }
    }

    if (best == NULL) {
        out->set = NULL;
        out->mins_until = 0;
        return false;
    }

    out->set = best;
    out->mins_until = (int16_t)(best->start_min - now_min);
    return true;
}

void ff_sched_toggle_star(fp_pack_t *p, uint16_t set_idx, ff_sched_alarm_t *alarm)
{
    if (set_idx >= p->n_sets) return;

    bool was_starred = p->sets[set_idx].starred;
    p->sets[set_idx].starred = !was_starred;

    /* Un-starring forgets any prior alarm firing for this set, so a later
     * re-star re-arms the T-15 alert immediately (fat-finger recovery) —
     * see ff_sched.h and docs/specs/S07-now-face.md ## Amendments. Only
     * relevant on the starred->unstarred transition; starring a
     * previously-unstarred set has no fired-bit to clear (ff_sched_alarm_tick
     * already self-clears unstarred sets' bits as a second line of
     * defense for callers that pass alarm=NULL here). */
    if (was_starred && alarm != NULL) {
        sched_bit_clear(alarm->fired, set_idx);
    }
}

uint16_t ff_sched_day_sets(fp_pack_t const *p, uint16_t day_doy,
                            fp_set_t const *out[], uint16_t max)
{
    uint16_t n = 0;
    for (uint16_t i = 0; i < p->n_sets && n < max; i++) {
        if (p->sets[i].day_doy != day_doy) continue;
        out[n++] = &p->sets[i];
    }
    return n;
}

bool ff_sched_day_tbd(fp_pack_t const *p, uint16_t day_doy)
{
    bool any = false;
    for (uint16_t i = 0; i < p->n_sets; i++) {
        if (p->sets[i].day_doy != day_doy) continue;
        any = true;
        if (sched_has_start(&p->sets[i])) {
            return false;
        }
    }
    return any;
}

void ff_sched_alarm_init(ff_sched_alarm_t *st)
{
    memset(st, 0, sizeof(*st));
}

fp_set_t const *ff_sched_alarm_tick(ff_sched_alarm_t *st, fp_pack_t const *p,
                                     uint16_t day_doy, int16_t now_min)
{
    uint16_t best_idx = UINT16_MAX;

    for (uint16_t i = 0; i < p->n_sets; i++) {
        fp_set_t const *s = &p->sets[i];
        if (!s->starred) {
            /* Unstarred sets carry no fired memory: clearing here (in
             * addition to ff_sched_toggle_star's immediate clear when it's
             * given the alarm state) means a future re-star always
             * re-arms the T-15 alert even for callers that pass alarm=NULL
             * to ff_sched_toggle_star and only ever touch this state
             * through ticks. See ff_sched.h. */
            sched_bit_clear(st->fired, i);
            continue;
        }
        if (s->day_doy != day_doy) continue;
        if (!sched_has_start(s)) continue;
        if (sched_bit_get(st->fired, i)) continue;

        int16_t mins_until = (int16_t)(s->start_min - now_min);
        if (mins_until > 15) continue; /* not due yet */

        int16_t end;
        bool pct_valid_unused;
        sched_derive_end(p, s, &end, &pct_valid_unused);
        if (now_min > end) continue; /* already over; don't fire a stale alert */

        if (best_idx == UINT16_MAX || s->start_min < p->sets[best_idx].start_min) {
            best_idx = i;
        }
    }

    if (best_idx == UINT16_MAX) return NULL;

    sched_bit_set(st->fired, best_idx);
    return &p->sets[best_idx];
}
