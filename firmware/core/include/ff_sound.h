/**
 * ff_sound.h — core/sound: the event vocabulary, tone patterns, quiet-hours
 * policy and priority table for S27 sounds (docs/specs/S27-sounds.md).
 *
 * This header is the CORE half of S27: pure C11, zero I/O, zero clock —
 * exactly `ff_flare_mark.h`'s split ("the shape lives in core, the pixels
 * happen elsewhere"), applied to audio instead of graphics. A pattern here
 * is DATA (a short list of `{freq_hz, ms}` steps); nothing in this file
 * ever makes a sound. The DEVICE HAL that turns a `ff_sound_pattern_t` into
 * PCM samples on the Waveshare board's PCM5101 I2S DAC is a separate,
 * stacked PR (this PR is core + shell + settings + sim only — see
 * `docs/specs/S27-sounds.md`'s "Out of scope").
 *
 * ## The three questions this header answers
 *  1. **What sound goes with an event?** `ff_sound_pattern_for(ev)` — a
 *     fixed table of short, hand-picked musical patterns (see the spec for
 *     the note choices and why).
 *  2. **Should it play at all, right now?** `ff_sound_should_play(ev,
 *     sounds_on, quiet_now)` — the master-switch + quiet-hours policy.
 *  3. **If two events collide, which wins?** `ff_sound_priority(ev)` /
 *     `ff_sound_preempts(incoming, playing)` — the small priority table a
 *     HAL with exactly one speaker needs to arbitrate.
 *
 * ## Quiet hours: FLARE_* (and now MULTITAP_TICK) are the exemptions
 * (interpretation calls)
 * `ff_quiet_now` normally silences everything (haptics, banners). This
 * spec makes FLARE_INCOMING and FLARE_SENT the exception — "safety beats
 * quiet" — for the same reason `ff_shell.h`'s "HAPTICS AND QUIET HOURS"
 * section overrides quiet hours for `ff_flare_result_t.should_alert`
 * unconditionally: a flare is "come find me" or "I am flaring", and a
 * swallowed one at 3 a.m. is the one alert that must always land. FLARE_SENT
 * is included alongside FLARE_INCOMING (not just the receive side) because
 * the sender needs the same confirmation that their own flare actually
 * went out regardless of the hour — silence here could read as "did that
 * work?" at the exact moment the answer matters most. Flagged per
 * AGENTS.md as an interpretation call — see the spec's own "Questions" for
 * the counter-argument (a flare's amber takeover/haptic already wakes
 * everyone; the SOUND on top of that could reasonably stay quiet-gated
 * too) if a future PR wants to revisit it.
 *
 * `FF_SOUND_MULTITAP_TICK` (fix/quick-flare-detection, 2026-09-03) joins
 * the same exemption — a SECOND, independent interpretation call, flagged
 * per AGENTS.md: the quick-flare gesture (5x HOME) is deliberately usable
 * "at 2 AM, screen off" (docs/specs/S10-flare.md's own framing for why the
 * gesture exists at all), and a wearer mid-burst in the dark needs the
 * same progress feedback regardless of quiet hours — a burst that silently
 * drops a tap during quiet hours (no blip to notice by) is exactly the
 * "finicky" failure mode this PR exists to fix, and reintroducing it only
 * during quiet hours would be a worse regression than a short, quiet tick
 * sound breaking the quiet-hours contract for the one gesture that is
 * itself a panic/safety signal. See docs/specs/S10-flare.md's Amendments
 * for the fuller writeup and the counter-argument (a bare tick, unlike
 * FLARE_INCOMING's takeover, has no OTHER quiet-hours-breaking side effect
 * to "already excuse" it).
 *
 * ## Sounds-off is absolute
 * `sounds_on == false` silences every event, `FF_SOUND_FLARE_INCOMING`
 * included — unlike haptics/quiet-hours (where the flare alert overrides
 * the WINDOW but never the user's own master switch, `ff_shell.h`'s
 * `shell_haptic_alert` doc comment), there is no "the user turned sounds
 * off but a flare should sound anyway" case in this spec: a puck that is
 * silenced is silenced. Mirrors `shell_haptic_alert`'s own
 * `settings.haptics` gate, applied here to `settings.sounds_on`.
 *
 * ## TAP is gated by a SECOND setting, outside this function
 * `ff_sound_should_play(FF_SOUND_TAP, sounds_on, quiet_now)` answers only
 * the sounds-on/quiet-hours half of TAP's policy — it does NOT know about
 * `ff_settings_t.ui_ticks` (this function's 3-argument shape, fixed by the
 * spec, has no slot for a fourth setting that applies to exactly one
 * event). The caller ANDs in `ui_ticks` itself: `ff_sound_should_play(
 * FF_SOUND_TAP, sounds_on, quiet_now) && ui_ticks`. `ff_shell_should_tap_
 * sound()` (app/include/ff_shell.h) is the one place that composes this
 * for the whole app — see its own doc comment for why the decision lives
 * there rather than in this header.
 *
 * ## Priority / preemption
 * The device HAL has exactly one speaker: at most one pattern can be
 * playing. `ff_sound_priority(ev)` ranks every event into one of three
 * tiers (see the enum below); `ff_sound_preempts(incoming, playing)` is
 * the yes/no the HAL asks on every new event while something is already
 * playing. The spec's two concrete rules — "FLARE_INCOMING preempts
 * anything" and "a MESSAGE never interrupts a FLARE_*" — are both
 * consequences of the tier assignment, not special-cased: URGENT (only
 * FLARE_INCOMING) preempts every tier including itself (a second flare
 * restarts the alert, which is correct — "newest wins" mirrors
 * `ff_flare_h`'s own "newest wins the takeover" rule); LOW (MESSAGE, TAP)
 * never preempts NORMAL or URGENT.
 */
