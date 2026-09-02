# S08 · app/signals — feed, replies, T9 composer

## Purpose
The social face: event feed (pulses/rally/messages/statuses), canned replies, and the T9 composer. Mockups "Signals" and "Compose — T9" are layout authority.

## Core: event feed (`core/include/ff_feed.h`)
```c
typedef enum { FEED_PULSE, FEED_TEXT, FEED_RALLY, FEED_STATUS, FEED_FLARE } ff_feed_kind_t;
typedef struct { ff_feed_kind_t kind; uint32_t from_node; uint32_t at_ms;
                 char text[64]; bool unread; } ff_feed_item_t;
// Ring buffer, newest first, cap 32; unread count drives a badge on the page dot.
void ff_feed_push(ff_feed_t *f, ff_feed_item_t const *it);
```
Wiring: mc events + ff_proto decodes → feed pushes (app glue `app/ff_wiring.c` — the ONLY file allowed to include core+meshclient+app together).

## Core: T9 engine (`core/include/ff_t9.h`) — reusable lib
```c
void ff_t9_reset(ff_t9_t *t);
void ff_t9_key(ff_t9_t *t, uint8_t key /*0-9*/, uint32_t now_ms); // multi-tap: same key <900ms cycles
void ff_t9_backspace(ff_t9_t *t);
void ff_t9_space(ff_t9_t *t);          // key 0
char const *ff_t9_text(ff_t9_t const *t);   // committed + pending char
```
v1 = **multi-tap only** (2→abc cycling, 900 ms commit timer, key 1 cycles `. , ? !`). Predictive + pack dictionary = v1.5 spec addendum; API reserves `ff_t9_suggest()`.

## Behavior
- Canned replies OMW / 5 MIN / PULSE send instantly (text "omw" / "5 min" / PULSE packet) to the feed item's sender if a reply-context exists, else broadcast.
- Composer: reached from Signals "+"; TO = selected crew member; SEND → `mc_send_text`; sent item appears in feed optimistically with pending marker (ack UX v1.5).
- Rally row tap → sets rally as radar/map target (S06 selection can target landmarks: `ff_crew_select_rally()`).

## Acceptance criteria
1. T9: key sequence for "omw" (666,6,9 with waits) yields "omw"; rapid same-key cycles a→b→c→a; timer commits; backspace removes pending first then committed.
2. Punctuation key and space behave per spec; 160-char cap enforced.
3. Feed: 33rd push evicts oldest; unread count increments and clears on face view.
4. Wiring: injected mc on_private PULSE from paired node → feed item + haptic callback; from unpaired node → dropped (crew filtering).
5. Goldens: `inbox_feed.json`, `compose_mid.json` (matching mockup content) pass.
6. Canned OMW from a pulse context sends to that sender (mock mc captures dest).

## Slices
a) T9 engine + tests · b) feed + wiring + crew filter · c) Signals render + goldens · d) Compose render + input plumbing + goldens.

## Amendments

- **2026-08-23, owner ruling (Compose keypad: digits/symbols question).** Slice (a)'s T9 engine is multi-tap letters only (`0`=space, `1`=punctuation, `2`-`9`=letters) — it cannot produce digits, so the original slice (d) brief said the Compose keypad must never print a digit legend it can't honor. That left an open product question: how does anyone type "stage 3" or "5 min" at all? Ruling: the keypad gets **mode pages**, cycled by a dedicated mode key that always shows the current mode's name (never a mystery toggle) —
  - **ABC** (default): the merged multi-tap letter behavior, unchanged. Legends show letter groups only, per the original brief — this mode still can't produce a digit, so it still never claims to.
  - **123**: a numeric page where every key types its literal digit (1-9, 0). Legends show the digits.
  - **SYM**: a page of curated symbols/shortcuts — the owner's framing: "could have emojis or emoticons there too... the crew's real vocabulary is 🔥💀👽✨🙏❤️ more than prose."

  Implementation guidance (from the ruling) was tiered by preference: (1) real emoji via a curated custom LVGL font subset, which would need `ff_t9.h` extended to accept multi-byte UTF-8 insertion; (2) an acceptable fallback — ABC/123 plus an ASCII-emoticon SYM page, which transmits as plain text every Meshtastic receiver (including older pucks and phone apps) already renders correctly.

  **Shipped: tier 2 (the ASCII-emoticon fallback).** Font subsetting/generation tooling and a licensed emoji glyph source are a real asset-authoring task that doesn't fit alongside this PR's feed/wiring/Signals/Compose scope — tracked as a follow-up: https://github.com/jakeholland/firefly/issues/22. `core/include/ff_t9.h` gained one new, purely additive entry point either tier needed regardless — `ff_t9_insert_text()`, which commits any pending char then atomically appends a plain-ASCII string (all-or-nothing against the 160-char cap) — because even tier 2's multi-character shortcuts (`:)`, `<3`, ...) have no way to express as a single per-key multi-tap press. Every pre-existing `ff_t9` test still passes unmodified; `ff_t9_insert_text` has its own new test coverage in `core/tests/test_t9.c` (atomic insert, cap all-or-nothing rejection leaving `t` untouched, commits-pending-first, and the classic "backspace must remove one character of a multi-char shortcut at a time" regression check).

  Acceptance criterion 2's "punctuation key and space behave per spec" predates this amendment and is unaffected (ABC mode's key 1/key 0 behavior is unchanged) — this amendment only adds the 123/SYM pages alongside it. See `firmware/app/screens/scr_compose.c`'s header comment for the render-side detail, and `compose_abc.json`/`compose_123.json`/`compose_sym.json` + their goldens for all three pages.

