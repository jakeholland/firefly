# S09 · app/map — vector map face

## Purpose
Radar's alternate view: the festpack's features + crew dots + rally pin + YOU marker. Mockup "Map — Lost Lands" is layout authority.

## Behavior
- Camera: fixed-fit v1 — bounding box of all features (fallback: 1 km square around origin) mapped into the 412 circle with 24 px margin; north-up. Pan/zoom is v2.
- Transform: feature east/north meters (from S05) → screen px; single scale+offset; crew/rally/YOU share it (`ff_map_xform_t` in core, unit-tested).
- Render: polygons as filled 13%-alpha + 1.3 px stroke in kind/stage colors; labels ≥10 px mono, centered; crew dots = 18 px rings with initials (stale ⇒ dashed); YOU = white arrow rotated to heading with "YOU" label; rally pin amber. Features with n_pts 0 (untraced) are skipped except stages, which render as labeled 30 m circles at origin-offset stubs **only if** they carry a point; otherwise omitted (no invented geometry).
- Tap anywhere → back to Radar (per mockup pill).

## Acceptance criteria
1. xform: fixture bbox → all points inside circle radius − margin; aspect preserved; degenerate single-point bbox handled.
2. Crew dot screen positions match hand-computed fixture (3 members) within 1 px.
3. Untraced-feature policy: Lost Lands pack (all polygons null) renders labels-only stub state without crash — golden `map_untraced.json`.
4. Golden with synthetic traced pack (`map_traced.json`: 5 polygons) matches.
5. YOU arrow rotates with heading fixture; hidden + "NO FIX" chip when no fix.

## Slices
a) xform + tests · b) render + goldens.

## Amendments

- **2026-08-25, implementation PR — routing: Map is a THIRD MODAL face
  (alongside Compose/Settings), not a fourth swipe tile.** The spec's
  own framing ("Radar's alternate view", "Tap anywhere -> back to
  Radar") reads as this codebase's existing modal-dismiss idiom
  (`FF_INTENT_BACK` popping `ff_route_t.modal`) rather than the bounded
  three-face swipe axis (`RADAR < NOW < SIGNALS`, `app/ff_route.c`),
  which has no "tap anywhere" exit and whose page-dot row would need to
  grow for a fourth tile. `[api]`: `ff_app_face_t` gains
  `FF_APP_FACE_MAP`; `ff_route_push_modal` accepts it alongside
  `FF_APP_FACE_COMPOSE`/`FF_APP_FACE_SETTINGS`; `ff_intent_t` gains
  `FF_INTENT_OPEN_MAP`. `k_swipe_axis` (`ff_route.c`) is UNCHANGED — this
  choice touches no existing radar/now/signals golden. See
  `ff_app_state.h`'s `FF_APP_FACE_MAP` comment for the full reasoning.
  **Left deliberately unwired in this PR:** a visible tap target ON
  Radar that emits `FF_INTENT_OPEN_MAP` — adding one would mean editing
  `scr_radar.c`/`radar_layout.c`'s reserved-chrome registry, which risks
  shifting existing radar goldens' dot/arrow placement, and this repo's
  own review culture (S16's Settings-judgment-call precedent) treats
  "ship the reachable screen before its own affordance exists" as
  honest under-claiming, not a defect, when the alternative is guessing
  mockup pixel geometry this agent cannot see. Tracked as a follow-up.
- **2026-08-25 — untraced-feature render policy, spelled out precisely.**
  The spec's own wording ("skipped except stages... only if they carry a
  point; otherwise omitted") is read as: `n_pts == 0` -> omitted entirely
  (no polygon, no label — nothing to honestly anchor one to); `n_pts ==
  1` -> a STAGE gets the named 30 m labeled stub circle, any OTHER kind
  gets a label only, anchored at that one point, no invented shape (the
  spec's stub treatment is stated for stages specifically); `n_pts >= 3`
  -> the normal filled+stroked polygon. `n_pts == 2` (not in the spec's
  own worked examples) renders as a plain stroked line between the two
  points, no fill — the same "draw exactly what the data states, invent
  nothing past it" reading. See `scr_map.c`'s header comment.
- **2026-08-25 — the fixed-fit circle uses the INSCRIBED SQUARE, not the
  bare diameter.** Scaling so the bbox's longer side spans
  `2*(radius-margin)` only guarantees the bbox's flat edges stay inside
  the circle; a feature at a non-square bbox's corner can still land
  outside it. Scaling to the side of the square inscribed in the usable
  circle (`(radius-margin)*sqrt(2)`) is the only choice that keeps every
  bbox point — corners included — inside the circle, which is what AC1
  actually requires ("all points inside circle radius - margin"). Costs
  some unused margin on a non-square bbox (the common case); accepted as
  the honest trade against ever letting a real feature clip. See
  `core/include/ff_map.h`'s doc comment.
- **2026-08-25 — degraded-precision crew honesty, and where the Radar
  reuse stops.** `stale` (dashed/hollow ring) and `place` (an asserted
  position, carried through but with no map-specific render yet — a
  recorded gap, see `ff_app_map_crew_t`'s doc comment) map onto a map dot
  the same way they map onto a Radar ring dot. `imprecise` does NOT reuse
  Radar's treatment verbatim: Radar can fall back to an honest AREA
  string because it never drew a pin-point shape to begin with, but a
  map dot IS a pin-point claim by construction, so an imprecise crew
  member renders as a larger, hollow, no-initial "fuzzy" ring instead of
  the normal 18 px dot — never a crisp point a degraded fix can't
  support. See `scr_map.c`'s `map_draw_crew`.
- **2026-08-25 — rally has no live source yet.** `ff_crew_select_rally()`
  is not implemented in core (`core/include/ff_crew.h`'s own documented
  gap, deferred to S06/S08). `ff_app_map_t.has_rally` is therefore always
  `false` on a live projection today; the render path and its fixture/
  golden (`map_traced.json`) exist and are exercised, ready for the day
  a real rally selection lands.
