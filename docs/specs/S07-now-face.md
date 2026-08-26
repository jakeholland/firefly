# S07 · app/now — schedule engine + Now face

## Purpose
"What's playing, what's next, don't miss your set." Engine in core, face renders it. Mockup "Now — lineup" is layout authority.

## Interface (`core/include/ff_sched.h`)
```c
typedef struct { fp_set_t const *set; int16_t mins_left; uint8_t pct_done; } ff_now_row_t;
typedef struct { fp_set_t const *set; int16_t mins_until; } ff_next_t;
// now_min = minutes from local midnight of current festival day (derived from clock+utc offset)
uint8_t ff_sched_now_playing(fp_pack_t const *p, uint16_t day_doy, int16_t now_min,
                             ff_now_row_t out[], uint8_t max);          // one per stage w/ live set
bool    ff_sched_next_starred(fp_pack_t const *p, uint16_t day_doy, int16_t now_min, ff_next_t *out);
void    ff_sched_toggle_star(fp_pack_t *p, uint16_t set_idx);           // persisted via S11
// Alarm: returns set needing alert (T-15min crossing since last call), else NULL. Idempotent per set.
fp_set_t const *ff_sched_alarm_tick(ff_sched_alarm_t *st, fp_pack_t const *p,
                                    uint16_t day_doy, int16_t now_min);
```

## Behavior
- Sets with null times are listed in a per-day lineup list (scroll) but excluded from now/next/alarms. **When all times are null (current Lost Lands state) the face shows the day lineup + "SET TIMES TBD" banner** — the pack-update story.
- Sets crossing midnight (end < start) belong to the day they start; `now_min` may exceed 1440 for late-night queries (day rolls at 06:00 local, not midnight — festival days end late).
- Alarm fires once per starred set at T≤15 min; alarm during quiet hours (S11) suppressed unless it's a flare (n/a here). Alarm action: haptic pattern + Now face banner.
- Rendering: three now-rows max (progress bars in stage colors), starred next card with countdown, "IN N MIN" ≥13 px, page dot #2.

## Acceptance criteria
1. now_playing: fixture with 3 concurrent + 1 finished + 1 future returns exactly the 3, pct_done correct at boundaries: start=0%, monotonically increasing, set leaves "now" exactly at end (half-open); zero-gap changeover yields exactly one row: the starting set. See ## Amendments.
2. Midnight-crossing set (23:30–01:00) is "now" at 00:30 with correct day attribution; day rolls at 06:00.
3. next_starred picks earliest future starred set; none starred/future → false.
4. Alarm: advancing clock past T-15 fires exactly once; re-tick no refire; two stars 5 min apart fire in order.
5. All-null-times pack: now/next return empty/false; TBD path flagged in view struct.
6. Goldens: `now_live.json` (mocked times) and `now_tbd.json` (real Lost Lands pack) match.

## Slices
a) engine + tests · b) face render + goldens · c) alarm + haptic hook + star persistence.

## Amendments

- **2026-08-25, PR #65 independent review, finding 1 — duplicate `start_min`
  on one stage must not double-render.** The 2026-08-24 derivation ruling
  below (item 2) fixed WHAT a null `end_min` derives to, but not what
  happens when two sets share the exact same `(stage_idx, day_doy,
  start_min)` tuple — malformed or duplicate pack data (a copy/paste
  error, a support-act slot re-announced before a drop). Both ties derive
  the identical `effective_end` (the derivation already excludes a tied
  sibling as its own source), so both were reported "now" for the
  identical window on the same stage — a NEW failure mode this PR
  introduced: before, such a starts-only duplicate silently vanished
  (safe, if uninformative); after the 08-24 fix, it double-rendered,
  violating `ff_sched_now_playing`'s own "one row per stage" contract.
  Ruling: same "ties → lower set index" rule this module already applies
  in `ff_sched_next_starred`/`ff_sched_alarm_tick` — the lower-index set
  wins the slot outright, the higher-index one is suppressed from
  `ff_sched_now_playing`'s output entirely (not merely deduped after the
  fact). Implemented in `ff_sched_now_playing`; regression-tested by
  `S07_2026_08_25_duplicate_start_same_stage_dedupes` (the reviewer's
  exact repro shape) and `S07_2026_08_25_duplicate_start_different_stage_both_render`
  (negative control: two different stages sharing a `start_min` is
  ordinary concurrent scheduling, not a duplicate, and both still render).

