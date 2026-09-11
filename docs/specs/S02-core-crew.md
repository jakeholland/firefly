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

- **2026-09-11, maintainer decision — bounded unpaired-LRU roster
  eviction** (issue #266: "`ff_crew` roster (FF_CREW_MAX 8, no eviction)
  fills with strangers on a busy public mesh"). `ff_crew_t`'s fixed 8
  slots previously had a hard "no eviction in v1" policy (old AC2): once
  full, `ff_crew_upsert`/`ff_crew_set_paired` returned NULL/no-op for any
  node not already present, EVEN IF every occupied slot was merely heard,
  never paired. On a busy public mesh this starves real pairing before it
  can ever happen.

  **Root cause, verified before writing this amendment (AGENTS.md "measure,
  not reasoning harder"):** firmware's own RF ingestion was NOT actually
  exposed to this — `app/ff_wiring.c` and `app/ff_shell.c`'s
  `shell_ev_rx_meta`/`shell_ev_position` already gate every inbound path
  behind a READ-ONLY `ff_crew_find` first (see `ff_shell.h`'s "THE ROSTER
  TRUST POLICY" block and `ff_wiring.h`'s matching note, both from the
  earlier S08 PR #25 fix) — an unknown/unpaired sender is noted in the
  separate, bounded, LRU-evictable `core/include/ff_heard.h` list and
  NEVER touches `ff_crew_t` at all. The actual live exposure, confirmed by
  reading `app/FireflyKit/Sources/FireflyModel/CoreStore.swift`'s
  `apply(nodeUpdate:)` (wired up in PR #265's M1 integration, which is
  what surfaced #266): it calls `crew.onPosition`/`crew.onRSSI`/
  `crew.onHeard`/`crew.setIdentity` **unconditionally** for every node
  snapshot the mesh client reports, with none of `ff_shell.c`'s
  paired-gating and no companion-app equivalent of `ff_heard_t` to bounce
  strangers into instead. On a busy public mesh (the bench Heltecs already
  hear ~199 nodes; Lost Lands will be busier) the 8-slot roster fills with
  strangers via the companion app before the wearer pairs anyone, and the
  pre-amendment policy then let NO further pairing succeed at all.

  **Ruling: fix this in `ff_crew_t` itself, not only in the Swift
  bridge.** Teaching `CoreStore.swift` to replicate `ff_shell.c`'s
  paired-gate (or to grow its own `ff_heard`-equivalent) would fix the one
  known caller, but the issue is filed and scoped as `core:` for a reason
  — `ff_crew_upsert`/`ff_crew_set_paired`/`ff_crew_on_*` are the shared
  contract every current AND future caller (firmware, the companion app,
  the sim bench) relies on, and a caller-side workaround leaves the same
  footgun for the next one. The core API should be safe to call the way
  its own doc comments already describe it ("find-or-creates the slot"),
  not safe only for callers that happen to pre-gate correctly.

  **New policy (`ff_crew.h`/`ff_crew.c`):** `FF_CREW_MAX` stays 8 — no
  struct growth (`_Static_assert` in `ff_crew.h`, `firmware/tools/
  check_dram_budget.py` both still pass; nothing new was added to
  `ff_crew_member_t` or `ff_crew_t`, only `crew_find_or_create`'s
  internal behavior changed). Paired members are **pinned**: never a
  candidate for eviction, full stop. Unpaired ("merely heard") members
  share the SAME `FF_CREW_MAX` array slots as a **bounded LRU keyed on
  `last_heard_ms`**. When the roster is full (`count == FF_CREW_MAX`) and
  a genuinely new node id needs a slot — reached through
  `ff_crew_upsert`, `ff_crew_set_paired`, `ff_crew_on_position`,
  `ff_crew_on_rssi`, or `ff_crew_on_heard`, all of which still share the
  one `crew_find_or_create` implementation — the UNPAIRED occupant with
  the OLDEST `last_heard_ms` is evicted and its slot reused for the new
  id. An unpaired occupant that has never once been the target of
  `ff_crew_on_heard` (`has_heard == false` — e.g. a slot created purely
  via `ff_crew_upsert`/`ff_crew_on_position`/`ff_crew_on_rssi` with no
  accompanying "heard" call) is treated as maximally stale and evicted
  before any occupant with real heard evidence, on the reasoning that "no
  heard timestamp at all" is a weaker claim on the slot than "heard X ms
  ago", however large X is. The node currently being upserted is never
  itself an eviction candidate — it doesn't own a slot yet by
  definition. `crew_find_or_create` returns NULL — an **honest failure**
  — only when every one of the 8 occupied slots is paired; by
  construction that is exactly the "8 already paired" case, since if
  `paired_count < count == FF_CREW_MAX` there is always at least one
  unpaired occupant to evict (pigeonhole). Net effect: **pairing a node
  not currently in the roster always succeeds while `paired_count <
  FF_CREW_MAX`** (evicting the LRU stranger if the roster happens to be
  full of them), and fails honestly only once all 8 slots are genuinely
  paired — exactly the issue's proposed fix, implemented as one shared
  policy inside the existing 8 slots rather than a second array.

  **`ff_crew_set_paired` signature change `[api]`:** `void` →
  `bool`, returning whether `node_id` ended up in the roster with the
  requested `paired` value (false only on the "8 already paired, `node_id`
  wasn't one of them" failure). Every call site is audited in this same
  change (grep for `ff_crew_set_paired`); C callers that ignore the
  return value (most existing ones — `ff_shell.c`'s `shell_pair` already
  pre-checks via `ff_crew_upsert`'s own NULL return) compile and behave
  identically. The Swift bridge (`CrewStore.setPaired`) now returns and
  is `@discardableResult`, so the not-yet-built pairing UI can show an
  honest "roster full" failure instead of a silent no-op.

  **What happens to an evicted stranger's data — decision: DROPPED,
  entirely, including RSSI trend history.** A reused slot is
  `memset`-zeroed exactly like a brand-new one (same code path,
  `crew_find_or_create`'s existing zeroing), and its parallel RSSI
  trend-window ring buffer (`ff_crew_t.rssi_hist[idx]`/
  `rssi_hist_count[idx]`/`rssi_hist_head[idx]`) is reset alongside it —
  nothing about the evicted node's position, battery, status, RSSI
  history, or heard timestamp survives. This is a deliberate
  simplification, not an oversight: an evicted stranger was, by
  definition, the LEAST recently heard unpaired occupant in the roster at
  the moment of eviction — the roster's least valuable entry — and if
  that same node id is heard again later it starts a fresh record like
  any other new node, which is honest (CLAUDE.md: never fake freshness)
  rather than surprising (a lingering `rssi_dbm`/`pos` from a since-evicted
  sighting would be stale data with no way for a reader to know it
  belonged to a DIFFERENT occupancy of the slot). The "surface recently
  heard strangers for pairing" use case this data would otherwise serve
  is `core/include/ff_heard.h`'s job, not `ff_crew_t`'s — see below.

  **Selection/persistence order — an accepted, documented consequence,
  not fixed here.** `ff_crew_selected`'s self-heal ("first paired
  member") and `shell_sync_paired_settings`'s persisted `paired_ids` both
  read the roster in SLOT order. Before this amendment, with zero
  eviction ever, slot order was ALWAYS identical to pairing chronological
  order (every new id was simply appended). After this amendment, a
  paired member's slot is still stable for its entire paired lifetime
  (paired members are pinned, never moved or evicted) — but slot order no
  longer necessarily equals the order the wearer actually paired people
  in, if an eviction happened to reuse a lower-index slot for a
  later-paired friend than a higher-index slot already held. Preserving
  literal pairing chronology would need a separate monotonic sequence
  field per member — struct growth this amendment deliberately avoids
  (`check_dram_budget.py`, "do not silently grow the struct"). Flagged
  here rather than silently guessed at; revisit if product ever needs
  "my crew, in the order I added them" as a literal guarantee.
  `firmware/app/ff_shell.c`'s `shell_pair` also had a latent bug this
  amendment's eviction would have exposed: its app-assigned `color_idx`
  used to be derived from `sh->crew.count - 1` (valid only when every new
  member is APPENDED, i.e. exactly the old no-eviction world) — fixed in
  this same change to use the member's actual slot index
  (`m - sh->crew.members`), which is correct whether the slot was
  appended or reused via eviction.

  **"Add from heard nodes" — should it read the same roster? Reasoned
  answer: NO for firmware, YES-BY-DEFAULT-ALREADY for the companion app,
  and that split is correct, not an oversight.** `firmware/app/ff_shell.c`'s
  CREW page already reads its "add from heard nodes" list from
  `sh->heard` (`core/include/ff_heard.h`, `FF_HEARD_MAX` = 16), a
  SEPARATE bounded LRU sized independently of how many friends are
  already paired — this is NOT changed by this amendment, and should not
  be: `ff_crew_t`'s own unpaired-LRU capacity is `FF_CREW_MAX -
  paired_count`, which SHRINKS toward zero as the wearer pairs more
  friends and hits exactly zero at a full 8/8 roster — precisely the
  moment "who else is nearby, in case I want to swap someone in" matters
  most. Pointing the CREW page at `ff_crew_t` instead of `ff_heard_t`
  would be a strict regression (fewer, shrinking slots vs. a fixed 16),
  so it stays on `ff_heard_t`. The companion app (`app/FireflyKit`) has
  no Swift-side equivalent of `ff_heard_t` yet, so a future Nearby list
  there will necessarily read `CrewStore.members(now:)` filtered to
  `paired == false` — which this amendment makes honestly bounded and
  self-evicting instead of "fills up and then silently stops admitting
  strangers", a real improvement — but it inherits the same
  shrinks-as-you-pair ceiling `ff_heard_t` was built to avoid on the
  firmware side. Flagged, not silently accepted as equivalent: a
  companion-app-side `ff_heard` bridge (mirroring firmware's split) is
  the correct long-term fix for that gap and is out of scope for this PR.

  **New AC10 — roster eviction policy** (`firmware/core/tests/
  test_crew.c`'s `S02_AC10_*` group):
  - Filling all `FF_CREW_MAX` slots with never-paired strangers, then
    pairing a brand-new node succeeds and evicts the least-recently-heard
    stranger (by `last_heard_ms`), never an arbitrary one.
  - With all `FF_CREW_MAX` slots paired, a 9th pairing attempt fails
    honestly (`ff_crew_upsert` returns NULL, `ff_crew_set_paired` returns
    false) and leaves every existing paired member untouched.
  - A paired member is never evicted no matter how many strangers arrive
    afterward — exercised both directly and via a fuzz-style burst.
  - Eviction order follows `last_heard_ms` strictly, including the
    never-heard-is-maximally-stale rule above; a targeted fixture with
    known timestamps names the exact expected victim at each step.
  - Evicting a stranger who had a position/RSSI/status recorded drops all
    of it cleanly — the reused slot reads exactly like a brand-new one
    (`has_pos == false`, `battery_pct == -1`, `rssi_dbm == INT16_MIN`,
    `has_heard == false`), never a stale leftover field from the evicted
    occupant.
  - Fuzz smoke: 10k random heard/pair/unpair operations over a bounded id
    space never corrupt roster invariants — `count` never exceeds
    `FF_CREW_MAX`, no duplicate `node_id`s, every currently-paired member
    is never seen to vanish or change identity across the run, no
    out-of-bounds slot index is ever produced.