#ifndef FF_SOUND_H
#define FF_SOUND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The event vocabulary (spec S27). One value per distinct "why did the
 * puck just make a noise" the product defines — screens/shell name the
 * EVENT, never a frequency or duration directly; only this header's
 * pattern table does that.
 */
typedef enum {
    /** Confirmation for the SENDER that a flare went out — any trigger
     *  (the Radar CLOSE-mode FLARE button, the 5x-HOME quick-flare
     *  multitap): "flare start -> FLARE_SENT" is unconditional on the
     *  trigger, per the shell wiring's own doc comment. Short rising
     *  3-note. */
    FF_SOUND_FLARE_SENT = 0,
    /** A paired crew member's flare just took over the screen. Urgent,
     *  repeated twice — the loudest, most demanding thing this puck's
     *  speaker ever plays (spec: "the loudest thing the puck makes").
     *  Preempts anything else playing (see `ff_sound_priority`). */
    FF_SOUND_FLARE_INCOMING,
    /** A new inbox message / banner. One soft blip — deliberately the
     *  quietest, shortest pattern in the table so it never competes with
     *  the two flare sounds above. */
    FF_SOUND_MESSAGE,
    /** A rally point was received. Two-note. */
    FF_SOUND_RALLY,
    /** Battery crossed INTO the low band (<= `FF_BATT_LOW_PCT`,
     *  `ff_radar.h`). Fires once per crossing, not on every tick the
     *  battery happens to read low — see `ff_shell.c`'s
     *  `shell_batt_low_crossing` for the edge-detector this drives.
     *  Descending two-note (the falling shape reads as "going down"). */
    FF_SOUND_BATT_LOW,
    /** A UI button press "tick" — gated by BOTH `sounds_on` and the
     *  separate `ui_ticks` setting (default OFF: a tick on every single
     *  press is the kind of thing that annoys fast; the maintainer can
     *  flip the default later). See this header's top comment. */
    FF_SOUND_TAP,
    /** fix/quick-flare-detection (2026-09-03): a short progress blip on
     *  presses 2, 3 and 4 of a LIVE quick-flare (5x HOME) run — never on
     *  the 1st press (an ordinary lone HOME tap must stay silent) and
     *  never on the 5th (which already gets `FF_SOUND_FLARE_SENT` once
     *  the send is confirmed on the wire). Gated by `sounds_on` ONLY —
     *  deliberately NOT by `ui_ticks` (this is progress feedback for a
     *  panic gesture, not a decorative click on every button) — see
     *  `ff_shell_multitap_edge`'s doc comment (app/include/ff_shell.h)
     *  for the exact call site, and this header's "Quiet hours" section
     *  for why this event is ALSO exempt from quiet hours, alongside the
     *  two FLARE events. */
    FF_SOUND_MULTITAP_TICK,
    FF_SOUND_COUNT, /* not a real event; the vocabulary's size, for range checks/tests */
} ff_sound_event_t;

/**
 * One note (or rest) in a pattern. `freq_hz == 0` is a REST of `ms`
 * milliseconds of silence (spec: "freq 0 = rest") — every pattern below
 * that repeats a phrase uses a rest step to separate the repeats audibly,
 * rather than the HAL having to infer a gap from two adjacent same-length
 * notes.
 */
typedef struct {
    uint16_t freq_hz;
    uint16_t ms;
} ff_sound_step_t;

/** Every pattern in the table below stays within this step budget (spec:
 * "keep every pattern... <= 8 steps"). */
#define FF_SOUND_PATTERN_MAX_STEPS 8

/** Every pattern in the table below stays within this duration budget
 * (spec: "keep every pattern <= 1.5 s total"). */
#define FF_SOUND_PATTERN_MAX_MS ((uint32_t)1500u)

typedef struct {
    ff_sound_step_t steps[FF_SOUND_PATTERN_MAX_STEPS];
    uint8_t         n; /* steps[0..n) are meaningful; n <= FF_SOUND_PATTERN_MAX_STEPS */
} ff_sound_pattern_t;

