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
- Crew ring dots are heading-relative and can land anywhere on the ring regardless of what fixed chrome (chips, buttons, the name/distance block, the status bar, the page-dot row) happens to occupy that bearing. `scr_radar.c` applies a generic keep-out policy: fixed chrome claims rectangles, and any dot that would overlap one is pushed out along the nearest edge (not merely pulled toward center — a purely radial pull-in cannot escape a keep-out band a southward bearing points straight into) and clamped to stay inside the puck. See `radar_resolve_dot_collision` (PR #16 UX review finding #3).
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
