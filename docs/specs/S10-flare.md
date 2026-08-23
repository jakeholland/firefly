# S10 · flare flow

## Purpose
The loudest signal: send/receive come-find-me. Mockups "Radar — close range" (send button) and "Flare — takeover" (receive) are layout authority.

## Behavior
- **Send:** FLARE button (close-range face) or Crew row long-press → confirm sheet ("FLARE to crew?") → `ff_proto_encode_flare(dur=300)` broadcast with want_ack; sender UI enters "FLARING" state (own screen pulses amber, status line "you are flaring — crew arrows locked on you"), cancel button sends FLARE_END. Auto-end at dur.
- **Receive (paired sender only):** full-screen takeover regardless of current face; haptic pattern (3 long) **overrides quiet hours**; GO → radar with sender force-selected + flare-lock (selection cycling disabled until flare ends/dismissed); DISMISS → back, feed item remains. Takeover expires at dur or FLARE_END.
- Multiple flares: newest wins the takeover; feed keeps all.
- State machine in core (`ff_flare.h`): IDLE/SENDING/RECEIVED(node,expiry)/LOCKED; transitions pure, tested; app renders.

## Acceptance criteria
1. State table: 12 transition tests incl. expiry, FLARE_END, newest-wins, unpaired-sender-ignored.
2. Receive during quiet hours still fires haptic callback (flag asserted in mock).
3. GO locks selection: `ff_crew_select_next` no-ops while LOCKED; unlock on expiry restores cycling.
4. Sender auto-end at dur: clock-advance test emits FLARE_END exactly once.
5. Goldens: `flare_takeover.json`, `radar_flare_locked.json` (radar with lock chip), `flaring_self.json`.

## Slices
a) state machine + tests · b) takeover + sender UI + goldens (needs S06 shell).

## Amendments
- **2026-08-23, PR #15:** Independent review of slice (a) flagged that the original `ff_flare.h` shape — one shared "current flare" slot for IDLE/SENDING/RECEIVED(node,expiry)/LOCKED — conflated two, and in one case three, genuinely independent facts. Two rulings, both implemented in PR #15:
  - **Ruling 1 (HIGH — outbound send and inbound receive are independent):** if a paired crew member's flare arrives while I am mid-send, the single-slot design silently overwrote my `SENDING` state with the incoming `RECEIVED`, with no `FLARE_END` emitted for my own flare — `ff_flare_send_cancel()` then permanently no-op'd (state was no longer `SENDING`) for the rest of my flare's duration, with zero recourse. Ruling: "I am sending a flare" and "I am receiving someone's flare" are separate, concurrent state — they are legitimately both true at once in the field (I flare for help; a crew member flares back or independently 20s later; neither is stale or wrong). `ff_flare_t` now tracks `sending`/`send_expiry_ms` as a field group that nothing inbound ever reads or writes; `ff_flare_send_cancel()` works regardless of any incoming flare, and each of the two timers (my send, the incoming takeover) expires independently in `ff_flare_tick`.
  - **Ruling 2 (MEDIUM — an established LOCK is never silently replaced):** the spec's "newest wins the takeover" line is genuinely ambiguous about whether an already-`GO`'d LOCKED selection is "the takeover" a newer flare can win away, or a stickier commitment the user already made. Ruling: a newer paired flare MAY still raise its own full-screen takeover (the urgency framing is real — the crew member needs to be seen) but does NOT touch an existing lock. If I've pressed GO and am walking toward Dana (LOCKED to her), and Kev flares, I see Kev's takeover while my selection stays locked on Dana underneath; DISMISS on Kev's takeover returns me to the intact Dana lock; GO on Kev's takeover is the explicit decision that switches the lock to Kev. Rationale: silently changing where the arrow points mid-walk without an explicit choice is exactly the "confident but wrong arrow" failure this product exists to prevent (CLAUDE.md: "honest data over pretty data" — the currently-locked node must always be a fact the user chose, not one the network chose for them). `ff_flare_t` now tracks the pending takeover (`takeover_active`/`takeover_node_id`/`takeover_expiry_ms`) and the lock (`locked_node_id`/`locked_expiry_ms`) as separate fields; only `ff_flare_go()` writes `locked_node_id`, only `ff_flare_dismiss()`/its own expiry/a matching FLARE_END clear it.

  Acceptance criterion 1's "IDLE/SENDING/RECEIVED(node,expiry)/LOCKED" state-table language above predates this amendment; see `ff_flare.h`'s doc comment for the current three-independent-fields shape (`sending`, `takeover_active`+node+expiry, `locked_node_id`+expiry) that replaces it, and `test_flare.c`'s `S10_ruling1_*`/`S10_ruling2_*` tests for the regression coverage.