- **2026-08-24, end-to-end find against the first real festpack (`fix/s07-starts-only`)
  — "timed" means a known `start_min`; `end_min` is optional, not required.**
  Found running the actual product loop this project exists for: build, load
  a real festival's pack, look at the Now face. The Bass Canyon 2026 pack
  (`fest-almanac/packs/bass-canyon/2026/festpack.json`) publishes 82 real
  set-time starts and zero ends (`end` is `null` on every entry — that is
  simply how a set-time grid is published: the end is implied by the next
  act on the same stage). `ff_sched.c`'s `sched_times_known()` required
  BOTH `start_min` AND `end_min` before treating a set as anything but TBD,
  so this real pack — with real, published start times — rendered "SET
  TIMES TBD". That is not a missing-data edge case; it is a straightforward
  lie about data the puck has. The bug was invisible to every prior test
  because the only real-world fixture on hand (Lost Lands) is currently
  ALL-null (`now_tbd.json`'s case): a fixture with no start times at all
  can never exercise "start known, end not," so the AC5 acceptance
  criterion's "all-null-times pack" case was satisfied while the actual
  starts-only shape was never tested.

  **Ruling:**
  1. A set with `start_min >= 0` IS timed, regardless of `end_min`.
     `ff_sched_day_tbd`'s predicate becomes "no set on the day has a
     start_min" — end_min plays no part in it, in either direction. AC5's
     "all-null-times pack" text is unchanged (still correct), but no longer
     the only shape that must resolve away from TBD; a starts-only day now
     also correctly resolves to NOW_LIVE/NOW_MIXED rather than NOW_TBD.
  2. A null `end_min` is DERIVED from the next known `start_min` on the
     SAME stage, SAME day — that is the published semantics of a set-time
     grid, and it composes with this spec's existing half-open now-window /
     zero-gap-changeover ruling above (PR #9 review) exactly the way two
     sets with explicit times already do: at the derived boundary minute,
     the starting set wins. The derivation is a scan by
     (stage_idx, day_doy, start_min), never a `p->sets[]` array-order
     lookup — the real Bass Canyon pack lists each stage's sets
     HEADLINER-FIRST (descending by start_min), so array order is not
     schedule order.
  3. The last known-start set on a stage/day, with no later same-stage
     start to derive an end from: it is LIVE once started, but `pct_done`
     is UNKNOWABLE. Per this project's never-let-absence-carry-meaning
     convention, `ff_now_row_t`/`ff_app_now_row_t` gain an explicit
     `pct_valid` flag (`[api]`); the renderer (`scr_now.c`) omits the
     progress-bar element entirely when false, rather than showing an
     empty or fabricated fill. No default duration is invented. It stops
     counting as "now" at the festival day window's own end (`now_min`
     reaching 1800, per `ff_sched.h`'s festival-day contract) — the one
     boundary this module actually knows.

  Implemented in `firmware/festpack/src/ff_sched.c` (`ff_sched_now_playing`,
  `ff_sched_next_starred`, `ff_sched_day_tbd`, `ff_sched_alarm_tick`) and
  `firmware/app/ff_shell.c` (`shell_project_now`'s unknown-time lineup
  filter, which used to require both fields before excluding a set —
  the same bug, one layer up: a starts-only set used to land in the
  "still unknown" lineup instead of `rows`). Regression-tested by
  `firmware/festpack/tests/test_sched.c`'s `S07_2026_08_24_*` cases
  (including a fixture-level case built from
  `firmware/festpack/tests/fixtures/bass-canyon-shape.festpack.json`,
  modeled on the real pack's shape) and `firmware/app/tests/test_shell.c`'s
  `S07_2026_08_24_starts_only_set_is_live_not_lineup`. Verified against the
  real Bass Canyon pack end-to-end via the sim's ctl interface (mid-Excision,
  Friday night, Canyon Stage) — see the PR body for the screenshot.

- **2026-08-23, PR #46 review (S16 slice b1), finding D3 — `now_state_t` is
  missing a state, and the substitute mis-states the cause.** There is no
  member for *"a festpack is loaded, but the puck does not know what time it
  is."* That is not an exotic state: it is the **normal boot path**, since
  `ff_wall_t.src` stays `FF_WALL_UNKNOWN` until a plausible mesh timestamp
  latches during the `want_config` handshake, and a pack can be loaded
  before that.

  `app/ff_shell.c`'s projection currently falls back to `NOW_NO_PACK`, the
  least-claiming of the five existing members. It never invents a clock —
  but `scr_now.c:419-433` renders it as **"NO FESTIVAL LOADED / Load a
  festpack to see what's playing"**, which does not under-claim, it
  *mis*-claims: it names the wrong missing fact and instructs the user to
  redo something they have already done. `NOW_TBD` would be worse still,
  asserting the day's set times are unknown — a statement about the data
  rather than about our clock.

  Resolution: S07 gains `NOW_TIME_UNKNOWN` (`[api]`) with copy naming the
  actual missing fact. Tracked as
  [#48](https://github.com/jakeholland/firefly/issues/48); the fallback in
  `ff_shell.c` carries the issue link so it cannot harden into intended
  behaviour by default.

  **Implemented, closing #48.** `now_state_t` (`ff_app_state.h`) gains
  `NOW_TIME_UNKNOWN` as its sixth, last member (appended, per the
  renumbering caution both this Amendment and S16 slice a's
  `ff_app_face_t` work give — every already-committed fixture/golden's
  numeric encoding stays stable). `ff_shell.c`'s `shell_project_now`
  now checks pack-loaded and clock-known as two SEPARATE early returns —
  no pack at all -> `NOW_NO_PACK` (narrowed back to its original,
  literal meaning); pack loaded but `wall.src == FF_WALL_UNKNOWN` ->
  `NOW_TIME_UNKNOWN`. `scr_now.c` gets a `now_render_time_unknown` arm:
  "WAITING FOR TIME FIX" / "Clock hasn't synced from the mesh yet" —
  echoing the radar face's own NOFIX vocabulary for the same "honestly
  waiting on a signal" shape, never mentioning a festpack or a schedule.
  New fixture `tests/fixtures/now_time_unknown.json` + golden
  `tests/golden/now_time_unknown.png`. Regression-tested by
  `firmware/app/tests/test_shell.c`'s
  `S16_b1_now_projection_needs_both_a_pack_and_a_known_clock` (now pins
  `NOW_TIME_UNKNOWN`, not the old `NOW_NO_PACK` workaround) and the new
  `S48_now_no_pack_holds_regardless_of_clock_state` (pins that
  `NOW_NO_PACK` stays reachable, and means literally no pack, even once
  the clock later latches — the mutation this guards against is the two
  early-return checks in `shell_project_now` being collapsed back into
  one or reordered).


- **2026-08-22, PR #9 review (AC1 "now" window: half-open, not inclusive-both-ends).** Ruling from the spec owner during independent review of the slice (a)+(c) implementation. Original text read "pct_done correct at boundaries (start=0%, end=100%)", which an inclusive-both-ends window (`start_min <= now_min <= effective_end`) satisfied literally — but at a zero-gap same-stage changeover (set A ends the same minute set B starts on the same stage, ordinary festival scheduling) that window made **both** A and B "now" simultaneously, contradicting the Interface's own "one per stage w/ live set" contract and the Now face's one-row-per-stage layout. Ruling: the window is **half-open**, `start_min <= now_min < effective_end`. At the changeover minute the *starting* set wins; the ending set is no longer "now". Consequence: `pct_done` never displays a literal 100 while a set is still live (it caps at the last minute's value) — this is correct UI behavior, not a bug: a set showing "100% done" while its progress bar is still on screen reads as finished, not playing. AC1's text above is updated to match. Implemented in `firmware/festpack/src/ff_sched.c` (`ff_sched_now_playing`); regression-tested by `S07_AC1_zero_gap_changeover_single_row` and the rewritten boundary tests in `firmware/festpack/tests/test_sched.c`.
- **2026-08-22, PR #9 review (`ff_sched_toggle_star` signature: adds an optional alarm-state parameter).** Same review. `ff_sched_toggle_star(fp_pack_t *p, uint16_t set_idx)` from the Interface block above is now `ff_sched_toggle_star(fp_pack_t *p, uint16_t set_idx, ff_sched_alarm_t *alarm)` (`alarm` may be NULL). Un-starring an already-fired starred set now clears that set's alarm fired-bit (immediately when `alarm` is passed through here; otherwise self-healing on the next `ff_sched_alarm_tick` call, since that function also clears any currently-unstarred set's fired-bit as it scans) — so a later re-star re-arms the T-15 alert instead of silently never firing again. This is deliberate "fat-finger recovery": per-field testing expectation is that a user who accidentally un-stars a set they're about to see, then re-stars it, still gets alerted. See `firmware/festpack/include/ff_sched.h` for the full contract; regression-tested by `S07_AC4_unstar_restar_rearms`.
- Both amendments are on top of the already-documented module-placement deviation (`firmware/festpack/` instead of `core/include/ff_sched.h`) from the original PR; that call was reviewed and adjudicated correct (see PR #9 review comments), no change to that decision.
