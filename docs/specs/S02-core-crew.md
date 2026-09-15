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

- **2026-09-13, owner decision — auto-crew on the crew channel (the
  puck half of `docs/specs/A02-crew-join.md`).** Jake, 2026-09-13:
  *"anyone who has the crew code IS crew automatically (no manual add),
  with a per-person hide for stragglers… the puck must behave the same
  way — today that only exists as the dev-only
  `CONFIG_FF_DEV_TRUST_CHANNEL`."* A02 owns the app, the crew-code
  codec and the product reasoning; **this amendment owns the puck**.
  The codec (`FIRE-` + 6 Crockford-base32 symbols → HKDF-SHA256 →
  32-byte PSK → a channel named with the code at index 0) is shared
  byte-for-byte between the two, through one fixture file,
  `docs/specs/fixtures/A02-crew-codes.json`.

  **Why this is a policy change and not a flag flip.** `ff_shell.h`'s
  "THE ROSTER TRUST POLICY" block says the paired roster never grows
  from anything the radio says, with exactly one compiled-out exception
  (`ff_shell_dev_trust_all`, reachable on device only via
  `CONFIG_FF_DEV_TRUST_CHANNEL`, "off by default and never meant to
  ship"). That policy was written when membership had no other
  definition. A02 gives it one: **possession of the crew channel's key
  is membership.** A node the radio decrypted on our crew channel has
  proved it holds a 32-byte key that only came from someone who had the
  code. That is a stronger claim than the old policy's "the radio said
  so", and it is the claim the roster may now grow on. The policy
  sentence is amended, not deleted: *the roster grows from an explicit
  user action, or from proof of the crew key — and from nothing else.*

  ### A. `FF_CREW_AUTO_ON_CHANNEL` replaces `FF_DEV_TRUST_CHANNEL`

  New Kconfig under Firefly bring-up, **`default y`** — the shipped
  behaviour, not a stopgap:

  ```
  config FF_CREW_AUTO_ON_CHANNEL
      bool "Crew = whoever is heard on the crew channel"
      default y
  ```

  `CONFIG_FF_DEV_TRUST_CHANNEL` is **deleted**, not deprecated in
  place: keeping a second, differently-named gate for behaviour that is
  now the default is exactly the kind of drift the Kconfig help text
  problem ("never ship on by default") would otherwise inherit.
  `docs/hardware/comms-brain.md`'s "Pairing (crew roster) — bench/field
  stopgap" section is rewritten to match. The sim's
  `ffsim --dev-trust-all` **stays** and keeps its current meaning — it
  is a single-node dev harness affordance (it also suspends the self
  filter and latches the wall clock), not a product behaviour, and
  conflating the two is what made this confusing in the first place.

  `[api]` — `firmware/app/include/ff_shell.h`:
  ```c
  /* was ff_shell_dev_trust_all(ff_shell_t *, bool) */
  void ff_shell_set_auto_crew(ff_shell_t *sh, bool enabled);
  bool ff_shell_auto_crew(ff_shell_t const *sh);
  ```
  The single audited growth path is unchanged: auto-admission still
  routes through `shell_pair`, so there remains exactly one place the
  roster grows.

  ### B. Which packets admit, exactly

  Identical to A02 §4.1, restated in this tree's terms. A packet admits
  its sender iff **all** of:

  1. it reached the shell decrypted (a packet the radio could not
     decrypt never reaches `mc_client` at all);
  2. its channel index is the index the crew channel occupies on this
     radio — resolved by name-and-PSK match against the radio's channel
     table, cached per link, re-resolved on reconnect; **never assumed
     to be 0** (`mesh.proto`: the channel index is "inherently a local
     concept");
  3. `from` is neither 0 nor our own node id;
  4. `from` is not on the hide list (§C);
  5. `via_mqtt == false`;
  6. the portnum is `NODEINFO_APP` (4), `POSITION_APP` (3),
     `TEXT_MESSAGE_APP` (1) or Firefly's own private portnum
     `FF_PORTNUM` = 269 (`firmware/core/include/ff_proto.h`).
     **269 is not `PRIVATE_APP`** — in `portnums.proto`
     `PRIVATE_APP = 256` and `ATAK_FORWARDER = 257`; 269 is simply a
     value Firefly picked inside the documented private range 256-511
     and must be matched by raw value (A02 §4.1 clause 6).

  Deliberately **not** admitting: `TELEMETRY_APP` (it refreshes an
  existing member's presence through the existing unconditional
  `ff_crew_on_heard` call, but carries neither identity nor intent);
  any other channel index; anything via MQTT; and — the one that
  matters most here — the **`want_config` NodeInfo replay**. The replay
  is a synthesized nodeDB dump, not a live `MeshPacket`, and cannot
  prove the node was ever heard on our channel. Note *why*, because the
  obvious reason is wrong: `NodeInfo` does carry a `channel` field
  (`mesh.proto` field 7), but it is *"only populated if its not the
  default channel"* — and Firefly's crew channel IS the primary at
  index 0, so the field is unset for exactly the nodes in question and
  is indistinguishable from unset-for-a-stranger. It is also a latched
  summary rather than an observation. This is the same ruling this
  spec's 2026-09-07 amendment
  already made for presence ("the boot/reconnect NodeInfo REPLAY path…
  does not flow through `on_rx_meta`"), applied to admission, and it
  falls out of routing admission through `shell_ev_rx_meta` rather than
  `shell_ev_node`. No new guard is needed; a test pins it.

  `[api]` — `firmware/meshclient/include/mc_client.h`, `mc_rx_meta_t`
  gains the two facts clause 2 and clause 5 need, which it does not
  carry today:
  ```c
  /* The channel index the radio reports for this packet. Presence-
   * flagged: a DM (`to` == our id) may arrive with no meaningful
   * channel, and absent must never read as 0. */
  bool     has_channel_index;
  uint32_t channel_index;
  /* MeshPacket.via_mqtt. A crew is people who are here; an MQTT path
   * can replay. */
  bool     via_mqtt;
  ```
  and a channel-table read, which the client currently drops on the
  floor during `want_config`:
  ```c
  typedef struct {
      uint8_t  index;
      char     name[12];   /* Meshtastic's own limit: < 12 bytes */
      uint8_t  psk[32];
      uint8_t  psk_len;    /* 0, 1, 16 or 32 */
      bool     is_primary;
  } mc_channel_t;
  /* mc_events_t gains: */
  void (*on_channel)(void *u, mc_channel_t const *ch);
  ```
  Both are additive; every existing callback and caller is unaffected.

  ### C. Hide — `ff_hidden.h`, a new bounded list in core

  Hide is **per node id, local to this puck, never transmitted.**
  Nobody is told they were hidden: on a mesh where possession of the
  key is membership there is no "kick", and a UI that implied otherwise
  would be lying about what the radio is doing.

  **Hide is implemented as unpair + remember**, which is also how the
  8-slot cap is managed (§E): hiding a member frees a roster slot, and
  the hide list is what stops clause 4 from re-admitting them on their
  very next packet.

  New header `firmware/core/include/ff_hidden.h`, deliberately a
  separate bounded list rather than fields on `ff_crew_t` — the exact
  precedent `ff_heard.h` set, and for the same reason: `ff_crew_t` is
  under a DRAM budget (`firmware/tools/check_dram_budget.py`) and
  should not grow for state that is not per-member.

  ```c
  #define FF_HIDDEN_MAX 16
  typedef struct { uint32_t ids[FF_HIDDEN_MAX]; uint8_t count; } ff_hidden_t;
  void ff_hidden_init(ff_hidden_t *h);
  bool ff_hidden_add(ff_hidden_t *h, uint32_t node_id);    /* false if full */
  bool ff_hidden_remove(ff_hidden_t *h, uint32_t node_id);
  bool ff_hidden_contains(ff_hidden_t const *h, uint32_t node_id);
  uint8_t ff_hidden_count(ff_hidden_t const *h);
  ```

  **No LRU here, unlike `ff_heard_t`** — a hide is a user decision, and
  silently forgetting one would put someone back on the wearer's radar
  without being asked. A full list fails honestly (`false`) and the
  CREW page says *"You've hidden as many people as your puck can
  remember (16). Unhide someone first."*

  Persisted in NVS alongside `paired_ids` (S11/S21 semantics), keyed by
  crew code so leaving and rejoining a crew restores the hides you had.

  Effect on the puck: a hidden member leaves the radar ring, the map
  face, FIND targets, the inbox list and the crew count. Their messages
  still arrive and their thread is still reachable from the CREW page's
  HIDDEN sub-view — hidden is "off my radar", not "blocked".

  ### D. The puck shows the crew code

  The puck **derives the code from its own channel name** — A02 §1.3
  makes `ChannelSettings.name` and the canonical code the same 11
  bytes, so there is no second source of truth and nothing extra to
  persist. `ff_crewcode.h` (core, slice A) validates the name against
  the alphabet; a channel name that is not a valid code reads as "no
  crew code" rather than being rendered as one.

  Settings → CREW gains a **SHOW CODE** full-screen face:

  ```
              [ QR of FIRE-4K9M7X ]

                    FIRE-4K9M7X

        Anyone who scans or types this is in your crew.
                        [ BACK ]
  ```

  > **2026-09-15 amendment — the QR carries the bare code, not the deep
  > link.** This section originally showed the QR encoding
  > `firefly://crew?v=1&code=FIRE-4K9M7X` (35 bytes), matching A02
  > §1.8's phone-side QR byte for byte. Owner report from the field:
  > the phone's scanner struggled with that QR up close on the puck's
  > 1.46" glass, decoding only from further away than a wearer showing
  > a puck across a tent has room for. Cause, measured: `lv_qrcode`
  > always encodes in BYTE mode at ECC MEDIUM
  > (`qrcodegen_encodeBinary`, never the alphanumeric mode qrcodegen
  > also offers), and 35 bytes needs QR version 3 (29x29 modules) —
  > under 6px a module in this face's 170px canvas. The bare canonical
  > code alone (11 bytes) is everything A02 §1.2's `CrewCode.parse`
  > needs — scanned, typed, or read aloud, tag included — and it fits
  > QR version 1 (21x21 modules, byte-mode capacity 14 bytes at ECC
  > MEDIUM) in the same canvas: ~40% more pixels per module, decodable
  > from further away. **This is a puck-only change.** The app's own
  > Start/Join QR (A02 §1.8, `CrewStartView`) is unaffected and still
  > carries the full deep link — that QR is scanned from a screen at a
  > comfortable distance and needs the link's `name` parameter and
  > `v=1` version gate a bare code cannot carry. `ff_crewcode_invite_url`
  > and `cw->invite_url` are unchanged and still built every projection
  > for that consumer (and any future one, e.g. an NFC share); the SHOW
  > CODE face's QR simply now reads `cw->crew_code` instead. S02_AC14
  > below is amended to match. `firmware/app/screens/scr_settings.c`'s
  > `_Static_assert(FF_CREWCODE_LEN <= 14u, ...)` pins the byte-capacity
  > arithmetic at compile time; `test_scr_crewcode_qr.c` cross-checks it
  > against the live encoder.

  - QR rendering uses LVGL's own `lv_qrcode`. **Verified present** in
    the pinned LVGL — `src/libs/qrcode/{lv_qrcode.c,qrcodegen.c}` with
    `lv_qrcode_create`/`set_size`/`set_dark_color`/`update`, in both
    v9.5.0 pinned by `firmware/CMakeLists.txt` (sim) and the 9.5.0 the
    ESP-IDF component manager resolves from `^9.2.0`
    (`targets/esp32s3/dependencies.lock`). No new dependency. But it is
    **off by default and enabled in two different places**, which is the
    sdkconfig trap S15 already paid for once:

    | Target | Where | What |
    |---|---|---|
    | sim | `firmware/lv_conf.h` | `#define LV_USE_QRCODE 1` |
    | device | `firmware/targets/esp32s3/sdkconfig.defaults` | `CONFIG_LV_USE_QRCODE=y` **and `CONFIG_LV_USE_CANVAS=y`** |

    The device needs canvas explicitly because `lv_qrcode`'s class
    derives from `lv_canvas_class`, and `lv_conf_internal.h` defaults
    `LV_USE_CANVAS` to **1 without Kconfig (the sim) but to 0 with it
    (ESP-IDF)** — so the sim builds and the device does not link, with
    nothing in `lv_conf.h` to explain it. `LV_USE_QRCODE` itself
    defaults to 0 on both. Slice D's first commit is these two config
    lines plus a build of each target.
  - The deep-link string is still built by the same core function the
    app uses (`ff_crewcode_invite_url`), against the same fixture — kept
    for any other consumer (e.g. a future NFC share), but as of the
    2026-09-15 amendment above the SHOW CODE face's own QR no longer
    encodes it; the QR encodes the bare `crew_code` instead.
  - **There is no remote trigger, and this is deliberate.** A02 §2.4:
    the phone and the puck are two clients of the *same* comms brain,
    not mesh peers of each other, so there is no packet the app could
    address to the puck. The app's "Show on puck" button therefore
    shows instructions (*"On your puck: SETTINGS → CREW → SHOW CODE"*)
    and sends nothing. A real trigger needs a new `ff_proto` message and
    is out of scope.

  ### E. The cap stays 8 on the puck

  `FF_CREW_MAX` is unchanged. The 2026-09-11 unpaired-LRU amendment
  already guarantees that *pairing a node not currently in the roster
  always succeeds while `paired_count < FF_CREW_MAX`*, so auto-
  admission inherits a roster that evicts strangers rather than
  wedging. At a genuinely full 8/8 paired roster, `shell_pair` returns
  false and **the 9th joiner must not be dropped silently** — the CREW
  page shows the honest overflow (*"2 more people are on this crew than
  your puck can track (8 is the limit). Hide someone to make room."*)
  and the overflow ids are surfaced from the existing `ff_heard_t`
  list, which is already bounded, LRU-evicted and sized (16) for
  exactly this job. Whether 8 is still the right number after the field
  test is A02 §8's open question, not this amendment's.

  ### F. Honest-data rules unchanged

  Nothing here invents a position, a name, a time or a freshness.
  Specifically: admission records membership only — it never calls
  `ff_crew_on_position`, and the existing rule that a position with no
  usable age is not recorded at all (S16 AC9) is untouched. A member
  admitted with no NodeInfo yet renders as **"NEW CREW MEMBER"** with
  its assigned colour and a `NAME?` chip, never a blank row and never a
  hex id; the name fills in when NodeInfo actually arrives. Presence
  keeps both axes separate exactly as the 2026-09-07 amendment requires
  — the SHOW CODE face and the CREW page report "heard" ages, never
  position ages dressed up as them.

  ### New acceptance criteria

  - **S02_AC11 — admission rule.** A decrypted NodeInfo/Position/Text/
    `FF_PORTNUM`-269 packet on the crew index from an unknown id admits it
    (colour assigned, `paired == true`). Each of these admits nobody,
    as its own test: another channel index; `via_mqtt == true`; a
    telemetry packet; a `want_config` replay entry; our own id; a
    hidden id; any packet at all with `FF_CREW_AUTO_ON_CHANNEL=n`.
  - **S02_AC12 — index resolution.** The crew index is resolved by
    name-and-PSK match against the channel table, cached per link,
    re-resolved on reconnect; with no matching channel, nothing is ever
    admitted and the CREW page reports "not on this crew's channel" —
    it never falls back to index 0.
  - **S02_AC13 — hide.** `ff_hidden_*` round-trips through NVS; a
    hidden id is unpaired, freeing a slot; it is never re-admitted
    while hidden; unhiding re-admits on the next qualifying packet; a
    full list fails honestly rather than evicting a user decision.
  - **S02_AC14 — code face.** A valid channel name renders the code and
    a scannable QR whose payload matches the bare canonical crew code
    (`canonical` in `docs/specs/fixtures/A02-crew-codes.json`, NOT
    `ff_crewcode_invite_url`'s deep link — amended 2026-09-15, above)
    byte-for-byte; an invalid or empty channel name renders "no crew
    code", never a fabricated one. Sim golden: `crew_show_code.json`.
    The QR's own version/module-count is pinned separately: it must fit
    QR version 1 (21x21 modules) under `qrcodegen`'s BYTE mode at ECC
    MEDIUM, which `firmware/app/screens/tests/test_scr_crewcode_qr.c`
    checks against the live encoder and
    `firmware/app/screens/scr_settings.c`'s `_Static_assert` pins at
    compile time from the byte-capacity arithmetic.
  - **S02_AC15 — overflow.** With 8/8 paired, a 9th qualifying sender
    is not admitted, is surfaced from `ff_heard_t` in the CREW page's
    overflow list with its honest last-heard age, and is admitted on
    its next packet once a member is hidden.

  Bench requirement: S02_AC11's positive case, S02_AC12 and S02_AC14
  cannot be believed from the sim alone — two radios and a puck, per
  A02 slice D.

- **2026-09-14, slice D2 — the puck STARTS a crew.** Slice D gave the
  puck a crew code to *display*; it still could not *make* one. A02 §2
  assumes the organiser holds a phone — it mints the code there, writes
  the channel over BLE, and the puck is a passive display. **The
  festival topology is asymmetric and that assumption does not hold**:
  Jake carries the puck (no camera, no keyboard — a T9 compose keyboard
  only), and the phone belongs to somebody else. A crew that can only be
  created from a phone is a crew the person wearing the hardware cannot
  start.

  So the puck creates it: mint a code from its own CSPRNG, derive the
  PSK with the SAME `ff_crewcode` HKDF the app uses (byte for byte
  against `docs/specs/fixtures/A02-crew-codes.json`), write the derived
  channel to its comms brain over the SAME Meshtastic admin path the
  owner-name push already uses, **verify the write by re-reading the
  radio**, and then show the code on slice D's SHOW CODE face so phones
  can scan it. A02 §2 owns the phone's version of this flow; **this
  amendment owns the puck's**, and the codec is shared, not duplicated.

  ### A. START CREW, step by step

  1. **Preconditions, before anything reaches the radio.** The link must
     be connected; this puck must know its own node id (the admin write
     is addressed to it — that is what puts it on Meshtastic's
     no-passkey local-admin path); and the radio's LoRa region must not
     be `UNSET`. Each refusal is reported as **itself**, never as a
     generic "can't right now": an UNSET region says, in these words,
     *"Set the radio region on the phone first."* A02 §1.7 is
     unchanged and load-bearing — **Firefly never writes `lora_config`
     and never guesses a region from a locale.**
  2. **Snapshot.** Before the first write, record what the radio holds
     on the crew index (name, PSK, precision) — once, and never
     overwritten by a Firefly crew channel. This is what LEAVE restores.
     A radio whose channel table this handshake has not reported yet
     records nothing; an un-taken snapshot is honestly absent.
  3. **Mint.** 30 CSPRNG bits (`esp_random` on the device,
     `/dev/urandom` in the sim, a scripted counter in a test — the
     source is INJECTED, `ff_shell_set_random`) → `ff_crewcode_from_bits`
     → the canonical code → `ff_crewcode_psk`. **There is no fallback.**
     A puck whose target never wired up a CSPRNG refuses to mint rather
     than minting from a tick count: 30 bits is a privacy fence
     (A02 §1.6) and a predictable 30 bits is no fence at all.
  4. **Write.** The channel of A02 §1.5, unchanged: name = the code,
     index 0, PRIMARY, `position_precision = 32` **always explicitly
     present** (an absent `module_settings` reads as the default on
     Meshtastic's side, so omitting it to mean 0 would silently ship
     full precision), uplink/downlink off, no `lora_config`. Sent as
     `AdminMessage.set_channel` with `want_ack`, to this node's own id.
  5. **Verify.** An ACK means the admin frame was delivered. It does
     **not** mean the radio holds what was asked for. The only proof is
     a read-back: re-run `want_config` and confirm the channel table's
     row at the crew index matches the written name AND key, byte for
     byte. A row at that index that does not match is a decisive
     failure, not a reason to keep waiting — the radio has answered the
     question and the answer was no.
  6. **Persist nothing extra.** The code lives only as the channel name;
     the puck derives it back on boot, exactly as slice D §D specifies.
     The pre-crew snapshot is the one new persisted record.

  **LEAVE CREW** is the same machine with the snapshot as its target
  channel, and the same write-and-verify path. With no snapshot recorded
  it fails with its own reason rather than resetting to the factory
  default: *"your radio goes back to its old settings"* and *"your radio
  goes back to the factory default"* are different promises, and only
  one of them is the one the button makes.

  ### B. The face states what is actually happening

  `WRITING` → `VERIFYING` → `READY` / `FAILED`, in plain words, with the
  reason on every failure. The vocabulary is the radio's truth rather
  than a reassuring summary: *"CHECKING IT SAVED"* is a real step that
  can really fail, and hiding it behind *"saving…"* would hide the only
  part of this that proves anything.

  Both actions are gated behind a **one-tap confirm face** in plain
  words, because a channel write reboots the comms brain and replaces
  the crew, and that is not something a mis-tap on a scrolling list gets
  to do:

  > **Start a new crew?**
  > Your radio saves it and restarts for a few seconds.
  > `[ NOT NOW ]  [ START ]`

  > **Leave the crew?**
  > Your radio goes back to its old settings.
  > `[ NOT NOW ]  [ LEAVE ]`

  Settings → CREW offers **exactly one** of START CREW (no valid crew
  code resolved) and LEAVE CREW (one resolved), and **neither** when the
  puck cannot see its radio's channel table — in that case the row is a
  muted sentence, not a greyed button. A control that cannot do what it
  says is worse than a sentence explaining why.

  **The sentence has to be the right one** (added in review, PR #312).
  A refusal reported as itself at the confirm face and reported as a
  generic "still reading your radio's settings" on the page in front of
  it is still a generic refusal, because the page is the only surface a
  wearer reaches without pressing a button that is not there. The
  UNSET-region case in particular never resolves on its own and is the
  one the wearer can act on, so the CREW page says it in §A.1's own
  words — *"Set the radio region on the phone first."* — and keeps
  "still reading" for a region that has genuinely not been REPORTED yet.
  An absence and a reading get different sentences here for the same
  reason they get different flags in `ff_shell_crew_op_status_t`.

  One consequence, flagged rather than discovered: slice D deliberately
  did NOT clear `crew_code` on a handshake (display-only, no flicker on
  reconnect). D2 adds the other half — a **completed** handshake that
  resolved no crew channel clears it, because before D2 nothing on the
  puck could remove a crew channel and LEAVE makes that the expected
  outcome. A SHOW CODE face still offering the code of a crew the radio
  has just been taken off is exactly the fabricated code that face
  exists to refuse.

  ### C. `[api]` surface

  `firmware/core/include/ff_crewcode.h` — the codec's ENCODE half, beside
  its decode half so a generator cannot drift from the parser:
  ```c
  #define FF_CREWCODE_BITS 30u
  bool ff_crewcode_from_bits(uint32_t bits, char out[FF_CREWCODE_LEN + 1u]);
  ```

  `firmware/core/include/ff_crewstart.h` — NEW. The pure state machine
  (idle → generating → writing → verifying → ready/failed), its bounded
  retry and timeouts, the pre-crew snapshot's serialization, and the
  injected `ff_crewstart_rand_fn`. No radio, no clock, no RNG of its
  own: `ff_shell.c` performs each requested action and reports the
  result back, which is what makes the whole sequence testable against a
  scripted event stub rather than only against a bench.

  `firmware/meshclient/include/mc_client.h`:
  ```c
  /* mc_channel_t gains, presence-flagged: */
  bool     has_position_precision;
  uint32_t position_precision;

  /* mc_events_t gains — the ONE thing a crew start needs from Config: */
  void (*on_lora_region)(void *u, uint32_t region);

  int  mc_client_set_channel(mc_client_t *c, uint32_t dest, mc_channel_t const *ch,
                             uint32_t *out_packet_id);
  bool mc_client_get_channel_snapshot(mc_client_t const *c, uint8_t index, mc_channel_t *out);
  ```
  All additive; every existing callback and caller is unaffected.

  `firmware/app/ff_wiring.h` — `ff_wiring_sender_t` gains
  `send_admin_set_channel` and `request_config`, appended so existing
  positional initializers still compile.

  `firmware/app/include/ff_shell.h` — `ff_shell_set_random`,
  `ff_shell_crew_start`, `ff_shell_crew_leave`, `ff_shell_crew_dismiss`,
  and `ff_shell_crew_op_status` (one projection, read by both the CREW
  faces and the bench console, so the two can never disagree — the rule
  `ff_shell_mesh_name_status` already sets for the NAME row).

  `firmware/core/include/ff_dbgcmd.h` — `crew` / `crew start` /
  `crew leave`, following `cal`'s exact shape. Both actions route
  through `ff_shell_crew_start`/`ff_shell_crew_leave`, the SAME body the
  Settings CONFIRM face reaches, exactly as `name <text>` shares
  `shell_apply_name_commit` with the NAME row's DONE button.

  ### New acceptance criteria

  - **S02_AC16 — START.** A start on a connected, region-set radio mints
    a valid code from the injected CSPRNG, writes A02 §1.5's channel
    (index 0, PRIMARY, precision 32, name = the code, PSK = the key that
    code derives), and reaches READY only after a read-back whose name
    AND key AND `position_precision` match (see AC18's precision clause
    below for exactly what "match" means for that last field). Settings
    → CREW offers exactly one of START/LEAVE and neither when the
    channel table is unresolved; the REQUEST intent opens the confirm
    face and writes nothing; a second press while a write is in the air
    is ignored, not queued.
  - **S02_AC17 — LEAVE and the snapshot.** The pre-crew channel is
    captured once before the first write, never overwritten by a Firefly
    crew channel, survives a reboot, and is restored byte for byte by
    LEAVE through the same write-and-verify path. With no snapshot,
    LEAVE fails with `NO_SNAPSHOT` and writes nothing. LEAVE's own
    read-back verify never applies the AC18 precision clause below — the
    restored channel is under no obligation to be precision 32.
  - **S02_AC18 — honest failure.** Each of these is reported as itself,
    with its own test — on the CREW page as well as on the status face,
    since a wearer whose region is UNSET is never offered the button
    that would otherwise be the only way to read the reason: an UNSET
    region (before any write); a region not
    yet reported (which is NOT the same as UNSET); no entropy source; a
    routing NAK (retried, bounded, then reported as a NAK); no ACK at
    all (retried, bounded, then reported as a timeout); a refused send;
    a read-back that never arrives; and — the one that matters most —
    **a read-back that arrives and disagrees**, which must be FAILED and
    never READY.

    **Precision is part of "disagrees," not only name and key** (added
    in review, PR #312; closed in PR #316 once the 2026-09-14 bench
    round could finally tell the two failure shapes apart). #47's hazard
    is a radio that ACKs the crew channel write and echoes back the
    right name and key while silently keeping positions coarse — the
    puck reaches READY with a real code on the glass while the crew's
    own positions are km-scale. So a START's read-back verify checks
    `position_precision` too: a row that STATES a value other than 32 is
    **always** MISMATCH, unconditionally — not a configurable case, the
    same as a wrong PSK. A row that states no precision at all is the
    one case a sim alone cannot settle, because "never echoed" and
    "echoed and wrong" decode identically: `FF_CREW_PRECISION_STRICT`
    (Kconfig default `y`; `ff_crewstart_begin_start`'s `precision_strict`
    parameter, re-stated from Kconfig at boot the same way `auto_crew`
    is) says whether an absent field fails the START the same way a
    wrong one does, or is trusted and reported honestly as
    "unreported" — never a bare 0, which is a real, different, and
    worse value a radio can genuinely state. The default is `y` on bench
    evidence, not a guess: a Heltec V3 on Meshtastic 2.7.x, measured
    2026-09-14, echoes `module_settings.position_precision` back in its
    channel table after a URL import that set it, so a real radio's
    silence here is a real signal. This is why PR #312 shipped the
    name/key check alone rather than guess which way an untested absence
    should fail. The same live fact — the crew channel's own current
    `position_precision`, independent of whether any `ff_crewstart` run
    happened this session — surfaces on the bench console's `crew`
    status line (`precision=<n|unreported>`) and on the SHOW CODE face
    (one honest line: "exact positions" iff proven exactly 32, else
    "positions coarse - start the crew again", folding stated-but-wrong,
    unreported, and no-crew-channel-at-all into the same "not exact" a
    wearer acts on the same way regardless of which it is).

  Bench requirement: S02_AC16's positive case and S02_AC17's restore
  cannot be believed from the sim alone. The bench console's
  `crew` / `crew start` / `crew leave` exist so the orchestrator can
  drive exactly that over USB.

- **2026-09-14, bench finding — ask for a name on admission, both
  sides.** A node auto-admitted (this amendment's own §B) on a
  Position/Text/`FF_PORTNUM` packet — i.e. anything but NodeInfo itself
  — stays "New crew member" (A02 §4.4) until its OWN radio gets around
  to its next periodic NodeInfo broadcast, which Meshtastic schedules on
  the order of HOURS apart, not minutes. That is a needlessly long,
  entirely avoidable wait: the mesh already has a mechanism for asking a
  node to identify itself on demand (`NODEINFO_APP` with
  `Data.want_response` set — verified against `meshtastic/firmware` tag
  `v2.7.26.54e0d8d`, `src/modules/NodeInfoModule.cpp`: any node that
  receives such a packet from a sender that isn't itself replies with
  its own `User`, via the same generic `MeshModule` reply mechanism
  `mc_send_get_owner_request`'s own doc comment already documents for
  `AdminModule`). This amendment adds the ask, on both the puck and the
  companion app, without touching the admission rule itself (§B is
  unchanged — this only follows a successful admission, never causes
  one).

  **Rate limit, not a one-shot.** An ordinary admission asks exactly
  once, by construction (the roster-growth path is reached only while a
  sender is not yet paired). The throttle is the safety net for
  admit/un-admit churn — hide/unhide, or the 2026-09-11 unpaired-LRU
  eviction cycling a busy roster's last slot — which could otherwise
  re-trigger an over-the-air ask on every cycle before an earlier one
  has had time to be answered. Ten minutes: comfortably under the
  hours-scale periodic interval this feature exists to shortcut, long
  enough that a reply (or its absence) has had time to show up.

  **The request carries our own `User`, never an empty payload.**
  `NodeInfoModule::handleReceivedProtobuf` does not merely check the
  `want_response` bit: it decodes the request's payload as a
  `meshtastic_User` and hands it to `NodeDB::updateUser`, which
  overwrites the peer's stored record of the sender with it (the one
  escape being the PKI guard, which drops a `User` that doesn't carry
  the public key the peer already holds — and replies anyway). So an
  "empty ask" is not payload-free: it is a wire claim that this node has
  no name, which any peer without our key on file believes, blanking the
  very name this feature exists to exchange. The names sent are the ones
  the RADIO last reported for its own owner (the want_config nodeDB
  entry for `my_node_id`, refreshed by a `get_owner_response`) — never
  invented, and absent rather than empty when the radio has said
  nothing. Meshtastic's own clients send their `User` on this exact
  request (`Meshtastic-Apple`'s `exchangeUserInfo`).

  `[api]` — `firmware/meshclient/include/mc_client.h`:
  ```c
  int mc_send_nodeinfo_request(mc_client_t *c, uint32_t dest, uint32_t *out_packet_id);

  /* A live NODEINFO_APP MeshPacket's User fields ONLY — no position, no
   * battery, no last_heard/hops summary (those belong to the DIFFERENT
   * message the want_config replay decodes into mc_nodeinfo_t/on_node). */
  typedef struct {
      bool has_long_name;  char long_name[MC_NAME_MAX];
      bool has_short_name; char short_name[MC_NAME_MAX];
  } mc_user_reply_t;

  /* mc_events_t gains — fires for ANY live NodeInfo (solicited or an
   * unsolicited re-announcement; the payload can't tell the two apart),
   * NEVER for the want_config replay (that stays on_node/mc_nodeinfo_t,
   * unchanged): */
  void (*on_nodeinfo_reply)(void *u, uint32_t from, mc_user_reply_t const *user);
  ```
  `firmware/app/ff_wiring.h` — `ff_wiring_sender_t` gains
  `send_nodeinfo_request`, appended so existing positional initializers
  still compile (the three in this tree were updated in this same
  change).

  **New core module** — `firmware/core/include/ff_nodeinfo_req.h`: the
  rate-limit decision and its bounded (`FF_CREW_MAX`-sized, LRU-evicting)
  per-node memory, pure C11, independently unit-tested
  (`core/tests/test_nodeinfo_req.c`) with no roster/radio in the loop.

  **Wiring** (`app/ff_shell.c`): `shell_try_admit` — the one place a
  live packet grows the roster — checks the freshly-admitted member's
  `name` field immediately after `shell_pair` succeeds; empty and due
  per `ff_nodeinfo_req_should_send` sends through
  `sender.send_nodeinfo_request`. A NEW handler, `shell_ev_nodeinfo_
  reply` (wired to `on_nodeinfo_reply`), writes the name onto the
  existing roster slot exactly like `shell_ev_node`'s own name-write for
  the replay path, but touches NOTHING else (amendment §F's rule
  extended: a live User-only reply has no position/time/freshness to
  invent either). Deliberately does not touch `ff_nodeinfo_req` itself —
  the reply is the ANSWER to a request already made, not new grounds to
  ask again.

  **Never on the replay.** `shell_ev_node` (the want_config path) never
  calls `shell_try_admit` — this was already true before this amendment
  (the whole point of the 2026-09-07 and 2026-09-13 replay-is-not-
  evidence rulings) and remains the reason the NodeInfo-request hook,
  living entirely inside `shell_try_admit`, cannot fire from a replay by
  construction, not by an added guard.

  **New acceptance criterion**, `firmware/app/tests/test_shell.c`'s
  `NIR_*` group:
  - **NodeInfo-request-on-admission.** A nameless admission sends
    exactly one request, to the admitted node. An admission of an
    already-named member (a hide/unhide cycle after naming) sends none.
    The want_config replay never sends one. The ten-minute rate limit is
    honoured across a hide/unhide churn cycle, including the boundary.
    A live reply names the member through the existing display-name
    read path. Mirrored at the meshclient layer
    (`firmware/meshclient/tests/test_meshclient.c`): the outgoing
    packet's `want_response` bit and its own-`User` payload (including
    that it is the RADIO's reported owner and never another node's
    name); a live NODEINFO_APP packet decodes to `on_nodeinfo_reply`
    and never `on_node`; a corrupt payload counts `decode_errors` and
    fires nothing.

  App-side counterpart: `docs/specs/A02-crew-join.md` owns the
  equivalent product rule for the companion app
  (`CrewMembershipEngine`/`MeshtasticClient`); this amendment is the
  puck's half only.
