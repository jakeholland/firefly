# S17 · usability hardening — legible when tired, drunk, or colorblind

## Purpose

The app is feature-complete; this makes it *usable* in the state it's used in —
hour 9, day 2, 2 a.m., one thumb, and for the ~8% of men who can't tell the
crew colors apart at all. The through-line: **never let a single perceptual
channel carry meaning alone.** Colour is backed by shape and text; a tap target
is big enough to hit without aiming.

Scoped by owner intent (2026-08-25): a **colorblind toggle** (not colorblind by
default — keep the brand colours), and **no Festival Mode** (deferred). The
hardening that isn't a toggle — tap-target floor — is baseline, not optional,
because the person who needs forgiveness can't reach a setting to enable it.

## Slice a — crew visual distinction (closes #43, #74)

The durable identifier for a crew member is already their **initial letter** in
the ring, backed by colour. This slice makes that redundancy hold everywhere.

1. **Extend the crew palette to 8, keeping the brand 4 as indices 0–3.**
   `ff_theme_crew_color` currently wraps a 4-entry table mod-4, so members 4–7
   duplicate 0–3 — a full crew guarantees indistinguishable adjacent cluster
   wedges (#43). Add four more distinct colours as indices 4–7. **Indices 0–3
   are byte-identical to today**, so every scene with ≤4 crew renders unchanged
   (no existing golden moves); only 5+ member scenes gain the new colours. That
   closes #43 without discarding the brand palette.

2. **A colorblind toggle in Settings** (`ff_settings_t.colorblind`, new `[api]`
   field + `FF_SETTING_COLORBLIND`, wired through the existing SETTING_SET
   path and the Settings face). When on, `ff_theme_crew_color` returns a
   **colorblind-safe 8-colour palette** instead — designed for the common
   dichromacies (deutan/protan): maximise luminance separation, avoid red↔green
   and the blue↔purple confusions, don't rely on hue distance alone. Cite the
   palette's basis in the header (e.g. an Okabe–Ito-derived set, which is the
   standard public-domain colorblind-safe 8). The toggle is read at render time;
   flipping it re-tints live (no restart).