- **2026-09-02, maintainer device feedback (SEND placement + key-size audit).** On-glass report: SEND sat immediately adjacent to SPACE in the bottom DEL/0/SEND row (8px apart, the bare adjacency floor) and was getting hit by accident. Ruling, going forward: **SEND must never be adjacent to SPACE or DEL** — no shared edge, and specifically at least one key-width (or a >=12px dead gap) of separation — with SPACE staying centered in whatever row it occupies. Shipped fix: SEND and the mode chip trade places. SEND moves to the composer header's top-right corner (the "watch convention": send lives next to the message field, not behind the space bar); the mode chip drops into SEND's old bottom-row slot, which is harmless to fat-finger (mis-tapping it only cycles ABC/123/SYM/T9, never sends). The two controls now sit ~269px apart vertically with no shared row at all — see `firmware/app/screens/scr_compose.c`'s header comment ("SEND relocation") for the full geometry and the maintainer-gap test (`test_scr_intent.c`'s `S99_compose_space_and_send_clear_the_maintainer_gap_ask`).

  Same pass re-ran the "largest key that still fits the glass" search (`ff_layout_safe_margin_x`, never hand math) and found 2px of unclaimed headroom: the T9 grid's key height grows **54px -> 56px** (`FF_COMPOSE_KEY_H`/`FF_COMPOSE_BOTTOM_ROW_H`), the ceiling for a three-column bottom row at this Y (57px measures under the 44px hit floor at the pole). Freed from sharing its row with SEND, the bottom row's two non-SPACE keys (DEL, mode chip) are now sized to the exact 44px hit floor and SPACE — this keypad's most-tapped key by far — takes every remaining pixel.

  Every clickable control on this screen (T9 keys, SPACE, DEL, SEND, the mode chip, the PRED candidate chips) was audited for (a) `LV_STATE_PRESSED` visual feedback and (b) a full-shape hit area (the whole visible control is the tap target, not just its label) — both are now asserted directly (`test_scr_intent.c`'s `S99_compose_every_key_has_press_state_feedback`, `S99_compose_pred_candidates_have_press_state_feedback`, and `S99_compose_send_full_area_tap_emits_send_exactly_once`), on top of `test_face_hit_targets.c`'s existing size/adjacency/circle-containment sweep at the new sizes.

- **2026-09-02, PR #148 code review (2 blocking, 2 should-fix — all four addressed in the same PR).** Independent review of the SEND-relocation amendment above found four gaps, all fixed:
  1. **PRESS_LOCK (blocking).** LVGL's default `LV_OBJ_FLAG_PRESS_LOCK` keeps a control "pressed" — and still fires `CLICKED` on release — even after a real touch has slid off it, so a finger that pressed SEND (or any key) and dragged away before lifting still sent the message. Fixed the same way `scr_launcher.c` (#145) already fixed it for the launcher's own drag-across hazard: `compose_clear_press_lock()` clears the flag on every clickable control in `scr_compose.c` (all seven `lv_button_create` call sites). Proven with a REAL synthetic-indev drag (not a synthetic click event) in `test_scr_intent.c`'s `S99_compose_drag_off_send_emits_nothing` / `S99_compose_drag_off_key_emits_nothing`; mutation-checked (re-enabling PRESS_LOCK on SEND fails the drag-off test — see this PR's body for the exact output).
  2. **TO label covered by SEND (blocking).** "TO: DANA" rendered as "TO: DAN" — the label was centered across the whole puck with no width bound, so it ran UNDER SEND (drawn after it, z-order on top) instead of stopping before it. Fixed: TO is now bounded to the honest available band between BACK's right edge and SEND's left edge (each with the standard 8px clearance) with `LV_LABEL_LONG_MODE_DOTS`, so anything that doesn't fit ellipsizes cleanly instead of being painted over. New fixture/golden `compose_to_long` (`to_name: "WHOLE CREW"`) proves the long-name case.
  3. **SEND's corner past the bezel-margin bar (should-fix).** `compose_safe_margin_x`'s chord-at-one-Y approximation guarantees a ROW's full horizontal span is safe at that row's own farthest Y line, but says nothing about a single CORNER control's true 2D distance from center — SEND's own farthest corner measured ~201px from the puck's center, past the intended `radius(206) - safety(10) = 196px` bar, even though the row-wide chord math it was originally positioned with was exactly right for its own (different) question. Fixed with `compose_send_x()`, which solves the corner question directly (same shared `ff_layout_chord_half_width` primitive, evaluated against the safety-reduced radius). SEND also narrowed 64px -> 48px so its now-further-left right edge doesn't eat further into TO's reserved band. New corner-distance test: `S99_compose_send_corner_clears_bezel_margin_bar`.
  4. **DEL regressed 52x54 -> 44x56 (should-fix).** Pinning DEL to the bare 44px hit floor (the first-draft shape of the SEND-relocation fix) shrank it from its pre-amendment 52px width — a regression in a "make buttons bigger" change. Fixed: MODE (this row's least-tapped key) is the one pinned to the floor; DEL gets a dedicated 52px back. The bottom row was also raised 2px (`FF_COMPOSE_BUBBLE_GRID_GAP` 8->6, the maximum this file's own PRED-strip-clearance assert allows) to buy back some of the row width that redistribution alone couldn't — worth ~8px of row width near the pole, not just 2px. Final measured widths: DEL 52px, SPACE 52px, MODE 44px (all at 56px height).

