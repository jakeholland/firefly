# Tap targets on the puck's glass

Owner decision, Jake, 2026-09-14: do a sizing pass before Lost Lands.
This note is the measurement behind it — the px↔mm arithmetic, what every
interactive element measured before and after, and, for the places that
did not reach the target, the geometry that says why.

Everything in the tables below is **measured output**, not arithmetic.
`targets/sim/tests/test_tap_target_sizing.c` walks the real LVGL object
tree that `ffsim` itself builds for each committed fixture and reads each
control's `lv_obj_get_click_area` (the object's box *plus* any
`ext_click_area` — the same rect `lv_obj_hit_test` checks a real touch
against). Run it with `FF_TAP_INVENTORY=1` to reproduce the dump:

```
FF_TAP_INVENTORY=1 ./build/targets/sim/test_tap_target_sizing
```

## The conversion, and why the old floor was too low

The panel is a Waveshare ESP32-S3-Touch-LCD-1.46: a 412×412 pixel array
behind a round window about **36 mm** across.

```
412 px / 36 mm = 11.44 px/mm          1 mm = 11.44 px
```

That is roughly **three times** a phone's density at 1× (~3.9 px/mm), and
it is the whole reason this pass exists. `FF_THEME_MIN_HIT_PX` is 44 —
the iOS/Material minimum — carried into this codebase without being
re-derived for this glass. On a phone 44 pt is ~11 mm of finger. Here 44
px is **3.8 mm**. Both puck UX reviews reached that independently:

> "44 px is not a generous floor, it's under half the stated 9 mm outdoor
> target" — `ux-puck-maya` §(e)

`FF_THEME_MIN_HIT_PX` **did not move**. It is the absolute floor every
control on every face must clear, enforced device-wide by
`targets/sim/tests/test_face_hit_targets.c`, and lowering the number of
things that check it would weaken that sweep. The pass adds a *second,
higher* bar (`ff_theme.h`):

| constant | px | mm | what it governs |
|---|---:|---:|---|
| `FF_THEME_MIN_HIT_PX` | 44 | 3.85 | absolute floor, every face (unchanged) |
| `FF_THEME_HIT_CHIP_PX` | 52 | 4.55 | in-list secondary controls (quick-reply chips) |
| `FF_THEME_HIT_KEY_PX` | 50 | 4.37 | compose T9 keys — see "why 80×80 does not fit" |
| `FF_THEME_HIT_DOT_PX` | 64 | 5.59 | invisible hit areas around small indicators |
| `FF_THEME_HIT_LIST_PX` | 72 | 6.29 | dense secondary lists |
| `FF_THEME_HIT_PRIMARY_PX` | 80 | 6.99 | **primary actions and list rows** |
| `FF_THEME_HIT_COMFORT_PX` | 100 | 8.74 | the "ideally" target, where the face allows |

## The second circle: the bezel, not the framebuffer

The panel's round window sits ~5 px right of the 412-wide pixel array and
the bezel lip eats the rest, which `ff_theme.h` records as
`FF_THEME_GLASS_CX/CY/R = (208, 206, 200)` (see
`docs/hardware/glass-offset.md`). Every layout in this codebase was
framed against the *framebuffer's* inscribed circle instead —
`(206, 206, 206)` — which is 6 px larger and 2 px to the left.

That is not cosmetic. Measured, before this pass:

| control | corner | distance from glass centre | verdict |
|---|---|---:|---|
| compose BACK | (120, 24) | 202.2 px | 2.2 px under the bezel |
| compose DEL | (93, 371) | 201.1 px | 1.1 px under the bezel |
| inbox BACK | (109, 30) | 201.9 px | 1.9 px under the bezel |
| Settings title card | top shoulders | — | the "flat shoulders" `ux-puck-maya` §(e) flagged by eye |

`ff_layout_bezel_margin_x` (new, `app/screens/ff_layout.[ch]`) is the fix:
the same chord arithmetic `ff_layout_safe_margin_x` already did, but with
the circle's centre and the band's centre allowed to differ, so a row
laid out symmetrically about the puck can still be checked against a
circle that is not centred there. `scr_compose.c`, `scr_inbox.c` and
`scr_settings.c` now compute every margin against the glass.
`ff_layout_safe_margin_x` is now a one-line call into it, so the two can
never disagree.

**Cost, stated plainly:** every band this touches is ~8–11 px narrower
than it was, because the glass really is smaller than the framebuffer.
The compose bottom row loses the most (226 → 202 px of usable width);
that width was never real.

## The tables

`hit` is the measured click area. `mm` is `px / 11.44`. Rows marked **★**
are the ones that reached the owner's 80 px (7 mm) floor.

### Launcher

