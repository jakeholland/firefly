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
centre **on the app**; the puck raises its own pivot — see "Pivot rule"
below for why and by how much. With the bearing as the forward axis (0° =
"up"/ahead) and "perpendicular" as 90° from it, all of the following are
measured from whichever pivot that surface uses:

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

## Pivot rule

**The app keeps its rotation pivot at the ring centre.** It has no fixed
chrome the dart can collide with — `RadarRingView.swift`'s dart is drawn
over an otherwise-empty ring, so there is nothing for a centred dart's
base or notch to run into, and no reason to move the pivot away from the
centre this whole doc otherwise assumes.

**The puck raises its pivot 25 px above the ring centre** (owner, Jake,
2026-09-15, code review of PR #333 — `RADAR_LAYOUT_ARROW_PIVOT_DY_PX =
-25` in `radar_layout.h`, negative = up in that file's screen-space
convention). This is a puck-only divergence, not a change to the shared
shape: the dart is still the identical notched dart defined above, with
the identical `L`, `W`, and per-vertex offsets from *its own* pivot — only
where that pivot sits relative to the ring centre differs by surface.

**Why**: centring the dart on the ring centre (this doc's own change) put
the dart's BASE and NOTCH behind the tip for the first time — on the app
that's harmless (nothing back there to hit), but the puck has a fixed
name/distance/chip text stack sitting just below its ring centre for
LIVE/STALE/PLACE/LOST-with-a-fix, and a narrower version of the same stack
for SIGNAL's ghost variant. `radar_layout_resolve_arrow`'s collision search
already shortens the dart along its bearing to clear that stack, but with
the pivot AT the centre, the code review's own tenth-of-a-degree sweep
measured a worst-case resolved reach of just **35.25 px** — a dart
one-fifth of its own nominal 166.5 px length, for a shape whose entire
purpose is "come find me."

**The numbers.** The review swept candidate pivot shifts against the real
registries before recommending one:

| shift | worst-case reach | locked FLARE-chip margin |
|---|---|---|
| 0 (pre-fix) | 35.25 px | 68.75 px |
| 20 px | 55.25 px | 28.75 px |
| **25 px (chosen)** | **63.25 px** | **19.75 px** |
| 30 px | 67.25 px | 10.75 px |
| 31 px | 67.25 px | 9.75 px (last value ≥ the 8 px floor) |
| 32 px+ | 67–83 px | < 8 px — violates S10's guard |

25 px was chosen because it recovers nearly all of the reach the ceiling
allows (63.25 px vs. ~67–83 px beyond the guard) while leaving the locked
FLARE lock chip a full 19.75 px of margin above the 8 px floor
`S10_locked_arrow_head_clears_the_lock_chip` enforces — comfortable
headroom rather than riding the 31–32 px cliff edge. Re-measured after
raising the pivot (radar_layout.c's real resolver, not hand algebra): the
worst bearing moves from ~6.3° off-axis (unraised, where an off-axis base
*corner* pokes furthest into the stack) back to exactly due north (raised,
where the base midpoint dominates again), and the SIGNAL-ghost registry's
own locked-chip margin improves alongside the main stack's, from 84.75 px
to 35.75 px — both still comfortably clear of the 8 px floor.

**What doesn't change**: the collision-shortening rule itself
("never move off-axis, only ever give up length") and the shortening
search's mechanics (a single scale factor applied to the whole dart) are
untouched — raising the pivot only changes where the shortened dart's
points are measured FROM, not how shortening works. `radar_layout.h`'s own
comment on `RADAR_LAYOUT_ARROW_PIVOT_DY_PX` has the full derivation and is
the canonical source if these numbers and the code ever drift.

**The tested invariant moved with it.** Before this fix,
`test_radar_layout.c`'s sweep test pinned "the tip lies on the bearing ray
from the ring centre" as this codebase's concrete encoding of CLAUDE.md's
"an arrow may go short, it must never point somewhere it doesn't mean."
Raising the puck's pivot means the tip's position *from the true centre*
is no longer purely `length · (sinθ, -cosθ)` — it now carries the pivot's
fixed vertical offset too. The test is updated to check "the tip lies on
the bearing ray from the PIVOT (centre + pivot offset)" instead — the
dart's pointing AXIS (tip through base) is still exactly bearing-aligned
either way, which is the property the honesty rule actually cares about;
only the point the ray is measured from changed, and that point is itself
a named, reviewed constant rather than a silent drift.

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
under the FLARE lock chip for a narrow bearing cone around due north.
`RADAR_LAYOUT_ARROW_REACH_LOCKED_PX` is still numerically equal to
`RADAR_LAYOUT_ARROW_LEN_PX` (locked and unlocked share the same starting
search reach) — that part is unchanged by the pivot raise. What the raised
pivot changes is the *margin* by which the resolved dart clears the chip,
since raising the pivot both recovers reach (the dart's whole point of
existing) and moves the dart's points closer to the chip's own band — both
effects measured, not assumed, by re-running the search:

- **LIVE/STALE/PLACE/LOST-real-fix registry**, locked, bearing 0 (the same
  worst-case bearing the original fix's derivation used): resolved reach
  is now `63.25 px` (was `39.25 px` pre-raise), tip at `y = -88.25` — the
  chip's bottom edge is at `y = -108`, a **19.75 px** margin (was
  `68.75 px` pre-raise).
- **SIGNAL-ghost registry** (narrower stack, `y ∈ [26, 176]`), same
  bearing: resolved reach `47.25 px` (was `23.25 px`), tip at
  `y = -72.25`, a **35.75 px** margin (was `84.75 px` pre-raise).

Both margins shrank versus the pre-raise numbers — recovering reach
necessarily trades away some of that slack — but both stay comfortably
above the `8 px` floor `S10_locked_arrow_head_clears_the_lock_chip`
enforces, with no special casing needed. So: **locked and unlocked still
resolve to the identical starting reach**, and the FLARE lock chip is
still provably clear at the new pivot, just with a smaller (and now
precisely re-measured) margin than the pivot-at-centre version had.
`radar_layout_resolve_arrow`'s `locked` parameter at every `scr_radar.c`
call site is unchanged (still passed through, still selects between the
two named constants) — this fix touched only the pivot the whole dart is
computed about, not the locked/unlocked branching.

If the ARROW_REACH_LOCKED_PX/ARROW_LEN_PX values, the pivot offset, or the
chip's position are ever revisited such that locked reach would again come
within the 8px floor of the chip's band, `radar_layout_resolve_arrow`'s
ordinary registry search is still the backstop — it always shortens
further if the resolved dart's tip, base corners, or notch land inside
*any* reserved rectangle, the chip included were it ever registered.

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
| Placeholder / outline (bearing not real) | LOST (real prior fix, "ghost"), SIGNAL ghost | Outline only, no fill: four edges (tip→right, right→notch, notch→left, left→tip) drawn as a DASHED thin outline (matching the app's own `ArrowShape().stroke(..., style: StrokeStyle(lineWidth: 2, dash: [4, 4]))` for this case) — same "outline-only head, distinct in kind from a dimmer copy" ruling as before, now outlining the whole dart instead of just the old triangular head. Corrected 2026-09-16 (code review of PR #333): this cell previously said "plain (non-dashed)", describing the OLD pre-this-PR puck-only head treatment, not what either surface actually draws now — both `RadarRingView.swift`'s ghost case and `scr_radar.c`'s `RADAR_ARROW_GHOST` (`radar_draw_dart_outline(..., dashed=true)`) use a dashed outline, matching this file's own S06-radar-face.md counterpart ("dashed outline only, no fill"). GHOST and STALE are still visually distinct in kind, not degree: GHOST has no fill at all, STALE does. |

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
