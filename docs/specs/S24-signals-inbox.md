# S24 — Signals inbox (inbox → thread rework)

Status: draft (2026-08-28). Supersedes the Signals-face SCREEN of
[S22](S22-signals-rework.md) — the S22 **core carries over unchanged**
(`ff_sigview` presence/targeting concepts, the S22(d) send wiring + rally
confirm, the S23 live demo) — and supersedes S22 slice (c) (composer
quick-replies now live in threads). Design canvas (authoritative for layout):
https://claude.ai/code/artifact/9a79dd0f-a9ab-4282-97cf-d7eb05ef1de7 —
revised per the ux-raver review (2026-08-28); its four blockers are ACs here.

## Why

On-glass testing of the S22 screen: one round face holding a unified list +
target line + three actions is too crowded, and taps mis-land. The maintainer's
direction: a watch-native **inbox → thread** model — "more screens that are
easier to tap." The UX review confirmed the model and set the bar: legible
honest presence, a rally that cannot misfire, 44px escapes, press-down
feedback on everything.

## Screens (each does ONE thing; big targets; all round-fit via `ff_layout`)

1. **Inbox** (the Signals face): conversation list, unread-first —
   the **CREW** conversation (broadcast traffic) + one row per paired member
   (direct traffic), each with avatar/crew-color, name, last-line preview,
   mono age, numbered unread badge. Quiet members (no messages) sit below with
   an honest presence line. A solid-amber `+` FAB bleeds off the bottom-right
   rim (corner slice).
2. **Thread · CREW**: broadcasts as a scrollable message list (texts, rally
   callouts, pulse lines), newest at bottom; the same `+` FAB (scoped to CREW).
3. **Thread · 1:1**: direct messages with presence in the header
   (`SEEN <age>`, stale-amber); quick-reply chips **OMW / IN 5 MIN / PULSE**
   (one-tap sends); the `+` FAB (scoped to that member).
