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
(none yet)
