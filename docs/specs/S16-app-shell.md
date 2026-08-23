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
dispatch, and the projection of live core state into the view snapshot the
screens already know how to render.**

It is deliberately specced *before* S15. Board bring-up should be plugging a
display driver into a loop that already works on the desktop, not inventing
the loop while also fighting unfamiliar hardware for the first time.

## The defect this also fixes

There are currently **two divergent inbound-event pipelines**, and they
disagree about trust:

| | `app/ff_wiring.c` | `targets/sim/live.c` |
|---|---|---|
| Unknown sender | `ff_crew_find()` — read-only, never allocates | `ff_crew_upsert()` — allocates a roster slot |
| Pairing | Required; unpaired events dropped | **`ff_crew_set_paired(..., true)` unconditionally** |
| Feed | `ff_feed_push()` into `ff_feed_t` | Its own `live_feed_push()` straight into view state |

`ff_wiring.c`'s read-only lookup exists because PR #25's review found that
find-or-create on untrusted RF input let a hostile node exhaust the roster's
fixed slots and permanently block real pairing. `live.c` does the exact thing
that finding forbade, and then trusts every node it hears.

In the sim that reads as a convenience, and it was a reasonable scaffold when
it was written — but it has two real consequences today: **live mode never
exercises the crew filter at all** (so the hardening is untested end to end
outside unit tests), and this is the code shape most likely to be copied onto
the device target, where it is the original vulnerability restored. The shell
must leave exactly one pipeline standing, and it must be `ff_wiring`'s.

Auto-pairing stays available to the sim, but only as an explicit, named dev
affordance (`--dev-trust-all`) that announces itself — never as the default
behaviour of the code path the device will run.

## Core: routing (`core/include/ff_route.h`) — pure, no LVGL