/*
 * Named musical-note frequencies (Hz, equal temperament, A4=440), so the
 * pattern table below (ff_sound.c) reads as notes rather than bare
 * numbers, and so a test can assert "FLARE_SENT is C5-E5-G5" by name.
 * Only the notes this table actually uses are named.
 */
#define FF_SOUND_NOTE_C5  523u
#define FF_SOUND_NOTE_E5  659u
#define FF_SOUND_NOTE_G5  784u
#define FF_SOUND_NOTE_A5  880u
#define FF_SOUND_NOTE_B5  988u
#define FF_SOUND_NOTE_C6 1047u

/** MULTITAP_TICK's own note — a plain, non-musical frequency (not one of
 * the named equal-temperament notes above, same posture as TAP's 1200 Hz:
 * "a tick, not a musical phrase") chosen distinctly HIGHER than TAP's
 * 1200 Hz so the two short utility blips are still tellable apart by ear
 * if a wearer happens to have both `ui_ticks` on and a quick-flare run in
 * progress at once. */
#define FF_SOUND_MULTITAP_TICK_HZ 1600u

/**
 * ff_sound_pattern_for — the fixed, hand-authored pattern for `ev`. Every
 * returned pattern satisfies `n <= FF_SOUND_PATTERN_MAX_STEPS` and a total
 * duration `<= FF_SOUND_PATTERN_MAX_MS`, enforced as a RUNTIME property
 * by `test_sound.c`'s sweep over every event (not `_Static_assert` — see
 * ff_sound.c's own comment on why a compile-time check on this table's
 * contents is not GCC-portable) so a future edit to the table can't
 * silently regress either budget.
 *
 * Returns NULL for an `ev` outside `[0, FF_SOUND_COUNT)` — reject, not a
 * guessed/empty pattern, matching this codebase's "explicit unknown, never
 * silently plausible" data discipline.
 */
ff_sound_pattern_t const *ff_sound_pattern_for(ff_sound_event_t ev);

/**
 * ff_sound_should_play — the sounds-on / quiet-hours policy (see this
 * header's top comment for the full reasoning):
 *   - `sounds_on == false` -> false for every event, no exception.
 *   - `quiet_now == true` -> true ONLY for FF_SOUND_FLARE_SENT,
 *     FF_SOUND_FLARE_INCOMING, and (fix/quick-flare-detection,
 *     2026-09-03) FF_SOUND_MULTITAP_TICK ("safety beats quiet" extended
 *     to the panic gesture's own progress feedback — see this header's
 *     "Quiet hours" section); false for every other event.
 *   - otherwise -> true for every event in `[0, FF_SOUND_COUNT)`.
 *   - an `ev` outside `[0, FF_SOUND_COUNT)` -> false (reject, not guess).
 *
 * Does NOT know about `ff_settings_t.ui_ticks` — TAP's second gate is the
 * caller's to apply (see this header's top comment); this function's
 * answer for FF_SOUND_TAP is the sounds_on/quiet-hours half only.
 */
bool ff_sound_should_play(ff_sound_event_t ev, bool sounds_on, bool quiet_now);

/**
 * ff_sound_priority — this event's tier, higher = more urgent. Three
 * values used (gaps left between them so a future event can slot in
 * without renumbering everything):
 *   - 20: FF_SOUND_FLARE_INCOMING (URGENT — "the loudest thing the puck
 *     makes"; preempts anything, including a currently-playing
 *     FLARE_INCOMING of its own — "newest wins", `ff_flare.h`'s rule).
 *   - 10: FF_SOUND_FLARE_SENT, FF_SOUND_RALLY, FF_SOUND_BATT_LOW (NORMAL).
 *   -  0: FF_SOUND_MESSAGE, FF_SOUND_TAP, FF_SOUND_MULTITAP_TICK (LOW —
 *     spec: "a MESSAGE never interrupts a FLARE_*"; TAP and
 *     MULTITAP_TICK join MESSAGE at the bottom tier as short,
 *     easily-dropped utility sounds — a MULTITAP_TICK that got dropped
 *     because something else was playing costs nothing: it is pure
 *     progress feedback, never the thing that decides whether the
 *     gesture itself fired).
 * An `ev` outside `[0, FF_SOUND_COUNT)` returns 0 (the lowest tier, never
 * preempts anything — the safe default for an unrecognised event).
 */
int ff_sound_priority(ff_sound_event_t ev);

/**
 * ff_sound_preempts — should `incoming` interrupt a pattern of kind
 * `playing` that is currently sounding? `ff_sound_priority(incoming) >=
 * ff_sound_priority(playing)`. The HAL's whole arbitration rule: if
 * nothing is currently playing, there is nothing to preempt — the HAL
 * simply plays `incoming` and never needs to call this at all.
 */
bool ff_sound_preempts(ff_sound_event_t incoming, ff_sound_event_t playing);

#ifdef __cplusplus
}
#endif

#endif /* FF_SOUND_H */
