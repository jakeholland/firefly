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

| constant | px | mm | what it governs | enforced by |
|---|---:|---:|---|---|
| `FF_THEME_MIN_HIT_PX` | 44 | 3.85 | absolute floor, every face (unchanged) | `test_face_hit_targets.c` |
| `FF_THEME_HIT_CHIP_PX` | 52 | 4.55 | in-list secondary controls (quick-reply chips) | `_Static_assert` in `scr_inbox.c` |
| `FF_THEME_HIT_KEY_PX` | 50 | 4.37 | compose T9 keys — see "why 80×80 does not fit" | `_Static_assert` in `scr_compose.c` |
| `FF_THEME_HIT_DOT_PX` | 64 | 5.59 | the inbox action-popup's close, the one small round control bounded by its own stack | `_Static_assert` in `scr_inbox.c` |
| `FF_THEME_HIT_LIST_PX` | 72 | 6.29 | dense secondary lists | **nothing yet** — see "Lineup" |
| `FF_THEME_HIT_PRIMARY_PX` | 80 | 6.99 | **primary actions and list rows** | `test_tap_target_sizing.c` R2 + asserts |
| `FF_THEME_HIT_COMFORT_PX` | 100 | 8.74 | the "ideally" target, where the face allows | `test_tap_target_sizing.c` R1 (launcher) |

Two corrections from this PR's own review, both of the "a number that
guards nothing reads like a guard rail" kind:

- `FF_THEME_HIT_KEY_PX` shipped at **68** in `ff_theme.h` while this table
  and that file's own prose said 50, and nothing anywhere checked either
  value. It is 50 now — the keypad's true, measured ceiling — and
  `scr_compose.c` carries the `_Static_assert` that holds
  `FF_COMPOSE_GRID_ROW_H` and `FF_COMPOSE_BOTTOM_ROW_H` above it.
