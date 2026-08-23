# S16 · app/shell — the running application

## Purpose

Every face renders. Nothing *runs*. This spec closes that gap.

Today the repo can draw any screen from a fixture, one frame at a time, and
that's the whole of it. There is no object that owns a live `mc_client_t`, an
`ff_wiring_ctx_t` and an `ff_app_state_t` together across time; no face
switching; no route from a touch event to a domain action. Every interactive
control on Signals, Compose, Radar and the nav shell is bound to a stub
callback — not because the behaviour is unclear, but because there is nowhere
to call into ([#23](https://github.com/jakeholland/firefly/issues/23)).

The shell is that missing owner: **lifecycle, event loop, routing, input
dispatch, wall-clock resolution, and the projection of live core state into the
view snapshot the screens already render.**

It is specced *before* S15 deliberately. Board bring-up should be plugging a
display driver into a loop that already works on the desktop, not inventing the
loop while also fighting unfamiliar hardware for the first time.

## Two defects this closes

### 1. Two inbound pipelines that disagree about trust

| | `app/ff_wiring.c` | `targets/sim/live.c` |
|---|---|---|
| Unknown sender | `ff_crew_find()` — read-only, never allocates | `ff_crew_upsert()` (`live.c:78`) |
| Bare Position from unknown node | n/a — doesn't handle positions | `ff_crew_upsert()` (`live.c:102`) |
| Pairing | required; unpaired dropped | `ff_crew_set_paired(..., true)` unconditionally (`live.c:81`) |
| Feed | `ff_feed_push()` after a paired check | `live_feed_push()` with **no crew check in either direction** (`live.c:109,123`) |

`ff_wiring.c`'s read-only lookup exists because PR #25's review found that
find-or-create on untrusted RF input lets a hostile node exhaust the roster's
fixed slots and permanently block real pairing. `live.c` does that, on the
*more* untrusted trigger (a bare Position — no name, no handshake), and its
feed path has no filter at all.

`live.c` is `targets/sim/`-only, so nothing ships vulnerable. It matters
because it is **the only worked example in this repo of "attach a transport,
wire five `mc_events_t` callbacks, pump them into core"** — and S15 is the spec
that needs exactly that. It will be read as the reference implementation
because it is the only one.

**Scoping correction (PR #34 review, D1).** "One surviving pipeline and it must
be `ff_wiring`'s" understated the work: `ff_wiring` handles 2 of 5 callbacks
(`on_private`, `on_text`), both feed-only, and contains no `ff_crew_upsert` at
all. The roster-exhaustion risk lives entirely in `on_node`/`on_position` —
which `ff_wiring` does not implement. Retiring `live.c` therefore *deletes the
only implementation of the half that carries the risk*.

So the shell owns `on_state`/`on_node`/`on_position`/`on_my_info` itself, and
inherits `ff_wiring.h`'s ruling verbatim as the roster policy:

> **Inbound radio traffic never grows the paired roster.** Unknown senders go
> to `ff_heard` (bounded, LRU-evictable). Only an explicit user pairing action
> calls `ff_crew_upsert`.

Auto-pairing survives as a sim-only dev affordance, `--dev-trust-all`, and is
**compiled out** on device (`#if FF_TARGET_SIM`) rather than merely defaulted
off — a runtime flag still ships the auto-pair branch into device firmware, one
stray default change from being live.

### 2. Reconnect silently refreshes stale positions

`mc_client` auto-reconnects and restarts `want_config`, which replays
meshtasticd's cached node DB: a burst of `on_node`/`on_position` carrying **old
positions that arrive now**. `live.c:99-107` stamps every one with the local
clock, ignoring `mc_position_t.time` and `rx_time`. Post-reconnect, every crew
member instantly reads LIVE off cached data — a textbook violation of the
honest-data rule, and what the surviving pipeline inherits unless forbidden.

`ff_crew_on_position`'s fourth parameter is already named `rx_time_ms`, so a
past receive time can be stamped. **A position carrying `has_rx_time` is
stamped from `rx_time`, never from the local clock.**

## Wall clock — the seam that doesn't exist yet

`ff_clock_t` is monotonic-only and says so ("milliseconds since some arbitrary
epoch"). But `ff_sched.h` names *"resolving wall-clock time into a `(day_doy,
now_min)` pair"* as an explicit **caller** responsibility, and no module owns
it. `ff_quiet_now()` and `ff_water_tick()` both take `now_min`. Without this,
the entire Now face is unreachable through the shell and quiet hours cannot be
evaluated. S16 is the only candidate owner, so it claims it.

Everything needed already exists:

- `mc_position_t.rx_time` is **unix seconds**, stamped by the local radio — the
  comms brain is a stock Meshtastic node with an L76K GPS, so it knows real
  time. First position with `has_rx_time` establishes an offset against the
  monotonic clock; wall time is monotonic + offset from then on.
- `fp_pack_t.utc_offset_min` converts unix → local minutes-of-day, and
  `fp_pack_t.utc_offset_assumed` already flags when that offset was a default
  rather than stated by the pack.
- `ff_sched.h:63-75` defines the festival-day mapping (`now_min` in
  `[360, 1800)`; a 01:00 set belongs to the *previous* `day_doy`).

**The honesty rule, and it is the point of this section:** until a position
with `has_rx_time` arrives, **the puck does not know what time it is**, and
must say so rather than guessing. Not "assume boot time", not "start at
00:00" — unknown, explicitly, the same way an unknown position is. A Now face
showing a plausible-but-invented clock is precisely the failure this project
exists to avoid. Quiet hours cannot be evaluated in that state; the water nudge
does not fire.

```c
typedef enum { FF_WALL_UNKNOWN = 0, FF_WALL_MESH, FF_WALL_USER } ff_wall_src_t;
typedef struct { ff_wall_src_t src;   /* UNKNOWN: every field below is meaningless */
                 uint16_t day_doy; int16_t now_min;
                 bool offset_assumed; /* from fp_pack_t.utc_offset_assumed */
               } ff_wall_t;
ff_wall_t ff_shell_wall(ff_shell_t const *sh);
```

## App: routing (`app/include/ff_route.h`)

**Placement (PR #34 review, §2): `app/`, not `core/`.** The original
justification — "must be testable without LVGL" — does not select core:
`app/screens/` already holds four pure, LVGL-free, unit-tested modules
(`ff_layout`, `radar_layout`, `now_layout`, `flare_fmt`). The deciding argument
is the other way: `core/` today has *zero* knowledge that screens exist, and a
face enum would make core's contents change whenever a face is added.

**And there is no second enum.** `ff_app_face_t` is extended with the two
members it already lacks (`FF_APP_FACE_NONE = 0`, `FF_APP_FACE_FLARE` — the
takeover is a real shipped full-screen face) and routing uses it. A parallel
`ff_route_face_t` would re-open the DRIFT GUARD problem `ff_app_state.h:22-37`
records paying for once already. `[api]`.

```c
typedef struct { ff_app_face_t base;   /* RADAR|NOW|SIGNALS */
                 ff_app_face_t modal;  /* COMPOSE|SETTINGS, or FF_APP_FACE_NONE */
               } ff_route_t;

void ff_route_init(ff_route_t *r);
/* dir: -1 toward RADAR, +1 toward SIGNALS. NOT a gesture direction — the
 * target maps its gesture to these, and a rightward finger drag maps to -1. */
bool ff_route_swipe(ff_route_t *r, int8_t dir);
bool ff_route_push_modal(ff_route_t *r, ff_app_face_t f);
bool ff_route_pop_modal(ff_route_t *r);

/* takeover is NOT stored: it is ff_flare_t's fact, and ff_flare_tick clears it
 * autonomously on expiry. Caching it here would desync — same rationale as
 * ff_flare_on_flare_rx taking a plain `bool paired`. */
ff_app_face_t ff_route_visible(ff_route_t const *r, bool takeover);
```

`has_modal` is deliberately absent: `modal == FF_APP_FACE_NONE` is the whole
predicate. A separate flag would give a 3-state lifecycle 4 representable
combinations — the shape PR #21's review ruled out for `now_state_t`.

Rules, **each with an acceptance criterion below**:

1. **Swipe is bounded, not wrapping.** A swipe past either end is a no-op.
   Wrapping at 2 a.m. with one thumb means you never know which direction gets
   you home. (AC1)
2. **Any modal suppresses swipe entirely.** While Compose is up, a horizontal
   drag must never slide the composer away and lose a half-typed message. (AC2)
3. **Takeover overrides what's visible without mutating the route.** Clearing
   it restores the exact prior base+modal, draft intact. (AC3)
4. **Input dispatch targets `ff_route_visible()`, never `base`.** While a
   takeover is up, Compose receives no intents at all — a touch landing where
   SEND was does not send. This is S10 Ruling 3's principle at the routing
   layer. (AC3b)

## App: the shell (`app/include/ff_shell.h`)

Target-agnostic — the object both `targets/sim` and `targets/esp32s3` drive.
Lives in `app/`, never in a target. `ff_shell_t` is **opaque**; targets hold a
pointer. Its footprint must be stated and asserted, as `ff_app_state_t` does
(`_Static_assert(sizeof(ff_app_state_t) <= 8*1024)`) — note `fp_pack_t` alone
carries a ~48 KB budget, so whether the pack lives inside the shell or beside
it is a decision the implementer must make explicitly and document.

```c
typedef struct {
    ff_clock_t const *clock;      /* monotonic; wall time is derived, see above */
    ff_store_t const *store;      /* settings persistence (S11) */
    mc_transport_t    transport;  /* UART on device, TCP in sim */
    void (*haptic)(void *user);
    void  *haptic_user;
} ff_shell_cfg_t;

/* All int returns: 0 on success, negative on failure (matches ff_live_load_pack). */
int  ff_shell_init(ff_shell_t *sh, ff_shell_cfg_t const *cfg);
int  ff_shell_load_pack(ff_shell_t *sh, char const *json, size_t len);
bool ff_shell_tick(ff_shell_t *sh, uint32_t now_ms);   /* true = rendered view changed */
ff_app_state_t const *ff_shell_view(ff_shell_t const *sh);
void ff_shell_intent(ff_shell_t *sh, ff_intent_t const *in);
void ff_shell_close(ff_shell_t *sh);
```

`ff_shell_load_pack` takes bytes, not a path — the device has no filesystem the
way the sim does, and `fp_parse` is already bytes-based. The target reads the
file with its own documented budget; the shell parses. (Confirmed correct in
review; no change.)

**The dirty bit is computed over the *rendered* projection, not the raw one.**
Several `ff_app_state_t` fields are pure functions of elapsed time and change
every tick by construction (`send_expires_in_ms`, `age_str`, `pct_done`,
`mins_until`). A whole-struct `memcmp` would return `true` on every frame in
the field while still passing a naive idle test, and slice (d) would buy
nothing. Compare at the renderer's granularity — the strings and states that
actually reach the screen.

**Haptics and quiet hours.** `ff_flare_result_t.should_alert` explicitly
*overrides* quiet hours and must be honoured unconditionally, never re-gated
through `ff_quiet_now`. Feed-push haptics (`ff_wiring.c:59-61`) *are* quiet-hours
gated. Getting this backwards silences a flare at 3 a.m., which is the one alert
that must always land. (AC11)

### Intents

Screens stay pure renderers: they emit a semantic intent, the shell decides.
This is the seam replacing every stub callback in #23.

```c
typedef enum {
    FF_INTENT_SWIPE, FF_INTENT_BACK, FF_INTENT_OPEN_COMPOSE, FF_INTENT_OPEN_SETTINGS,
    FF_INTENT_CANNED_REPLY, FF_INTENT_SEND_TEXT, FF_INTENT_MARK_FEED_READ,
    FF_INTENT_SELECT_CREW, FF_INTENT_SELECT_RALLY,
    FF_INTENT_T9_KEY, FF_INTENT_T9_SPACE, FF_INTENT_T9_BACKSPACE,
    FF_INTENT_T9_MODE, FF_INTENT_T9_INSERT,     /* SYM shortcuts, ff_t9_insert_text */
    FF_INTENT_FLARE_START, FF_INTENT_FLARE_END,
    FF_INTENT_TAKEOVER_GO, FF_INTENT_TAKEOVER_DISMISS, FF_INTENT_RELEASE_LOCK,
    FF_INTENT_SETTING_SET,
} ff_intent_kind_t;

typedef struct {
    ff_intent_kind_t kind;
    union {
        int8_t  swipe_dir;                      /* SWIPE */
        ff_wiring_canned_reply_t reply;         /* CANNED_REPLY */
        uint32_t node_id;                       /* SELECT_CREW, OPEN_COMPOSE (0 = broadcast) */
        uint8_t  rally_idx;                     /* SELECT_RALLY */
        uint8_t  t9_key;                        /* T9_KEY: 0-9 */
        char const *text;                       /* T9_INSERT (not owned; copied) */
        struct { ff_setting_id_t id; int32_t value; } setting; /* SETTING_SET */
    } u;                                        /* validity per kind, ff_flare_result_t convention */
} ff_intent_t;
```

`FF_INTENT_SEND_TEXT` carries no payload: the draft is shell-owned T9 state.
That is required — `scr_compose.c:151` currently holds `static ff_t9_t s_t9`,
reset on every build, so AC3 and AC10 are unimplementable until it moves.

`FF_INTENT_RELEASE_LOCK` is separate from `TAKEOVER_DISMISS` on purpose. S10
Ruling 3 split `ff_flare_release_lock()` from `ff_flare_dismiss_takeover()`
precisely so that tapping "stop navigating" as a new takeover arrives cannot be
silently routed into the dismiss branch, swallowing the takeover unseen.
Overloading one intent here re-creates that exact race one layer up, through
the seam designed to be the only path from UI to core.

## Behavior

- **Frame loop.** The target calls `ff_shell_tick` on a cadence it chooses (SDL
  callback in sim, an ESP-IDF task on device). The shell is passive — it never
  sleeps, blocks, or owns a thread.
- **Link state is first-class.** *connected* / *reconnecting* / *no link*, shown
  in the status bar. A stale view during reconnect must not present itself as
  live (see defect 2).
- **`my_node_id`** comes from `mc_events_t.on_my_info`; the shell must not treat
  its own traffic as inbound.
- **Settings persist** via the injected `ff_store_t` — loaded at init, saved on
  change, never every tick.
- **Unread clears on view** (S08 AC3). The shell's call, not the screen's.

## Layering — a correction to a prior claim

The draft asserted this seam "keeps `ff_wiring.c` the only file including core +
meshclient + app together." **That invariant is already false** — `live.h`
includes `ff_app_state.h` + `mc_client.h` + core headers today (a good argument
for retiring it) — and `ff_shell.h` breaks it by construction, since
`ff_shell_cfg_t` holds an `mc_transport_t`.

That is fine and correct, but must be stated rather than claimed away: the rule
becomes **`ff_wiring.c` and `ff_shell.c` are the two such files**, and
`ff_wiring.h`'s "deliberately the ONE file" comment is updated in slice b1. An
implementer who takes the old sentence literally will contort the design to
preserve an invariant this spec deliberately changes.

## Acceptance criteria

Each maps to a slice (right column). Tests are named `S16_ACn_...`.

| # | Criterion | Slice |
|---|---|---|
| 1 | `ff_route_swipe` is bounded: from RADAR, `swipe(-1)` returns false and leaves `base == RADAR`; the call sequence `swipe(+1)`, `swipe(+1)`, `swipe(+1)` ends at SIGNALS, not RADAR. | a |
| 2 | Any modal suppresses swipe: with `modal == COMPOSE` **and** with `modal == SETTINGS`, `ff_route_swipe` returns false and `base` is unchanged. | a |
| 3 | `ff_route_visible(r, true)` returns `FF_APP_FACE_FLARE` while leaving `base` and `modal` byte-identical; `ff_route_visible(r, false)` afterwards returns the prior modal (COMPOSE) — not `base`. | a |
| 3b | While takeover is active, `ff_shell_intent` dispatches only to the visible face: `FF_INTENT_SEND_TEXT` and `FF_INTENT_BACK` are rejected and the draft is unchanged; clearing the takeover restores dispatch to Compose with the draft intact. | c3 |
| 4 | Dirty bit is exact against the *rendered* projection. With `--mock-clock`: (a) a fully idle shell returns `false` for 1000 consecutive ticks; (b) with a flare in flight it returns `true` only on the tick where the rendered countdown string changes (1 s granularity) and `false` on every intervening tick. | b1, d |
| 5a | A packet from a never-heard sender produces no feed item, no new crew slot, and exactly one `ff_heard` entry. | b1 |
| 5b | A packet from a known-but-unpaired member produces no feed item and no new `ff_heard` entry. | b1 |
| 6 | Without `--dev-trust-all`, `ffsim --connect` routes every inbound event through the same entry points the shell uses, and a node that has sent only NodeInfo + Position produces zero feed items. The flag auto-pairs on NodeInfo only, logs a line naming itself at startup, and is absent from a non-sim build (compile-time assertion). | b2 |
| 7 | `FF_INTENT_CANNED_REPLY{.reply=OMW}` with a non-empty feed sends "omw" to `items[0].from_node`; with an empty feed it broadcasts. Mock sender captures dest. | c2 |
| 8 | A `FF_INTENT_SETTING_SET` is persisted: shell closed, re-inited against the same store, value survives. | e |
| 9 | A transport drop moves link state to `reconnecting`, and the reconnect's `want_config` replay does not refresh any position's age. Member last positioned at T, drop at T+40 s, reconnect at T+5 min replaying that cached position → `ff_crew_freshness` reads `FF_FRESH_STALE`; the same replay at T+12 min reads `FF_FRESH_LOST`. Positions with `has_rx_time` are stamped from `rx_time`, never the local clock. | b1 |
| 10 | Sequence test via the ctl socket (**not** the single-frame golden harness): draft typed → flare injected → takeover renders → takeover cleared → composer returns with draft intact. Requires a new ctl `flare` command. | c3, d |
| 11 | `ff_flare_result_t.should_alert` fires the haptic during quiet hours; a feed-push haptic during quiet hours does not. | b1 |
| 12 | Wall clock: before any `has_rx_time` position, `ff_shell_wall().src == FF_WALL_UNKNOWN` and the Now face renders its unknown-time state rather than a clock. After one arrives, `(day_doy, now_min)` resolves per `ff_sched`'s festival-day mapping — including a 01:00 local time mapping to the *previous* `day_doy` with `now_min == 1500`. | b1 |

## Slices

| Slice | Scope | Depends on |
|---|---|---|
| **a** | `ff_route` + `ff_app_face_t` extension `[api]` + unit tests. AC1, 2, 3. | — |
| **b1** | `ff_shell` skeleton (init/tick/view/close), core→view projection, all five `mc_events_t` callbacks with the roster trust policy, wall-clock derivation, haptic/quiet composition. `main.c` untouched, still on `live.c`. AC4(a), 5a, 5b, 9, 11, 12. | a |
| **b2** | Cut `targets/sim` over: retire `live.{c,h}` + its tests, rewire `--connect` and the ctl loop, add `--dev-trust-all`, update the e2e fixture. AC6. | b1 |
| **c1** | Define `ff_intent_t`, `ff_shell_intent`, the emit seam, and the three navigation-only stubs (nav long-press, compose back, signals `+`). | b1 |
| **c2** | Core-mutating stubs (compose SEND, rally tap, canned replies, radar FLARE) + the five-signature `[api]` change dropping `ff_flare_t *` from `ff_scr_nav_build`, `ff_scr_radar_build`, `ff_scr_flare_build_takeover`, `ff_scr_flare_build_sender_overlay`, `ff_scr_flare_selection_locked`. AC7. | c1 |
| **c3** | Move `static ff_t9_t s_t9` out of `scr_compose.c` into shell state; convert the ~6 keypad paths to intents; replace direct `lv_tick_get()` reads with the injected clock; **disable LVGL tileview's own swipe handling** (`scr_nav.c` creates tiles with `LV_DIR_HOR`, so it scrolls on its own gesture handling and AC2 will fail while it does). AC3b. | c2 |
| **d** | Render lifecycle: build-once/update-in-place driven by the dirty return. Closes #17 and #29. Adds the ctl `flare` command. AC4(b), 10. | b1, c3 |
| **e** | Reconnect UI + link state in the status bar, settings write-through and persistence round-trip. AC8. | b1 |

Dependency graph: `a → b1 → {b2, c1, e}`, `c1 → c2 → c3`, `{b1, c3} → d`.
Slice **a** has no dependencies and can start immediately.

## Amendments to prior specs

Both required in slice b1/c1; the repo's mechanism is a `## Amendments`
section, as S08 and S10 carry.

- **S06** — line 49's *"swipe left/right = face nav (owned by S06's shell
  `scr_nav.c`)"* is superseded: ownership moves to `ff_route`/`ff_shell`, and
  `scr_nav.c` becomes chrome only. S06 slice (d)'s *"FLARE button hook (fires
  S10 callback)"* likewise — it emits an intent instead.
- **S13** — `live.{c,h}` is retired and `--connect` re-pointed at the shell.
  `live.h`'s self-documented "deliberate, PR-flagged spec-gap deviation" note
  should be closed rather than left dangling.

## Open question for the implementer

**Where does `fp_pack_t` live?** ~48 KB against `ff_shell_t`'s stated budget.
Inside the shell makes it non-stack-allocatable on device; beside it means the
target owns pack lifetime. Decide explicitly and document in the header — do
not leave it implied.
