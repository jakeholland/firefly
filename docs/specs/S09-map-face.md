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
- **2026-08-25 — degraded-precision crew honesty.** `stale` (dashed/
  hollow ring) and `place` (an asserted position, carried through but
  with no map-specific render yet — a recorded gap, see
  `ff_app_map_crew_t`'s doc comment) map onto a map dot the same way
  they map onto a Radar ring dot. `imprecise` gets a MAP-SPECIFIC
  render, corrected here (PR #73 tier-3 review finding #3 — the first
  version of this entry claimed "Radar can fall back to an honest AREA
  string because it never drew a pin-point shape to begin with", which
  is FALSE: Radar's crew ring dots (`ff_radar_dot_t`, `ff_radar_compute`)
  have no imprecision handling at all today — an imprecise member gets
  an ordinary, undegraded, crisp dot there. `ff_radar_view_t.dist_
  imprecise` is a narrower, different fact: it only degrades the
  *selected* member's distance TEXT, never any ring dot's geometry). So
  the map's fuzzy-ring treatment is not "matching what Radar already
  does" — it is a genuinely MORE honest treatment than Radar's current
  status quo: a map dot IS a pin-point claim by construction (unlike a
  bare distance number, which can honestly degrade to an area string),
  so an imprecise crew member renders as a larger, hollow, no-initial
  "fuzzy" ring instead of the normal 18 px dot. Giving Radar's own ring
  dots the same treatment is tracked as issue #74, not this PR's job.
  See `scr_map.c`'s `map_draw_crew`.
- **2026-08-25 — rally has no live source yet.** `ff_crew_select_rally()`
  is not implemented in core (`core/include/ff_crew.h`'s own documented
  gap, deferred to S06/S08). `ff_app_map_t.has_rally` is therefore always
  `false` on a live projection today; the render path and its fixture/
  golden (`map_traced.json`) exist and are exercised, ready for the day
  a real rally selection lands.
- **2026-08-25, PR #73 fix round — five findings from independent
  tier-3 + Bailey (UX) review, both of whom rendered the REAL,
  currently-merged Lost Lands pack (`fest-almanac`) rather than only
  this PR's hand-spaced synthetic fixtures, and both found the same two
  blocking defects that way.**
  1. **[BLOCKING] Silent feature/point truncation, closed.**
     `FF_APP_MAP_MAX_FEATURES`/`FF_APP_MAP_MAX_POLY_PTS` (`ff_app_state.h`)
     were sized against this repo's own vendored (at-the-time all-null)
     test fixture, not the real pack — which has 13 `map.features`
     (9-point "Venue extent" the largest polygon) the moment anyone
     actually renders it. `shell_project_map` silently kept only the
     first 8, in pack order, dropping RV/tent camping, Village
     Marketplace and First Aid with zero signal anything was missing —
     on the exact face meant to say where medical is. Raised to 20
     features / 16 points (real headroom over both the real pack and
     the fest-almanac schema's own ~15-feature guidance — see
     `ff_app_state.h`'s doc comment for the exact numbers) AND any
     overflow that still happens now sets `ff_app_map_t.truncated` (+
     `features_omitted`), rendered as an honest "+N MORE" chip
     (`scr_map.c`'s `map_draw_truncated_indicator`) rather than silently
     dropped — matching `fixture.c`'s existing fail-loud convention on
     the same cap. `ff_shell_t`'s stated footprint grew 16 KB -> 22 KB
     to hold it (`ff_shell.h`'s own doc comment states the exact
     measured numbers and why 22, not a round-trip-safe smaller or
     larger figure).
  2. **[BLOCKING] Concave polygon fill, closed.** The v1 fan-
     triangulation (vertex 0) is only correct for convex/star-shaped
     input; the real pack's "Venue extent" (9 points) is concave (a
     reflex turn at vertex 7, hand-verified via a cross-product
     convexity sweep) and visibly mis-filled, bleeding outside the true
     boundary. Replaced with `ff_map_triangulate` (`core/include/
     ff_map.h`), an ear-clipping triangulation correct for convex OR
     concave simple polygons, unit-tested directly against the real
     Venue-extent coordinates (`core/tests/test_map.c`) — no LVGL, no
     golden-pixel proxy needed to prove the geometry is right. Per this
     entry's own principle (a wrong fill is worse than no fill):
     `ff_map_triangulate` returning a negative "cannot safely triangulate
     this" answer makes `scr_map.c` fall back to STROKE-ONLY rather than
     risk a mis-filled shape — reachable only for degenerate/
     self-intersecting input, which real simple festival geometry should
     never produce.
  3. **[BLOCKING] Camera fit dominated by large boundary polygons,
     closed — `[api]`-adjacent interpretation of "bounding box of all
     features".** Fitting the bbox to EVERY vertex of every feature let
     one or two large boundary polygons ("Venue extent", "Wompy Woods
     treeline" in the real pack) dominate the scale, crushing the
     cluster of things a rider actually cares about (stages, pond,
     camping, ...) into an unreadable smear on real data — the four
     hand-spaced synthetic goldens never surfaced this because nothing
     in them was that lopsided. **Reading, going forward: "bounding box
     of all features" means the bbox of each feature's own
     REPRESENTATIVE ANCHOR POINT** (the single point for a 1-point
     feature, the vertex centroid for 2-or-more), not of every vertex of
     every feature's full extent — `ff_map_xform_fit` itself (the core
     geometry, AC1's own guarantee) is UNCHANGED; only what
     `ff_scr_map_build` feeds it changed. A large polygon's own vertices
     can therefore legitimately render outside the fitted circle now;
     circular clipping the sim's square window to match was attempted
     (`lv_obj_set_style_clip_corner`) and reverted — it reliably hangs
     `ffsim --headless` at this file's draw-object count (issue #75
     tracks the real fix). The consequence until then is sim-only
     cosmetic (a real, physically-round device cannot show anything past
     its own edge regardless). A secondary, cheap mitigation also
     landed: a minimal deterministic label-collision nudge
     (`map_place_label_decluttered`) for the residual case where two
     features' anchor points still land close enough for their labels to
     overlap — bounded, distance-based, not a full text-width-aware
     layout (that would be `radar_layout.h`-scale work; not this fix
     round's scope).
  4. **Entry point wired — swipe-up on the tileview.** The Map face
     shipped fully unreachable (no swipe tile, no tap target, and the
     ctl socket's `swipe` command only accepts `left`/`right`) — both
     reviewers independently refused to accept "the renderer exists" as
     sufficient, correctly: unlike the S16 Settings-judgment-call
     precedent (a genuinely unbuilt renderer, deferred honestly), this
     was a COMPLETE renderer with no door, which is worse, not a lesser
     version of the same thing. `scr_nav.c`'s `nav_swipe_gesture_cb`
     gains an `LV_DIR_TOP` case emitting `FF_INTENT_OPEN_MAP`, from any
     of the three tiles (the shell's routing already treats Map
     uniformly as a modal over whatever base is current, so restricting
     the gesture to Radar-only would be arbitrary). Chosen over a
     visible tap-target specifically because it adds ZERO drawn pixels
     to any existing radar/now/signals golden (verified: all 30
     pre-existing goldens stayed byte-identical through this whole fix
     round) — a real, working entry point without reopening
     `radar_layout.h`'s reserved-chrome registry mid-fix-round. A
     visible, discoverable affordance is still wanted (Bailey: "even an
     unstyled corner glyph beats invisible") and is tracked as issue
     #76, not silently dropped.
  5. **Cheap fixes folded in.** `FF_THEME_MAP_CAMPING` was bit-for-bit
     `FF_THEME_CREW_VIOLET` AND the real pack's own color for "The
     Crater" stage (`#b08cff`) — changed to a warm tan distinct from
     both. `FF_THEME_MAP_POI` was bit-for-bit `FF_THEME_COLOR_INK` (the
     label text color itself), so a POI polygon's outline visually
     merged with the words drawn on top of it (real pack: "Venue
     extent", "Wompy Woods treeline", both kind `poi`) — changed to a
     distinct muted slate-blue. Two mutation-uncaught test gaps closed:
     the paired-only crew gate (`shell_project_map`) and the `n_pts==0`
     omit branch (`ff_map_feature_render_kind`, now a pure core function
     unit-tested directly rather than reachable only through a loose
     golden-pixel-diff threshold).
