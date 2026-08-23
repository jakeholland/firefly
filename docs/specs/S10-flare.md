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
