# S18 · wall-clock trust — what may change the puck's mind about the time

## Purpose

Since S16 slice b1 (PR #46), the wall-clock latch is the **sole** source of
every crew member's position age on the Radar and Map faces. That was the right
call — it killed the "stamp it with the local clock" defect — but it makes the
latch load-bearing for the honesty of every freshness reading. A clock moved
wrong makes the *whole crew* read wrong, which is the exact failure class S16
exists to close, reached through the time axis instead of the position axis.

Three open issues are three facets of one question — this spec answers it as a
system, not three patches:

- **#49** — a single unpaired stranger's timestamp can re-latch the clock, and
  everyone's freshness shifts with it.
- **#50** — during cold-boot nodeDB replay, positions age against a *mid-burst*
  clock instead of the settled best estimate (ordering-dependent pessimism).
- **#40** — the absolute plausibility window is a fixed 2026–2030 guard that
  decays silently on a promise.

**The governing principle** is S10's own ruling, applied to time: *an established
trust is never silently overturned by a lower-trust source.* The puck may
bootstrap its clock from anything (a cold start must begin somewhere), but once
it has a **trusted** sense of time, a lone stranger doesn't get to move it.

## The trust model

A wall observation carries a **trust tier**, set at the shell boundary (`ff_wall`
stays pure; the shell knows who's paired):

- **BOOTSTRAP** — any plausible reading, including from an unpaired/never-heard
  node. Used only when no latch exists yet. A fresh puck has an empty roster and
  must start somewhere.
- **TRUSTED** — a reading attributable to a paired crew member, or the local
  comms-brain's own GPS-disciplined receive clock (that node *is* you).
- **CORROBORATED** — two or more *independent* sources agreeing. **Deferred from
  v1** (design-review PR #88): the `ff_wall_observe(st, unix_s, rx_ms, tier)`
  signature carries no source identity, so "two independent sources" cannot be
  distinguished from one node repeating itself — corroboration as it would ship
  gives no real margin over TRUSTED-only against the actual threat (a single
  broken/hostile node). TRUSTED-only fully closes #49. Corroboration (defending
  against *colluding* nodes) needs source identity threaded to the observe and is
  its own follow-up; noted, not silently dropped.

**Re-latch rule (the #49 fix) — source-gated, not provenance-gated.** The gate
conditions on the *incoming* observation's tier, NOT on what tier established the
current latch (design-review PR #88: `ff_wall_state_t` has no latch-provenance
field, and adding one creates an upgrade-path hole — a stranger-bootstrapped
latch that never gets promoted stays forever movable). The simpler, sufficient
rule:

- **Establishing** a latch (none exists yet) accepts any tier, BOOTSTRAP included
  — a cold start must begin somewhere.
- **Moving** an existing latch to a *disagreeing* time (> `FF_WALL_RELATCH_DELTA_S`)
  requires the *incoming* observation be TRUSTED. A BOOTSTRAP-tier (unpaired)
  reading can never move an existing latch, regardless of how that latch was set.

This closes #49 with **no new latch state and no upgrade path**: a fresh puck
bootstrapped off a stranger cannot then be dragged by a *second* stranger (the
exact scenario), and it is still corrected the moment a TRUSTED source disagrees.
A backwards GPS step stays correctable because self's GPS-disciplined reading is
TRUSTED (see the wiring note below). Agreeing observations are always accepted
(AGREED) regardless of tier — agreement moves nothing.

**Wiring the TRUSTED sources (design-review holes 3 & 4 — the spec must state
these or slice a will look buggy):**
- **Self/GPS is the gold anchor, but is currently unreachable from the wall.**
  `shell_ev_position` calls `shell_drop_as_self` and returns *before* the
  `ff_wall_observe` call, so the comms brain's own GPS-disciplined time never
  latches the wall today. Slice a must reorder so **self's own position/receive
  time reaches `ff_wall_observe` as TRUSTED** (dropped for crew/feed purposes,
  but its clock is exactly the trustworthy anchor). This is the primary time
  source; a puck paired to its own GPS brain should trust that over any peer.
- **Backward correction is via live Position `rx_time` only, never NodeInfo.**
  The shipped #46 rule (`shell_observe_wall_nodeinfo`) discards any backward-
  moving NodeInfo reading before it reaches `ff_wall_observe`, and that rule
  **stays, unconditional of trust** — it exists to stop stale replay dragging the
  clock back, which trust doesn't change. So AC3's "a paired member corrects the
  clock backward" is satisfied through the live `on_position` path (real-time
  receipt, trustworthy), not the NodeInfo replay path. State this so the layering
  isn't rediscovered as a slice-a "bug."

## Slice a — trust-gated re-latch (#49)

`ff_wall_observe` grows a trust parameter (`[api]`): `ff_wall_observe(st, unix_s,
rx_ms, tier)`. The gate:
- No latch yet → accept any tier that clears the plausibility window (LATCHED).
- Latch exists, observation AGREES (within delta) → AGREED. Agreement moves
  nothing and is accepted at any tier.
- Latch exists, observation DISAGREES → re-latch **only** if `tier == TRUSTED`;
  else REJECTED, latch untouched, and the rejection is observable (a stranger
  tried to move the clock — bench-visible, not silent). (Corroboration deferred,
  see the trust-model note above.)

Shell boundary: `shell_observe_wall` classifies the source — paired member (via
`ff_crew_find`, never create) or self → TRUSTED; unknown/unpaired → BOOTSTRAP.
Never let an unpaired node's disagreeing time reach an established latch.

**One pre-existing branch is deliberately left ungated (design-review PR #88):**
`ff_wall_observe`'s expired-latch / monotonic-clock-went-backwards path
(`ff_wall.c` ~147–155) re-latches unconditionally, *before* the trust check, when
the current latch is older than the max-age bound (~7 days) or the monotonic
clock jumped backward. This stays trust-blind on purpose — a latch stale that
long is, per `ff_wall.h`'s own docs, "not something to keep trusting anyway," so
any plausible reading re-anchoring it is an improvement over a week-stale time.
Slice a must NOT gate this branch; only the *disagreement-within-a-fresh-latch*
re-latch is trust-gated. State it in the code so it isn't "fixed" into a bug.

### Slice a acceptance criteria
1. Empty-latch bootstrap from an unpaired node (BOOTSTRAP tier) succeeds — cold
   start works.
2. An **established** latch (established at *any* tier, including a
   stranger-bootstrapped one) is NOT moved by a lone unpaired node disagreeing by
   > delta: `ff_wall_observe` returns REJECTED, `now_min` unchanged. This is the
   headline #49 fix, and it must hold specifically for a latch that was itself
   bootstrapped off an unpaired node (the second-stranger scenario).
3. An established latch IS moved by a TRUSTED source (a paired member, or self)
   disagreeing by > delta — a backwards GPS step stays correctable — via the live
   `on_position` path (NOT NodeInfo, which stays forward-only per #46).
4. Self's own GPS-disciplined reading reaches `ff_wall_observe` as TRUSTED
   (verify the `shell_ev_position` reorder: self is still dropped for crew/feed,
   but its clock latches the wall).
5. The rejection is observable (ctl `wall` dump or a counter), not a silent
   no-op — a stranger attempting to move the clock is visible at the bench.

## Slice b — settle-then-age the replay burst (#50)

The cold-boot `want_config` replay streams NodeInfos whose cached positions
currently age against a running-maximum latch, so ascending-`last_heard` order
reads everything `NEVER` (honest but needlessly pessimistic) while descending
reads correctly. Fix: **defer aging until the latch settles.**

- Buffer each replayed position that would have aged against a still-moving latch
  in a fixed `FF_CREW_MAX`-bounded array (no allocation): `(node_id, lat, lon,
  last_heard)`.
- On the first `ff_shell_tick` after the link reaches READY (the burst is over),
  age each buffered entry against the then-settled latch and feed
  `ff_crew_on_position`. Drop entries the roster no longer holds; any entry still
  older than the window stays `NEVER`.

No `mc_client` change required — "first tick after READY" is the burst-end
signal. This needs a small **READY-edge-detection field** in the shell (a
`was_ready` bool, or equivalent) so the re-age pass runs exactly on the
link's not-ready→ready transition, once per handshake — the spec's "first
tick after READY" is that edge, not every ready tick.

### Slice b acceptance criteria
1. Ascending and descending replay of the same node set produce the **same**
   freshness after the settle pass (order-independence — the #50 defect).
2. A 3-hour-cached node in a burst whose max `last_heard` is "now" reads `LOST`
   (the reviewer's `PROBE_cold_boot_replay_stamps_cached_positions_as_LIVE`
   stronger outcome), not `NEVER`.
3. A node genuinely older than the window still reads `NEVER` (correctness
   preserved — precision recovery must never over-claim).
4. The buffer is bounded (`FF_CREW_MAX`), no allocation; overflow drops oldest
   with a bench-visible note, never silently.

## Slice c — pack-derived plausibility window (#40)

The honest bound on "is this a plausible time" is "is it near the festival we're
at." Tighten the window to the loaded pack; keep the fixed window only for the
pre-pack bootstrap it exists for.

- **No pack loaded:** the current fixed `[FF_WALL_EPOCH_FLOOR,
  FF_WALL_EPOCH_CEILING)` bootstrap window (unchanged — it must work during the
  handshake before anything loads).
- **Pack loaded:** window tightens to `[event_start − margin, event_end +
  margin]` from `fp_pack_t`'s dates (margin ≈ 2 weeks for early-entry/teardown).
  A 2030 pack carries 2030 dates, so the window **moves with the data** — the
  fixed window's 2030 decay stops mattering the moment a real pack is loaded.
- **The fixed bootstrap window's decay** still needs a backstop for the
  no-pack-yet case. Add a CI guard that fires when the **build date** is within
  12 months of `FF_WALL_EPOCH_CEILING` — a loud, dated warning (not a
  calendar-flaky pass/fail: it's a proximity alarm with a year of runway, which
  #40 distinguishes from date-dependent CI). Turns silent 2030 decay into a
  tracked deadline.

Layering: the pack-derived bound is computed at the shell/app boundary (`ff_wall`
core takes the effective window as input — it must not include festpack). The
pack→window derivation is a pure function over `fp_pack_t`'s `year`/`start_doy`/
`end_doy` (confirmed present — design-review PR #88), unit-testable with no shell.
Note the civil-date→unix math it needs is currently `static` inside `ff_wall.c`;
expose it (or a documented equivalent) rather than duplicating a second copy that
can drift.

### Slice c acceptance criteria
1. With no pack, the fixed bootstrap window applies (unchanged behavior).
2. With the Lost Lands pack loaded, a timestamp in Sep 2026 is plausible; one in
   Sep 2029 is rejected (the tightened window works).
3. The pack→window function handles the after-midnight/multi-day span and a
   null-dated pack (falls back to the fixed window, honestly).
4. The build-date-proximity guard fires when built within 12 months of the
   ceiling and is off otherwise (test it by feeding a synthetic build date, not
   the real clock — no calendar flakiness).

## Slices & order

a (trust-gated re-latch, core `ff_wall` + shell) → b (settle-then-age, shell) →
c (pack-derived window, shell boundary + `ff_wall` window-as-input). All pure/core
+ shell — no hardware. a is the headline honesty fix (#49); do it first. b and c
are precision/robustness and can follow in either order.

## Amendments

**2026-09-02 — display format: `ff_fmt_clock` (S21 amendment).** This spec
resolves *when* the wall clock is allowed to change; the *display* of the
resolved minute-of-day was, until now, an ad hoc "HH:MM" formatter living in
the shell (`shell_project_clock_str`, app/ff_shell.c) — not core logic, and
not settable. S21's clock-format ask moved the formatter into core as a pure
function, `ff_fmt_clock(buf, n, minute_of_day, valid, clock_24h)`
(`firmware/core/include/ff_wall.h` / `ff_wall.c`), alongside the rest of this
module's minute-of-day math:
- `clock_24h == false` (the new default): 12-hour, no leading zero on the
  hour, lowercase am/pm suffix — `"9:46 pm"`, `"12:00 am"` (midnight).
- `clock_24h == true`: 24-hour, unchanged from the pre-amendment format —
  `"21:46"`.
- `valid == false` (i.e. `w.src == FF_WALL_UNKNOWN`) still writes `""`,
  unchanged: the screen renders that as `"--:--"`, never an invented time —
  this spec's honesty rule is untouched by the format choice.

The flag lives in `ff_settings_t.clock_24h` (S11/`ff_settings.h`, format
version bumped to 7) and is surfaced as a Settings CLOCK row
(docs/specs/S21-settings-rework.md). `clock_str`'s buffer budget
(`FF_RADAR_CLOCK_LEN`, `ff_radar.h`) grew from 6 to 9 bytes to fit the
12-hour form's am/pm suffix (`FF_WALL_CLOCK_STR_LEN`, statically asserted
equal). No change to the trust/latch/window logic in this spec — this
amendment is display-only.
