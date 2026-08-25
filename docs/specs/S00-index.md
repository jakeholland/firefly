# Spec index & build order

Specs are contracts. Acceptance criteria (AC) become test names (`S01_AC3_...`). Status: ☐ unclaimed ◐ in progress ✅ merged.

| # | Spec | Depends on | Lost Lands MVP? | Status |
|---|---|---|---|---|
| S01 | core/geo — bearing, distance, heading fusion | — | YES | ✅ merged (PR #6) |
| S02 | core/crew — crew model & freshness state machine | S01 | YES | ✅ merged (PR #11) |
| S03 | meshclient — Meshtastic client library | — | YES | ✅ merged (PR #7) |
| S04 | firefly protocol — pulse/flare/rally/status packets | S03 | YES | ✅ merged (PR #10) |
| S05 | festpack — pack parser | — | YES | ✅ merged (PR #5) |
| S06 | app/radar — Radar face (live/stale/close) | S01,S02 | YES | ✅ merged (PR #13 compute, #16 face+shell) |
| S07 | app/now — schedule engine + Now face + alarms | S05 | YES | ✅ merged (PR #9 engine, #21 face) |
| S08 | app/signals + T9 composer | S03,S04 | YES (T9 predictive: no) | ✅ merged (PR #14 engine, #25 feed+UI) |
| S09 | app/map — vector map face | S05,S02 | YES | ✅ merged (PR #73) |
| S10 | flare flow — send/receive/takeover | S04,S06 | YES | ✅ merged (PR #15 core, #20 UI) |
| S11 | settings & persistence | — | YES | ◐ a,b merged (PR #4, #68); c = ZONES backend |
| S12 | first-run flow | S08(T9),S11 | stretch (device can ship pre-named) | ☐ |
| S13 | sim target — SDL/headless + screenshots | — | YES (dev-critical, build FIRST) | ✅ merged (PR #2, #12, #19) |
| S14 | testing & CI | S13 | YES (build FIRST) | ✅ merged (PR #2, #12, #19) |
| S15 | esp32s3 target — ESP-IDF build + UART + sensors | most | YES (when boards arrive) | ☐ |
| S16 | app/shell — event loop, routing, input dispatch, wall clock | S02,S03,S06,S08,S10,S11,S13 | YES | ✅ merged (PR #36,#37,#46,#54,#56,#58,#59,#60,#61) |

**Wave plan:** Wave 0 = S13+S14 (the loop itself). Wave 1 = S01,S03,S05,S11 (parallel, no deps). Wave 2 = S02,S04,S07. Wave 3 = S06,S08,S10. Wave 4 = S16, then S09,S12,S15.

S16 was not in the original wave plan — the specs covered every core module, every face and both build targets, but nothing owned *the running application*. The gap only became visible once every face had landed and their controls had nowhere to call into (#23). It precedes S15 deliberately: bring-up should plug a display driver into a loop that already works, not invent the loop on unfamiliar hardware.

Explicitly cut from v1 (post–Lost Lands): WiFi-FTM ranging, SoftAP setup portal (S12 ships name+calibrate on-device only), predictive T9 (multi-tap ships), voice, BPM, camera, festival-wide channel.
