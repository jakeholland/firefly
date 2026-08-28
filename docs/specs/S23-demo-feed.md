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
- **AC5** Text/content come from the demo festpack, never hardcoded outside
  `tests/fixtures`.
- **AC6** Goldens stay deterministic: they use fixed snapshots (or a fixed
  seed + fixed tick count), never wall-clock-driven live generation.

## Slice plan

- **(a) core `ff_demofeed`** — the seeded generator + event/presence schedule,
  pure C11 + Unity tests (deterministic sequence for a seed). No app, no clock.
- **(b) live demo clock** — advancing-clock mode + the `CONFIG_FF_DEMO_LIVE`
  switch in `app_main.c`, leaving S20 static as default.
- **(c) app apply loop** — drive `ff_demofeed_tick` from the demo clock each
  tick and apply events through `ff_wiring`; presence pokes.
- **(d) content + polish** — demo festpack string table for canned signals;
  optional position/status jitter for the other faces.

## Sequencing (with S22)

S22 (a) [core view-model] is unblocked and goes first. Then S23 (this) as the
build/demo harness. Then S22 (b/c/d) [Signals screen + composer + wiring] built
and demoed against the live feed.

## Out of scope

- Real mesh reconnection / hybrid demo+mesh (demo is mesh-*less* by definition).
- Replacing S20's static snapshot — S23 is additive; static stays the default.
