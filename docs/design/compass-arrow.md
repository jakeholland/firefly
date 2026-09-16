# One compass arrow — app and puck share the same dart

Owner (Jake), 2026-09-15: "We should use the same arrow style in the puck as
the app has. Maybe align both to be a tad fatter." This doc is the single
geometric definition both surfaces implement from. Written first, per the
task brief, so the app and firmware changes are two renderers of one shape
rather than two hand-matched approximations of each other.

## The shape

A notched dart (a four-point kite with a concave "swallowtail" notch on the
tail edge), centered on the ring/compass centre, rotated by the bearing.
Same silhouette the app's `RadarRingView.swift` `ArrowShape` already drew —
this change widens it and gives the puck the identical shape instead of its
own separate outward-pointing triangle-on-a-stick.

Let **L** = total length, tip to base line. The rotation pivot is the ring
centre. With the bearing as the forward axis (0° = "up"/ahead) and
"perpendicular" as 90° from it:

- **Tip**: `+0.5·L` along the bearing.
- **Base corners** (2 points): `-0.5·L` along the bearing, offset `±0.5·W`
  perpendicular.
- **Notch**: on the axis (no perpendicular offset), at `-0.25·L` along the
  bearing — i.e. `0.75·L` back from the tip, `0.25·L` in front of the base
  line. This is what turns a plain triangle into a dart: the tail edge is
  pulled forward into a point instead of running straight across.
- **Width** `W = 0.23·L` — "a tad fatter." The app's shape today works out to
  `28 / (0.9·radius) ≈ 0.19`; 0.23 is about 20% fatter than that.

Outline, in order: tip → right base corner → notch → left base corner → tip.
Filled, this is star-shaped from the tip, so it triangulates as
`(tip, right, notch)` + `(tip, notch, left)` — both triangles convex, their
union exactly the dart, no separate polygon-fill primitive required.

## Per-surface `L`

Both surfaces already had a governing radius; `L` is 0.9× it, unchanged from
what the app does today — only `W`'s ratio to `L` changes.

- **App** (`RadarRingView.swift`): `ringRadius = side * 0.42` (unchanged);
  `L = 0.9 * ringRadius`. The SwiftUI shape is drawn in a local rect
  `width: W, height: L`, `rotationEffect` around the rect's own centre
  (`.position(center)` after rotation — same as today), tip at
  `(midX, minY)`, base corners at `(minX, maxY)`/`(maxX, maxY)`, notch at
  `(midX, maxY * 0.75)`. That local-rect layout is *already* exactly this
  geometry (tip/base symmetric about the rect's vertical centre, notch at
  0.75 of the height from the top = `0.75·L` from the tip). The only line
  that changes is the frame: `width: 28` (fixed points) becomes
  `width: 0.23 * height` (`height` already being `L`).