| element | before | after | mm after | |
|---|---|---|---:|---|
| RADAR hub | 120×120 | 120×120 | 10.5 | unchanged, already over the comfort target |
| INBOX / LINEUP / MAP / SETTINGS / MUSIC satellites | 88×88 | **100×100** | 8.7 | ★ |

Room check: the satellites orbit 128 px from the puck centre, so at 100 px
across their farthest corner sits ~181 px from the glass centre — 19 px
inside `FF_THEME_GLASS_R` — while their inner edge still clears the hub's
by 18 px.

One consequence worth knowing: `ff_scr_nav_remainder_clears_floor`
(`scr_nav.c`), which decides whether a control partly covered by the
notification banner stays clickable, now uses the 80 px floor instead of
44. At 88 px the top satellite's uncovered remainder under a banner was
88×37 and was masked; at 100 px it measures 100×44, which cleared the
*old* floor by one pixel and would have left a 3.8 mm sliver of satellite
live under a notification.

### Radar

| element | before | after | mm after | |
|---|---|---|---:|---|
| FLARE (CLOSE mode) | 200×48 | **176×58** | 15.4 × 5.1 | ceiling — see below |
| crew ring dots | 34 px, not interactive | unchanged | 3.0 | **not delivered** — see "Crew dots" |

FLARE could not reach 80 px tall, and the constraint is the circle:

- CLOSE mode stacks the outermost pulse ring (bottom edge at dy 35), the
  name, the trend chip and FLARE. After lifting the chip 4 px the
  button's top cannot rise above dy 120.
- At 200 px wide the button's farthest corner sat |dx| = 102 from the
  glass centre, capping |dy| at √(200² − 102²) = 172.
- **Narrowing bought height**: at 176 px the corner is |dx| = 90, which
  allows |dy| up to √(200² − 90²) = 178.6. 120 → 178 is 58 px.

176×58 has more *area* than 200×48 (10 208 vs 9 600 px²) and grows the
axis a thumb was actually missing on, by 21 %.

### Compose (T9)

| element | before | after | mm after | |
|---|---|---|---:|---|
| BACK | 44×44 @ (120,24) | 44×44 @ (136,24) | 3.85 | moved on-glass |
| SEND | 48×44 | 48×44 | 4.2 | unchanged |
| keys row 0 (1/2/3) | 118×50 | 112×50 | 4.4 | narrower: bezel |
| keys row 1 (4/5/6) | 122×50 | 116×50 | 4.4 | narrower: bezel |
| keys row 2 (7/8/9) | 106×50 | 101×50 | 4.4 | narrower: bezel |
| DEL | 64×56 @ (93,316) | 64×56 @ (105,316) | 5.6 × 4.9 | moved on-glass |
| SPACE | 90×56 | 74×56 | 6.5 × 4.9 | narrower: bezel |
| MODE | 56×56 | 48×56 | 4.2 × 4.9 | narrower: bezel |

#### Compose: why 80×80 keys do not fit

The owner's brief asked for 80×80 keys with ≥8 px gaps, and to paginate
rather than shrink if a face could not fit. Neither is available here,
and the reason is arithmetic rather than preference:

- Three 80 px keys in a row with two 8 px gaps need **256 px of width**.
- A 256 px-wide band fits inside the safe circle only for
  y ∈ [58, 354] — a **296 px** vertical band.
- Four rows of 80 px with three 8 px gaps need **344 px**.

344 > 296, by 48 px, with *no header, no draft line and no message
bubble at all*. Removing the safety inset entirely and using the full
206 px framebuffer radius still only yields 322 px of band. The deficit
is inherent to a 3×4 keypad on a 36 mm circle.

Pagination is worse than shrinking here, not better: splitting the keypad
across two pages doubles the keystrokes for every character on a keyboard
whose whole problem is already keystrokes-per-word. The T9 keypad's
current geometry is at its measured ceiling; `FF_COMPOSE_GRID_ROW_H` and
`FF_COMPOSE_BOTTOM_ROW_H` are held there by build-time asserts against
`FF_THEME_HIT_KEY_PX`. Getting real 7 mm keys on this face needs a
different input method (a swipe/wheel selector, or the companion app),
not a re-layout.

### Inbox / Signals

| element | before | after | mm after | |
|---|---|---|---:|---|
| feed / picker rows (hit) | 288×60 | **268×80** | 23.4 × 7.0 | ★ |
| thread quick-reply chips | 66/96/74 × 44 | 66/96/74 × **52** | 4.6 tall | chip floor |
| action popup rows | 280×66 | **280×80** | 24.5 × 7.0 | ★ |
| action popup close | 54×54 | **64×64** | 5.6 | |
| Rally WHERE rows (hit) | 308×44 | **298×80** | 26.0 × 7.0 | ★ |
| Rally WHEN | 86×56 | **86×80** | 7.5 × 7.0 | ★ |
| Rally Send | 166×56 | **148×80** | 12.9 × 7.0 | ★ |
| compose FAB (on-glass square) | 63×63 | **80×80** | 7.0 | ★ — see below |
| sub-screen BACK | 44×44 @ (109,30) | 44×44 @ (113,36) | 3.85 | **could not grow** |

