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

## Amendments

- **2026-08-23, PR (slice b1) — the slice table said "all five `mc_events_t`
  callbacks". There are seven, and the count was wrong in two separate ways.**

  `mc_events_t` today has `on_state`, `on_node`, `on_position`, `on_text`,
  `on_private`, `on_my_info` and `on_rx_meta`
  (`firmware/meshclient/include/mc_client.h`). The b1 row's "five" was:

  - **off by one when this spec was written.** Five is the count of what
    `targets/sim/live.c` wires, which is where the number came from — and
    the narrative in "Two defects this closes" quite correctly uses it that
    way ("wire five `mc_events_t` callbacks", "`ff_wiring` handles 2 of 5").
    But `on_my_info` already existed and was never a `live.c` callback, and
    this same spec's Behavior section requires the shell to own it
    ("**`my_node_id`** comes from `mc_events_t.on_my_info`"). Carrying
    `live.c`'s count into the *shell's* scope line silently dropped a
    callback the spec elsewhere demands.
  - **off by one again after [#39](https://github.com/jakeholland/firefly/pull/39)**,
    which added `on_rx_meta` (per-packet RSSI/SNR/hop path) after this spec
    was merged.

  Corrected to seven in the slice table. Recorded rather than quietly fixed
  because the number is load-bearing for review: "did this PR wire them
  all?" is checkable only against a correct count, and a scope line that
  undercounts is how a callback ships unhandled. `on_rx_meta` in particular
  is not decorative — it is the only frequent-enough source to feed
  `ff_crew_rssi_trend`'s 5 s window, and it carries the `rx_path`
  qualifier without which RSSI must not be attributed to `from` at all.

  The b1 row also now lists **AC5c**, which its own row in the criteria
  table already assigned to b1.

- **2026-08-23, PR (slice b1) — a NodeInfo's `last_heard` may bootstrap the
  wall-clock latch but must not re-latch it backwards.**

  The wall-clock section names `mc_nodeinfo_t.last_heard` as the bootstrap
  source, correctly: it is populated unconditionally, including during the
  `want_config` replay, so the offset latches during the handshake. What
  the section does not say is what happens on the *second* handshake.

  `last_heard` means "when the nodeDB last heard this node", which is by
  construction `<= now`. On a reconnect replay a cached node's `last_heard`
  can be many minutes stale. Offered to `ff_wall_observe` unconditionally,
  it disagrees with the existing latch by more than
  `FF_WALL_RELATCH_DELTA_S` and **re-latches the wall clock backwards by
  that node's staleness** — pinning the puck's idea of "now" to the moment
  of the replay. Every position age then computes against a clock that says
  the replay instant *is* now, which is defect 2 of this very spec,
  reconstructed one layer up. AC9 fails on exactly this, and the failure
  looks like a wall-clock bug rather than the position-age bug it is.

  Ruling, implemented in `app/ff_shell.c`:

  > A NodeInfo's `last_heard` is offered to the latch when nothing has
  > latched yet, or when it is **later** than the latch predicts. A reading
  > earlier than predicted is evidence about the node, not about our clock,
  > and is ignored. `mc_position_t.rx_time` from `on_position` is a
  > per-packet *local receive* time rather than a cached summary, so it is
  > offered unconditionally and remains the re-latch source in both
  > directions.

  Residual limit, stated rather than papered over: a comms-brain clock that
  steps *backwards* is not re-latched from NodeInfo alone. It is re-latched
  by the first live `on_position`.

- **2026-08-23, PR (slice b1) — `fp_pack_t` lives BESIDE the shell, and
  `ff_shell_cfg_t` gains a field for it.** Answering the "Open question for
  the implementer" at the foot of this spec. The target owns pack storage
  and passes `ff_shell_cfg_t.pack`; `ff_shell_load_pack` parses into it.
  Rationale: the pack's ~48 KB budget is four times everything else in the
  shell put together, so folding it in would make the shell's stated
  footprint one field's footprint and destroy the `_Static_assert`'s only
  purpose; and on device the two objects want different memory (the shell
  is touched every tick and belongs in internal SRAM, the pack belongs in
  PSRAM, which is where `fp_pack.h` already assumes it lives). Measured
  footprints at the time of writing: `ff_shell_t` 12,240 B against a
  16 KB budget; `fp_pack_t` 23,696 B against its own 48 KB budget.

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
> adds a roster slot.

**The rule is about effect, not about which function is called.**
`ff_crew_on_position` itself calls `crew_find_or_create` internally
(`ff_crew.c`), so an implementer can satisfy "only pairing calls
`ff_crew_upsert`" to the letter while upserting on every inbound position —
this spec's own headline defect, through a different door. The shell must
therefore `ff_crew_find()` first and call `ff_crew_on_position` **only for a
member already in the roster**; an unknown sender's position is dropped and the
sender noted in `ff_heard`.

Auto-pairing survives as a sim-only dev affordance, `--dev-trust-all`, and is
**compiled out** on device (`#if FF_TARGET_SIM`) rather than merely defaulted
off — a runtime flag still ships the auto-pair branch into device firmware, one
stray default change from being live.

### 2. Reconnect silently refreshes stale positions

`mc_client` auto-reconnects and restarts `want_config`, which replays
meshtasticd's cached node DB: a burst of **`on_node`** callbacks carrying **old
positions that arrive now**. `live.c:99-107` stamps every one with the local
clock. Post-reconnect, every crew member instantly reads LIVE off cached data —
a textbook violation of the honest-data rule, and what the surviving pipeline
inherits unless forbidden.

**The replay carries no `rx_time`** (PR #34 round-2 review, W1).
`mc_client.c:222` calls `mc_position_from_pb(&ni->position, false, 0, ...)` —
`has_rx_time` is hardcoded false on the NodeInfo path, because `rx_time` is a
`MeshPacket` field and a `NodeInfo` is not a `MeshPacket`. So an "if
`has_rx_time`, stamp from it" rule is silent on precisely the path this defect
is about, and a test written as an `on_position` call would pass while the real
replay — which arrives as `on_node` — stayed broken.

The honest age source for a replayed position is `mc_nodeinfo_t.last_heard`
(unix seconds, populated unconditionally at `mc_client.c:230`, currently unused
by anything). So the rule has three branches and no fallback to "now":

> A position's age is stamped from `rx_time` when it arrives over the air
> (`on_position`), from `last_heard` when it arrives in a NodeInfo replay
> (`on_node`), and **never from the local clock**. If neither is available the
> age is *unknown* — which is `FF_FRESH_NEVER`, not fresh.

`ff_crew_on_position`'s fourth parameter is already named `rx_time_ms`, so a
past receive time can be stamped.

## Wall clock — the seam that doesn't exist yet

`ff_clock_t` is monotonic-only and says so ("milliseconds since some arbitrary
epoch"). But `ff_sched.h` names *"resolving wall-clock time into a `(day_doy,
now_min)` pair"* as an explicit **caller** responsibility, and no module owns
it. `ff_quiet_now()` and `ff_water_tick()` both take `now_min`. Without this,
the entire Now face is unreachable through the shell and quiet hours cannot be
evaluated. S16 is the only candidate owner, so it claims it.

The pieces exist, but **not** where the first draft said. `rx_time` rides only
on live over-the-air packets, so it cannot bootstrap anything — the handshake
would establish no offset and the puck would sit in UNKNOWN until the first
spontaneous Position arrived. The bootstrap source is
`mc_nodeinfo_t.last_heard`, unix seconds, populated on every NodeInfo including
the want_config replay — so the offset latches during the handshake.

- **Offset**: `last_heard` (or `rx_time`, when live) minus the monotonic clock
  at the moment of receipt. Wall time is monotonic + offset thereafter.
- **Local time**: `fp_pack_t.utc_offset_min`, with `utc_offset_assumed` already
  flagging a defaulted rather than stated offset.
- **Festival day**: `ff_sched.h:63-75`'s mapping (`now_min` in `[360, 1800)`;
  01:00 belongs to the *previous* `day_doy` at `now_min == 1500`). Confirmed to
  compose correctly with a unix→local conversion.

**Re-latch, don't latch once.** Not for drift — ~15 s over three days against
minute-granularity consumers is irrelevant. The reason is that the comms
brain's clock **steps** when GPS locks: before lock it reports an uncorrected
RTC. A puck that latched the pre-lock offset is then confidently wrong forever
while `src == FF_WALL_MESH` asserts that it knows.

The gate is two tiers, with concrete values — an unnamed "plausibility
threshold" is not implementable, and two implementers would pick 60 s and an
hour and both pass the AC:

- **Absolute floor (primary):** a compile-time constant `FF_WALL_EPOCH_FLOOR`,
  set to the build date. Any timestamp before it is not a time, it's an
  uncorrected RTC — reject it and stay `UNKNOWN`. This is what catches the
  pre-GPS-lock case, and it needs **no pack and no festival data**, which
  matters because the bootstrap happens during the want_config handshake,
  before `ff_shell_load_pack` is called.
- **Re-latch delta:** a fresh reading disagreeing with the latched offset by
  more than **30 s** re-latches. Wide enough to swallow transport and
  processing jitter, far narrower than any real clock step.
- **Pack dates (secondary, only once a pack is loaded):** a timestamp outside
  the festpack's event window is suspect and worth surfacing, but it is *not*
  the bootstrap gate and must never be required for one — b0 has no pack.

**The honesty rule, which is the point of this section:** until an offset
latches, **the puck does not know what time it is** and must say so rather than
guess. Not boot time, not 00:00 — unknown, explicitly, the same as an unknown
position. A Now face showing a plausible invented clock is precisely the
failure this project exists to avoid. Quiet hours cannot be evaluated in that
state; the water nudge does not fire.

```c
typedef enum { FF_WALL_UNKNOWN = 0, FF_WALL_MESH } ff_wall_src_t;
typedef struct { ff_wall_src_t src;   /* UNKNOWN: every field below is meaningless */
                 uint16_t day_doy; int16_t now_min;
                 bool offset_assumed; /* the UTC offset was defaulted, not stated */
               } ff_wall_t;
ff_wall_t ff_shell_wall(ff_shell_t const *sh);
```

**There is no user-set time source.** A `FF_WALL_USER` was drafted and cut: it
is the one source that cannot be checked against a plausibility gate — an
unfalsifiable number typed on a T9 keypad at 3 a.m. that would outrank
`UNKNOWN` and defeat this whole section. If it is ever wanted, S12 (first-run)
is its owner.

**Gap this exposes, requiring an S11 amendment.** `ff_settings_t` has no UTC
offset field, so with no pack loaded there is no way to reach local time — and
quiet hours is a *settings* feature that has nothing to do with any festival.

Resolution order, and **"stated" is load-bearing**: `fp_parse` *always*
populates `utc_offset_min`, defaulting to −240 (EDT) with
`utc_offset_assumed = true`. So the order is

> pack offset **when `!utc_offset_assumed`** → settings offset when set →
> pack's assumed default → `FF_WALL_UNKNOWN`

The naive `if (pack_loaded) use pack.utc_offset_min` makes the settings field
dead code the moment any pack loads, and lets an assumed default silently
outrank a value the user configured deliberately.

The new settings field needs its own `utc_offset_set` flag: `int16_t` has no
free sentinel, because **0 is legitimately UTC**. Absence must not be encoded
as a value that means something — the same ruling as `stage_color_valid` and
`FF_FRESH_NEVER`. `[api]` change to S11, recorded in the amendments section.

## App: routing (`app/include/ff_route.h`)

**Placement (PR #34 review, §2): `app/`, not `core/`.** The original
justification — "must be testable without LVGL" — does not select core:
`app/screens/` already holds four pure, LVGL-free, unit-tested modules
(`ff_layout`, `radar_layout`, `now_layout`, `flare_fmt`). The deciding argument
is the other way: `core/` today has *zero* knowledge that screens exist, and a
face enum would make core's contents change whenever a face is added.

**And there is no second enum.** `ff_app_face_t` is extended with the two
members it already lacks (`FF_APP_FACE_NONE = 0`, `FF_APP_FACE_FLARE`) and
routing uses it. A parallel `ff_route_face_t` would re-open the DRIFT GUARD
problem `ff_app_state.h:22-37` records paying for once already. `[api]`.

Renumbering is safe — verified across all 23 fixtures (every one encodes the
face **by name**, none omits the key), and nothing indexes an array by ordinal
or persists a face ordinal. Two traps to carry into implementation, both of
which pass CI while changing meaning:

- **`fixture.c`'s unknown-face default must stay `RADAR`**, not `NONE`. That
  runs *against* the convention `ff_app_state.h:155-162` documents ("the enum's
  first member is the least-claiming state"), so an implementer following the
  documented rule would flip it — and `fx_enum()` degrades silently
  ([#28](https://github.com/jakeholland/firefly/issues/28)), so nothing would
  say so. State it in the code comment.
- **`fixture_view.c` has an explicit `default:` label**, so `-Wswitch` under
  `-Werror` will *not* flag the new members in any consumer. The compiler will
  not help here; the switches must be audited by hand.

**`active_face` is never `FF_APP_FACE_FLARE`.** The member exists so
`ff_route_visible()` has something to return — it is a *routing* answer. The
takeover remains `ff_flare_t`'s single fact, and `face_dispatch.c:17-20`
keeps dispatching on `flare.takeover_active` as it already does. Writing FLARE
into `ff_app_state_t.active_face` would put the same fact in two places and
re-create the `ff_route_t.takeover` desync one layer down. (AC13)

The consequence reads oddly and is worth stating plainly: `ff_route_visible()`'s
one distinctive return value is the very value AC13 forbids storing in
`active_face`. That is coherent — the function answers *"where does the next
intent go?"*, which is an input-dispatch question, not a render instruction.
Rendering continues to read `takeover_active` directly.

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
        struct { ff_setting_id_t id;            /* SETTING_SET */
                 union { int32_t i; char const *s; } v; } setting;
    } u;                                        /* validity per kind, ff_flare_result_t convention */
} ff_intent_t;

/* One member per mutable ff_settings_t field. Split int/string because
 * `my_name` is char[16] and compass_cal is a struct — an int32_t-only payload
 * could not carry the one setting users actually type. */
typedef enum {
    FF_SETTING_IMPERIAL, FF_SETTING_SHARE_MODE, FF_SETTING_HAPTICS,
    FF_SETTING_NIGHT_GLOW, FF_SETTING_WATER_MIN,
    FF_SETTING_QUIET_FROM_MIN, FF_SETTING_QUIET_TO_MIN,
    FF_SETTING_UTC_OFFSET_MIN,   /* new field, see the wall-clock section */
    FF_SETTING_MY_NAME,          /* string payload */
} ff_setting_id_t;
```

`compass_cal`/`cal_valid` are deliberately absent: calibration is written by
S12's calibration ritual, not by a settings toggle, and giving it a generic
setter would let any caller assert a calibration it never performed.

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
| 9 | A transport drop moves link state to `reconnecting`, and the reconnect's `want_config` replay does not refresh any position's age. **Driven as an `on_node` callback carrying `has_position` + `last_heard`** — the shape the real replay takes, since `mc_client.c:222` hardcodes `has_rx_time = false` on that path. Member last positioned at T, drop at T+40 s, reconnect at T+5 min replaying that cached position → `ff_crew_freshness` reads `FF_FRESH_STALE`; the same replay at T+12 min reads `FF_FRESH_LOST`. A replayed position with `last_heard == 0` reads `FF_FRESH_NEVER`, never fresh. | b1 |
| 10 | Sequence test via the ctl socket (**not** the single-frame golden harness): draft typed → flare injected → takeover renders → takeover cleared → composer returns with draft intact. Requires a new ctl `flare` command. | c3, d |
| 11 | `ff_flare_result_t.should_alert` fires the haptic during quiet hours; a feed-push haptic during quiet hours does not. | b1 |
| 12 | Wall clock: before any timestamp, `ff_shell_wall().src == FF_WALL_UNKNOWN` and the Now face renders its unknown-time state rather than a clock; `ff_quiet_now` is not evaluated and the water nudge does not fire. A NodeInfo carrying `last_heard` latches the offset (the bootstrap path — `rx_time` alone cannot, being live-packet-only). `(day_doy, now_min)` then resolves per `ff_sched`'s mapping, including 01:00 local → previous `day_doy`, `now_min == 1500`. | b0 |
| 12b | A timestamp before `FF_WALL_EPOCH_FLOOR` is rejected and leaves `src == FF_WALL_UNKNOWN` (the uncorrected-RTC case, tested with no pack loaded). Once latched, a reading disagreeing by >30 s re-latches rather than being ignored; one disagreeing by less does not. | b0 |
| 12c | Offset resolution order: a pack with `utc_offset_assumed == true` does **not** outrank a set settings offset; a pack with `utc_offset_assumed == false` does. With neither set, `src == FF_WALL_UNKNOWN` rather than a defaulted guess. | b0 |
| 5c | A position from a node not in the roster is dropped and the sender noted in `ff_heard` — asserted by roster count before and after, since `ff_crew_on_position` would otherwise create the slot internally. | b1 |
| 13 | `ff_app_state_t.active_face` is never `FF_APP_FACE_FLARE` in any projection, including while a takeover is active — the takeover stays `ff_flare_t`'s single fact and `face_dispatch` keeps reading `takeover_active`. | b1 |

## Slices

| Slice | Scope | Depends on |
|---|---|---|
| **a** | `ff_route` + `ff_app_face_t` extension `[api]` + unit tests. AC1, 2, 3. | — |
| **b0** | Wall-clock derivation as a standalone unit: offset latch/re-latch, plausibility gate, unix→local→`(day_doy, now_min)`. Testable against synthetic timestamps with no shell, no transport. Includes the `ff_settings_t` UTC-offset field `[api]`. AC12, 12b. | — |
| **b1** | `ff_shell` skeleton (init/tick/view/close), core→view projection, **all seven** `mc_events_t` callbacks with the roster trust policy, haptic/quiet composition. `main.c` untouched, still on `live.c`. AC4(a), 5a, 5b, 5c, 9, 11, 13. | a, b0 |
| **b2** | Cut `targets/sim` over: retire `live.{c,h}` + its tests, rewire `--connect` and the ctl loop, add `--dev-trust-all`, update the e2e fixture. AC6. | b1 |
| **c1** | Define `ff_intent_t`, `ff_shell_intent`, the emit seam, and the three navigation-only stubs (nav long-press, compose back, signals `+`). | b1 |
| **c2** | Core-mutating stubs (compose SEND, rally tap, canned replies, radar FLARE) + the five-signature `[api]` change dropping `ff_flare_t *` from `ff_scr_nav_build`, `ff_scr_radar_build`, `ff_scr_flare_build_takeover`, `ff_scr_flare_build_sender_overlay`, `ff_scr_flare_selection_locked`. AC7. | c1 |
| **c3** | Move `static ff_t9_t s_t9` out of `scr_compose.c` into shell state; convert the ~6 keypad paths to intents; replace direct `lv_tick_get()` reads with the injected clock; **disable LVGL tileview's own swipe handling** (`scr_nav.c` creates tiles with `LV_DIR_HOR`, so it scrolls on its own gesture handling and AC2 will fail while it does). AC3b. | c2 |
| **d** | Render lifecycle: build-once/update-in-place driven by the dirty return. Closes #17 and #29. Adds the ctl `flare` command. AC4(b), 10. | b1, c3 |
| **e** | Reconnect UI + link state in the status bar, settings write-through and persistence round-trip. AC8. | b1 |

Dependency graph: `{a, b0} → b1 → {b2, c1, e}`, `c1 → c2 → c3`, `{b1, c3} → d`.
Slices **a** and **b0** have no dependencies and can start immediately, in
parallel — b0 needs no shell, no transport and no display, only synthetic
timestamps.

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
- **S11** — `ff_settings_t` gains a UTC-offset field `[api]`. Quiet hours is a
  settings feature with no festival dependency, but without a pack there is
  currently no path to local time at all, so it silently cannot be evaluated.
  Resolution order: pack's stated offset → settings offset → `FF_WALL_UNKNOWN`.
  Recorded in slice b0.

## Open question for the implementer — ANSWERED in slice b1

**Where does `fp_pack_t` live?** ~48 KB against `ff_shell_t`'s stated budget.
Inside the shell makes it non-stack-allocatable on device; beside it means the
target owns pack lifetime. Decide explicitly and document in the header — do
not leave it implied.

> **Answer: beside.** `ff_shell_cfg_t.pack` is caller-provided storage and
> `ff_shell_load_pack` parses into it. See the third Amendments entry above
> for the reasoning, and `app/include/ff_shell.h` for the header
> documentation this question asks for.