- **2026-09-02, PR #148 code review round 3 (blocking, round 2's TO fix regressed).** Round 2's fix bounded TO to the ~52px gap between BACK and SEND in the header row — that made even an ORDINARY, short recipient name unreadable ("TO: DANA" -> "TO: D..."), not just a genuinely long one; ellipsis is only acceptable for names that are actually too long. Fixed: TO moves to its own row just below the header (`FF_COMPOSE_TO_Y`), where BACK/SEND aren't competing for the space — measured (a standalone LVGL harness against `lv_font_montserrat_14`, this PR's body has the full table) at a deliberately chosen 130px width that renders every demo crew name (DANA, KEV, RILEY, MAYA, SAM), "EVERYONE", and "CREW" **in full**, while a genuinely long name (`compose_to_long.json`'s "WHOLE CREW") still ellipsizes with `LV_LABEL_LONG_MODE_DOTS`. The row's own honest chord-derived width at this Y is ~288px — comfortably clearing a naive "≥200px" bound — but was measured to be wide enough that **no** value `to_name` can hold (`FF_APP_NAME_LEN` caps it at 15 chars) ever needs to truncate there, which would make the ellipsis path dead code for any real input; 130px is the deliberate, narrower, MEASURED choice that keeps the DOTS path provably reachable while still comfortably exceeding what any real name needs.

  The T9 grid did not move (`FF_COMPOSE_GRID_Y` stays the literal value 142, frozen from round 2's should-fix 4 — this round's review explicitly required it): the new TO row's vertical cost comes entirely out of the message bubble (`FF_COMPOSE_BUBBLE_H`, now a *derived* quantity — `GRID_Y - BUBBLE_GRID_GAP - BUBBLE_Y` — rather than an independently chosen literal, so it can never silently drift out of sync with GRID_Y again), which shrinks from 60px to 46px.

  PRED mode has no spare vertical room to duplicate this new row (`FF_COMPOSE_PRED_STRIP_Y` is already pinned to the maximum this file's own PRED-strip-clearance assert allows, with the draft line's own y sitting only 2px below the header) — the recipient is instead prepended as the first segment of the PRED draft's own flex row (`compose_build_pred_draft`), reusing that line's existing height rather than needing a second one. Rendered as `"DANA: omw to the|"`, dim-colored, no separate "TO:" — the composer context already implies it.

  New tests (`test_scr_intent.c`): `S99_compose_to_short_name_renders_in_full_no_dots` (direct assertion the rendered text contains no `"..."`), `S99_compose_to_every_demo_crew_name_renders_in_full`, `S99_compose_to_long_name_ellipsizes_cleanly`, `S99_compose_pred_mode_shows_recipient_on_draft_line` (also guards against double-rendering both the dedicated TO row and the draft-line prefix at the same y — the exact bug an earlier draft of this fix visually shipped, caught before commit).
