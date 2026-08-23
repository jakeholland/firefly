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
1. now_playing: fixture with 3 concurrent + 1 finished + 1 future returns exactly the 3, pct_done correct at boundaries (start=0%, end=100%).
2. Midnight-crossing set (23:30–01:00) is "now" at 00:30 with correct day attribution; day rolls at 06:00.
3. next_starred picks earliest future starred set; none starred/future → false.
4. Alarm: advancing clock past T-15 fires exactly once; re-tick no refire; two stars 5 min apart fire in order.
5. All-null-times pack: now/next return empty/false; TBD path flagged in view struct.
6. Goldens: `now_live.json` (mocked times) and `now_tbd.json` (real Lost Lands pack) match.

## Slices
a) engine + tests · b) face render + goldens · c) alarm + haptic hook + star persistence.
