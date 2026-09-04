/**
 * ff_sound.c — see ff_sound.h.
 */
#include "ff_sound.h"

#include <stddef.h>

/*
 * The pattern table. One row per `ff_sound_event_t`, in enum order, so
 * `ff_sound_pattern_for` is a plain bounds-checked index — see the
 * `_Static_assert` block below for how the two per-pattern budgets (step
 * count, total duration) are enforced at compile time, and `test_sound.c`
 * for the runtime re-verification the header promises.
 *
 * Note choices (documented here, mirrored in docs/specs/S27-sounds.md):
 *   - FLARE_SENT: C5-E5-G5, a rising major triad — "confirmation", the
 *     classic happy/settled rising shape.
 *   - FLARE_INCOMING: a sharp C6-G5 "chirp", played TWICE with a short
 *     rest between (spec: "repeated twice" — "the loudest thing the puck
 *     makes").
 *   - MESSAGE: one soft blip, E5 — deliberately the shortest, quietest
 *     entry (a single ~80 ms note) so it never competes with the two
 *     flare sounds.
 *   - RALLY: G5-B5, a two-note rising interval — distinct from
 *     FLARE_SENT's three-note triad and MESSAGE's single blip.
 *   - BATT_LOW: E5-C5, a two-note FALLING interval (the descending shape
 *     reads as "going down" — mirrors RALLY's shape inverted, so the two
 *     two-note events are still easy to tell apart by ear).
 *   - TAP: a single very short, non-musical click (not a "note" in the
 *     same sense as the other five — just a tick).
 *   - MULTITAP_TICK (fix/quick-flare-detection, 2026-09-03): a single
 *     very short, non-musical blip at `FF_SOUND_MULTITAP_TICK_HZ`
 *     (ff_sound.h — distinctly higher than TAP's 1200 Hz so the two
 *     utility sounds stay tellable apart by ear), ~40 ms per the spec's
 *     "short progress blip" ask.
 */
static ff_sound_pattern_t const kPatterns[FF_SOUND_COUNT] = {
    [FF_SOUND_FLARE_SENT] = {
        .steps = {
            {FF_SOUND_NOTE_C5, 90},
            {FF_SOUND_NOTE_E5, 90},
            {FF_SOUND_NOTE_G5, 130},
        },
        .n = 3,
    },
    [FF_SOUND_FLARE_INCOMING] = {
        .steps = {
            {FF_SOUND_NOTE_C6, 110},
            {FF_SOUND_NOTE_G5, 110},
            {0, 150}, /* rest between the two repeats */
            {FF_SOUND_NOTE_C6, 110},
            {FF_SOUND_NOTE_G5, 110},
        },
        .n = 5,
    },
    [FF_SOUND_MESSAGE] = {
        .steps = {
            {FF_SOUND_NOTE_E5, 80},
        },
        .n = 1,
    },
    [FF_SOUND_RALLY] = {
        .steps = {
            {FF_SOUND_NOTE_G5, 110},
            {FF_SOUND_NOTE_B5, 150},
        },
        .n = 2,
    },
    [FF_SOUND_BATT_LOW] = {
        .steps = {
            {FF_SOUND_NOTE_E5, 150},
            {FF_SOUND_NOTE_C5, 220},
        },
        .n = 2,
    },
    [FF_SOUND_TAP] = {
        .steps = {
            {1200, 20},
        },
        .n = 1,
    },
    [FF_SOUND_MULTITAP_TICK] = {
        .steps = {
            {FF_SOUND_MULTITAP_TICK_HZ, 40},
        },
        .n = 1,
    },
};

/* Only the vocabulary's SIZE is checked at compile time — an enum
 * constant is a genuine integer constant expression in every C compiler.
 * The per-pattern step-count/duration budgets are NOT (also) enforced
 * via `_Static_assert` here: an EARLIER version of this file tried
 * `_Static_assert(kPatterns[FF_SOUND_FLARE_SENT].n <= ..., ...)`, which
 * compiles under clang (this repo's local gate) but fails GCC (CI's
 * authority, CLAUDE.md's "Build" section) with "expression in static
 * assertion is not constant" — reading an element out of a `static
 * const` array, even at a compile-time-known index, is not an integer
 * constant expression per the C11 standard (6.6p6); clang accepts it as
 * a GNU extension GCC does not extend `_Static_assert` to cover. So the
 * runtime sweep this header already promises
 * (`S27_every_pattern_stays_within_step_and_duration_budget`,
 * test_sound.c) is the ONLY enforcement of both budgets — a real ctest
 * failure on every build, not a weaker guarantee than a compile-time
 * guard would give, just a differently-timed one. */
_Static_assert(FF_SOUND_COUNT == 7, "ff_sound_event_t grew - add a kPatterns row and update test_sound.c's sweep");

ff_sound_pattern_t const *ff_sound_pattern_for(ff_sound_event_t ev)
{
    if ((int)ev < 0 || ev >= FF_SOUND_COUNT) return NULL;
    return &kPatterns[ev];
}

bool ff_sound_should_play(ff_sound_event_t ev, bool sounds_on, bool quiet_now)
{
    if ((int)ev < 0 || ev >= FF_SOUND_COUNT) return false;
    if (!sounds_on) return false;
    if (quiet_now) {
        /* fix/quick-flare-detection (2026-09-03): MULTITAP_TICK joins
         * the two FLARE events in the quiet-hours exemption — see
         * ff_sound.h's "Quiet hours" section for the reasoning
         * (interpretation call, flagged there per AGENTS.md). */
        return ev == FF_SOUND_FLARE_SENT || ev == FF_SOUND_FLARE_INCOMING || ev == FF_SOUND_MULTITAP_TICK;
    }
    return true;
}

int ff_sound_priority(ff_sound_event_t ev)
{
    switch (ev) {
    case FF_SOUND_FLARE_INCOMING:
        return 20;
    case FF_SOUND_FLARE_SENT:
    case FF_SOUND_RALLY:
    case FF_SOUND_BATT_LOW:
        return 10;
    case FF_SOUND_MESSAGE:
    case FF_SOUND_TAP:
    case FF_SOUND_MULTITAP_TICK:
    case FF_SOUND_COUNT:
    default:
        return 0;
    }
}

bool ff_sound_preempts(ff_sound_event_t incoming, ff_sound_event_t playing)
{
    return ff_sound_priority(incoming) >= ff_sound_priority(playing);
}
