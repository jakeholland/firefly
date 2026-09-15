# Puck UX usability review — tap targets, fluidity, flows

**Date:** 2026-09-15 · **Reviewer:** interaction design pass (round-glass / LVGL / capacitive touch)
**Asked for:** Jake, 2026-09-15 — "queue a UX usability pass for the puck; button size as well as fluidity of UX."
**Scope:** firmware UX only. This document changes no firmware. It adds one reusable measurement
script (`firmware/tools/check_glass_clipping.py`).

---

## 0. Method — what is measured, and what is not

Everything numbered below came out of a tool, not an eye:

| what | how |
|---|---|
| hit rects for every control on every committed fixture | `FF_TAP_INVENTORY=1 ./targets/sim/test_tap_target_sizing` — walks the real LVGL tree `ffsim` builds and reads `lv_obj_get_click_area` (box **plus** `ext_click_area`). Sweep summary: **119 fixtures, 601 clickable elements, 0 violations** — 71 of the 119 build any clickable at all. |
| adjacent-target gaps | pairwise edge-to-edge sweep over that same dump (containment pairs excluded) |
| painted content outside the bezel's glass circle | `firmware/tools/check_glass_clipping.py` (new, this PR) over all 119 goldens |
| per-face LVGL tree build cost | `cmake --build … --target ff_bench && ./ff_bench` (host: AppleClang 21, Apple Silicon) |
| glyph cap heights | pixel bounding boxes measured out of the committed goldens |
| colour contrast | WCAG 2.1 relative-luminance ratios computed from `ff_theme.h`'s own hex constants |
| every face rendered | `firmware/tests/run_goldens.sh` — **119/119 pass, 0.0000 % diff**, clang build, this worktree |

