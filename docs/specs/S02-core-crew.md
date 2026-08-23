# S02 · core/crew — crew model & freshness

## Purpose
The domain model the whole UI projects: who is my crew, where were they last, how much do we trust it. Owns the honesty rules.

## Interface (`core/include/ff_crew.h`)
```c
#define FF_CREW_MAX 8
typedef enum { FF_FRESH_LIVE, FF_FRESH_STALE, FF_FRESH_LOST, FF_FRESH_NEVER } ff_freshness_t;
typedef struct {
  uint32_t node_id;            // Meshtastic node num
  char     name[16];           // short name, crew-visible
  char     initial;            // display letter
  uint8_t  color_idx;          // index into theme crew palette
  bool     paired;             // in my crew (vs merely heard)
  ff_latlon_t pos; uint32_t pos_age_ms; bool has_pos;
  int8_t   battery_pct;        // -1 unknown
  char     status[20];         // free-text status ("RAGING"), empty if unset
  int16_t  rssi_dbm;           // last direct-packet RSSI, INT16_MIN if never direct
  uint32_t rssi_age_ms;
} ff_crew_member_t;

typedef struct { ... } ff_crew_t;
void ff_crew_init(ff_crew_t *c, ff_clock_t const *clock);
ff_crew_member_t *ff_crew_upsert(ff_crew_t *c, uint32_t node_id); // NULL if full & unpaired
void ff_crew_set_paired(ff_crew_t *c, uint32_t node_id, bool paired);
void ff_crew_on_position(ff_crew_t *c, uint32_t node_id, ff_latlon_t p, uint32_t rx_time_ms);
void ff_crew_on_rssi(ff_crew_t *c, uint32_t node_id, int16_t rssi_dbm);
ff_freshness_t ff_crew_freshness(ff_crew_member_t const *m, uint32_t now_ms);
// Selection for the radar face:
ff_crew_member_t *ff_crew_selected(ff_crew_t *c);
void ff_crew_select_next(ff_crew_t *c);
// Distance formatting honoring units setting; writes e.g. "320 m" / "1.1 km" / "980 ft" / "0.6 mi":
void ff_fmt_distance(char *buf, size_t n, float meters, bool imperial);
void ff_fmt_age(char *buf, size_t n, uint32_t age_ms); // "8 SEC" / "4 MIN" / "2 HR"
```

## Behavior — thresholds (product decisions, fixed here)
- **LIVE**: pos_age < 45 s. **STALE**: 45 s – 10 min. **LOST**: > 10 min (radar shows last-known + big last-seen; crew row goes amber). **NEVER**: no position ever.
- **Close range** (S06 consumes): distance < 30 m **or** (rssi_age < 10 s and rssi > −60 dBm). Hot/cold trend = sign of smoothed RSSI delta over 5 s window (`ff_crew_rssi_trend()`: −1/0/+1).
- Positions never expire out of the model — honesty means showing old data as old, not hiding it.
- Selection skips unpaired members; wraps; survives members appearing/disappearing.
- ft/m: metric shows m under 1 km then km (1 decimal); imperial shows ft under 1000 ft then mi (1 decimal).

## Acceptance criteria
1. Freshness transitions at exactly 45 s and 600 s (boundary tests inclusive: 45 000 ms ⇒ STALE).
2. Upsert: existing id returns same slot; 9th unpaired member rejected; 9th when one is unpaired but slot freeable — still rejected (fixed policy: no eviction v1).
3. `on_position` updates age from injected clock; freshness NEVER→LIVE on first fix.
4. Close-range predicate truth table (8 rows: distance/rssi/age combos) matches spec.
5. RSSI trend: monotonic rising fixture → +1; falling → −1; flat/noisy ±2 dBm → 0.
6. Distance formatting: 5, 999, 1000, 1049, 1500 m in both unit systems match exact strings.
7. Age formatting: 8 s, 45 s, 59 min, 61 min → "8 SEC","45 SEC","59 MIN","1 HR".
8. Zero heap allocation (static assert on struct sizes; valgrind-clean under test harness).

## Slices
a) model + upsert + freshness · b) formatting · c) close-range + RSSI trend · d) selection.