- **Puck** (`radar_layout.h`/`.c`, `RADAR_LAYOUT_RING_RADIUS_PX = 185`):
  `L = 0.9 * 185 = 166.5 px`. Unlike the app, the puck's arrow can collide
  with fixed chrome (name/distance/chip stacks, status bar, page dots) and
  has to shorten along its bearing to clear it (`radar_layout_resolve_arrow`)
  — CLAUDE.md's honesty rule: an arrow may go short, it must never point
  somewhere it doesn't mean. `RADAR_LAYOUT_ARROW_LEN_PX` keeps its existing
  *name* and its existing *meaning* — the tip's reach from centre, the
  resolver's search-starting length — but its *value* becomes `0.5·L =
  83.25 px` (`0.45 · RADAR_LAYOUT_RING_RADIUS_PX`) instead of the old 140.
  `RADAR_LAYOUT_ARROW_WIDTH_RATIO = 0.23` replaces the old fixed
  `HEAD_LEN_PX`/`HEAD_WIDTH_PX` pair (a separately-sized triangular head
  bolted onto a full-length shaft) — the new shape has no separate "head":
  the whole dart *is* the arrow, and shortening scales the whole dart
  in toward the centre (tip, base corners, and notch all move together,
  proportionally), not just the old head sliding down a fixed-length shaft.

  Numbers: `L = 166.5`, tip reach `= 83.25`, `W = 0.23 * 166.5 ≈ 38.3`
  (half-width `≈ 19.15`), notch reach `= -0.25 * 166.5 = -41.625` (`41.625`
  behind centre, `124.875` behind the tip = `0.75 * 166.5`).

## Before / after (puck)

| | before | after |
|---|---|---|
| Shape | full-length shaft (centre → base) + separate filled/outline triangular head stuck on the end, pointing *outward from centre* | one notched dart, *centred on* the ring centre (tip ahead, base behind) |
| `RADAR_LAYOUT_ARROW_LEN_PX` | `140` (tip's reach from centre; old shaft+head both lived inside this) | `83.25` (`0.45 · RING_RADIUS_PX`; tip's reach from centre — same role, new value) |
| Head shape constants | `ARROW_HEAD_LEN_PX = 46`, `ARROW_HEAD_WIDTH_PX = 34` (fixed px, independent of reach) | replaced by `ARROW_WIDTH_RATIO = 0.23` (`W` scales with `L`, so shortening scales the whole dart, not just a fixed-size head sliding along a shrinking shaft) |
| `ARROW_REACH_LOCKED_PX` | `92` — a separate, shorter search-starting length while the FLARE lock chip shows, derived to keep the *old* head's topmost pixel clear of the chip's band | same value as the new unlocked `ARROW_LEN_PX` (`83.25`) — the shorter centred dart's ordinary reach already clears the chip's band (bottom edge at y=-108) by ~25px, well past the 8px floor this fix chain uses, so a separate shorter cap is no longer load-bearing. Kept as a distinct named constant (not deleted) for call-site/API stability and because a future chip reposition could reintroduce the conflict this constant exists to name. |
| Collision search | 1-D search shortens the tip's reach; the head (fixed 46×34) slides down the shrinking shaft, testing tip + 2 head-base corners each step | 1-D search shrinks a single scale factor applied to the *whole* dart (tip reach, base reach, notch reach, and half-width all move together); tests tip + 2 base corners + notch each step |

## Locked / CLOSE-mode behaviour

The original `ARROW_REACH_LOCKED_PX` fix existed to stop the arrow's
*head* — "come find me," the product's whole point — from being painted
under the FLARE lock chip for a narrow bearing cone around due north. With
the centred dart's much shorter ordinary reach (83.25px vs the old 140px),
the tip's worst-case excursion toward the chip (bearing 0, the same
worst-case bearing the original derivation used) is `-83.25`, and the
chip's bottom edge is at `-108` — a 24.75px gap, already past the 8px floor
`S10_locked_arrow_head_clears_the_lock_chip` checks for, with no special
casing. So: **locked and unlocked now resolve to the identical starting
reach.** `radar_layout_resolve_arrow`'s `locked` parameter at every
`scr_radar.c` call site is unchanged (still passed through, still selects
between the two named constants), so no call site needed editing — only
the two constants' *values* converged. This is a simplification, not a
regression: the invariant the original fix guaranteed ("a locked arrow's
head can never reach the chip's band, at any bearing") now holds by
construction rather than by a separate shorter search bound, and the test
that pins it is unchanged and still green.

If the ARROW_REACH_LOCKED_PX/ARROW_LEN_PX values are ever revisited such
that locked reach would again come within the 8px floor of the chip's
band, `radar_layout_resolve_arrow`'s ordinary registry search is still the
backstop — it always shortens further if the resolved dart's tip, base
corners, or notch land inside *any* reserved rectangle, the chip included
were it ever registered.

## Collision-shortening rule (S06 update)

`docs/specs/S06-radar-face.md`'s "arrow 140 px glyph" sentence and its
description of the shortening search (fixed head sliding down a shaft) are
updated to describe the centred dart and the new proportional-scale search.
The rule itself — "never move off-axis, only ever give up length" — is
unchanged; only the shape being shortened and what "shortening" scales
changed. See that spec's own Amendments section for the dated entry.

## Puck arrow treatments (mirroring the app's three)

The app already drew three treatments of `ArrowShape`; the puck's three
`radar_arrow_style_t` values are redefined to match, replacing the old
"tail line + head" split (which no longer makes sense — the new dart has
no separate tail):

| Treatment | When | Puck rendering |
|---|---|---|
| Filled amber (real bearing) | LIVE, PLACE (muted color) | Both dart triangles filled, full opacity, no outline. |
| Stale | STALE | Both dart triangles filled at reduced opacity (`LV_OPA_71`, unchanged from before) in the stale amber, **plus** a dashed outline traced around all four edges (tip→right, right→notch, notch→left, left→tip) at fuller opacity — LVGL has no native dashed-line primitive, so the dash is approximated the same way the old tail dashing was: short sub-segments with gaps, now applied per-edge instead of only along the old tail. Mirrors the app's `fill(opacity 0.28).overlay(stroke dashed)`. |
| Placeholder / outline (bearing not real) | LOST (real prior fix, "ghost"), SIGNAL ghost | Outline only, no fill: four edges (tip→right, right→notch, notch→left, left→tip) drawn as plain (non-dashed) thin segments — same "outline-only head, distinct in kind from a dimmer copy" ruling as before, now outlining the whole dart instead of just the old triangular head. |

`FF_THEME_COLOR_AMBER` / `FF_THEME_COLOR_STALE_AMBER` / `FF_THEME_COLOR_MUTED`
and their opacities are unchanged from before this change — only the shape
being painted with them changes.

## FLARE takeover arrow

Checked both `app/Firefly/Sources/*/FlareTakeoverView.swift` and the puck's
flare face (`firmware/app/screens/scr_flare.c`, `flare_fmt.c/.h`): **neither
draws an arrow.** `FlareTakeoverView.swift`'s own doc comment is explicit —
"S10's own honest vocabulary: a real bearing/distance when both positions
are known, otherwise a plain admission that neither is — never a fabricated
arrow" — it renders `"<distance> <compass point> of you"` as text. The puck
side (`scr_flare.c`) has no triangle/arrow drawing calls at all. No change
needed on either FLARE surface.
