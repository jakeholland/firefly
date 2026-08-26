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
- **CORROBORATED** — two or more independent sources agreeing within
  `FF_WALL_RELATCH_DELTA_S`. Strongest; a hostile node can't fake it without
  colluding peers.

**Re-latch rule (the #49 fix).** Moving an *established* latch to a disagreeing
time requires a source at least TRUSTED, or CORROBORATION. A single BOOTSTRAP-tier
(unpaired) reading can **establish** a clock but can never **move** one that a
trusted source set. A genuine backwards GPS step stays correctable because the
comms brain's own GPS reading is TRUSTED — the case the current unconditional
re-latch exists to preserve is preserved, just gated by trust instead of open to
everyone.

Bootstrap-tier readings that merely *agree* with the current latch still count
toward corroboration (agreement is cheap trust); only *disagreement* is gated.

## Slice a — trust-gated re-latch (#49)

`ff_wall_observe` grows a trust parameter (`[api]`): `ff_wall_observe(st, unix_s,
rx_ms, tier)`. The gate:
- No latch yet → accept any tier that clears the plausibility window (LATCHED).
- Latch exists, observation AGREES (within delta) → AGREED, and record the
  agreement (feeds corroboration).
- Latch exists, observation DISAGREES → re-latch **only** if `tier >= TRUSTED`
  or corroboration is met; else REJECTED, latch untouched, and the rejection is
  observable (a stranger tried to move the clock — bench-visible, not silent).

Shell boundary: `shell_observe_wall` classifies the source — paired member (via
`ff_crew_find`, never create) or self → TRUSTED; unknown/unpaired → BOOTSTRAP.
Never let an unpaired node's disagreeing time reach an established latch.

### Slice a acceptance criteria
1. Empty-latch bootstrap from an unpaired node succeeds (cold start works).
2. An established TRUSTED latch is NOT moved by a lone unpaired node disagreeing
   by > delta (the exact #49 probe: `now_min` unchanged; the observe returns
   REJECTED). This is the headline fix.
3. An established latch IS moved by a paired member (or self/GPS) disagreeing by
   > delta (backwards GPS step stays correctable).
4. Corroboration: two independent unpaired nodes agreeing on a disagreeing time
   within delta re-latch; one alone does not. (If corroboration is judged too
   complex for v1, an implementer may ship TRUSTED-only and record the deferral
   with a follow-up — but say so; don't silently drop it.)
5. The rejection is observable (ctl `wall` dump or a counter), not a silent no-op.

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

No `mc_client` change required — "first tick after READY" is the burst-end signal.

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
pack→window derivation is a pure function over `fp_pack_t` dates, unit-testable
with no shell.

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
(none yet)
