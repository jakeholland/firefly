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