3. **Cluster wedges get a same-colour divider.** Even with 8 colours, a big crew
   can put two same-index members adjacent in a rim cluster. A 1px background-gap
   between adjacent wedges makes them countable regardless of colour — the count
   is what matters (the standing #18 ruling). Belt-and-suspenders with (1), and
   the only thing that helps a *monochromat*.

4. **Imprecise-precision crew get a shape treatment on Radar (#74).** The Map
   face already fuzzes a degraded-precision dot; Radar still draws it crisp — the
   same fact rendered two ways. Give the radar crew dot the map's fuzzy-ring
   idiom so a ~5.8 km-cell friend never looks metre-accurate. This is the same
   principle as the whole slice: precision is a fact, carried by shape, not left
   to a colour or a crisp edge implying more than the data holds.

### Slice a acceptance criteria
1. `ff_theme_crew_color(0..3)` returns the exact current brand hexes; `(4..7)`
   returns four new distinct colours; no index wraps/duplicates within 0–7.
2. `colorblind=true` returns the colorblind-safe palette for all 8 indices;
   `false` returns the brand palette; the field round-trips through the store
   (shell closed + re-inited preserves it).
3. Cluster wedges of two same-index members are separated by a visible divider
   (golden with an 8-member cluster forcing a repeat).
4. An imprecise-precision crew member renders with the fuzzy treatment on Radar,
   not a crisp dot; a full-precision member is unchanged (golden pair).
5. Goldens: every ≤4-crew existing golden byte-identical (palette indices 0–3
   unchanged); new goldens for the 8-colour crew, the colorblind palette, and
   the radar-imprecise dot.

## Slice b — tap-target floor (baseline, enforced in CI)

`test_face_hit_targets.c` already sweeps every face for controls that fall off
the round glass. Extend it into the tested invariant that makes the app hittable
while impaired:

1. **Every tappable control's hit area is ≥ `FF_HIT_MIN_PX` (44px) on its
   shorter dimension**, with hit-slop allowed to extend *beyond* the visible
   chip (the tappable region ≥ the drawn region). Assert this across all faces
   in the existing sweep — a build gate, not a guideline. Bailey caught the 10px
   Settings pill gap by eye (PR #68); this catches the next one by machine.
2. **Adjacent independent controls are ≥ `FF_HIT_MIN_GAP_PX` apart** (centre-to-
   centre or edge-to-edge — justify) so a thumb can't hit two at once. The
   Settings double-chip rows (#68) set the precedent; make it a rule.
3. Where a real control violates the floor, **fix the layout** (this is the
   point — it surfaces real mis-tap traps), or if a control is legitimately
   exempt (an indicator, not a control), mark it non-interactive so the sweep
   skips it honestly rather than loosening the floor.

### Slice b acceptance criteria
1. The sweep fails if any interactive element's hit area < 44px shorter side;
   passes on the current faces once real violations are fixed (enumerate what
   was fixed).
2. The sweep fails if two interactive elements are closer than the gap floor;
   passes once fixed.
3. Mutation: shrinking any control below the floor fails the sweep (not absorbed
   by a golden threshold — an assertion, per the proxy-check rule).

## Also folds in

- **#77 label polish** — nudged labels crossing their stub-ring, and (if #75
  didn't already) ultra-long labels off-circle. Legibility, same phase. Slice b
  or its own small PR — implementer's call.

## Slices & order

a (crew distinction, theme + crew render across faces) then b (tap-target floor,
the hit-target sweep + layout fixes) — sequential, because both touch screen
files and running them together courts merge pain. Slice a is the owner's
headline ask; ship it first.

## Amendments

- **2026-08-25, slice a implementation — interpretation calls (AGENTS.md: "note the interpretation in the PR body" — recorded here too, per this repo's own convention of amending the spec once a slice resolves an ambiguity it hits).**

  - **AC3's wedge divider already existed.** The 1px background-gap between adjacent cluster wedges this AC asks for turned out to already be `radar_layout.c`'s `RADAR_LAYOUT_CLUSTER_WEDGE_GAP_DEG` (12°, added for issue #18/PR #41 for a different reason — legibility of the wedge ring in general, not specifically same-color pairs) — it separates EVERY adjacent wedge pair by dark marker fill regardless of color, which already satisfies "countable regardless of colour". No new render logic was needed; this slice adds `radar_cluster_8_samecolor.json`/its golden (an 8-member cluster with two adjacent members forced to the same `color_idx`) as the regression proof this AC asks for, plus confirmed via mutation (zeroing the gap) that the PRE-EXISTING geometry tests in `test_radar_layout.c` (`test_cluster_wedges_are_equal_gapped_and_cover_the_ring`, `test_cluster_wedges_two_members_split_left_right`) catch the regression — the golden's own 0.5% pixel-diff threshold, checked directly, does NOT (a dropped gap changed only 0.11% of the frame). The geometry tests are the real enforcement here, same as this repo's own `tests/fixtures/README.md` already says for `radar_close_collision.json`'s resolver coverage.

  - **Issue #74's fuzzy ring keeps the map idiom (hollow, no-initial ring) but NOT the map's literal enlarged footprint.** `scr_map.c`'s `FF_MAP_IMPRECISE_RING_PX` is ~2.2x the normal dot. Radar's ring-dot placement (`radar_layout_resolve_dots`) only guarantees adjacent MARKERS are `>= RADAR_LAYOUT_DOT_PX` apart, not more — a visually-enlarged single dot at that spacing could overlap a neighbour the resolver placed right at that boundary. Kept at the same `RADAR_LAYOUT_DOT_PX` footprint every other lone dot uses; "not a crisp dot" is carried by dropping the initial letter and a markedly thicker, softer-opacity border instead of by size. Scoped to the LONE-dot render path only — a clustered member that is also imprecise still draws as an ordinary crew wedge, the same documented "known gap, not silently invented" precedent this file's own `d->place` handling already set for the clustered case.

  - **The colorblind-safe palette's 8th colour is not Okabe-Ito's literal 8th.** The canonical set's 8th entry is black, picked for a white page; black is invisible against this app's near-black `FF_THEME_COLOR_BG`. Substituted with a pale, low-saturation mauve (kept out of the crew-green hue band deliberately, so the "extra" colour doesn't itself introduce a red/green-adjacent confusion). The other seven Okabe-Ito hexes are unmodified.

  - **Two moderate (not exact-duplicate) collisions in the unmodified Okabe-Ito hexes, left as-is and flagged rather than retuned.** Measured (CIE76 ΔE\*ab): Okabe-Ito's "orange" (`#E69F00`) sits ΔE≈18 from this app's `FF_THEME_COLOR_STALE_AMBER` chip (`#FFB454`); its "sky blue"/"blue" (`#56B4E9`/`#0072B2`) sit ΔE≈21/23 from `FF_THEME_MAP_POI` (`#6B8CAE`). None are the bit-for-bit duplicate PR #73 found and fixed for `FF_THEME_MAP_CAMPING`/`FF_THEME_CREW_VIOLET` — retuning a cited, externally-verified colorblind-safe set is exactly the kind of well-meant edit that can quietly undo the verification the citation exists for, so the canonical hexes stand. Flagged for a human legibility pass, same as this bullet exists to do.

  - **Settings' new COLORBLIND row is a single full-width self-describing chip ("COLORBLIND ON"/"COLORBLIND OFF"), not the label+chip shape rows 2/3 use.** It is the lowest row on the face, close enough to the puck's pole that `settings_safe_margin_x` returns a much larger margin there than higher rows get — a fixed-110px chip (rows 2/3's shape) left the label only ~28px of width, under the 44px hit-target floor (`test_face_hit_targets.c` caught this in review). A single chip spanning the row's own margin-to-margin width scales with the available space instead of fighting it, the same way row 1's half-width chips already do.

- **2026-08-25, code review round 1 (PR #83) — closed the AC4 analogue of the AC3 proxy-check gap above, and two documentation notes.**

  - **[Blocking, fixed] AC4's render branch had no assertion coverage — only the golden pair.** Same failure mode as AC3's wedge-gap mutation, just not caught before the first review pass: disabling `scr_radar.c`'s entire `if (d->imprecise) { … }` block (dot renders crisp, as if fully precise) left `ctest` at 37/37 and `run_goldens.sh` at 43/43 — the affected golden (`radar_dot_imprecise.png`) moved by 0.4506%, under the 0.5% threshold. Closed by two new tests in `app/screens/tests/test_scr_intent.c` (`S17a_AC4_radar_precise_dot_renders_filled_with_its_initial`, `S17a_AC4_radar_imprecise_dot_renders_as_hollow_ring_with_no_initial`) that build a single-dot `ff_radar_view_t` directly, call `lv_obj_update_layout`, and assert `lv_obj_get_style_bg_opa`/`border_width`/`border_opa` and the dot's own label text on the real rendered `lv_obj_t` — the same LVGL-introspection idiom `test_face_hit_targets.c` already uses, applied to style state instead of hit-rects. Re-ran the reviewer's exact mutation on a fresh build (binary hash confirmed changed): the new imprecise-dot test fails (`Expected 0 Was 255` on `bg_opa`); the precise-dot test still passes (positive control, unaffected).

  - **[Non-blocking, documentation] AC1's "byte-identical" claim is enforced by `test_theme.c`, not the goldens.** Mutating `FF_THEME_CREW_PINK` by one LSB (`0xFF5CA8` -> `0xFF5CA9`) fails `test_theme.c` immediately but leaves every golden green (`radar_crew8.png` moves 0.4333%, under threshold). Stated explicitly here for the same reason AC3's note above is: the goldens freeze a render for regression *detection at the pixel level*, they do not re-verify semantic facts like an exact hex value or a style property — `test_theme.c`'s exact-value assertions (not the goldens) are AC1's actual enforcement, the same way the new AC4 render tests above (not the goldens) are AC4's.

  - **General pattern, for the next radar/map slice:** this PR closed two independent instances of the same gap (AC3's wedge-gap mutation, AC4's render-branch mutation) where a single-element visual change was real but small enough, relative to the whole 456x456 frame, to sit under `run_goldens.sh`'s shared 0.5% pixel-diff threshold. That threshold is tuned for whole-scene regression detection, not for guaranteeing any one small, specific behavior — a golden pair is evidence a human can eyeball, not proof CI will catch a regression to that one behavior. When a slice's acceptance criterion is about ONE element's specific style/geometry (a border, an opacity, a gap, an exact color), the default should be a direct assertion against the rendered object (LVGL introspection, `test_radar_layout.c`-style geometry, or a unit-level exact-value check) alongside the golden, not the golden standing in alone — write the assertion first, keep the golden for what it's actually good at (whole-scene "did anything unexpected move").
