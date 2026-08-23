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

- **2026-08-22, PR #9 review (AC1 "now" window: half-open, not inclusive-both-ends).** Ruling from the spec owner during independent review of the slice (a)+(c) implementation. Original text read "pct_done correct at boundaries (start=0%, end=100%)", which an inclusive-both-ends window (`start_min <= now_min <= effective_end`) satisfied literally — but at a zero-gap same-stage changeover (set A ends the same minute set B starts on the same stage, ordinary festival scheduling) that window made **both** A and B "now" simultaneously, contradicting the Interface's own "one per stage w/ live set" contract and the Now face's one-row-per-stage layout. Ruling: the window is **half-open**, `start_min <= now_min < effective_end`. At the changeover minute the *starting* set wins; the ending set is no longer "now". Consequence: `pct_done` never displays a literal 100 while a set is still live (it caps at the last minute's value) — this is correct UI behavior, not a bug: a set showing "100% done" while its progress bar is still on screen reads as finished, not playing. AC1's text above is updated to match. Implemented in `firmware/festpack/src/ff_sched.c` (`ff_sched_now_playing`); regression-tested by `S07_AC1_zero_gap_changeover_single_row` and the rewritten boundary tests in `firmware/festpack/tests/test_sched.c`.
- **2026-08-22, PR #9 review (`ff_sched_toggle_star` signature: adds an optional alarm-state parameter).** Same review. `ff_sched_toggle_star(fp_pack_t *p, uint16_t set_idx)` from the Interface block above is now `ff_sched_toggle_star(fp_pack_t *p, uint16_t set_idx, ff_sched_alarm_t *alarm)` (`alarm` may be NULL). Un-starring an already-fired starred set now clears that set's alarm fired-bit (immediately when `alarm` is passed through here; otherwise self-healing on the next `ff_sched_alarm_tick` call, since that function also clears any currently-unstarred set's fired-bit as it scans) — so a later re-star re-arms the T-15 alert instead of silently never firing again. This is deliberate "fat-finger recovery": per-field testing expectation is that a user who accidentally un-stars a set they're about to see, then re-stars it, still gets alerted. See `firmware/festpack/include/ff_sched.h` for the full contract; regression-tested by `S07_AC4_unstar_restar_rearms`.
- Both amendments are on top of the already-documented module-placement deviation (`firmware/festpack/` instead of `core/include/ff_sched.h`) from the original PR; that call was reviewed and adjudicated correct (see PR #9 review comments), no change to that decision.
