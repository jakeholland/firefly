# S23 — Demo-feed infra (live synthetic event source)

Status: draft (2026-08-28). Builds on [S20](S20-demo-mode.md) (demo mode /
frozen clock / embedded festpack). Independent of [S22](S22-signals-rework.md)
but is the harness that makes S22 (and Radar/Now/Map) worth building against —
messages that *arrive* and presence that *decays*, not hand-authored snapshots.

## Why

S20 demo mode seeds a **static** snapshot (Firefly Fields, Sat 21:30, a *frozen*
clock) with no mesh. Great for a stable screenshot; lifeless for building the
new Signals list, whose whole point is incoming signals accumulating and crew
presence drifting LIVE→STALE→LOST. S23 adds a **live synthetic event source**: a
drop-in stand-in for the radio that injects randomized-but-reproducible activity
over time into the *same* core structures the real mesh wiring feeds.

## Model

### Live demo clock (extends S20's frozen clock)

S20's `ff_bringup_now_ms()` is frozen so the projection is stable. S23 adds a
**live** demo mode where the demo clock *advances* by real elapsed time from the
seeded epoch, so ages update and scheduled events fire. A switch selects
behavior; the S20 static snapshot stays the default:

- `CONFIG_FF_DEMO_MODE` off → real mesh, real clock (field build). Unchanged.
- `CONFIG_FF_DEMO_MODE` on, **static** (S20 default) → frozen clock, snapshot,
  no generator.
- `CONFIG_FF_DEMO_MODE` on, **live** (S23, e.g. `CONFIG_FF_DEMO_LIVE`) → clock
  advances from the seeded epoch; the generator drives ongoing activity.

### Pure-core generator (`core/`), app applies

The event logic is a pure C11 module — `ff_demofeed` — with **zero I/O and a
seeded PRNG** (xorshift, no `rand()`/time):

```
ff_demofeed_init(state, seed, epoch_ms);
/* Emits 0..N due events since the last call, given the current demo clock. */
uint8_t ff_demofeed_tick(ff_demofeed_t *s, uint32_t now_ms, ff_demo_event_t *out, uint8_t max);
```

An `ff_demo_event_t` is a synthetic sighting: `{from_node, kind (pulse/rally/
text/status/flare), text_ref, at_ms}` plus presence pokes (a node was "heard"
now, updating its `rssi_age`). Determinism is the contract: **same (seed, now_ms
sequence) ⇒ same event stream**, so it is unit-testable and golden-safe.

The **app** applies emitted events through the *existing* mesh path so nothing
downstream can tell demo from real — reuse `ff_wiring`
([ff_wiring.c](../../firmware/app/ff_wiring.c)):
`ff_wiring_on_text` / `ff_wiring_on_private` (which run `wiring_push_if_paired`
→ `ff_feed_push` with `unread=true`, or `ff_heard_note` for unpaired). The demo
crew are paired (seeded from the demo festpack), so their events land in the
feed exactly like real ones.

### What it generates

- **Incoming signals** on a jittered interval (bounded random gap, seeded): a
  random demo crew member sends a random kind; text drawn from the demo festpack
  string table (never hardcoded outside test fixtures — [S05](S05-festpack.md)).
- **Presence dynamics** — periodic "heard" pokes so members drift LIVE→STALE→
  LOST and recover, exercising `ff_freshness_t` on every face.
- **Optional** position jitter / status changes for Radar/Now/Map (shared infra,
  not Signals-only; gate behind the same live switch).

## Honest-data guardrail (enforced in review — [[firefly-touch-cal-default]])

The whole layer is fabricated data and must read as demo, never as real:

- Compiled out entirely when `CONFIG_FF_DEMO_MODE` is off — **no** `ff_demofeed`
  symbols, strings, or festpack in a field build (S20 embed-gating discipline).
- It only ever writes through the demo clock + demo festpack; it can never inject
  a fabricated freshness/time that a production build would read as real.
- Out-of-band "this is the demo build" labelling is sufficient (no on-glass DEMO
  badge — prior decision); the generator itself invents nothing dishonest,
  because within demo mode *everything* is understood to be synthetic.

## Acceptance criteria

- **AC1** `ff_demofeed` is pure C11, zero heap, seeded PRNG; `ff_demofeed_tick`
  is deterministic for a given (seed, now_ms sequence) — asserted by unit tests
  over a fixed schedule.
- **AC2** Emitted events are applied via the real `ff_wiring` path; a synthetic
  pulse/rally/text is indistinguishable in `ff_feed_t`/`ff_crew_t` from a mesh
  one (same push, same `unread`, same paired-filter behavior).
- **AC3** Live demo clock advances from the seeded epoch; crew presence visibly
  transitions LIVE→STALE→LOST→heard-again over time.
