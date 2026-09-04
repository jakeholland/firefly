# S24 — Signals inbox (inbox → thread rework)

Status: draft (2026-08-28). Supersedes the Signals-face SCREEN of
[S22](S22-signals-rework.md) — the S22 **core carries over unchanged**
(`ff_sigview` presence/targeting concepts, the S22(d) send wiring + rally
confirm, the S23 live demo) — and supersedes S22 slice (c) (composer
quick-replies now live in threads). Design canvas (authoritative for layout):
https://claude.ai/code/artifact/9a79dd0f-a9ab-4282-97cf-d7eb05ef1de7 —
revised per the ux-raver review (2026-08-28); its four blockers are ACs here.

**Code identifiers renamed to inbox/lineup on 2026-09-02** (mechanical,
no behaviour change — the on-glass strings were already Inbox/Lineup as of
2026-09-01): `scr_signals.{c,h}` -> `scr_inbox.{c,h}`, `FF_APP_FACE_SIGNALS`
-> `FF_APP_FACE_INBOX`, `FF_SIG_SUB_*` -> `FF_INBOX_SUB_*`, etc. This spec's
own title/filename are unchanged (history); `ff_sigview` is unchanged (it
names the reused presence/target-vocabulary data, not this screen).

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
- **(b) inbox screen + nav**: scr_inbox becomes the inbox; thread-stack
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

## Amendments