Which face is showing is domain state, not chrome (CLAUDE.md: "if you're
writing an `if` about domain behavior inside a screen file, it belongs in
core"). Routing is therefore pure C11 and unit-testable with no display.

```c
typedef enum { FF_ROUTE_RADAR, FF_ROUTE_NOW, FF_ROUTE_SIGNALS,
               FF_ROUTE_COMPOSE, FF_ROUTE_SETTINGS } ff_route_face_t;

typedef struct { ff_route_face_t base;      /* swipeable face: RADAR|NOW|SIGNALS */
                 ff_route_face_t modal;     /* COMPOSE|SETTINGS, or FF_ROUTE_NONE */
                 bool            has_modal;
                 bool            takeover;  /* flare owns the screen right now */
               } ff_route_t;

void ff_route_init(ff_route_t *r);
bool ff_route_swipe(ff_route_t *r, int8_t dir);        /* -1 left, +1 right */
bool ff_route_push_modal(ff_route_t *r, ff_route_face_t f);
bool ff_route_pop_modal(ff_route_t *r);
void ff_route_set_takeover(ff_route_t *r, bool on);
ff_route_face_t ff_route_visible(ff_route_t const *r);  /* what to actually draw */
```

Rules, each one an acceptance criterion below:

- **Swipe is bounded, not wrapping.** Radar ↔ Now ↔ Signals. A swipe past
  either end is a no-op, not a wrap to the far side — wrapping at 2 a.m. with
  one thumb means you never know which direction gets you home.
- **A modal suppresses swipe entirely.** While Compose is up, a horizontal
  drag is text selection or nothing — it must never slide the composer away
  and lose a half-typed message.
- **Takeover overrides everything and restores exactly.** A flare arriving
  while Compose is open must not discard the draft: takeover is a temporary
  override of what's *visible*, not a mutation of the route. When it clears,
  the composer is back with the draft intact.
- **Takeover does not silently redirect a modal's actions.** Existing ruling
  from S10: an established navigation lock is never silently replaced. Same
  principle applies to the screen itself.

## App: the shell (`app/include/ff_shell.h`)

Target-agnostic — this is the object both `targets/sim` and `targets/esp32s3`
drive. It lives in `app/`, never in a target.

```c
typedef struct {
    ff_clock_t const *clock;      /* injected, per ARCHITECTURE.md */
    ff_store_t const *store;      /* settings persistence (S11) */
    mc_transport_t    transport;  /* UART on device, TCP in sim */
    void (*haptic)(void *user);
    void  *haptic_user;
} ff_shell_cfg_t;

int  ff_shell_init(ff_shell_t *sh, ff_shell_cfg_t const *cfg);
int  ff_shell_load_pack(ff_shell_t *sh, char const *json, size_t len);
bool ff_shell_tick(ff_shell_t *sh, uint32_t now_ms);   /* true = view changed */
ff_app_state_t const *ff_shell_view(ff_shell_t const *sh);
void ff_shell_intent(ff_shell_t *sh, ff_intent_t const *in);
void ff_shell_close(ff_shell_t *sh);
```

`ff_shell_tick` pumps the transport, advances core (crew freshness, radar
compute, schedule, flare timers, water nudge), projects the result into the
`ff_app_state_t` the screens already consume, and **returns whether anything
visible changed**. That return value is the whole basis of the render
lifecycle: today the tileview rebuilds all three faces on every render
([#29](https://github.com/jakeholland/firefly/issues/29)) and LVGL teardown
blocks live re-rendering ([#17](https://github.com/jakeholland/firefly/issues/17)).
Both are the same missing concept.

`ff_shell_load_pack` takes bytes, not a path — the device has no filesystem
the way the sim does, and the app layer must not grow file I/O.

### Intents — how a screen talks to the shell

Screens stay pure renderers. They never call core, never call `mc_*`, and
never decide anything; they emit a semantic intent and the shell decides.

```c
typedef enum { FF_INTENT_SWIPE, FF_INTENT_OPEN_COMPOSE, FF_INTENT_BACK,
               FF_INTENT_CANNED_REPLY, FF_INTENT_SEND_TEXT,
               FF_INTENT_SELECT_CREW, FF_INTENT_SELECT_RALLY,
               FF_INTENT_FLARE_START, FF_INTENT_FLARE_END,
               FF_INTENT_TAKEOVER_GO, FF_INTENT_TAKEOVER_DISMISS,
               FF_INTENT_OPEN_SETTINGS, FF_INTENT_MARK_FEED_READ } ff_intent_kind_t;
```

This is the seam that replaces every stub callback in #23, and it keeps the
layering rule intact: `ff_wiring.c` remains the only file including core +
meshclient + app together.

## Behavior

- **Frame loop.** The target owns the clock and calls `ff_shell_tick` on a
  cadence it chooses (SDL frame callback in sim, an ESP-IDF task on device).
  The shell is passive — it never sleeps, blocks, or owns a thread.
- **Reconnect.** Transport loss is a first-class state, not a crash and not a
  freeze. The status bar must distinguish *connected*, *reconnecting* and
  *no link* — and, per the project's core value, a stale view during a
  reconnect must not keep presenting itself as live. Crew freshness already
  ages positions correctly; the shell must not paper over the gap by
  re-stamping arrival times on reconnect.
- **Settings persist across restart** via the injected `ff_store_t`; loaded
  during init, saved on change, never on every tick.
- **Unread clears on view.** S08 AC3 — arriving at Signals clears the badge.
  This is the shell's call, not the screen's.

## Acceptance criteria

1. `ff_route_swipe` at either end is a no-op; the sequence Radar→Now→Signals
   →(swipe right)→Signals holds position rather than wrapping to Radar.
2. A pushed modal suppresses swipe: `ff_route_swipe` returns false and leaves
   `base` unchanged while Compose is up.
3. `ff_route_set_takeover(true)` changes `ff_route_visible()` only; clearing it
   restores the exact prior base+modal, including an open Compose.
4. `ff_shell_tick` returns false on a tick where nothing observable changed,
   and true when it did — verified by ticking a shell with no traffic.
5. An inbound packet from an unpaired node produces no feed item and does not
   allocate a crew slot, driven through the **shell** rather than through
   `ff_wiring` directly (this is the regression guard for the divergence
   above; `S16_AC5_unpaired_sender_ignored_through_shell`).
6. `--dev-trust-all` is required for the sim to auto-pair; without it, live
   mode drops unpaired traffic exactly as the device would.
7. Intents route correctly: `FF_INTENT_CANNED_REPLY` with the newest feed item
   as context sends to that sender (mock sender captures dest); with an empty
   feed it broadcasts.
8. Settings written, shell closed, shell re-inited against the same store →
   values survive.
9. Transport drop mid-session moves the link state to reconnecting without
   re-stamping position ages; a crew member that went stale during the outage
   still reads stale afterwards.
10. Goldens: a takeover arriving over an open Compose, then clearing, renders
    the composer with its draft intact.

## Slices

a) `ff_route` + unit tests (pure core, no deps — mergeable alone).
b) `ff_shell` skeleton: init/tick/view/close, single `ff_wiring` pipeline,
   `live.c`'s duplicate retired, `--dev-trust-all` added.
c) Intents + wiring every stub callback from #23; screens emit, shell decides.
d) Render lifecycle: build-once/update-in-place driven by `ff_shell_tick`'s
   dirty return — closes #17 and #29.
e) Reconnect + link state in the status bar, settings persistence round-trip.

Slice (a) has no dependencies and can start immediately. (d) is the one that
should land before S15, since bring-up will otherwise inherit a renderer that
rebuilds every face on every frame.

## Open questions for the implementer

- **Does `ff_route` belong in `core/` or `app/`?** Specced as core because it's
  domain policy and must be testable without LVGL. If it turns out to need an
  app-layer type, say so in the PR rather than silently relocating it.
- **Compose draft ownership.** AC3 requires the draft to survive takeover.
  `ff_t9_t` already holds the text; confirm it lives in shell state and not in
  the screen, or AC3 cannot pass.