**The FAB.** Both reviews named it (`ux-puck-maya` §(e): "the single worst
place to put a primary action"). Its hit rect is corner-anchored and
deliberately bleeds into the masked letterbox corner, so the number that
describes it is not the rect — it is the largest square at its **near**
corner that is on glass, which is what a thumb can actually reach. With
the anchor at (300,300) that square was 63×63. Solving

```
(x1 + w − 208)² + (x1 + w − 206)² ≤ 200²   for w = 80
```

gives an anchor of (268,268), which is where it now sits. The visible
amber lens and the `+` glyph are unchanged; the reachable target went
5.5 mm → 7.0 mm. The rows' and chips' right-hand clearance is derived
from the anchor, so they gave up the 32 px the FAB gained rather than
colliding with it. `test_tap_target_sizing.c` checks the inscribed square
rather than the rect for exactly this class of control.

**The back button — the one thing on this face that could not grow.** It
is a circle pinned to the left of a row whose *centre* carries the
sub-screen title ("RALLY", the thread name, "NEW MESSAGE"). Those titles
are centred, so their left edge lands at x ≈ 166 whatever BACK does;
BACK's right edge has to stay at or under 158 for the 8 px adjacency
floor. Working back through the (now bezel-accurate) chord: right edge
≤ 158 ⇒ left margin ≤ 114 ⇒ half-width ≥ 92 ⇒ the band's top at y ≥ 36.
At y = 36 a 64 px circle lands at x 113..177 — straight through the
caption. 44 px lands at 113..156 and clears it by 10 px. Growing it
requires the title to stop being centred, which is a layout decision, not
a sizing one. Mitigation that already ships: S28's **left-rim swipe is
BACK** on every sub-screen, and a rim gesture is not a 3.8 mm target.

The feed shows three rows at rest instead of four. That is the deliberate
trade: the list scrolls, so a fourth row is one flick away, whereas a row
a gloved thumb cannot hit is not recoverable by scrolling.

### Flare

| element | before | after | mm after | |
|---|---|---|---:|---|
| takeover GO | 190×56 | 190×**64** | 16.6 × 5.6 | ceiling |
| takeover DISMISS | 190×50 | 190×**56** | 16.6 × 4.9 | ceiling |
| sender CANCEL | 140×48 | 140×**64** | 12.2 × 5.6 | |

The takeover stack is the one screen where the safety *gap* matters more
than the target size — GO drops an existing radar lock, DISMISS does not,
and PR #20's review set a 16 px separation between them for that reason.
The band available is fixed:

- a 190 px-wide button's farthest corner is |dx| = 97 from the glass
  centre, so |dy| ≤ √(200² − 97²) = 174.9 — the stack cannot go below
  dy 174;
- the lock-disclosure chip bottoms out at dy 27 and GO needs ~10 px under
  it, so the stack cannot start above dy 37.

That is **137 px for two buttons plus the 16 px gap** — 121 px of button,
against the 176 px two 80 px buttons would need. The gap is not available
to trade: widening either hit area into the dead space between them buys
millimetres by making the expensive mis-tap easier. Sized to the band
instead, bottom-anchored.

### Lineup

**Not applicable.** The Lineup face builds **zero clickable objects** —
its set rows are a reading surface, not a list of controls (confirmed by
the inventory dump: no `lineup_*` fixture contributes a single clickable
element). `FF_THEME_HIT_LIST_PX` (72) is defined and documented for the
day those rows become tappable; nothing asserts against it yet, which is
the honest state rather than a floor that guards nothing.

### Settings

Only the item the owner named was in scope for this pass (a concurrent
branch owns the Settings → CREW page):

| element | before | after | |
|---|---|---|---|
| title card | framed to r=206 @ (206,206) | framed to `FF_THEME_GLASS_*` | the "flat shoulders" are gone |
| SCREEN toggle pills | 84×48 | 78×48 | the narrower band moved the pill group onto the "SCREEN" caption; 78 restores the 8 px gap |
| all other rows | 48 tall | 48 tall | **not raised — follow-up** |

Settings list rows are 48 px (4.2 mm). They are list rows and by the
owner's own rule they should be 80, and the face can afford it (the list
scrolls). They were left alone deliberately: a concurrent branch
(`feat/puck-auto-crew`) is editing the same file's CREW page, and
`FF_SETTINGS_ROW_H` is shared with the compass-calibration buttons, whose
placement would have to be re-derived at the same time. **Follow-up.**