- **2026-09-02, maintainer decision — PULSE retired end to end:** this
  document's PULSE references (the quick-reply chip row "OMW / IN 5 MIN /
  PULSE", the action popup's "Pulse (amber, 'ping')" row, "PULSE sends
  immediately") describe the as-built (c)/(d) slices at the time; left as
  historical record. As of 2026-09-02 PULSE is retired everywhere (see
  `S04-firefly-protocol.md`'s Amendments): the thread quick chips are OMW /
  IN 5 MIN / FLARE (FLARE already replaced the outbound Pulse chip/popup
  row since PR #129, "in send to crew we should have flare not pulse");
  the "pulsed you" / "pulsed the crew" thread wording and its amber pulse
  mark are removed from `scr_inbox.c`'s rendering, and the demo feed
  (`S23-demo-feed.md`) never generates a PULSE kind. Incoming rendering for
  a retired-type wire frame is honestly nothing: no feed item, no banner
  (`S26-device-lifecycle.md`'s Notifications section) — see `ff_wiring.h`'s
  header note for the drop ruling.

- **2026-09-03, maintainer decision — quick-reply chips inline in the CREW
  thread's free space** (maintainer's question: "the group thread has a
  bunch of unused blank space — should we put the quick replies there or
  some other affordance?"; decision: put them there). Screen 2 ("Thread ·
  CREW") above is amended: it now ALSO carries the quick-reply chips
  screen 3 describes for 1:1 — OMW / IN 5 MIN / FLARE, same intents
  (`FF_INTENT_CANNED_REPLY` / `FF_INTENT_INBOX_FLARE`), same shared
  chip-building code (`scr_inbox.c`'s `inbox_build_chips`) — rendered
  INLINE, anchored just above the FAB row, whenever the message list does
  NOT overflow the band and leaves at least one chip row's worth of free
  space below the last bubble. When the thread overflows (no free space to
  give), CREW keeps the pre-amendment layout exactly: no inline row, the
  FAB (Compose/Rally/Flare via the popup) the only way to reach a send —
  the row never covers a message. AC4 is amended accordingly (was "expose
  quick chips (1:1)"; now "expose quick chips: 1:1 unconditionally, CREW
  when the band has free space").

  **New AC4a — CREW inline quick-reply chips (free-space gated):**
  - the free-space test is `band_bottom - last_bubble_bottom >=
    chip_h + 2*chip_gap` (`scr_inbox.c`'s `FF_INBOX_CHIP_FREE_MIN_PX`,
    60px = 44px chip height + 2×8px clearance) — tested by
    `S24c_crew_thread_short_shows_quick_chips_tap_omw_emits_canned_reply`
    (2-message CREW thread, `tests/fixtures/inbox_thread_short.json` /
    its golden) and `S24c_crew_thread_empty_shows_quick_chips` (0
    messages, the maximum possible free space);
  - an overflowing CREW thread renders NO inline row, unchanged from
    before this amendment — `S24c_crew_thread_long_has_no_quick_chips`
    (the existing 12-message fixture; its golden is byte-identical to
    before this PR, confirming the "no free space" branch is pixel-for-
    pixel unchanged);
  - a message arriving that pushes a previously-short thread into
    overflow drops the row on the next rebuild, never painting it over
    the new message — `S24c_crew_thread_chip_row_disappears_when_new_
    messages_overflow`. The render key already covers this: `ff_shell.c`'s
    `shell_render_key` starts with `memcpy(key, v, sizeof(*key))`, so
    `key->inbox.thread.msg_count` is the raw, uncoarsened count before any
    of that function's later coarsening — a message count change already
    marks the S24 dirty key dirty, unconditionally. No `ff_shell.c` change
    was needed for this amendment;
  - a chip's own hit target, press feedback, and drag-off-cancels
    behavior are unchanged (shared code with the 1:1 strip) —
    `PL_inbox_crew_chip_drag_off_emits_nothing` mirrors the existing
    1:1 `PL_inbox_chip_drag_off_emits_nothing`;
  - `test_face_hit_targets.c`'s fixture sweep picks up
    `inbox_thread_short.json` automatically (it iterates every
    `tests/fixtures/*.json`) — no sweep code change needed, 0 violations;
  - mutation-verified (fresh rebuild, object hash confirmed changed):
    forcing the inline row to always render (ignoring the free-space
    gate) fails `S24c_crew_thread_long_has_no_quick_chips` AND
    `S24c_crew_thread_chip_row_disappears_when_new_messages_overflow`;
    reverted with a targeted one-line restore, not `git checkout`.

  **Interpretation call, flagged for the maintainer:** the 1:1 thread's
  own quick-reply strip stays UNCONDITIONAL (not free-space-gated) even
  though this amendment's task brief asked for "the same code path" for
  both thread types — `S24_direct_thread_shows_at_least_4_rows_at_rest`
  is a prior, already-reviewed AC (PR #149) whose own comment says
  explicitly that "chips present... must not be satisfiable by silently
  removing the quick-reply strip to buy the 1:1 thread more room," and
  its fixture (`s24_make_direct_thread_long`, 12 messages) genuinely
  overflows the 1:1 band — so applying the CREW free-space gate to 1:1
  too would have made a long 1:1 thread's chips disappear, regressing
  that AC. Read literally, "the group thread has a bunch of unused blank
  space" is a CREW-specific complaint anyway (1:1's band,
  `FF_INBOX_THREAD_LIST_H_1TO1`, is already 18px shorter than CREW's
  specifically to reserve the chip strip's room unconditionally, which
  is a different, already-shipped solution to the same "give the chips a
  home" problem). "Same code path" is honored at the level that matters
  for reuse — the SAME chip-building function (`inbox_build_chips`) and
  the SAME emitters/intents serve both thread types; only the GATING
  condition differs, and only because 1:1's is a compile-time constant
  (always true) while CREW's is genuinely data-dependent. Also
  interpretation: the task brief says "the FAB popup remains the way to
  reach them" for the overflow case — the action popup
  (`inbox_build_popup`) actually has only Compose/Rally/Flare rows, no
  OMW/IN 5 MIN rows (never did, for either thread type); read as "the FAB
  remains a way to send an equivalent message" (Compose, or Flare),
  not literally "the popup also carries OMW/IN 5 MIN," since it doesn't.
  Both readings are the maintainer's to override.

  **Fixture/golden note:** none of the three pre-existing thread fixtures
  changed pixel content. `inbox_thread_crew.json` (4 messages, 184px
  content in the 200px band) has only 16px free — under the 60px floor,
  so it still renders no inline row, unchanged.
  `inbox_thread_crew_long.json` overflows outright, unchanged.
  `inbox_thread_direct.json`'s 1:1 chips were already unconditional, so
  the new gate (unconditionally true for 1:1) changes nothing. Verified
  via `run_goldens.sh` (no `--update-golden`) before generating the one
  new golden this amendment adds
  (`tests/fixtures/inbox_thread_short.json` /
  `tests/golden/inbox_thread_short.png`): `git status --porcelain
  firmware/tests/golden/` showed only the new file as untracked, every
  other golden byte-identical on disk.