- `FF_THEME_HIT_DOT_PX` was described as the hit area around a Radar crew
  ring dot. That hit area **does not exist** (see "Crew dots on Radar —
  not delivered"). The only thing it actually floors today is the inbox
  action-popup's close button, and its comment now says so.

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
inside `FF_THEME_GLASS_R` — while their inner **disc** edge still clears
the hub's by 18 px (128 − (60 + 50)).

**Round controls now hit-test round** (added in this PR's review round,
and it is the reason the 100 px satellites survived it). Repairing
`test_face_hit_targets.c`'s composite-control detection — see "The
adjacency sweep was checking almost nothing" below — immediately
surfaced this face: the hub's 120×120 hit rect and the two lower
satellites' 100×100 ones **overlap by 35×7 px at their corners**. The
overlap is not new (at the old 88 px satellites the same corners touched
with a 29×1 px sliver) and it is not a placement error — the discs are 18
px apart. It is a shape error: LVGL's default hit test is the bounding
square, so a tap on visibly empty glass just down-left of the RADAR hub
opened MAP. `scr_launcher.c` now gives the hub and satellites an
`LV_EVENT_HIT_TEST` handler that requires the point to be inside the
disc, and the sweep measures two such controls disc-to-disc
(`sweep_is_disc_control`, a geometric classifier: advanced hit-testing +
a square rect + `LV_RADIUS_CIRCLE`, and then `lv_obj_hit_test` asked
directly — all four corners of the square must come back rejected and the
centre accepted. The flags alone were not enough: `LV_OBJ_FLAG_ADV_HITTEST`
with no handler leaves LVGL's answer at the plain bounding box, and since
a disc gap is always the larger quantity, mis-classifying a box as a disc
can only ever hide an overlap. Second review round of this PR — mutation:
set the flag, drop the handler, and the sweep reports the 10 launcher
violations again instead of passing). Shrinking a control back under its
documented floor was the alternative, and there was no version of it that
worked — at the binding satellite angle (144°/216°, |dy| = 103.6) two
squares need `60 + sat/2 + 8 ≤ 103.6`, i.e. a 71 px satellite, *smaller
than the 88 px this pass started from*.

The shape is BEHAVIOUR, so it is pinned by a press, not only by the
sweep's geometry: `S26e_launcher_square_corner_off_both_discs_emits_nothing`
(`test_scr_intent.c`) derives a point inside both the hub's and the MAP
satellite's bounding squares and outside both painted discs, presses it
with the synthetic indev, and requires **no** intent — then presses the
hub's centre and requires Radar, so a hit shape that rejected everything
would fail rather than pass. Mutation: drop the `LV_EVENT_HIT_TEST`
registration and the empty-glass press emits one intent again.

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
current geometry is at its measured ceiling; `FF_COMPOSE_GRID_ROW_H` (50)
and `FF_COMPOSE_BOTTOM_ROW_H` (56) are held there by build-time asserts
against `FF_THEME_HIT_KEY_PX` — which this PR's review pointed out did
not exist when that sentence was first written, and now do. Getting real 7 mm keys on this face needs a
different input method (a swipe/wheel selector, or the companion app),
not a re-layout.

### Inbox / Signals

| element | before | after | mm after | |
|---|---|---|---:|---|
| feed rows (hit) | 288×60 | **268×80** | 23.4 × 7.0 | ★ |
| picker rows (hit) | 288×60 | **256×80** | 22.4 × 7.0 | ★ |
| thread quick-reply chips | 66/96/74 × 44 | 57/75/62 × **52** | 4.6 tall | chip floor; narrowed — see "The chip strip" |
| action popup rows | 280×66 | **280×80** | 24.5 × 7.0 | ★ |
| action popup close | 54×54 | **64×64** | 5.6 | |
| Rally WHERE rows (hit) | 308×44 | **298×80** | 26.0 × 7.0 | ★ (list viewport 178 → 176) |
| Rally WHEN | 86×56 | **86×80** | 7.5 × 7.0 | ★ |
| Rally Send | 166×56 | **148×80** | 12.9 × 7.0 | ★ |
| compose FAB (on-glass square) | 48×48 | **80×80** | 7.0 | ★ — see below |
| sub-screen BACK | 44×44 @ (109,30) | 44×44 @ (113,36) | 3.85 | **could not grow** |

**The FAB.** Both reviews named it (`ux-puck-maya` §(e): "the single worst
place to put a primary action"). Its hit rect is corner-anchored and
deliberately bleeds into the masked letterbox corner, so the number that
describes it is not the rect — it is the largest square at its **near**
corner that is on glass, which is what a thumb can actually reach. With
the anchor at (300,300) that square was **48.4×48.4** (4.2 mm) — measured
by the test, after a first hand-derivation of this number wrongly used the
framebuffer's centre instead of the glass's and came out at 63. Solving

```
(x1 + w − 208)² + (x1 + w − 206)² ≤ 200²   for w = 80
```

gives an anchor of (268,268), which is where it now sits. The visible
amber lens and the `+` glyph are unchanged; the reachable target went
**4.2 mm → 7.0 mm** — the FAB was, by this measure, the worst-placed
control on the device, exactly as `ux-puck-maya` §(e) called it.
`test_tap_target_sizing.c` checks the inscribed square rather than the
rect for exactly this class of control.

**The chip strip, and the tap the FAB was stealing.** The rows' and
chips' right-hand clearance is derived from the FAB's anchor
(`FF_INBOX_ROW_HIT_CLEAR_X` / `FF_INBOX_CHIP_MAX_RIGHT`, both 268 − 8 =
260), so they give up the 32 px the FAB gained rather than colliding with
it. The feed rows do. The quick-reply chips, as first landed, **did
not**: the strip was 252 px wide (66 + 96 + 74 plus two 8 px gaps)
against a chord at y 256 offering 214 px left of 260, and
`inbox_build_chips` centred the strip *"if it fits"* and otherwise fell
back to `x = margin` with no clamp. It did not fit, did not complain, and
ran to x2 = 297 — 30 px inside the FAB's hit rect, which is built later
and therefore wins LVGL's hit test. Measured: a press at (285,290), on
glass, on the visibly-drawn FLARE chip, emitted `FF_INTENT_INBOX_NEW`.

No height fixes that on its own. The widest chord this glass grants a 52
px band is 2 × (200 − 10 − 2) = 376 px of row, which leaves at most 240
px left of x 260 — under the 252 the old labels needed at *any* y. So the
padding around the labels narrows (the labels themselves are #303/#304's
plain-language wording and are unchanged): measured at Montserrat 14,
"OMW" is 41 px, "IN 5 MIN" 59, "FLARE" 46, and each chip takes its label
plus 8 px a side → 57 / 75 / 62, a 210 px strip. The 1:1 message band
gives back 6 px (182 → 176) so the strip can sit at y 258, clear of the
band — see the Rally/thread note below. Measured after: chips at x
49..258, FAB hit at 268, a 9 px gap.

The clearance is now a **build-time** guarantee rather than a runtime
hope. `FF_INBOX_CHIP_NEED_CHORD`'s `_Static_assert` is the fit condition
with the `sqrt` and the `ceil` algebraically removed — `NEED_CHORD² +
far_dy² ≤ GLASS_R²`, integers only, 168² + 104² = 39 040 ≤ 40 000 — so a
strip that does not fit is a compile error. The runtime clamp stayed, but
it now clamps the strip's RIGHT edge instead of falling back to the left
margin: if anything ever defeats the assert the failure mode is a cramped
left margin, never a chip the FAB eats. Guarded behaviourally by
`S24_thread_chip_strip_press_reaches_the_chip_not_the_fab` and
`S24_thread_fab_press_still_emits_inbox_new` (`test_scr_intent.c`).

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

**The thread band, and the delivery line.** `FF_INBOX_THREAD_LIST_H_1TO1`
was 182 (band ends y 262) with the chip strip starting at y 256 — a six
pixel overlap, against a comment claiming 2 px of clearance. The chips
are built after the list, so they won the paint, and
`inbox_thread_outbox_states.png` shipped with the newest message's
delivery line — "NOT SENT now" — sliced in half by the OMW chip. Fixed
both ways: the band is 176 (ends 256), the strip starts at 258, and a
`_Static_assert` ties the two together.

That exposed a second, quieter version of the same problem. With the chip
gone, the delivery line landed inside `inbox_build_bottom_fade`'s own 16
px gradient and rendered at roughly half opacity. A bottom fade *means*
"there is more below"; drawn unconditionally on a list parked at its own
bottom it says something untrue, and it charges for it on the one line
that says whether the message left the device. The thread's fade is now
gated on `lv_obj_get_scroll_bottom(list) > 0` — drawn exactly when
content really does continue past the viewport. Padding the list instead
was tried and measured to cost the fourth visible row
(`S24_direct_thread_shows_at_least_4_rows_at_rest` drops to 3).

**Rally's WHERE list.** `FF_INBOX_RALLY_LIST_H` was 178 against an 88 px
row height, so the viewport (y 88..266) cut the first PLACE row (206..294)
60 px in — a violet *selected* outline with no bottom edge, under the
fade, in `inbox_rally.png`. 176 = 2 × `FF_INBOX_RALLY_ROW_H` and keeps a
10 px gap to the pinned footer. Stated honestly, the number alone does not
fix it: 176 is the most this band can be (the footer's bottom edge is
already against the glass at y 354) and On Me (88) + the PLACES divider
(22) + one place row (88) is 198 px of content, so *no* viewport this face
can afford shows both whole. Whichever row sits at the bottom edge is
always partly cut; the defect was that it could be the **selected** one.
The list now scrolls the selected row into view (`lv_obj_scroll_to_view`,
the minimal scroll — a sel=0/sel=1 render still sits at the top). That is
also what makes the 6-place `inbox_rally_scrolled` fixture render
differently from the 2-place `inbox_rally` one: the two goldens had become
byte-identical, because at this row height both showed the same first
place and nothing else. Its `sel` moved to a landmark deep in the list
(The Grove, index 4) so the fixture named "scrolled" actually exercises
scrolling.

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
| SCREEN toggle pills | 84×48 | 76×48 | see below |
| ON/OFF toggle pills | 58×48 | 48×48 | see below |
| CREW row action pill | 96×48 | 76×48 | see below |
| all other rows | 48 tall | 48 tall | **not raised — follow-up** |

Those three widths are all the same measurement. Re-framing every
Settings band against the bezel's glass narrows the list rows from 262 px
to 240 px — 11 px per side — which moves every right-aligned control
group 22 px left and takes 22 px off every label column. Three places
could not absorb it, and each is fixed by sizing the control to the text
it actually carries rather than by nudging one row:

- **SCREEN** (`NORMAL`/`FLIPPED`): at 84 px pills the group started at
  x 66 against a "SCREEN" caption measuring 68 px — a 2 px overlap, in
  the golden. At 76 the group starts at 82 and the gap is 14. "FLIPPED"
  is 62 px, so it still has 7 px of padding a side.
- **COLORBLIND** (`ON`/`OFF`): the longest caption on any toggle row,
  116 px. At the shared 58 px pill the group started at 118 — a **2 px**
  gap, down from 24 before the pass. `ON`/`OFF` measure 23 and 30 px, so
  48 px pills are 9 px of padding on the wider of them and put the group
  at 138: a 22 px gap, back above where it started. Applied to every
  ON/OFF row, not just this one.
- **CREW rows**: #303 replaced "LOST" with "NO SIGNAL 15 MIN" and #307
  sized this row so that wording stays legible; the narrower band took
  the label column from 154 px to 132 and truncated it to
  "NO SIGNAL 15 …" in `crew_default.png`. The widest status
  `ff_fmt_age` can produce is "NO SIGNAL 48 MIN" at 150 px (swept over
  every two-digit minute value); the widest action word is "UNHIDE" at
  58. A 76 px pill leaves the column 152.

Checked on the rendered screen, not by this arithmetic:
`S_SET_every_settings_caption_clears_its_control_group` walks the built
page and requires every caption to clear its row's control group by
≥ 12 px (`FF_SETTINGS_VALUE_GAP`, the tightest gap this face is *designed*
to have), and `S_CREW_worst_case_status_renders_in_full` renders the
48-minute status and asserts `LV_LABEL_LONG_DOT` did not rewrite it.
Measured after: CLOCK 60, SCREEN 14, COLORBLIND 22, SOUNDS 64, UI TICKS
64, QUIET HOURS 23, UNITS 67, COMPASS 59, name row 12.

**Compass calibration.** The same 22 px took the instruction band from
302 px to 282, and "Rotate the puck slowly in a figure eight" is 289 px —
so it wrapped, leaving "eight" alone on a second line. The band's own
*top* edge binds here (it is 128 px above the glass centre), so the fix is
6 px of descent rather than smaller type or shorter copy:
`FF_CALCAL_INSTR_Y` 78 → 84 gives a 292 px chord, the sentence is one
line again, and its far corner sits √(148² + 122²) = 191.8 px from the
glass centre.

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
2. The compose bottom row's left edge moved right (x 93→105) and the left
   key column moved right with it (x 15→23), both as a consequence of the
   bezel-accurate margins. **Neither crosses the BACK band's own
   boundary**: the band is x ≤ 52, so the bottom row at x 93 was already
   well clear of it before the pass, and keys 1/4/7 are still inside it at
   x 23 (they were at 15). This entry previously claimed the bottom row
   moved "out of the BACK band", which is not a thing it was ever in — the
   pass shifted two rows a few pixels rightward and changed no zone
   membership at all. Recorded because the zone table above is only useful
   if it is read against the actual numbers.

## The adjacency sweep was checking almost nothing

Found in this PR's review, and fixed here because this pass leans on the
sweep to prove its own geometry.

`test_face_hit_targets.c`'s `sweep_same_composite_control` decides whether
two hit rects are really *one* control wearing two tap targets (a settings
row's dim label and its own value chip, say) and, if so, skips the 8 px
adjacency check between them. It identified a control by event descriptor
**index 0** — and `ff_scr_button_create` registers the shared
`ff_sound_emit(FF_SOUND_TAP)` handler, with a constant `NULL` user_data,
first on *every* button in the app. So index 0 was the same
`(cb, user_data)` pair everywhere, and every pair of real app buttons read
as one composite control.

Measured on the committed fixtures, with this pass's own geometry in
place: **576 pairs gap-checked, 3 464 skipped as "composite"**. After the
fix: **3 922 checked, 81 skipped** (81 is the real number of
label-plus-chip pairings), 850 by the other exclusions, 0 violations.
`scr_nav.c` carried a long comment asserting that the shared handler
"never accidentally aliases two DIFFERENT controls"; it is corrected in
place, including its own empirical note, which was the same fact read the
wrong way round.

Identity is now the control's whole **action set** — every
`(cb, user_data)` pair that is not one of `ff_scr_button_create`'s shared
infrastructure handlers, with that handler set *discovered at runtime* by
building one throwaway button through the real factory. A set, not "the
first non-shared descriptor": while fixing this, `scr_launcher.c` gained
an `LV_EVENT_HIT_TEST` handler (see Launcher above) registered ahead of
its intent callbacks, and under a first-descriptor rule every launcher
disc immediately aliased to every other one — 70 more pairs silently
skipped. There is no ordering dependency left to get wrong, and
`S17b_AC2_composite_control_detection` now asserts that directly.

## Crew dots on Radar — not delivered, and why

The brief asked for an invisible ≥64 px hit area around each 34 px crew
ring dot. **This is not in the change** — `FF_THEME_HIT_DOT_PX` exists and
is named for it, but nothing in `scr_radar.c` references that constant and
no hit sibling is built; its only enforcement today is the inbox popup's
close button, which its comment now says plainly. Three findings, in the
order they blocked it:

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

## What this PR's review changed

Recorded here rather than only in the PR thread, because several of the
numbers above moved:

| finding | what it was | what it is |
|---|---|---|
| inbox chip strip ran under the compose FAB | a press on the drawn FLARE chip emitted `FF_INTENT_INBOX_NEW` | chips 57/75/62, strip at x 49..258, 9 px clear of the FAB, `_Static_assert`ed |
| adjacency sweep aliased every button | 576 pairs checked, 3 464 skipped | 3 922 checked, 81 skipped, 850 other exclusions, 0 violations |
| launcher hub/satellite squares overlapped | 35×7 px, never reported | round controls hit-test round; sweep measures discs |
| `FF_THEME_HIT_KEY_PX` | 68 in code, 50 in this doc, enforced nowhere | 50, asserted in `scr_compose.c` |
| 1:1 thread band overlapped the chip strip | "NOT SENT now" sliced, then faded | band 176, strip 258, fade drawn only on real overflow |
| Rally WHERE viewport | 178 px, selected row guillotined; the two goldens byte-identical | 176 px + scroll-to-selected; `inbox_rally_scrolled` genuinely scrolled |
| Settings caption clearances | COLORBLIND 2 px, CREW status truncated, cal instruction wrapped | 22 px, full status, one line — all measured by tests |
| `ff_layout_bezel_margin_x` | no direct unit tests | 6, including the `band_cx > cx` case nothing else exercises |