## S28 rim zones vs. edge buttons

The owner asked whether the BACK/HOME edge-swipe zones are stealing taps
from controls parked near the rim. Audited from the same inventory dump.
The zones (S28, live config):

- **BACK rim:** `x ≤ cx − r + back_rim_px` = 208 − 200 + 44 = **x ≤ 52**
- **HOME rim:** `y ≥ cy + r − home_rim_px` = 206 + 200 − 64 = **y ≥ 342**

Controls whose hit rect enters those bands, after this pass:

| zone | controls |
|---|---|
| BACK (x ≤ 52) | compose keys 1/4/7 (x 23–46), name-editor left column, launcher MUSIC satellite (x 35), thread OMW chip (x 46) |
| HOME (y ≥ 342) | compose DEL/SPACE/MODE (bottom 371), Radar FLARE (383), takeover DISMISS (379), sender CANCEL (388), inbox FAB, Rally rows, launcher MAP/SETTINGS satellites (358), power-menu CANCEL (353) |

**No taps are being stolen, and the pass did not make this worse.**
A rim zone only gates where a touch may *start*; G1/G2 additionally
require ≥56 px (BACK) / ≥64 px (HOME) of travel within 500 ms with axis
lock. A tap — any touch that does not travel — is never a gesture and
reaches the control normally. When a gesture *is* recognised the glue
calls `lv_indev_wait_release`, so the widget under the finger gets
`PRESS_LOST` and no click fires; that is correct, and every control in
this codebase is built through `ff_scr_button_create`, which clears
`PRESS_LOCK` precisely so a drag that leaves a button cannot commit it on
release.

Two notes for the bench:

1. The launcher satellites moved *further into* both rim bands (x 41→35,
   bottom 352→358) as a side effect of growing them. They were already
   inside; the pass changed the amount, not the kind. A fast diagonal
   flick starting on the MUSIC satellite can be read as BACK — the same
   as before, and BACK from the launcher is a no-op (S28's own AC14).
2. The compose bottom row moved *out* of the BACK band (x 93→105) and the
   left key column moved right (x 15→23), both as a consequence of the
   bezel-accurate margins. Small improvement, not a designed one.

## Crew dots on Radar — not delivered, and why

The brief asked for an invisible ≥64 px hit area around each 34 px crew
ring dot. **This is not in the change.** Three findings, in the order
they blocked it:

1. **The dots are not interactive today.** `scr_radar.c` clears
   `LV_OBJ_FLAG_CLICKABLE` on every ring dot ("indicator only in this
   slice"), and S06's input contract is "tap centre = cycle selected
   member" — there is no tap-a-dot-to-select behaviour to give a bigger
   target *to*. A hit area with no action is dead weight, and it would
   trip the existing sweep's adjacency pass as a callback-less clickable.
2. **Wiring one needs a core change.** Selecting the tapped member means
   emitting `FF_INTENT_SELECT_CREW`, which carries `u.node_id` —
   and `ff_radar_dot_t` (`core/include/ff_radar.h`) has no node id. Adding
   it touches `core/`, `ff_radar.c`, the fixture parser *and* the fixture
   writer's round-trip contract. Per `AGENTS.md`, core changes are their
   own PR and their own (Tier 3) review.
3. **64 px hit areas collide with the ring's own geometry.** Dots merge
   into a cluster marker only when their resolved centres are within
   `RADAR_LAYOUT_DOT_PX` (34 px) of each other, so two *separate* dots can
   sit 35 px apart — 64 px hit boxes there overlap outright, violating
   `FF_HIT_MIN_GAP_PX`. And the ring radius is 185 px, so a 64 px box
   centred on a dot reaches 217 px from the glass centre, 17 px past
   `FF_THEME_GLASS_R`. Both are solvable (raise the cluster threshold to
   the hit size; clamp the box radially inward) but both change the
   *visual*, which the brief explicitly ruled out.

Proposed as its own slice, with a `## Questions` note added to
`docs/specs/S06-radar-face.md`.

## Where the remaining gap is

Ranked by how much finger is missing, for whoever picks up the follow-up:

| face · element | now | gap to 7 mm | blocked by |
|---|---:|---|---|
| compose T9 keys | 4.4 mm | −2.6 mm | the circle (proved above); needs a different input method |
| Settings list rows | 4.2 mm | −2.8 mm | nothing — deferred for branch conflict, should be done |
| inbox sub-screen BACK | 3.85 mm | −3.1 mm | the centred title sharing its row |
| takeover DISMISS | 4.9 mm | −2.1 mm | the 16 px safety gap, which is worth more than the millimetres |
| Radar FLARE | 5.1 mm | −1.9 mm | CLOSE mode's own stack |
| Radar crew dots | 3.0 mm | −4.0 mm | not interactive; see above |
