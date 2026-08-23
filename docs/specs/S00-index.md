# Spec index & build order

Specs are contracts. Acceptance criteria (AC) become test names (`S01_AC3_...`). Status: ☐ unclaimed ◐ in progress ✅ merged.

| # | Spec | Depends on | Lost Lands MVP? | Status |
|---|---|---|---|---|
| S01 | core/geo — bearing, distance, heading fusion | — | YES | ☐ |
| S02 | core/crew — crew model & freshness state machine | S01 | YES | ☐ |
| S03 | meshclient — Meshtastic client library | — | YES | ☐ |
| S04 | firefly protocol — pulse/flare/rally/status packets | S03 | YES | ☐ |
| S05 | festpack — pack parser | — | YES | ☐ |
| S06 | app/radar — Radar face (live/stale/close) | S01,S02 | YES | ☐ |
| S07 | app/now — schedule engine + Now face + alarms | S05 | YES | ☐ |
| S08 | app/signals + T9 composer | S03,S04 | YES (T9 predictive: no) | ☐ |
| S09 | app/map — vector map face | S05,S02 | stretch | ☐ |
| S10 | flare flow — send/receive/takeover | S04,S06 | YES | ☐ |
| S11 | settings & persistence | — | YES | ◐ slice a merged (PR #4) |
| S12 | first-run flow | S08(T9),S11 | stretch (device can ship pre-named) | ☐ |
| S13 | sim target — SDL/headless + screenshots | — | YES (dev-critical, build FIRST) | ☐ |
| S14 | testing & CI | S13 | YES (build FIRST) | ☐ |
| S15 | esp32s3 target — ESP-IDF build + UART + sensors | most | YES (when boards arrive) | ☐ |

**Wave plan:** Wave 0 = S13+S14 (the loop itself). Wave 1 = S01,S03,S05,S11 (parallel, no deps). Wave 2 = S02,S04,S07. Wave 3 = S06,S08,S10. Wave 4 = S09,S12,S15.

Explicitly cut from v1 (post–Lost Lands): WiFi-FTM ranging, SoftAP setup portal (S12 ships name+calibrate on-device only), predictive T9 (multi-tap ships), voice, BPM, camera, festival-wide channel.
