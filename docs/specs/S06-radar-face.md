# S06 · app/radar — the Radar face

## Purpose
The hero screen: honest arrow to the selected crew member, whole-crew ring, three states. Mockups are the layout authority (412×412 circle; "Radar — live / stale / close range" artboards).

## State in, pixels out
Radar renders `ff_radar_view_t` computed in core (`core/src/ff_radar.c`) from crew + geo + clock:
```c
typedef enum { RADAR_LIVE, RADAR_STALE, RADAR_LOST, RADAR_CLOSE, RADAR_NOFIX, RADAR_NOSEL } radar_mode_t;
// DRIFT GUARD (PR #12 review finding #5): until this lands in
// core/include/ff_radar.h, firmware/app/include/ff_app_state.h's
// ff_app_radar_t mirrors this struct field-for-field under a different
// type name (two anonymous structs with an identical member list are
// still distinct C types under the same typedef name — reusing
// ff_radar_view_t there would be a redefinition hazard the moment a
// screen file includes both headers). Whoever implements this slice:
// either (a) delete ff_app_radar_t and point ff_app_state_t.radar at
// the real ff_radar_view_t below (preferred), or (b) if both must
// coexist a while longer, update ff_app_state.h's copy in the same
// change as any field added/removed/retyped here.
typedef struct {
  radar_mode_t mode;
  float arrow_deg;            // smoothed screen rotation
  bool  arrow_valid;          // false in CLOSE/NOFIX/NOSEL
  char  name[16]; char dist_str[12]; char age_str[12];
  int8_t trend;               // −1/0/+1 (CLOSE mode hot/cold)
  struct { float ring_deg; char initial; uint8_t color_idx; bool stale; } dots[FF_CREW_MAX];
  uint8_t n_dots;
  char clock_str[6]; int8_t batt_pct; bool mesh_ok;
} ff_radar_view_t;
void ff_radar_compute(ff_radar_view_t *v, ff_crew_t *crew, float heading_deg,
                      ff_latlon_t my_pos, bool my_pos_ok, uint32_t now_ms);
```
- Mode: NOSEL if no paired member; NOFIX if `!my_pos_ok` or heading invalid (shows zone-chips per S02 data, arrow hidden, "NO FIX · RADIO ONLY" label); CLOSE per S02 predicate; else LIVE/STALE/LOST from freshness.
- Arrow smoothing: exponential, time-constant 250 ms, wrap-aware (`ff_geo_angdiff_deg`). Update rate 10 Hz.
- Crew ring dots at each member's bearing (heading-relative); stale members dashed.