- **AC4** Fully gated: `CONFIG_FF_DEMO_MODE` off compiles the entire layer out
  (link check: no `ff_demofeed_*` in a field build); the S20 static snapshot
  remains the default demo behavior when the live switch is off.
- **AC5** Demo content divides into two honestly-distinct classes, and the
  line between them is the AC5 contract (CLAUDE.md: "never hardcode festival
  content outside fixtures"):
  - **Festival content** — anything referencing the real (fictional)
    festival's geography or places: a rally's place name and its lat/lon,
    stage/landmark names, etc. This is **sourced from the demo festpack** at
    apply time (e.g. `ff_demo_rally_point` reads the loaded `fp_pack_t`'s
    venue origin + landmark name), never a literal. If the festpack yields no
    honest place, no such content is emitted — never a fabricated one.
  - **Synthetic demo chatter** — conversational crew filler ("who's got
    water?", "5 min out"): a stand-in for the message traffic a radio would
    carry, invented for the demo. This is **not** festival data and is exempt
    from festpack-sourcing; it is demo-gated (compiled out of any field build)
    and lives in C, not the festpack. It is deliberately **not** forced into
    the general `fp_pack` schema (which carries no chatter field) nor a
    bespoke demo-JSON parser: they are fake strings either way, and a schema
    change to hold them would buy no honesty (S23(d) resolution). The gating
    that keeps it out of a field build (AC4) is what makes the exemption safe.
- **AC6** Goldens stay deterministic: they use fixed snapshots (or a fixed
  seed + fixed tick count), never wall-clock-driven live generation.

## Slice plan

- **(a) core `ff_demofeed`** — the seeded generator + event/presence schedule,
  pure C11 + Unity tests (deterministic sequence for a seed). No app, no clock.
- **(b) live demo clock** — advancing-clock mode + the `CONFIG_FF_DEMO_LIVE`
  switch in `app_main.c`, leaving S20 static as default.
- **(c) app apply loop** — drive `ff_demofeed_tick` from the demo clock each
  tick and apply events through `ff_wiring`; presence pokes.
- **(d) content + polish** — demo string table for canned signals; festival
  content (a rally's place name + lat/lon) sourced from the demo festpack per
  AC5 (`ff_demo_rally_point`); the two nit fixes from #117 review. Jitter for
  the other faces:
  - **Presence drift** — shipped in (c): seeded presence pokes refresh
    `rssi_age`, so a member drifts LIVE→STALE→LOST and recovers over live
    demo time, moving on Radar/Now/Map/Signals (AC3).
  - **Status drift** — shipped: STATUS signals arrive over time on the
    generator's jittered cadence and land in the feed (Signals/Now).
  - **Position drift** — **deferred** (no fabricated position path invented,
    per the honest-data guardrail). Making crew *move* honestly needs either a
    new `ff_demofeed` event type (changing the merged S23(a) determinism
    contract + its goldens) or app-side machinery — a fresh-`rx_time`
    `on_position` on its own seeded schedule, plus curation so SAM stays
    no-fix and members keep their spatial spread. None is *small*, and the
    honest options all exceed this polish slice, so position movement is left
    to a follow-up. Presence + status drift already give the other faces
    live motion "beyond Signals"; positions stay at their seeded points until
    then. A future slice adds the seeded `on_position` schedule (waypoints
    anchored to the festpack venue origin, honest receive times).

## Sequencing (with S22)

S22 (a) [core view-model] is unblocked and goes first. Then S23 (this) as the
build/demo harness. Then S22 (b/c/d) [Signals screen + composer + wiring] built
and demoed against the live feed.

## Out of scope

- Real mesh reconnection / hybrid demo+mesh (demo is mesh-*less* by definition).
- Replacing S20's static snapshot — S23 is additive; static stays the default.

## Amendments

- **2026-09-02, maintainer decision — PULSE retired end to end:** this
  document's "pulse/rally/text/…" and "kind (pulse/rally/…)" phrasing
  describes the as-built generator at the time (5 kinds, `ff_feed_kind_t`'s
  FEED_PULSE..FEED_FLARE); left as historical record. As of 2026-09-02
  PULSE is retired (see `S04-firefly-protocol.md`'s Amendments):
  `ff_demofeed`'s SIGNAL kind draw is over the 4 remaining kinds
  (`FF_DEMOFEED_KIND_COUNT` 5 → 4, `ff_demofeed.h`), so the generator never
  emits a PULSE signal again. The change alters the RNG's per-signal kind
  draw (a `% 4` divisor in place of `% 5`, same xorshift32 state stream —
  see `ff_demofeed.c`), so AC1/AC6's fixed-seed determinism goldens were
  regenerated from the new generator: a fixed `(seed, epoch_ms,
  member_count, now_ms sequence)` still yields a byte-identical stream,
  just a DIFFERENT one than before this change (the determinism CONTRACT
  is unchanged; the CONCRETE sequence a given seed produces is not — see
  the retirement PR body for the regenerated golden list).
