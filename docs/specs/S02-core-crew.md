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

## Amendments

- **2026-09-07, maintainer decision — presence-heard-vs-position** (owner's
  own investigation, "why are we LOST?": `ff_crew_freshness` is POSITION
  age only, by design — it never considers whether the radio has heard
  anything from a node at all. Indoors (no GPS fix), a crew member's last
  outdoor position ages into LOST while their NodeInfo/telemetry keep
  arriving every few minutes; outdoors, a stationary friend's own radio
  may not re-send a position for up to 15 minutes (Meshtastic's default
  `position.position_broadcast_secs`) with smart broadcast, and the OLD
  10-minute LOST threshold was *shorter* than that one interval — a
  friend standing still was guaranteed to cross into LOST before their
  own radio ever sent a second fix. Ruling, binding: **LOST means the
  radio hasn't heard them; position age is a separate, honest fact that
  must never be reported as the same word.**

  **Interface additions (`core/include/ff_crew.h`):**
  ```c
  #define FF_CREW_HEARD_LIVE_MS ((uint32_t)120u * 1000u)  // 2 min
  #define FF_CREW_HEARD_LOST_MS ((uint32_t)600u * 1000u)  // 10 min
  typedef enum {
      FF_CREW_PRESENCE_HEARD, FF_CREW_PRESENCE_STALE,
      FF_CREW_PRESENCE_LOST, FF_CREW_PRESENCE_NEVER,
  } ff_crew_presence_t;
  // ff_crew_member_t gains:
  uint32_t last_heard_ms; bool has_heard;
  // New functions:
  void ff_crew_on_heard(ff_crew_t *c, uint32_t node_id, uint32_t rx_time_ms);
  ff_crew_presence_t ff_crew_presence(ff_crew_member_t const *m, uint32_t now_ms);
  ```
  `ff_crew_on_heard` is called for ANY packet from a node (NodeInfo,
  telemetry, a direct RSSI sample, a decoded text/private frame — never
  gated on carrying a position), independent of `ff_crew_on_position`.
  `ff_crew_presence` classifies HEARD (< 2 min) / STALE (2–10 min) / LOST
  (> 10 min) / NEVER (no packet ever), the SAME inclusive-toward-STALE
  boundary convention `ff_crew_freshness` already uses.

  **Thresholds changed (product decisions, this amendment):**
  - **Position freshness** (`ff_crew_freshness`, UNCHANGED axis, widened
    thresholds): LIVE < 45 s (unchanged). **STALE: 45 s – 20 min** (was 10
    min). **LOST: > 20 min** (was 10 min) — a full default Meshtastic
    broadcast interval (15 min) of slack past the cadence, so a
    stationary friend's own radio gets at least one full interval before
    their position reads LOST. This axis stays POSITION-ONLY: Radar/Map
    PLACEMENT (mode, arrow style, rim tint) keeps reading it alone — a
    stale position is still drawn as stale.
  - **Heard presence** (`ff_crew_presence`, NEW axis): HEARD < 2 min,
    STALE 2–10 min, LOST > 10 min, NEVER (no packet ever). This is the
    axis Inbox rows and the CREW page now read for their "SEEN
    <age>"/"LOST"/"LINKED" chip (via `ff_sigview_presence`, re-based onto
    this enum — see `docs/specs/S24-signals-inbox.md`'s own amendment).

  **Radar/Map amendment** (`docs/specs/S06-radar-face.md`'s domain, cross-
  referenced here since the fix touches `ff_radar_view_t`): PLACEMENT
  (mode/arrow/rim-tint/`dist_str`/`age_str`) is unchanged, still driven by
  `ff_crew_freshness` alone. `ff_radar_view_t` gains a `heard_presence`
  field (`ff_crew_presence` of the selected member, populated
  unconditionally next to the existing `place`/`stale` pair). Its one
  consumer is the RADAR_LOST renderer's never-fixed branch
  (`scr_radar.c`): a member with no position at all (`age_str == ""`)
  who has nonetheless been heard recently now reads **"NEAR, NO FIX" /
  "Heard recently, no GPS fix yet"** instead of the old, silence-implying
  "NO FIX YET" / "Waiting for their first GPS fix" — a friend heard but
  without a fix must never read as LOST. A member genuinely never heard
  keeps the old copy unchanged.

  **Wiring** (`app/ff_shell.c`'s `shell_ev_rx_meta`, `mc_events_t.on_rx_meta`
  — fires for every inbound MeshPacket naming a sender, before any
  payload event): a PAIRED sender's packet now calls `ff_crew_on_heard`
  unconditionally, deliberately NOT gated on `rx_path`/`has_rssi` the way
  the adjacent `ff_crew_on_rssi` call is — "is the radio still hearing
  them" doesn't care whether the packet arrived direct or relayed. SCOPE
  NOTE, flagged not implemented: the boot/reconnect NodeInfo REPLAY path
  (`shell_ev_node`, driven by `n->last_heard` from the want_config
  handshake) does not flow through `on_rx_meta` (it is a synthesized
  nodeDB dump, not a live MeshPacket) and so does not seed
  `last_heard_ms` — a member reconnecting after a puck restart reads
  NEVER-heard until their next LIVE packet. Left this way deliberately:
  seeding presence from replay would need the same D1 latch-circularity
  guard `shell_ev_node`'s position path already fights
  (`defined_the_latch` / `shell_replay_buffer`), and conflating that
  machinery with presence risked a much larger, less-reviewable change
  for a boot-only edge case — steady-state presence (the bug this
  amendment fixes) is unaffected, since ordinary NodeInfo/telemetry
  re-announcements ARE live MeshPackets and already fire `on_rx_meta`.

  **Radio config recommendation** (`docs/hardware/comms-brain.md`'s setup
  block, amended alongside this): outdoor position broadcasts are
  recommended down from the stock 900 s default to 120 s with smart
  broadcast for a festival crew — see that doc's own amendment for the
  exact command and the airtime tradeoff.

  **New AC9** — heard presence, independent of position:
  - `ff_crew_presence` returns NEVER before any `ff_crew_on_heard` call,
    HEARD/STALE/LOST from `now_ms - last_heard_ms` after one, at the
    documented 2 min / 10 min boundaries (inclusive toward STALE, same
    convention as AC1).
  - A member with `has_pos == false` (hence `ff_crew_freshness ==
    FF_FRESH_NEVER`) but heard 30 s ago reads `ff_crew_presence ==
    FF_CREW_PRESENCE_HEARD` — the two axes never conflate.
  - A member whose position is well past the (now 20-min) LOST threshold
    but who was heard moments ago reads `ff_crew_freshness == FF_FRESH_LOST`
    AND `ff_crew_presence == FF_CREW_PRESENCE_HEARD` simultaneously — both
    facts true and rendered separately, never collapsed into one word.
  - Tests: `firmware/core/tests/test_crew.c`'s `HEARD_*` group (predicate/
    threshold), `firmware/core/tests/test_sigview.c` (re-based
    `ff_sigview_presence` signature), `firmware/core/tests/test_radar.c`'s
    `S06_AC1_mode_lost`/`S06_AC1_close_by_rssi_wins_over_stale_gps`
    (bumped to the new 20-min position boundary),
    `firmware/app/tests/test_shell.c`'s
    `S16_AC9_want_config_replay_does_not_refresh_position_age` (same
    bump), `firmware/app/tests/test_demoapply.c`'s
    `test_S23c_poke_refreshes_presence` (re-based onto heard).

- **2026-09-08, rebase amendment — S29 folded into the heard record**
  (`docs/specs/S29-radio-only.md`, `feat/s29-radio-only-fallback`,
  rebased onto this commit): S29 was developed concurrently and had
  independently added its own `has_heard`/`heard_age_ms`/`heard_direct`
  tracker to `ff_crew_member_t` for its RADAR_SIGNAL view, flagged in its
  own spec as a "MERGE POINT" pending whichever branch landed on `main`
  first. Since this amendment's `last_heard_ms`/`has_heard`/
  `ff_crew_on_heard`/`ff_crew_presence` reached `main` first, S29's
  tracker was folded into THIS one at rebase rather than kept side by
  side: `ff_crew_on_heard` gained a fourth parameter, `bool direct`,
  unconditionally writing a new `heard_direct` field alongside
  `last_heard_ms`/`has_heard` (same "the latest sighting always wins"
  rule as the other two fields). `ff_crew_presence`'s behavior and
  signature are unchanged — `heard_direct` is not one of its inputs, it
  exists purely for RADAR_SIGNAL's direct-vs-relay distinction. This
  spec's own AC9 tests are unaffected; the new field is covered by S29's
  own `test_crew.c`/`test_shell.c` additions instead.
