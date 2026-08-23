# S09 · app/map — vector map face (stretch for Lost Lands)

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