## Rendering (LVGL, `app/screens/scr_radar.c`)
- Layout constants transcribed from mockup (arrow 140 px glyph rotated; name 21 px; distance 36 px mono; chip pill). Colors from `app/theme/ff_theme.h` (amber #FFC66B, stale #FFB454, live-green #9BE07B, crew palette pink/teal/violet/green). Fonts: Montserrat-LVGL stand-ins v1, custom fonts tracked as issue.
- STALE: dashed arrow at 28% opacity, amber rim tint, "LAST SEEN …" chip (12 px equivalent).
- LOST (a real, aged fix — distinct from a paired member who has never sent one at all, see `ff_radar.h`'s RENDERER CONTRACT): must read as a *different* state from STALE, not a dimmer copy of it — no rim tint, an outline-only ("ghost") arrowhead rather than a filled one, distance prefixed with `~` to signal imprecision, and a chip in a muted/dark color distinct from STALE's bright amber (PR #16 UX review, orchestrator ruling: "STALE says this is a few minutes old, LOST says do not trust this").
- CLOSE: arrow → three pulsing rings (LVGL anim, 1.2 s period), "~Nm" big, trend chip, FLARE button (48 px high, full hit area).
- Status bar alert color: mesh-loss ("NO MESH") and low battery both render in the amber alert color (`FF_THEME_COLOR_STALE_AMBER`), not flat chrome grey — losing the mesh radio breaks the puck's entire purpose and must look at least as alarming as a low battery, not less (PR #16 UX review finding #6). Low-battery threshold is `FF_THEME_BATT_LOW_PCT` (`app/theme/ff_theme.h`) = 15% — a product-judgment call, not derived from S02/S03 (neither defines a battery threshold), documented here so this is its one source of truth.
- **Layout resolution (`app/screens/radar_layout.h`/`.c`).** Crew ring dots (and the arrow) are heading-relative and can land anywhere regardless of what fixed chrome (chips, buttons, the name/distance block, the status bar, the page-dot row, the ring stack) happens to occupy that bearing. A single reserved-region registry is built once per render (every fixed chrome rectangle, no exceptions), and every movable element resolves against the WHOLE registry:
  - **The arrow** resolves by *shortening* along its true bearing (a 1-D search over length, testing the full head — tip + both base corners — against the registry union fresh at each step) until it clears every reserved rectangle or hits a length floor. It never moves off-axis — an arrow may go short, it must never point somewhere it doesn't mean.
  - **Each ring dot** resolves by searching for the nearest *clear angle* at the ring's fixed radius (a 1-D search over angle, same union-tested-per-candidate approach). Iterative "push out of whichever rectangle you're in" was tried first and rejected: when two reserved rectangles overlap each other (which they legitimately do, e.g. CLOSE's ring stack and its status bar/name-chip rects), push-out-of-A can land a point inside B and push-out-of-B lands it back inside A — an oscillation a fixed pass cap just stops mid-way through, still colliding. A fresh full-registry test per candidate has no such failure mode: there's nothing to oscillate between, only "does this candidate work, yes or no."
  - **Dot clustering, never hiding.** If two or more dots' resolved positions still land within one dot-diameter of each other (crew converging on nearly the same bearing — an ordinary festival scenario, not a hypothetical), they are merged into a single **cluster marker** showing the member count (a dot showing "3" instead of an initial) rather than any of them being hidden. Silently dropping a *known* crew member from the ring is an honesty failure in the same family CLAUDE.md's "never fake freshness, positions, or times" rules out — the app knows they're there; the screen must say so, even coarsely. Every dot is always accounted for in exactly one marker.
  - **Cluster marker styling: a wedge per member, not a badge** (issue #18, amended 2026-08-23 — supersedes this paragraph's original "in a neutral surface color rather than any one member's crew color"). The count digit sits inside a ring divided into one **wedge per clustered member, in that member's own crew color**, each wedge dimmed if that member's fix is stale — the same per-member freshness a lone dot's filled/ghost treatment carries. The original neutral styling was reasoned from a true premise (no *single* crew color applies honestly to a mixed group) to a conclusion that cost more than it saved: white-outline-plus-digit is the visual vocabulary of a notification badge, while every other crew element on this ring is color + letter, so the one marker meaning "several of your friends are standing together over there" was the one marker that read as generic chrome. Giving each member their own wedge answers the honesty objection instead of trading it away — no color is claimed *for the group*, and the marker now says *which* friends, which the neutral version could not say at all. Geometry (equal gapped slices, first wedge starting at 12 o'clock so a pair splits left/right) is `radar_layout_cluster_wedges` in `app/screens/radar_layout.h`/`.c`, unit-tested alongside the resolver; `scr_radar.c` only paints the angles it returns. The marker keeps its exact one-dot-diameter footprint, so no collision geometry changes.
  - See `radar_layout.h`'s doc comment for the full algorithm and its termination bounds, and `app/screens/tests/test_radar_layout.c` for the geometry-level regression coverage (a full tenth-of-a-degree sweep per mode, not a golden-PNG proxy) — PR #16 UX review round 3 and code review round 2.
- Input: tap center = cycle selected member; swipe left/right = face nav (owned by S06's shell `scr_nav.c`: three faces + page dots); long-press = settings (S11).

## Acceptance criteria (compute = unit; render = golden PNG)
1. Mode truth table (10 rows: fresh/stale/lost/close/nofix/nosel × pos/heading validity) exact.
2. Smoothing: step 0→90° reaches ≥81° at 600 ms, no overshoot >2°; 350→10° goes through 0, not 180.
3. Dots: 4-member fixture → bearings/colors/stale flags exact; unpaired members excluded.
4. Golden screenshots: fixtures `radar_live.json`, `radar_stale.json`, `radar_close.json`, `radar_nofix.json` render pixel-identical to goldens (allow ≤0.5% differing pixels for AA).
5. Tap-cycle changes selection; swipe changes face (sim input-injection test).
6. Compute is allocation-free and runs <1 ms on host (sanity perf bound).

## Slices
a) ff_radar_compute + tests · b) shell/nav + theme · c) live+stale render + goldens · d) close+nofix render + FLARE button hook (fires S10 callback).