4. **Action popup** (over the dimmed thread): three large color-coded rows —
   **Compose** (green, "write a message"), **Rally** (violet, "meet
   somewhere"), **Pulse** (amber, "ping") — plus a ≥44px close. Header states
   the scope (`SEND TO CREW` / `SEND TO DANA`). Pulse sends immediately;
   Compose opens the S08 composer targeting the scope; Rally opens the Rally
   screen.
5. **Rally screen**: **WHERE** — a scrollable radio list: `On Me` (rally to my
   live location) pinned, then STAGES/landmarks from the festpack (scales to
   any count); **WHEN** — a visibly tappable chip cycling `Now / +15m / +30m`;
   **Send** — one button that ECHOES its payload (`Send Rally · <place> ·
   <when>`). Close ≥44px, off the rim.

**Inbox `+` scope** (UX review): the inbox FAB first opens a **recipient
picker** (CREW pinned + each member, same big-row style), then the action
popup scoped to the pick. A thread's FAB skips straight to the popup.

## Model (pure C11 in `core/`; screens render + emit intents)

### Feed learns direction — `[api]` on ff_feed
`ff_feed_item_t` gains a direction fact the wiring already knows but drops:
whether the item arrived as a **broadcast** or **addressed to me** (from
`ff_wiring_on_text`'s `to` / the private path's addressing), plus **outgoing**
items (my own sends, pushed at send time so threads show both sides). This is
what splits the CREW thread (broadcasts + my broadcasts) from 1:1 threads
(directs to/from that member). Unknown direction is stored as unknown, never
guessed.

### Inbox/thread view-model — new core module `ff_inbox`
Built per tick from `ff_feed_t` + `ff_crew_t` (same sources as `ff_sigview`):
- **Conversations**: CREW + one per paired member. Each carries: unread count,
  newest item (kind + preview + age), and for members the honest presence
  category (reuse `ff_sigview_presence` — SEEN/LOST/LINKED). Order: unread
  first (newest first within), then read-with-traffic, then quiet members by
  presence freshness. 
- **Thread view**: the items of one conversation, oldest→newest, joined to
  identity; per-thread mark-read on open (drives the badges honestly).
- **Scope**: the open thread IS the send scope — no separate target state to
  desync (S22's target-line model retires with its screen; the S22(d)
  paired-at-send re-validation stays).

### Sends (reuse S22(d) machinery)
- **PULSE** → existing pulse send, addressed by scope.
- **COMPOSE** → existing composer with `compose_to_node` = scope.
- **RALLY** → existing rally send, but the place comes from the **user's
  pick**: `On Me` = my `my_pos` + nearest-landmark naming (S22(d) behavior,
  unknown pos ⇒ the option is disabled with an honest reason, never `{0,0}`);
  a stage/landmark = its festpack position + name. **WHEN rides in the rally
  name text** (`"Main Stage +15m"`) — `ff_proto_rally_t` has no time field and
  S04 is not changed by this spec; the name must still fit
  `FF_PROTO_RALLY_NAME_MAX` (truncate the PLACE, never the time suffix, or
  refuse — never send a misleading name). **Rally to CREW keeps a confirm**:
  the Send button's payload echo + a second explicit tap (armed state), per
  S22 AC4 and the flare-armed precedent.

## Interaction quality (the systemic asks — ACs, not niceties)

- **Press-down states on EVERY tappable control** (rows, chips, FABs, popup
  rows, keypad already done): pressed = amber flash or dim, per the composer's
  merged press-feedback convention.
- **Render-key discipline**: each screen's dirty key covers exactly its
  RENDERED projection (the flare lesson / S16_AC4a): a live thread must not
  rebuild — and destroy the control under a finger — from sub-bucket churn
  (ages within the same bucket, presence within the same category, feed items
  not in this thread). Popup/rally screens are opaque overlays in the key
  while up.

## Honest-data (review-enforced)

Presence text is `SEEN <age>` / `LOST` / `LINKED` from real freshness, in a
LEGIBLE tier (stale-amber, never the dimmest gray, never hidden behind heavy
fade — the UX review's blocker 1). The CREW header shows a roster fact
(`N CREW`), never a present-tense "N here". Ages are labeled so message-age
and seen-age can't be confused (`SEEN 1M` vs `1 MIN`). No fabricated
positions/times anywhere (rally rules above).

## Empty/edge states (UX review)

- No crew paired: inbox shows an honest "no crew linked yet" hint (pairing is
  out of scope, S22 note stands) — never a blank dead-looking face.
- Crew but no traffic: quiet-member rows only, with presence; CREW row shows
  "no signals yet".
- Everyone stale: rows keep the legible stale treatment (fade is
  de-emphasis, never the encoding of staleness).
- Long names/places ellipsize; timestamps never collide.

## Acceptance criteria

- **AC1** `[api]` ff_feed direction: broadcast/direct/outgoing recorded from
  the wiring truthfully; unknown stays unknown. Unit-tested.
- **AC2** `ff_inbox` (pure C11, zero heap, unit-tested): conversations with
  honest unread counts + previews + presence; thread extraction; per-thread
  mark-read; ordering as specified.
- **AC3** Inbox renders the conversation model (badges, previews, presence in
  the legible stale tier); tapping a row opens its thread; the FAB opens the
  recipient picker → popup. Goldens + hit sweep.
- **AC4** Threads render their items both-ways (in/out), mark read on open,
  and expose quick chips (1:1) / the FAB (both). Quick chips send one-tap to
  the thread's scope.
- **AC5** Popup: three actions + close, scope stated; Pulse sends immediately
  to scope; Compose opens the composer at scope; Rally opens the Rally screen.
- **AC6** Rally screen: scrollable WHERE (On Me pinned; festpack places),
  tappable WHEN, payload-echoing Send; crew-wide send takes a second (armed)
  tap; On Me disabled honestly without a fix; name+when fits the wire rule.
- **AC7** Every tappable control has a pressed state; all hit targets ≥44px
  with ≥8px adjacency incl. back/close (sweep-verified); nothing clips the
  glass.
- **AC8** Render keys cover exactly the rendered projection per screen —
  churn tests in the flare mold (same-bucket age change ⇒ clean; content
  change ⇒ dirty).
- **AC9** Edge states render as specified (fixtures for: no crew, no traffic,
  all-stale).

## Slice plan (normal flow: worktree agent → tiered review → squash)

- **(a) `[api]` core**: ff_feed direction + the `ff_inbox` module + tests.
  (Tier 3.)
- **(b) inbox screen + nav**: scr_signals becomes the inbox; thread-stack
  routing/intents; recipient picker; fixtures/goldens. (Tier 2 + ux pass.)
- **(c) thread screens**: CREW + 1:1, quick chips, mark-read wiring,
  press-down everywhere, churn keys. (Tier 2 + ux pass.)
- **(d) popup + rally**: action popup, Rally screen, send routing on the
  S22(d) seams, WHEN-in-name, confirm. (Tier 3 — sends + honesty.)

## Out of scope / notes

- Pairing UI still separate. The S22 screen's goldens/fixtures are replaced by
  (b)/(c)'s. The S22 `## Questions` on rally place is resolved by this spec
  (picker + On Me + WHEN); COMPOSE-doesn't-reset-target is moot (scope = the
  thread you're in). A protocol-level rally time field is an open S04 question
  if WHEN-in-name proves too crude in the field.