Build used: `cmake -S firmware -B … -DFF_TARGET=sim` + `--build -j8`, AppleClang 21.0.0, clean.
(Per CLAUDE.md, CI's GCC is the authority; nothing here depends on the compiler.)

### 0.1 The px→mm conversion is wrong by 2.9 %, and the two sources disagree

`ff_theme.h:428` defines `FF_THEME_PX_PER_MM 11.44f`, derived in `docs/hardware/tap-targets.md`
as `412 px / 36 mm`. That mixes two different measurements: **412 px is the pixel array**, and
**36 mm is the bezel window** — which is only 400 px of that array (`FF_THEME_GLASS_R` 200).
The owner's own brief uses `412 px / 37 mm ⇒ 11.13 px/mm`, and 1.46 in = 37.08 mm, which
reconciles cleanly: the 412 px array spans 37.08 mm, the 400 px visible circle spans 36.0 mm.

Consequence: every millimetre figure in `docs/hardware/tap-targets.md` is ~2.9 % **pessimistic**
(80 px reads as 6.99 mm; at 11.13 px/mm it is 7.19 mm). No verdict in this review changes under
either value, so this is a **minor** finding — but it is exactly the class of "a number that
guards something should be the right number" this codebase cares about. **Settle it with a
caliper on the bench** (§7) and fix the constant. **Effort: S.**

Every mm figure below uses the repo's conservative **11.44 px/mm** so it can be compared directly
against `tap-targets.md`.

### 0.2 What this review is standing on

`docs/hardware/tap-targets.md` (PR #311-era sizing pass) is unusually good work and this review
does **not** re-litigate it. Launcher, Radar, Compose and Inbox were measured, derived to their
geometric ceilings, and pinned by tests. The findings below are mostly about **the faces that pass
did not cover**, the **feedback layer** (which it did not touch), and **flow**.

---

## 1. Ranked findings

| # | severity | finding | measure |
|---|---|---|---|
| 1 | **blocker** | You cannot choose who the compass points at. `FF_INTENT_SELECT_CREW` is emitted by **no screen**; the Radar builds exactly **one** clickable object in the whole file. | `ff_shell.c:8041`, `scr_radar.c` |
| 2 | **blocker** | No press feedback at all on Radar's FLARE, every Settings control, every Crew control, SHOW CODE and compass-cal: no `LV_STATE_PRESSED`, no haptic HAL, and the UI tick sound defaults OFF and fires on CLICKED not PRESSED. | `scr_radar.c:1042`, `scr_settings.c:496`, `app_main.c:2130`, `scr_nav.c:70` |
| 3 | **major** | Tapping the **already-active** segment of a Settings toggle inverts the setting. Includes SCREEN NORMAL/FLIPPED — one stray tap turns the display upside down. | `scr_settings.c:613,671,693` |
| 4 | **major** | The notification banner sits **on top of** the status row: clock, `LINKED`/`NO RADIO` and battery % are all occluded while a banner is up. | `banner_on_radar.png`, `scr_banner.c:43` |
| 5 | **major** | The 80 px primary floor is enforced on **4 of 12** faces. Flare takeover, power menu, Settings, Crew, SHOW CODE, crew-op confirms and compass-cal are guarded only by the 44 px (3.85 mm) absolute floor. | `test_tap_target_sizing.c:142–190` |
| 6 | **major** | Four of the six launcher destinations (Lineup, Map, Music, Radar-not-CLOSE) build **zero** on-glass controls. The only exit is the left-rim swipe — the gesture S28 needed **three** amendment rounds to make work on real glass. | inventory: no `lineup_*`/`map_*`/`music_*` clickables |
| 7 | **major** | `SETTINGS` costs **71 ms** of pure LVGL tree construction per rebuild — 93× MAP, 20× RADAR — and nothing measures it. The ~520 ms Map stall was fixed **at the Map**; the stall *class* moved to the most-rebuilt list on the device. | `ff_bench` |
| 8 | **major** | `FF_THEME_COLOR_DIM` is **2.64:1** on background — below WCAG AA-large (3:1) — and carries message ages, delivery status (`SENT`, `WAITING`), subtitles and the SHOW CODE privacy line, at 0.87 mm cap height. | contrast calc, golden bbox |
| 9 | **major** | The crew-setup flow (`SHOW CODE`) is the last row of a ~1 085 px Settings list — **~830 px of scrolling**, on rows with no press feedback, to reach the #1 day-one task. | inventory `settings_default.json` |
| 10 | **minor** | `FF_HIT_MIN_GAP_PX` is 8 px = **0.70 mm**. The destructive `LEAVE` confirm sits 12 px (1.05 mm) from `NOT NOW`, while the flare takeover deliberately spends 16 px on the same problem. | inventory `crew_op_confirm_leave.json` |

---

## 2. (A) Tap targets — measured, every face

`hit` is the real click area (box + `ext_click_area`). `mm` is `px / 11.44`.
**Floors used here:** 9 mm for primary actions (FLARE / GO / BACK / HOME / POWER OFF /
confirm-destructive), 7 mm (`FF_THEME_HIT_PRIMARY_PX` 80) for everything else interactive.

### 2.1 The table

| face | control | hit px | mm | verdict |
|---|---|---:|---:|---|
| **Launcher** | RADAR hub | 120×120 (disc) | 10.5 | ✅ best target on the device |
| Launcher | INBOX / LINEUP / MAP / SETTINGS / MUSIC | 100×100 (disc) | 8.7 | ✅ clears 7 mm; 0.3 mm under 9 mm — fine, they are not primary |
| **Radar (CLOSE)** | FLARE | 176×58 | 15.4 × **5.1** | ⚠️ **3.9 mm under the 9 mm primary floor** — at its proven geometric ceiling; see §2.3 |
| Radar (all other modes) | *(nothing)* | — | — | ❌ **zero controls** — see finding 1 / 6 |
| Radar | crew ring dots | 34 px, non-clickable | 3.0 | ❌ not interactive at all (`scr_radar.c:306,491`) |
| **Flare takeover** | GO | 190×64 | 16.6 × **5.6** | ❌ **3.4 mm under 9 mm**; recoverable — see §2.3 |
| Flare takeover | DISMISS | 190×56 | 16.6 × **4.9** | ❌ **4.1 mm under 9 mm**; recoverable |
| Flare sender | CANCEL | 140×64 | 12.2 × 5.6 | ⚠️ the only way to stop a live flare, at 5.6 mm |
| **Power menu** | POWER OFF / REBOOT / CANCEL | 190×56 | 16.6 × **4.9** | ❌ **2.1 mm under 7 mm and the face has 349 px of band** — see §2.3 |
| **Inbox feed** | feed rows | 268×80 | 23.4 × 7.0 | ✅ |
| Inbox thread | quick-reply chips OMW / IN 5 MIN / FLARE | 57/75/62 × 52 | 5.0–6.6 × 4.5 | ⚠️ chip floor; geometry-bound (proved in tap-targets.md) |
| Inbox | compose FAB (reachable on-glass square) | 80×80 | 7.0 | ✅ rect is 144×144 but most is off-glass — the 80 is the honest number |
| Inbox sub-screens | BACK | 44×44 | **3.85** | ❌ **5.15 mm under 9 mm** — the worst primary on the device; see §2.3 |
| Inbox popup | rows | 280×80 | 24.5 × 7.0 | ✅ |
| Inbox popup | close | 64×64 | 5.6 | ⚠️ |
| Inbox Rally | WHERE rows / WHEN / Send | 298×80 / 86×80 / 148×80 | ≥7.0 | ✅ |
| **Compose (T9)** | keys | 101–116 × 50 | 8.8–10.1 × 4.4 | ⚠️ geometry-bound; needs a different input method, not a re-layout |
| Compose | BACK / SEND | 44×44 / 48×44 | 3.85 / 4.2×3.85 | ❌ under floor; bound by the keypad below |
| Compose | DEL / SPACE / MODE | 64/74/48 × 56 | 4.2–6.5 × 4.9 | ⚠️ |
| **Name editor** | keys | 107–118 × 44 | 9.4–10.3 × **3.85** | ❌ at the absolute floor, and this face has spare band (no message bubble) |
| Name editor | BACK / DONE | 44×44 / 64×44 | 3.85 / 5.6×3.85 | ❌ |
| **Settings** | every list row | 240 or 132 or 96 or 76 or 58 or 48 × **48** | ≤4.2 tall | ❌ **2.8 mm under 7 mm**; `tap-targets.md` already calls this "nothing blocking it, should be done" |
| Settings | ON/OFF pills | 48×48 | 4.2 | ❌ |
| Settings | 12H/24H, FT/M, −/+ | 58×48 | 5.1 × 4.2 | ❌ |
| Settings | NORMAL/FLIPPED | 76×48 | 6.6 × 4.2 | ❌ |
| **Crew** | BACK | 44×44 | 3.85 | ❌ |
| Crew | SHOW CODE / START CREW / LEAVE CREW rows | 240×48 | 21.0 × 4.2 | ❌ primary actions at 4.2 mm |
| Crew | HIDE / UNHIDE / ADD | 76×48 | 6.6 × 4.2 | ❌ |
| **Crew ops** | NOT NOW ‖ START / LEAVE | 132×48 | 11.5 × 4.2 | ❌ destructive confirm at 4.2 mm, 1.05 mm apart |
| Crew ops | SHOW CODE / DONE / BACK | 132×48 | 11.5 × 4.2 | ❌ |
| **SHOW CODE** | BACK (the only control on the face) | 120×48 | 10.5 × **4.2** | ❌ **4.8 mm under 9 mm** — and the face has 191 px of vertical band |
| **Compass cal** | CANCEL / DONE | 120×48 | 10.5 × 4.2 | ❌ |
| **Banner** | notification strip | 160×48 | 14.0 × 4.2 | ⚠️ transient toast; acceptable — but see finding 4 |
| **Lineup / Map / Music** | *(nothing)* | — | — | ❌ zero controls |
| Diagnostics | scroll surface / back | 240×242 / 44×44 | 21 / 3.85 | ⚠️ back under floor |

### 2.2 Adjacency and bezel

**Adjacent gaps.** The device-wide floor is `FF_HIT_MIN_GAP_PX` **8 px = 0.70 mm** (`ff_theme.h:544`).
A 2 mm gap would be 23 px, so essentially nothing on the puck clears 2 mm; that is a deliberate
consequence of a 36 mm glass and is mostly fine. Two exceptions are **not** fine:

- **`crew_op_confirm_leave`: `NOT NOW` (68–199) ‖ `LEAVE` (212–343) — 12 px = 1.05 mm.**
  `LEAVE` tears down the crew. The flare takeover spends **16 px** separating GO from DISMISS for
  exactly this reason (PR #20's review). Leaving a crew deserves at least the same. **Major.**
- `crew_op_ready`: `SHOW CODE` (242–289) over `DONE` (300–347) — **11 px = 0.96 mm**, vertically
  stacked, both 4.2 mm tall. **Minor.**

**Not a violation, checked and cleared:** Settings toggle pills sit **6 px** apart — under the 8 px
floor — but **both pills of a pair share one callback with NULL user_data** (`scr_settings.c:533`):
tapping either flips the row. They are one 102 px control, not two. The sweep is right to pass
them. (What *is* wrong with them is finding 3, below.)

**Bezel clipping of painted content** (`check_glass_clipping.py`, new). Measured against
`FF_THEME_GLASS_*` (208, 206, 200), where 200 is itself already pulled in 3 px from the 203 measured:

| golden | lit px outside glass | farthest | past the *measured* 203 px edge |
|---|---:|---:|---:|
| `map_untraced.png` | 511 | 212.3 px | 9.3 px (0.81 mm) |
| `radar_signal_lastknown.png` | 405 | 210.4 px | 7.4 px |
| `lineup_mixed.png` | 377 | 209.8 px | 6.8 px — the amber `SOME SET TIMES TBD` chip's top corners |
| `map_real_lost_lands.png` | 229 | 207.4 px | 4.4 px |
| everything else | ≤ 725 | ≤ 205.1 px | ≤ 2.1 px (anti-alias fringe) |

**Verdict: round-glass clipping of painted content is bounded and minor.** Nothing loses a digit or
a word; worst case is ~9 px of a map polygon and the shoulders of one Lineup chip. The persona
reviews' reports of a sliced battery percentage and flat Settings shoulders are **fixed** — I
re-measured `banner_on_radar.png`'s `74%` at (270,41)–(296,50), 176–179 px from the glass centre.
Attach `check_glass_clipping.py` to CI later if you want it to stay that way. **Polish. Effort: S.**

**Visual size ≠ hit rect — both directions:**

- *Hit smaller than visual:* on Settings **toggle** rows the caption (`COLORBLIND`, `SOUNDS`,
  `CLOCK`) is **dead**, while on **value** rows (`QUIET HOURS`, `COMPASS`, the name row) the caption
  **is** clickable, and on **nav** rows (`CALIBRATE TOUCH`, `DIAGNOSTICS`, `CREW`, `SHOW CODE`) the
  whole 240 px row is. Three different row-tap models stacked in one scrolling list, with no visual
  difference between them. **Major (flow).**
- *Hit larger than visual:* the inbox FAB's rect is 144×144 but only an 80×80 square is on glass —
  already correctly documented and measured. The launcher discs' 100×100 squares now hit-test round
  (`scr_launcher.c` `LV_EVENT_HIT_TEST`) — verified in the inventory. Both good.

### 2.3 Where the height actually is — four faces that can pay and don't

`tap-targets.md` proves Radar-FLARE, the T9 keypad and the Inbox chip strip are at their ceilings.
These four are **not**:

**Power menu** (`scr_power_menu.c:39`, `POWER_MENU_BTN_H 56`). At 190 px wide the farthest corner
is |dx| = 97 from the glass centre, so |dy| ≤ √(200² − 97²) = **174.9** → a **349 px** band
(y 31…381). Three 80 px buttons + two 16 px gaps = **272 px**, and the headline at dy −120 (y 74–98)
sits clear above it. Concrete: `POWER_MENU_BTN_H 56 → 80`, `OFF_DY −40 → −66`, `REBOOT_DY 40 → 30`,
`CANCEL_DY 120 → 126`. Farthest corner (300, 372) = 189.8 px, **10 px inside the glass**.
4.9 mm → **7.0 mm** on all three. **Effort: S.** The file's own comment still frames against
`FF_THEME_PUCK_RADIUS_PX` (206, the framebuffer) rather than `FF_THEME_GLASS_*` — fix that in the
same change; it clears either way today, but it is the exact trap #154/#155 were about.

**SHOW CODE BACK** (`scr_settings.c:1933–1935`). 120×48 at y 326, and it is the **only** control on
the face. At 120 px wide, |dx| = 57 → |dy| ≤ √(200² − 57²) = **191.7** → y ≤ 397.7. Concrete:
`FF_CREWCODE_BTN_H 48 → 80`, `FF_CREWCODE_BTN_Y 326 → 314`; bottom at 394, corner (265, 394) =
196.4 px, **3.6 px inside**. 4.2 mm → **7.0 mm**. **Effort: S.**

**Crew-op confirm buttons** (`scr_settings.c:2124–2128`). Narrow 132 → **124**, gap 12 → **16**
(matching the takeover's own safety gap), height 48 → **80**, `FF_CREWOP_BTN_Y 300 → 279`. New
x-span 74…337, |dx| = 129 → |dy| ≤ 152.8 → y ≤ 358.8; bottom 359. 4.2 mm → **7.0 mm**, and the
destructive/cancel separation goes 1.05 mm → **1.40 mm**. **Effort: S.**

**Flare takeover GO/DISMISS** — recoverable, but it costs the starburst. Today the stack is
starburst (y 35–105, 70 px of pure decoration) → headline → distance → lock chip (ends y 233) →
GO (244–307) → DISMISS (324–379). Narrowing to 176 px moves |dx| to 85, so |dy| ≤ **178.6** →
y ≤ 384.6. Bottom-anchor: DISMISS 304–384, gap 16, GO 208–288 — **80 px each, 7.0 mm**. That
requires everything above to end by y ≤ 200, i.e. **shrink the starburst from 70 px to ~35 px**
(or drop it: the black takeover is already unmissable without it). This is a real design trade,
not a free win — state it and let Jake pick. **Effort: M.**

**Inbox sub-screen BACK** (44×44, 3.85 mm) is the one `tap-targets.md` correctly calls unfixable
*as a circle sharing a row with a centred title*. The unconsidered option is to stop having a
circle: make the **whole header row** the back target — 298×56 (26.0 × 4.9 mm), title inside it,
chevron at the left. Target area goes from 1 936 px² to **16 688 px²** (8.6×) without moving a
single other pixel on the face. Still under 7 mm in the short axis, but it is the only change
available that is worth making here, and the left-rim swipe already backs it up. **Effort: M.**

---

## 3. (B) Fluidity

### 3.1 Press feedback — the single biggest fluidity defect (finding 2, **blocker**)

There are three possible channels for "the puck felt my finger". Right now, on several faces,
**all three are off**:

| channel | state |
|---|---|
| visual `LV_STATE_PRESSED` | present on launcher, compose, inbox, banner, flare takeover, power menu, touch-cal, name-editor DONE. **Absent** on Radar's FLARE (`scr_radar.c:1042–1053` — `ff_scr_button_create` + `lv_obj_remove_style_all`, no pressed style) and on **every** control built through `settings_make_pill` (`scr_settings.c:496`, `FF_SCR_PILL_PRESS_NONE`) — that is all of Settings, all of Crew, both crew-op confirm buttons, and SHOW CODE's BACK. **22 controls on `settings_default` alone.** |
| haptic | **no haptic HAL exists.** `app_main.c:2130`: "`cfg.haptic` left zeroed — see slice a (no haptic HAL yet)". The `haptics` setting defaults `true` and its Settings row is compiled out (`FF_SETTINGS_ROW_ENABLE_HAPTICS 0`) — honest, but it means S29's warmer/colder FIND pulses and every flare alert buzz are no-ops. |
| sound tick | `ff_sound_emit(FF_SOUND_TAP)` fires on `LV_EVENT_CLICKED`, **not** `LV_EVENT_PRESSED` (`scr_nav.c:70–73`) — correct for "was this a tap or a scroll", wrong as touch-down confirmation — and `ui_ticks` **defaults OFF**. |

So on Radar, the device's signature CTA — **FLARE** — gives a user with default settings *nothing*
between finger-down and the screen changing. Same for every Settings toggle and for leaving a crew.
The recorded failure mode is already in this repo's own history: "taps seem to pass through and I
tapped it multiple times before it actually worked" (`ff_shell.c:4619`). A user with no press
feedback taps again — and for a Settings toggle, **tapping again undoes it** (finding 3).

**Recommendation:** give `settings_make_pill` `FF_SCR_PILL_PRESS_TINT` (amber at `LV_OPA_40` for
`SURFACE`-filled pills, `PRESS_DIM` ink-wash for amber-filled ones — exactly what `scr_widgets.c`
already implements) and add `lv_obj_set_style_bg_color(btn, INK, LV_STATE_PRESSED)` +
`bg_opa LV_OPA_20` to Radar's FLARE. Zero geometry change. Then extend
`test_scr_intent.c`'s existing `every clickable compose control must carry an LV_STATE_PRESSED
style` assertion (line 490) to **every face**, not just compose. **Effort: S. Goldens: unchanged**
(pressed styles are `LV_STATE_PRESSED`-only, per `scr_widgets.h:39`).

### 3.2 Screen transition / blocking renders (finding 7, **major**)

`ff_bench`, host, `lv_obj_clean()` + `ff_build_face_screen()` only (no rendering):

| face | µs/iter | relative to MAP |
|---|---:|---:|
| **SETTINGS** | **71 090** | **93×** |
| LAUNCHER | 27 261 | 36× |
| INBOX | 26 265 | 34× |
| COMPOSE | 19 165 | 25× |
| RADAR | 3 617 | 4.7× |
| LINEUP | 916 | 1.2× |
| MAP | **765** | 1× |

These are host numbers; the **ratios** are what matters. The ~520 ms Map poll stall (S28 amendment
2026-09-07) was caused by the Map face's build cost, and it was fixed **at the Map** — the Map is
now the *cheapest* face on the device (22 objects for the whole tile). The FSM also got
`stall_gap_ms` tolerance as defence in depth, which is the right call. **But the stall class did
not go away; it moved.** Settings is now 93× the face whose build cost made BACK/HOME unreachable,
and it is the longest-lived list on the device.

Likely mechanism, stated as a lead rather than a conclusion: the three most expensive faces are the
three that call `lv_obj_update_layout()` **inside per-row helpers** —
`scr_settings.c:1594`, `scr_inbox.c:1528/1615/1621/1738` — each of which forces a full-tree layout
pass, giving O(rows²) behaviour. MAP, LINEUP, RADAR and COMPOSE call it zero times. **This needs a
device measurement before anyone optimises it** (§7).

Two things that are already right and should stay:
- Rebuilds are **deferred while a finger is down** on both the device and the sim
  (`test_ctl_rebuild_under_finger.c`), with object identity asserted, not just a counter.
- `shell_coarsen_age_ms` and the 0.1° `arrow_deg`/`ring_deg` quantisation kill sub-second render-key
  churn at the source (`ff_shell.c:4575`, `:4660`, `:4683`).

**QR generation on SHOW CODE is not a stall risk.** PR #326 cut the payload to the bare crew code,
which fits QR **v1-M (21×21)** in BYTE mode; `lv_qrcode_update` on a 21×21 matrix into a 170 px
canvas is negligible next to the 71 ms the Settings tree costs around it.

### 3.3 Gestures

**The stall-tolerant window fix is real and correct.** `ff_gesture.c`'s `stall_gap_ms` (150 ms,
applied **once**, to the window's clock only, never to `x0/y0`) plus AC12's negative control (a
genuinely slow swipe still returns NONE) is exactly the right shape — the tolerance is for *gaps*,
not for *slowness*. Round 3's `gesture_down_admitted` (rim-zone admission with a perpendicular
bound and a panel-coordinate sanity check) is likewise the right answer to "a flat circular pad can
never cover every y along a straight edge".

**What remains, and it is structural (finding 6, major):** Lineup, Map, Music, and Radar-outside-
CLOSE-mode build **zero** clickable objects. On those four faces the *only* on-glass way out is the
left-rim BACK swipe — the one interaction this repo's own bench logs document failing on real
hardware across three amendment rounds, and the one a gloved or wet thumb at the extreme edge of a
round window is least likely to land. The physical BOOT button is the backstop, and S26 correctly
makes it home-from-anywhere; but the case's BOOT cap is documented unreliable (S28's own "Why").

**Recommendation:** every face that builds no controls gets a visible on-glass BACK affordance.
Cheapest version that costs no new geometry: the left-rim swipe already exists — **draw it**. A
6 px-wide, 120 px-tall amber chevron hint at x = 8, vertically centred, rendered at `LV_OPA_30` on
exactly the faces with zero controls. It teaches the gesture (the persona reviews both found it
undiscoverable) and it marks the zone the touch controller actually reports. **Effort: M.
Goldens: `lineup_*` (7), `map_*` (8), `music_swarm_*` (4), `radar_*` (~25) change.**

**Tap/swipe conflict:** clean. A rim zone gates only where a touch may *start*; G1/G2 additionally
need ≥56/64 px of travel in the window with axis lock, and on recognition the glue calls
`lv_indev_wait_release` so the widget under the finger gets `PRESS_LOST`. Every button goes through
`ff_scr_button_create`, which clears `PRESS_LOCK` and cancels past 12 px of drift. **No taps are
being stolen.** Verified against the inventory: launcher MUSIC (x 35) and the thread's OMW chip
(x 46) sit inside the BACK band (x ≤ 52), and compose DEL/SPACE/MODE and the Radar FLARE sit inside
the HOME band (y ≥ 342) — all of them reachable by tap.

**Accidental taps on wake — one real hole.** S26's wake-only gate swallows the first touch at OFF
and SLEEP. The 2026-09-07 amendment deliberately made **DIM** deliver the press (correct: DIM is a
fully readable screen). But G3 — long-press ≥1 200 ms anywhere non-interactive on **Radar** — fires
a real crew-wide flare with **no arming grace period** (`ff_flare.h`: the send is immediate;
`CANCEL` appears only afterwards). So a 1.2 s incidental pocket press, on the Radar face, between
15 s and 30 s of idle, sends a flare. **Major.** Two candidate fixes, both cheap: (a) require the
long-press to *begin* while `ff_idle` is ACTIVE, not DIM — one term in `ff_gesture_glue.c`, same
shape as the existing gate; or (b) give QUICK_FLARE a 3 s armed countdown with a cancel before the
packet goes out. (a) is smaller and does not weaken the panic path. **Effort: S.**

### 3.4 Animation

There are exactly **two** animations in the app, both opacity pulses: Radar's CLOSE-mode pulse rings
(1 200 ms period, 150 ms stagger, `scr_radar.c:994–1003`) and the flare starburst
(`scr_flare.c:359,650`). There are **no** face transitions, **no** press animations and **no**
scroll-position animations.

That is mostly the right call for a battery-bound 412×412 panel where every state change is a
`lv_obj_clean()` + full rebuild — cross-fading two trees would double the cost of the most expensive
thing the device does. **Do not add face transitions.**

Two places where motion is already correct and deserve calling out: the Radar arrow is
**exponentially smoothed** at τ = 250 ms, wrap-aware (`ff_radar.c:10,38–60`), quantised to 0.1° for
the render key — that is textbook, and it is why the arrow reads as an instrument rather than a
jitter. And the LOST/STALE visual language (outline ghost arrow vs. dashed-filled + amber rim) does
its work in half a second with no text.

The gap is the ring dots: `ff_radar_dot_t.ring_deg` is recomputed from the **raw** heading every
tick with **no** smoothing (`ff_shell.c:4667` says so in as many words). They are quantised to 0.1°
so they do not churn the render key, but they are not *smoothed* — with a stationary puck and a
noisy compass, eight dots jitter around a ring while the arrow they surround sits still. **Minor.**
Fix: run `ring_deg` through the same `radar_smooth_step` the arrow uses. **Effort: S.**

### 3.5 Wake / sleep / idle vs. a festival glance

`ff_idle.h:123,127,137`: **DIM 15 s → OFF 30 s → LIGHT SLEEP 150 s.**

The screen is fully dark 30 seconds after the last touch — correct for battery, and S26's "wake to
whatever base was showing" is the right rule. But combined with finding 1 it produces this as the
*actual* glance path at a festival:

> touch (swallowed by the wake-only gate) → screen restores to whatever face you left it on →
> if that isn't Radar: BOOT → launcher → tap RADAR → the arrow points at **crew slot 0**, and there
> is no way to point it at anyone else.

**Three physical actions and ~2 s to reach a bearing you can't choose.** The lifecycle machinery is
fine; the cost is entirely finding 1.

### 3.6 The FLARE path from home, counted

| path | actions | wall time from home | notes |
|---|---|---|---|
| A — Radar CLOSE-mode FLARE button | tap RADAR, tap FLARE | ~1.5 s | **only available when the selected member is already close** — i.e. not the case you want it for |
| B — long-press on Radar (G3) | tap RADAR, hold ≥1 200 ms | ~2.0–2.5 s | the real touch path; **undiscoverable** (no first-run hint on any of the 119 goldens) |
| C — 5× BOOT within 2 500 ms | 5 button presses | ~2.5 s | works screen-off, gloved, no navigation. **The correct answer, and nothing on the device says so.** |
| from screen OFF | +1 swallowed wake touch, +1 BOOT if not on Radar | +1.5–2 s | path C is unaffected |

Path C is genuinely good engineering (Deshawn's review was right to single it out). The defect is
that it is invisible. **Recommendation:** one line on the splash or in an S12 first-run card —
`HOLD THE BUTTON 5× TO FLARE`. **Effort: S.**

---

## 4. (C) Flow — the five tasks

### Task 1 — glance at crew bearing · **BLOCKED**

`FF_INTENT_SELECT_CREW` is emitted by **no screen in the codebase** (grep across
`firmware/app/screens/` and `firmware/targets/`: zero hits). `ff_shell.c:8041` documents this
honestly in a `deliberate no-ops` block: *"the S06 tap-cycle gesture was never wired into
`scr_radar.c`, and the S26(e) nav rework … made the radar hub a plain launcher satellite rather than
the tap surface the spec describes."*

The consequence, traced to the end: `ff_crew_selected()` (`ff_crew.c:352–374`) self-heals to
**the first paired member in roster order**. The only user action anywhere that changes the
selection is accepting an incoming flare — `ff_crew_select_node` has exactly **one** call site,
`ff_shell.c:6966`, in the takeover's GO handler.

So on a puck with 8 crew, **the arrow always points at crew slot 0**, and the owner of a
friend-compass cannot choose their friend. Every other finding in this review is downstream of a
face that works; this one is the premise.

The core is ready: `ff_crew_select_next()` and `ff_crew_select_node()` both exist and are tested.
**Two candidate wirings, in preference order:**

1. **Make the crew ring dots tappable.** This is what `FF_THEME_HIT_DOT_PX` (64) was named for, and
   `tap-targets.md` already derived the three things that block it: the dots are
   `LV_OBJ_FLAG_CLICKABLE`-cleared (`scr_radar.c:306,491`); `ff_radar_dot_t` carries no node id, so
   wiring one is a `core/` change (its own PR, Tier 3); and a 64 px box on a 185 px ring reaches
   217 px from the glass centre and collides with a neighbouring dot at the 34 px cluster threshold.
   Both are solvable — clamp the hit sibling radially inward, raise the cluster threshold to the hit
   size — but both change the visual.
2. **Tap the centre disc to cycle** — S06's own original contract, one `ff_scr_button_create` on a
   ~120 px centre region emitting `FF_INTENT_SELECT_CREW`, zero core changes, zero visual change.
   Discoverability is poor (nothing says "tap to cycle") but 8 taps beats 0 options.

Do **2** now for the field test, **1** after. **Effort: S for (2), L for (1).**

### Task 2 — send a FLARE · works, undiscoverable

See §3.6. 2 taps / ~2 s once you know; 0 screen taps via the hardware path. The name-the-gesture
gap and the missing arming grace are the findings.

### Task 3 — read + reply to a DM · works, 3 taps

Launcher → INBOX (1) → row (1) → quick-reply chip (1). Good. Caveats:

- **`DIM`-coloured delivery status.** `SENT` and `WAITING` render at `FF_THEME_COLOR_DIM` —
  **2.64:1** (`scr_inbox.c:1589–1599`). `NOT DELIVERED` / `NOT SENT` correctly get `STALE_AMBER`
  (11.13:1) and `DELIVERED` gets green — the *bad* news is legible and the *routine* news is not,
  which is the right priority but the wrong floor. See §5.
- Anything beyond a canned chip means T9 at 4.4 mm keys. Already correctly diagnosed as needing a
  different input method, not a re-layout.

### Task 4 — join/start a crew + show code · **the worst flow on the device**

Launcher → SETTINGS (1 tap) → the row you need, `CREW`, sits at list-relative **y 1 038** in a
viewport that is **255 px tall** (`settings_default.json` inventory) → **~830 px of scrolling**,
roughly 4 flicks → tap CREW (1) → tap SHOW CODE (1). **3 taps + ~4 flicks**, on rows that give no
press feedback, to reach the one thing every user must do on day one.

And raising the Settings rows to 80 px (which they should be — `tap-targets.md` says so) makes this
**worse**: visible rows drop from ~3.5 to ~2.3, so the scroll grows to ~6 flicks. **The two changes
have to ship together.**

**Recommendation:** promote crew out of Settings. Either (a) a sixth launcher position — the
compass ring is currently 1 hub + 5 satellites, and the geometry that admitted 100 px satellites at
128 px orbit has room; or (b) at minimum, move `CREW` to the **top** of the Settings list, above
`DISPLAY`. (b) is a 20-line change and removes 830 px of scrolling. Do (b) now, consider (a) after
the field test. **Effort: S for (b), M for (a).**

Also: **`LEAVE` is 12 px (1.05 mm) from `NOT NOW`, both 4.2 mm tall, neither with press feedback.**
See §2.2 and §2.3.

### Task 5 — check the lineup · 1 tap, then a dead end

Launcher → LINEUP (1 tap). Fastest flow on the device. But the face builds **zero** controls: no
scroll affordance for `STILL TBD` beyond what fits, no way to look at tomorrow, and no on-glass exit
(§3.3). `FF_THEME_HIT_LIST_PX` (72) is defined and documented "for the day those rows become
tappable" — honest, and that day should be soon.

### Cross-cutting: BACK/HOME placement is inconsistent

| face | how you leave |
|---|---|
| Inbox sub-screens, Crew, Diagnostics, name editor, compose | a 44×44 circle, top-left-ish, at (109–145, 20–36) — four **different** x positions |
| SHOW CODE, compass cal, crew-op confirms | a 120–132 px **pill at the bottom** labelled BACK / NOT NOW / DONE |
| Lineup, Map, Music, Radar | **nothing** — rim swipe or BOOT only |
| Launcher | nowhere further to go (correct) |

Three different affordances and four different positions for one verb. **Recommendation:** pick
one — the top-left chevron — and put it on every non-launcher face, with the bottom pill reserved
for confirm/dismiss semantics only. **Effort: M.**

### Status vocabulary — clean, verified

Grepped every user-facing string in `core/src` and `app/screens`. The canonical set is intact:
`NO SIGNAL <age>` (`15 MIN` / `23 HR` / `48 MIN`), `NOT SEEN YET`, `NO LOCATION YET`,
`NEARBY, NO LOCATION`, `RELAYED`, `NOT DELIVERED`, `NOT SENT`, `LINKED`, `NO RADIO`, `SEEN <age>`,
`now`. **`LOST` survives only in `ff_debug_console.c:51` and the sim's `fixture_view.c:30`** — both
debug-only dumps, both correct to keep. `ff_fmt_age` is the single formatter
(`ff_crew.c:447–468`) and its `now` bucket doubles as the render-key coarsening, which is elegant.
**No deviations found.** The one thing to watch: the owner's brief writes the short forms
`NO SIGNAL 40M` / `6M AGO` while the code emits `NO SIGNAL 40 MIN` / `40 MIN`. The code's form is
the more legible one; the brief is paraphrasing. No change needed — but say so out loud so the next
pass doesn't "fix" it.

---

## 5. (D) Contrast and legibility, outdoors, at arm's length

**Measured cap heights** (pixel bounding boxes out of `radar_close.png` / `inbox_inbox.png`),
at 11.44 px/mm, and the visual angle each subtends at a 500 mm arm's length:

| role | font | measured cap px | mm | arcmin @ 500 mm |
|---|---|---:|---:|---:|
| distance readout (`~15 m`) | montserrat_36 | ~26 (derived, 0.72 ratio) | **2.27** | 15.6 |
| name (`Dana`, Radar) | montserrat_22 | 16 | **1.40** | 9.6 |
| inbox sender / headline | montserrat_20 | 14 | **1.22** | 8.4 |
| **everything else** — clock, `pm`, battery %, `LINKED`, every status chip, every Settings caption and pill label, every crew row, every message age, every delivery status | montserrat_14 (`FONT_CHIP` / `FONT_LABEL`) | **10** | **0.87** | **6.0** |

6 arcmin is the *acuity limit* for 20/20 vision in ideal conditions. It is not a reading size, and
this is a device used in sun, in motion, at a glance, often by someone whose eyes are not at their
best. **Every canonical status word on the puck is set at it.**

**Contrast ratios** (WCAG 2.1, computed from `ff_theme.h`):

| colour | on `BG` (0x0B0B10) | verdict |
|---|---:|---|
| `INK` 0xF2EFE6 | **17.08:1** | excellent |
| `AMBER` 0xFFC66B | **12.66:1** | excellent |
| `LIVE_GREEN` 0x9BE07B | 12.47:1 | excellent |
| `STALE_AMBER` 0xFFB454 | 11.13:1 | excellent |
| `CREW_GOLD` / `CREW_TEAL` | 13.64 / 11.17 | fine |
| `CREW_BLUE` / `CREW_ORANGE` / `CREW_MAGENTA` | 6.44 / 6.37 / 6.00 | fine as **dots**; too low for text |
| `MUTED` 0x8B8A97 | **5.78:1** | AA pass, AAA fail — marginal at 0.87 mm in sun |
| **`DIM` 0x55545F** | **2.64:1** | **fails AA (4.5) and AA-large (3.0)** |
| `SURFACE` 0x14141C vs `BG` | **1.07:1** | see below |

**Finding 8 (major): `DIM` at 0.87 mm.** `FF_THEME_COLOR_DIM` is used for text in 21 places across
`scr_inbox.c` and `scr_compose.c` — message ages, thread timestamps, `SENT`/`WAITING`, the `PLACES`
and `PICK A PLACE` labels, disabled names, the compose BACK glyph — and, on the SHOW CODE face, the
line that reads **`exact positions`**, which is a privacy disclosure rendered at 2.64:1.
`ff_theme.h:61` still describes `DIM` as *"the retired page-dot row's inactive-dot color, before
S26e"* — it was a **dot** colour that quietly became a **text** colour. **Fix: retire `DIM` as a
text colour entirely.** Every current use is either secondary text (→ `MUTED`, 5.78:1) or genuinely
de-emphasised state (→ `MUTED` at `LV_OPA_70`, ≈4.6:1, still AA). **Effort: S. Goldens: every
`inbox_*` (14) and `compose_*` (10) changes.**

**Finding: the `SURFACE` chip/card is invisible.** `SURFACE` on `BG` is **1.07:1** — a 7 %
luminance step. Under a glossy round cover glass in direct sun, with the panel at partial
brightness, that boundary is gone. This matters more than it looks: the Settings *inactive* pill is
`SURFACE` fill + `MUTED` label (`FF_SETTINGS_PILL_OFF_BG`, `:144`), and SHOW CODE's BACK is the same
— **you cannot see where the button is, only where the word is.** A 4.2 mm target you must aim at
by reading it is worse than a 4.2 mm target you can see. **Fix: give unfilled/inactive pills a
1 px `MUTED` border** (5.78:1 against the background, which *is* visible) instead of relying on the
fill. `ff_scr_pill_create` already supports exactly this (`filled = false` + `border_width`).
**Major. Effort: S. Goldens: every `settings_*` (12), `crew_*` (18).**

**The 12h clock.** `radar_close.png` renders `9:46 pm` — correct 12-hour form, lowercase meridiem,
at `MUTED`/montserrat_14, i.e. **0.87 mm at 5.78:1**. The meridiem marker is the part that carries
the am/pm ambiguity a festival-goer actually cares about at 4 a.m., and it is the smallest, dimmest
thing on the status row. **Minor. Fix:** render the time at `FONT_LABEL`→`FONT_MSG_BODY`
(montserrat_16, 1.0 mm) in `INK` (17:1) and keep only the meridiem at `MUTED`, or drop the meridiem
and use a 24 h default — the setting exists (`CLOCK 12H/24H`). **Effort: S.**

**Finding 4 (major): the banner occludes the status row.** `scr_banner.c:43` places
`BANNER_CY = RADAR_LAYOUT_STATUS_BAR_DY + 14.0f` → top edge **y 36**, and the status text measures
**y 41–53**. The banner is deliberately 2 px above the status text and covers it completely:
`banner_on_radar.png` shows `9:46` truncated, `LINKED` **entirely gone**, and the battery reduced
to a bare `%`. The two facts a user checks before trusting the device — *can it reach anyone* and
*will it last* — are hidden by the notification that made them look.

The fix is one constant and it makes the banner **better**, not just less harmful. Move
`BANNER_CY` to `RADAR_LAYOUT_STATUS_BAR_DY + 40.0f` (−120, puck-local y 86): top edge y 62 (9 px
below the status baseline), bottom y 110. At dy −144 the safe half-chord is
√(190² − 144²) = 124 px, so the banner could simultaneously widen **160 → 200 px** — its message
preview currently truncates at `"The Firefly To…"`. Farthest corner (308, 62) = **175.3 px**,
24.7 px inside `FF_THEME_GLASS_R`. **Effort: S. Goldens: `banner_on_radar`, `banner_on_launcher`,
`banner_on_thread` change.**

**Finding 3 (major): the segmented control lies.** Settings toggle pills are drawn as a segmented
control — active segment amber-filled, inactive `SURFACE` — which teaches "tap the option you
want". The callbacks ignore the event entirely and **invert the current value**:

```c
static void settings_clock_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_CLOCK_24H, s_settings.clock_24h ? 0 : 1);
}
```
`scr_settings.c:613–617`; identical shape at `:671` (SOUNDS), `:679` (UI TICKS), `:693`
(COLORBLIND), plus UNITS and SCREEN. So **tapping the lit segment flips the setting**. The worst
case ships today: `SCREEN NORMAL/FLIPPED` — one confirming tap on the already-lit `NORMAL` turns
the display upside down, and the user must then find the same row on an inverted screen to undo it.
Compounded by finding 2: with no press feedback, the instinct is to tap again — which inverts it
back, teaching the user that the control is broken.

**Fix, and mind the trap.** The obvious repair — pass the target value in `user_data` — would
**break the 6 px gap**: the adjacency sweep identifies a composite control by its whole
`(cb, user_data)` action set (PR #311's identity fix), so two pills with *different* `user_data`
stop being one control and their 6 px gap becomes a real violation against the 8 px floor. Keep
`cb` **and** `user_data` exactly as they are, and resolve which pill was pressed inside the
callback from `lv_event_get_target()`'s own `lv_obj_get_index()` within its row (left = 0,
right = 1) — then `set` rather than invert. Identity is unchanged, the exclusion still holds, the
geometry does not move. Pin it with a test: *pressing the active segment of every toggle row emits
no setting change*, plus a re-run of `test_face_hit_targets` to prove the composite exclusion still
fires. **Effort: S. Goldens: unchanged.**

**Colourblind mode is real and correct** — an 8-colour Okabe–Ito-derived palette
(`FF_THEME_CREW_CB_*`), all ≥5.4:1, with its own golden (`radar_crew8_colorblind.png`). Good.

---

## 6. Fix plan — four PR-sized slices

### Slice 1 — "the puck answers your finger" · **do this first**
*Findings 2, 3. Effort: S. No geometry moves.*

- `settings_make_pill` (`scr_settings.c:496`): `FF_SCR_PILL_PRESS_NONE` → `TINT`/`DIM` by fill.
- Radar FLARE (`scr_radar.c:1042–1053`): add `INK @ LV_OPA_20` on `LV_STATE_PRESSED`.
- Inactive pills: `filled=false` + 1 px `MUTED` border instead of an invisible `SURFACE` fill.
- Toggle callbacks resolve the pressed pill from `lv_obj_get_index()` and `set` rather than invert
  (NOT via `user_data` — see §5's trap: that would break the 6 px composite-pair exclusion).
- Widen `test_scr_intent.c:462–490`'s pressed-style assertion from compose to **every face**.

**Acceptance:** (1) every clickable object on every committed fixture carries an `LV_STATE_PRESSED`
`bg_opa` or `bg_color` — asserted, not asserted-for-compose; (2) a synthetic press on the *active*
segment of each of CLOCK / UNITS / SCREEN / COLORBLIND / SOUNDS / UI TICKS emits **zero**
`FF_INTENT_SETTING_SET`; (3) `test_face_hit_targets` still reports the toggle pairs as one composite control (the 6 px gap
stays legal); (4) mutation proof — restore the inverting callbacks and (2) fails.
**Goldens changed:** `settings_*` (12), `crew_*` (18) — inactive pills gain a border.

### Slice 2 — "you can point it at your friend"
*Finding 1. Effort: S. The one that makes the product work.*

- `scr_radar.c`: a centre-disc `ff_scr_button_create` (~120 px, disc hit-test like the launcher's)
  emitting `FF_INTENT_SELECT_CREW`; the shell handler already exists and is already tested.
- A one-line on-glass hint under the name in the multi-member case (`TAP TO SWITCH`, `FONT_CHIP`,
  `MUTED`) — or nothing, if Jake prefers the clean face.

**Acceptance:** (1) with 8 paired members, 8 synthetic centre taps cycle `ff_crew_selected()`
through every member and back; (2) the new control does not fire G3 — it is
`ff_scr_button_create`-built, so it carries `LV_OBJ_FLAG_USER_1` and S28's interactive check refuses
the long-press on it; (3) `S28_AC15`-style regression: a long press on empty Radar glass still
flares. **Goldens changed:** `radar_*` only if the hint ships (~25 files); none otherwise.

### Slice 3 — "the faces the sizing pass didn't reach"
*Findings 5, 10, plus the crew-flow depth from finding 9. Effort: M.*

- Power menu 56 → 80 px (`OFF_DY −66`, `REBOOT_DY 30`, `CANCEL_DY 126`); reframe the file's own
  corner check from `FF_THEME_PUCK_RADIUS_PX` to `FF_THEME_GLASS_*`.
- SHOW CODE BACK 120×48 @ y 326 → 120×**80** @ y **314**.
- Crew-op confirms 132×48 gap 12 → **124×80 gap 16** @ y **279**.
- Settings rows `FF_SETTINGS_ROW_H` 48 → 80 (**with** the CREW promotion below — they must ship
  together, see §4 task 4); re-derive the compass-cal button placement that shares the constant.
- Move `CREW` to the **top** of the Settings list.
- Extend `SIZING_RULES` (`test_tap_target_sizing.c:142`) to cover `settings`, `crew`, `power_menu`,
  `flare`, `compass_cal` — each at the ceiling this slice actually reaches, so the table stays a
  regression guard rather than an aspiration.
- Raise `FF_HIT_MIN_GAP_PX` to 16 for *destructive-adjacent-to-cancel* pairs specifically (a second
  named constant, `FF_HIT_MIN_GAP_DESTRUCTIVE_PX`), matching the takeover's existing 16 px.

**Acceptance:** (1) `test_tap_target_sizing` sweeps all 119 fixtures under a real rule (not
`inventory_only`) with 0 violations; (2) every button touched sits ≥3 px inside `FF_THEME_GLASS_R`
at its farthest corner, asserted from the rendered rect; (3) `CREW` is reachable with **zero**
scrolling from the Settings root. **Goldens changed:** `power_menu`, `crew_show_code*` (3),
`crew_op_*` (8), every `settings_*` (12), `compass_cal_*` (2).

### Slice 4 — "legibility and the banner"
*Findings 4, 8, the clock, the ring-dot jitter. Effort: M.*

- Retire `FF_THEME_COLOR_DIM` as a text colour: every text use → `MUTED` (or `MUTED @ OPA_70`).
  Keep the constant for non-text use, and correct its doc comment.
- `BANNER_CY` `STATUS_BAR_DY + 14` → `+ 40`; `BANNER_W` 160 → 200.
- Status-row time at montserrat_16 in `INK`.
- `ring_deg` through `radar_smooth_step` (the arrow's own τ = 250 ms path).
- Add `firmware/tools/check_glass_clipping.py` to CI as a *report*, not a gate.

**Acceptance:** (1) no `FF_THEME_COLOR_DIM` in any `lv_obj_set_style_text_color` call — grep-
asserted in a test; (2) a rendered banner's rect does not intersect the status row's rect, asserted
from the real objects; (3) `S26d_AC2_banner_corners_clear_glass_by_10px` still passes at the new
centre and the new width; (4) with a static puck and ±2° of injected compass noise, no ring dot's
rendered position moves. **Goldens changed:** `banner_on_*` (3), every `inbox_*` (14), every
`compose_*` (10), every `radar_*` (~25).

**Deliberately deferred, with reasons:** the flare-takeover 80 px buttons (costs the starburst —
a design call for Jake, §2.3); making the crew ring dots tappable (needs a `core/` change to
`ff_radar_dot_t` and changes the ring's visual — its own Tier 3 PR, as `tap-targets.md` already
proposed); crew as a sixth launcher position; the T9 keypad (correctly proved to need a different
input method, not a re-layout).

---

## 7. What I could not measure without the hardware — and how Jake measures it

The sim has no touch controller, no backlight, no sun, and a CPU roughly two orders of magnitude
faster than an ESP32-S3. Six things are therefore **unmeasured**, not measured-and-fine.

The device already carries the right instrument: `CONFIG_FF_DEBUG_CONSOLE=y` exposes a **`perf`**
command (`app_main.c:1310–1384`) reporting `frame`, `lvgl_refresh` and `flush` as
min/avg/max/n over a rolling 5 s window, plus a lifetime `face_rebuilds` counter and per-task stack
high-water marks. Console at **`/dev/cu.usbmodem1201`**, via
`scratchpad/puck_console.py PORT [--read SECS] [CMD ...]`.

| # | unmeasurable here | bench protocol |
|---|---|---|
| 1 | **Per-face rebuild cost on real silicon** (finding 7 — is Settings' 93× a problem or a rounding error at 240 MHz?) | Park on each face, leave it 30 s untouched, then: `puck_console.py /dev/cu.usbmodem1201 --read 1 perf perf`. Read `frame avg_us`/`max_us` and the delta in `face_rebuilds` between the two `perf` calls. Repeat for LAUNCHER / RADAR / INBOX / SETTINGS / MAP / LINEUP. **Flag: any face whose `frame max_us` exceeds 150 000** — that is the regime that made BACK/HOME unreachable on the Map. |
| 2 | **Touch-to-visible-feedback latency** | After slice 1, film the glass at 240 fps (iPhone slo-mo) pressing Radar's FLARE and a Settings pill. Count frames from finger contact to the pressed tint. **Target ≤ 100 ms (24 frames).** There is no software instrument for this; the camera is the instrument. |
| 3 | **Touch controller reporting near the bezel** — S28 round 3 showed the panel reports out to `x=5` along the whole left edge, but nobody has swept the *whole* rim | Build with the gesture glue's sample logging on. Drag one finger slowly around the rim, ~2 cm in from the edge, one full revolution. Log `(x, y)` and plot against the circle `(208, 206, 200)`. **Produces the true admission shape** — round 3's rim-zone OR is a good approximation of a curve nobody has actually drawn. |
| 4 | **Real bezel clipping** | Render `lineup_mixed`, `map_untraced`, `radar_signal_lastknown` on the device (the three worst in §2.2) and photograph each straight-on. Compare against the goldens. Confirms whether 9 px past `GLASS_R` is visible loss or anti-alias fringe under the lip. |
| 5 | **Outdoor legibility at 0.87 mm / 2.64:1** (finding 8) | Two people, midday, direct sun, puck at arm's length on a lanyard. Show `inbox_thread_outbox_states` and `crew_show_code`. Ask each to read the delivery status line and the `exact positions` line aloud **without leaning in**. Before and after slice 4. This is the only honest test of a contrast ratio. |
| 6 | **Gloved / wet / sweaty touch** — nothing in the pipeline compensates, and the smallest targets fail first | Latex glove + a spray bottle. 20 attempts each at: Radar FLARE, a Settings ON/OFF pill, SHOW CODE's BACK, the inbox FAB, a left-rim BACK swipe on the **Map**. Record hit rate per control. **The Map rim swipe is the one to watch** — it is the only exit from that face. |

One more that is not a measurement but a decision: **§0.1, the px/mm constant.** Put a caliper
across the visible glass window and across the full pixel array, and settle whether
`FF_THEME_PX_PER_MM` is 11.44 or 11.13. Both this review and `docs/hardware/tap-targets.md` are
written against 11.44 and would need a 3 % correction if 11.13 wins.

---

## 8. Goldens cited

All 119 pass byte-identical in this worktree (`firmware/tests/run_goldens.sh`, clang).
The ones this review reads as evidence, under `firmware/tests/golden/`:

`banner_on_radar.png` (finding 4 — the occluded status row) ·
`radar_close.png` (FLARE at 176×58; the 12 h clock; measured cap heights) ·
`settings_default.png` (the segmented-control affordance; the invisible `SURFACE` pill) ·
`settings_scrolled_bottom.png` (CREW at the very bottom) ·
`crew_show_code.png` (BACK at 4.2 mm; `exact positions` at 2.64:1) ·
`crew_op_confirm_leave.png` (LEAVE 1.05 mm from NOT NOW) ·
`crew_full.png`, `crew_default.png` (76×48 HIDE/ADD pills) ·
`inbox_inbox.png` (feed rows at 7.0 mm; the FAB) ·
`inbox_thread_direct.png`, `inbox_thread_outbox_states.png` (chips at 4.5 mm; `DIM` status text) ·
`inbox_rally.png` (80 px rows) ·
`flare_takeover_locked.png` (GO/DISMISS at 5.6/4.9 mm; the 70 px starburst) ·
`power_menu.png` (three 4.9 mm buttons in a 349 px band) ·
`lineup_mixed.png` (zero controls; the clipped TBD chip) ·
`map_untraced.png` (the largest measured bezel overrun, 9.3 px) ·
`compose_pred_mid.png` (4.4 mm keys) ·
`settings_name_edit.png` (3.85 mm keys with spare band) ·
`radar_crew8.png`, `radar_crew8_colorblind.png` (non-interactive ring dots; the CB palette) ·
`compass_cal_ritual.png` (CANCEL at 4.2 mm).

## 9. What is already right

Worth saying plainly, because most of this device is well built:

1. **`docs/hardware/tap-targets.md` is a model of how to do this work** — measured output rather
   than arithmetic, ceilings derived rather than asserted, and the things it *couldn't* deliver
   named with the geometry that blocked them. This review is mostly an extension of its method to
   the faces it didn't reach.
2. **The S28 gesture FSM's stall tolerance is the right fix for the right reason.** Tolerating
   *gaps* while still rejecting *slowness*, with AC12 as the explicit negative control against the
   naive "just widen the window" version, is careful engineering.
3. **The Map rewrite worked.** Hundreds of per-primitive `lv_obj_t`s → 22 objects for the whole
   tile, z-order preserved, 90 goldens byte-identical. It is now the cheapest face on the device.
4. **The arrow's τ = 250 ms wrap-aware smoothing, and the LOST/STALE visual language**, do their
   job in half a second with no text.
5. **The status vocabulary is disciplined and the jargon quarantine holds** — `LOST` survives only
   in two debug dumps, and `RSSI`/`SNR`/node ids never leave Diagnostics.
6. **`shell_coarsen_age_ms`** killing sub-second render-key churn at the source, with the same
   bucketing `ff_fmt_age` already used, is the kind of fix that solves three bugs at once.
