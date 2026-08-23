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
5. Goldens: `signals_feed.json`, `compose_mid.json` (matching mockup content) pass.
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
