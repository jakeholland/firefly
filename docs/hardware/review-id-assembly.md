# Firefly V2 case — industrial design & assembly review

**Reviewer role:** independent ID/assembly review (wearable-electronics enclosure background,
hands-on FDM experience), requested by the coordinator. **Read-only** — no changes made to
`hardware/case/firefly_case.py`, `params_current.py`, `params_trim.py`, exports, or README.
No PR opened.

**Worktree:** `/private/tmp/claude-501/review-id`, created via `git fetch origin` +
`git worktree add /private/tmp/claude-501/review-id origin/main`, HEAD at `51c5d16` (matches
the task's "main at 51c5d16 or later"). The main checkout was never touched.

**Sources reviewed:** `hardware/case/README.md` (passes 9–15b in full — first-print findings
through the USB-tunnel revert), `hardware/case/params_current.py`/`params_trim.py`, the
renders in `hardware/case/renders/` (pass 9g/10/11/12b/13/14/15 close-ups especially),
`hardware/case/SPEC.md`, `docs/hardware/comms-brain.md`, and the coordinator's display-mount
work at `/private/tmp/claude-501/case-analysis/docs/hardware/` (`plate-mounting-analysis.md`,
`plate-mounting-round2.md`, and the `r2-*.png` renders, including `r2-plan-candidate5-ears.png`
— the only artifact documenting Jake's chosen candidate 5, since round2.md's own text only
covers candidates 1–4). Variant discussed throughout: **trim** (56×103.8×28mm, the shipping
default and the one Jake has printed).

**Diagram:** `docs/hardware/review-id-diagram.png` (matplotlib, drawn from the live `PARAMS`
values cited below plus candidate 5's plan drawing) — a top-view overlay of today's screw/post
map against candidate 5's ears+crossbar, with this review's key findings annotated directly on
it.

---

## Summary

The case has clearly been hardened through 15+ iterative passes against real prints — the
README's own discipline (root-cause-before-fix, live Fusion probes, offline STL scans) is
genuinely unusual rigor for a hobbyist enclosure and it shows: manifold geometry, 0 interference,
clean overhangs, both variants, every pass. My review is aimed at the two things that kind of
process doesn't automatically catch: **whether the assembly is pleasant/robust for a human to
build twice under field conditions**, and **whether candidate 5 (the display mount Jake has
already chosen) is actually finished** — it is the least-verified of the five candidates the
coordinator drew, and it has one flagged, unresolved geometry conflict that goes directly to
one of Jake's own questions.

**Top line:** candidate 5 is a good direction (real screw-count and part-count reduction, see
Finding 1) but should not be treated as final — Finding 2 (crossbar vs. battery-plug access) is
a blocker, and Findings 3–4 (no section/interference verification, no plate = no separator
layer) need closing out before it goes to `firefly_case.py`. Independent of candidate 5, the
current shell has one real field-durability gap (Finding 8, button holes on the parting seam)
and several hand-assembly friction points (Findings 5–7, 10) worth fixing before the two pucks
go through repeated open/close cycles at a dusty, rained-on festival.

---

## 1. Assembly

### Finding 1 — Candidate 5 is a real simplification: −3 screws, −1 part [nice]

**Evidence.** Today's fastener count, from the README's own "Screw list" (trim): 5 case-halves
screws (A/B1/B2/C M2×12 + D M2×12) + 4 Screen-Plate posts (P1–P4 M2×6) + 3 display-to-plate
screws (S1–S3 M2×4) = **12 screws**, plus the Screen Plate itself as a separate injection/print
part. Candidate 5's own plan (`r2-plan-candidate5-ears.png`) removes the plate and P1–P4
entirely, splits screw D into **D1 (−19, 64)** and **D2 (19, 64)** at the two ear roots (M2×12
into Bottom, same as today's other case screws), and drives S1/S2/S3 (M2×4) straight up into
the display's own standoffs through the ears/crossbar instead of through a plate. Net: **6 case
screws (A/B1/B2/C/D1/D2) + 3 display screws (S1–S3) = 9 screws, one fewer printed part.**

**Assembly-order effect.** Today's final step is a 4-group screw pass (A/B1/B2/C → D → P1–P4 →
S1–S3, per README pass-9g "Finding 5"). Candidate 5 collapses this to 2 groups: S1–S3 are driven
**early**, while Top is still open (display seats directly against the ears/crossbar, no plate
to fit in between), and the case-halves screws (A/B1/B2/C/D1/D2) are driven once, last. One
fewer discrete fitting step, one fewer part to keep track of/scrap-print.

**Recommendation:** keep this. It's a genuine improvement on both counts the task asked about
(screw count, part count). Update the "Screw list" and assembly-order section accordingly when
candidate 5 lands.

### Finding 2 — Candidate 5's crossbar overlaps the battery-plug clearance window by ~1.7mm, unresolved [BLOCKER]

**Evidence.** `r2-plan-candidate5-ears.png` draws the new crossbar spanning **wall-to-wall at
y 29.0–34.5** (carrying screw S2), directly over the existing battery-plug clearance window
(`x −11.32..−3.67, y 32.8–38.0`, the cutout `BATTERY_CONNECTOR_TOP_EXTRA` pass-15/15b added to
the *plate* specifically so a fingertip/plug isn't pinched — see README pass-15 "Item 2"). The
coordinator's own diagram labels this overlap explicitly: *"battery-plug window overlaps the bar
by ~1.7mm: verify"* — i.e. flagged, not resolved. This is precisely the question the task asks
directly: *"the battery plug (window in the plate today; with no plate, is it free?)"* — **the
honest answer right now is no, or at least unverified**: removing the plate doesn't remove the
access problem, it just relocates the obstruction from a plate cutout to a load-bearing
structural crossbar in roughly the same place.

**Why this matters for assembly, not just geometry:** the battery connector has to be plugged in
by hand, with a body already threaded through the frame's wire notch, in a cavity that by pass-15
Item 2's own account was already "tight" enough that a bare ~2mm sliver of plate there "would
still pinch a fingertip." A crossbar is stiffer and closer to the connector than a thin plate
edge was; if it doesn't clear, this isn't a cosmetic miss, it's a "can't finish building the
puck" defect.

**Recommendation:** before any code lands, re-run the same live point-containment check pass 15
used for the plate's own window (`verify_battery_connector_access`'s pattern) against the actual
crossbar solid — shift the crossbar's y-span north (toward 34.5–40 instead of 29–34.5) or notch
it locally over the connector's own footprint, whichever a live Fusion check shows is cheaper.
Do this before candidate 5 gets implemented, not after — it's cheap to fix in a plan sketch and
expensive to discover during a 6-hour print.

### Finding 3 — Candidate 5 has no section view or live interference sweep, unlike candidates 1–4 [should-fix]

**Evidence.** Candidates 1–4 each got a plan view, two section views, and a Fusion isometric
render (`r2-plan/section/fusion-candidate{1,2,3,4}-*.png`), plus explicit numeric
tolerance/stiffness analysis (rib axial stiffness, ledge engagement gap, etc., round2.md §1).
Candidate 5 has **one plan-view PNG and nothing else** — no section, no isometric, no
stiffness number, no rotation/insertion-path check. It was evidently added after the round-2
write-up closed (its image timestamp is later than the rest of that pass's renders and the
round2.md file itself) and never got the same treatment.

**Recommendation:** before implementing, give candidate 5 the same pass: a sagittal section
through the crossbar and one ear, live Fusion interference check of the ears/crossbar against
the inserted display occurrence (not just the GPS/battery/header boxes the plan view checks by
eye), and a stiffness estimate for the display board spanning ear-to-crossbar unsupported (see
Finding 4) the way round 2 did for candidates 2–4's ribs/ledges.

### Finding 4 — No plate means no separator layer between the display and the comms-bay cavity [should-fix]

**Evidence.** Round 1's own framing (`plate-mounting-analysis.md` §0) lists the Screen Plate's
three jobs: (1) S1–S3 retention, (2) carrying screw D's post, (3) **"physically
separates/locates the display from the comms-bay cavity below."** Candidate 5 answers (1) and
(2) with the ears/crossbar/D1/D2, but drops (3) — there is no continuous barrier between the
display PCB's back side and the GPS-patch/compass/antenna assembly below it any more, only three
point-contacts (2 ears + 1 crossbar). The nearest real numbers on how close things already run
in this neighborhood: the compass-mount clearance above the GPS patch is **2.7mm spare** (README
pass-10 REDO), and the display's own PCBA/shield reaches as low as its measured bbox in the
FPC-relief investigation (pass 15 Item 1). Nothing in the candidate-5 material re-checks display-
underside-to-GPS-patch/compass clearance now that the plate isn't there to enforce a floor.

**Recommendation:** re-verify the compass module's top clearance and the GPS patch antenna's
routing against the display's real underside geometry (not the plate's old footprint) once
candidate 5 is modeled — this was previously guaranteed structurally by the plate's own
thickness; now it needs its own explicit check.

### Finding 5 — Antenna cable routing is a fussy, order-sensitive hand-assembly step [should-fix]

**Evidence.** README Finding 8 (pass 9e) routes the LoRa u.FL cable through a machined channel
just **4.389mm long**, and documents the assembly instruction explicitly: antenna leads must be
routed "as the boards go in, not after" (pass-9g "Finding 5", step 4). `renders/bay_inside.png`
shows just how tight this corner is in practice — the Wio's u.FL connector sits immediately
adjacent to a full-diameter screw boss, in the same dome-tip pocket as the lanyard lug. This is
a two-hands, watch-the-cable-while-you-seat-the-board operation, repeated for 2 pucks and any
field rebuild.

**Recommendation:** no case-geometry change needed (the channels are proven clear by
`verify_antenna_channels`), but the build sheet/README assembly-order list should call this out
as its own numbered sub-step with a literal warning ("dress the LoRa pigtail into its channel
*before* seating the Wio board — there is no way to do it after"), not just a parenthetical.

### Finding 6 — Compass and XIAO-power wiring are hand-soldered, no connector [should-fix]

**Evidence.** `comms-brain.md`'s "Soldering an IDC ribbon directly" section documents 4–5
individually-stripped conductors (RXD, TXD, G, 3V3, optionally BAT) soldered to the puck's 2×10
back header for the comms-brain link, and the compass section adds 4 more solder points (VCC,
GND, SDA, SCL) to the **same** header. The XIAO's own power option (Option A/B/C in that file)
is a third ad hoc wire/pad decision made per-build. None of these is a connector — every one is
a point that must be re-soldered if the case is ever reopened for repair, which for a
festival-fielded prototype with two units is not a hypothetical.

**Recommendation:** for at least the compass's 4 wires, a small in-line connector (JST-SH 4-pin
or similar, already common at this wire gauge) between the module's own short pigtail and the
back-header ribbon would let the case be opened/closed without a soldering iron in the field.
This is a BOM/process change, not a case-geometry change, but it directly affects "does a human
fumble this" for the two real pucks that will be serviced on-site.

### Finding 7 — Button caps "from inside" is correct but leaves zero room for error [nice]

**Evidence.** README pass-9b established (and pass 15's Item 5/6 reconfirmed) that caps must go
in from inside the open Top half, sliding outward until the head seats in the wall hole — an
outside-in insertion is "geometrically impossible by design." This is the right call given the
retention geometry, but it does mean the cap and its tab must be threaded through the guide rib
(now ceiling-anchored per pass 15 Item 5) at exactly the moment Top is otherwise empty and
easiest to see into — do this step before the display/plate go in, not after, or the button
mechanism becomes hard to reach and see around the display's edge.

**Recommendation:** confirm the README's numbered order still lists caps *before* the display
plate step for candidate 5's version of the order (today's pass-9g order lists it as step 3,
after the plate at step 2 — worth double-checking caps-before-plate doesn't create a new
"can't see the rib while seating the cap" problem once the plate is gone and the ceiling gusset
is the display's own nearest neighbor).

---

## 2. Space (Jake's question: 4 screws at the lanyard end, integrate bosses into the sides?)

### Finding 8 — The 4-screw lanyard-end cluster is real and confirmed visually; it was already "integrated into the sides" once (pass 14), which didn't free volume [should-fix]

**Evidence.** `renders/pass14_bottom_logo.png` shows exactly what Jake is describing: 4 screw
heads (A, B1, B2, C) clustered together at the −y (lanyard) end of Bottom's back face, plus 1
isolated screw (D) alone at the +y end — see this review's own diagram
(`review-id-diagram.png`) for the exact xy positions (A −15.5,−8 / B1 −12.5,−15 / B2 12.5,−15 /
C 15.5,−8, all absolute mm, both variants). Jake's own analogy ("like the top ears") already has
a precedent in the repo: **pass 14 merged A+B1 and C+B2 into two wall-anchored "corner blocks"**
per Jake's own sketch ("a buttress block the two screws land in, not two posts with a web") —
this is functionally the same move candidate 5 makes at the other end.

**But it did not free interior volume.** Pass 14's own writeup is explicit:
`CORNER_BLOCK_PAD` ended up at **0.0** — "the capsule/wedge's own radius is just `boss_dia/2`,
the exact footprint the old individual bosses always had" — after an early wider-pad attempt hit
real interference against the inserted board stack (up to 46mm³ against the Wio, 17mm³ against
the XIAO). So the lanyard-end integration that already happened bought **strength and
printability** (one solid mass instead of two thin posts + web, per Jake's own request), not
**space** — the screws still occupy the same 4 points, the same footprint.

**Recommendation on "do we really need 4":** the case's own screw list frames A/B1/B2/C as the
group that "pulls the two halves flush and square" (pass-9g Finding 5, step 8) — this end has no
display glass to protect and no plate to locate, so its job is purely shell compression +
carrying the lanyard's tension load into the shell. Two screws (one per corner block, at the
capsule's own centroid) pulling two already-large buttress blocks flush is a plausible
reduction, mirroring what candidate 5 accepts at the display end (2 screws, D1/D2, replacing 1)
for a similar-sized structure. This needs the same live check pass 14 used (does a
single-fastener corner block still verify clean 0-interference against the real board stack,
and does it hold the parting-line seam flat under the lug's own tension) before cutting the
count — flagged as a worthwhile follow-up study, not a proven-safe change yet.

### Finding 9 — The real freed-volume opportunity for a bigger battery is the plate footprint, not the lanyard bosses [should-fix]

**Evidence.** Round 1's own "(d) Sandwich" analysis (`plate-mounting-analysis.md`) states
plainly: *"Bottom's cavity directly under the plate's own footprint (y 10–70) is largely
empty"* — the battery (`y 2–32`) and GPS patch (`y 2–27`) both stay south of most of the plate,
and the 3-board comms stack (`y −24..−1.5`) is well south of all of it. The only real
obstructions between y=32 and the display end are the battery-plug clearance box and the
header-cutout column (`x 10.5–18, y 42.7–57.1`) — both narrow, localized features, not
wall-to-wall barriers. The lanyard-end screw bosses (A/B1/B2/C, y −8 to −15) sit *south* of the
battery's own start (y=2) — inside the comms-stack's own footprint zone, not competing with the
battery for room at all (see the diagram's overlay).

**Answer to "does a bigger battery fit if the bosses move into the wall": not really, because
the bosses were never in the battery's way.** The battery today (`x −20..20, y 2..32`, 40×30mm
footprint) already uses nearly the full straight-band width (cavity half-width ≈26mm trim,
battery half-width already 20mm). What's actually unclaimed is the **y 32–70ish band under the
display**, which candidate 5's own plate removal opens up structurally for the first time (no
more P1–P4 posts or plate edge to work around) — this is the real lever for a longer/bigger
battery, not the lanyard-end integration Jake asked about specifically.

**Recommendation:** if a bigger battery is actually wanted, evaluate extending the battery
footprint northward (larger y, toward the header cutout) now that candidate 5 removes the plate
that used to occupy that space, rather than expecting the lanyard-end boss consolidation
(Finding 8) to yield meaningful room — it won't, by the numbers above.

---

## 3. Field durability (18–20 Sept, dust/rain/sun/drops)

### Finding 10 — Both button holes sit exactly on the parting seam [should-fix]

**Evidence.** `renders/pass15_buttons.png` and `pass12b_power_button.png` both show the
stadium-shaped button opening straddling the Top/Bottom split line dead center — the parting
line runs directly through the middle of each button hole. Nothing in the README describes a
gasket, boot, or compliant seal at either the case's main seam or the button openings
specifically (the seam's own treatment is a chamfered alignment lip, pass 9 part 2 — a
mechanical registration feature, not a weather seal).

**Why this matters for the brief:** dust and rain are explicit field conditions. A button
opening that spans the seam is a direct, unobstructed path for dust/water ingress right at the
one place two printed halves can never seal as tightly as a single wall (any FDM print's
parting-line fit tolerance is coarser than its bulk wall). This compounds with pass 15 Item 6's
own finding that the Home plunger has **zero spare margin** at rest — a mechanism already living
close to its clearance limits is also the one sitting on the least-sealed opening in the shell.

**Recommendation:** at minimum, a compressible gasket or dab of grease/silicone at each button's
own seam crossing before the festival (field mitigation, no case change needed); as a real fix,
consider whether the button cutout could be biased slightly onto one half only (all-Top or
all-Bottom) in a future pass, removing the seam crossing entirely — flagged here as a design
question for whoever owns the button mechanism next, not attempted in this review.

### Finding 11 — Glass/window bezel sits flush with the flat top plateau, well inboard of the R10 shoulder [OK, noted]

**Evidence.** `renders/pass15_top_window_edge_wide2.png` and the side-profile renders
(`pass15_trim_right.png`, `pass15_trim_front.png`) show the window's chamfered rim sitting level
with the surrounding flat top face, itself set well inside the case's continuous R10 outer
fillet that runs the entire perimeter. On a drop, an edge or corner impact — the statistically
likely orientation for a puck tumbling off a lanyard — lands on that generous, continuous R10
curve, never on the glass rim directly. A dead-flat, face-down 1.5m drop (the less likely
orientation) would put the window rim and the flat plateau's edge at similar risk together,
which is an acceptable, common tradeoff for a round display puck and not something I'd ask to be
redesigned.

**Recommendation:** none — flagging as confirmed-OK since the task specifically asked whether
the bezel is proud or recessed. It reads as flush-with-a-protective-perimeter-shoulder, the
better of the two "wrong" answers.

### Finding 12 — USB-C port and window share the same dome cap; USB faces the same way as the display [nice/noted]

**Evidence.** `renders/pass12b_usb_end.png` shows the USB-C tunnel opening on the same +y dome
cap as the window (`usb_tunnel_y_start`=73.5, close to the window's own +y extent at
`window_center.y + window_dia/2` ≈72.65). Combined with the lanyard lug sitting at the opposite,
−y end (`lug_relief_box` y −29.5..−24.5) and its cord hole running through the case's own Z
(thickness) axis — the standard ID-badge convention — the puck will hang from a neck lanyard
with its long axis roughly vertical and the **display/USB end at the bottom**, swinging ~100mm
below the attachment point.

**Field implication:** a downward-facing USB-C port sheds rain well (good), but is awkward to
charge while worn (minor inconvenience, not a defect) and collects lint/dust from below over a
3-day festival, the same orientation a phone's charging port faces in a pocket. Not a proposed
change — just worth Jake's explicit sign-off that "screen hangs at the bottom, ~4 inches below
the neck" is the intended wearing pose, since it also affects Finding 15 below (hand feel).

### Finding 13 — GPS/compass retention across the 2.7mm spare relies on a hand-placed foam pad, not modeled [should-fix, pre-existing]

**Evidence.** README Known-limitation #28: the compass module is only trapped between the
ceiling pegs/pads and the GPS patch once "a ~2.0mm compressible foam pad is added to the patch's
own top face at assembly time" — explicitly "a real BOM/assembly-order item, not a modeled
part." Combined with a 1.5m drop spec and a component that free-floats vertically until that
foam is correctly placed and compressed, this is worth a physical check (does the foam actually
stay compressed and centered after a drop, or can the module shift enough to lose its two-peg
XY registration under shock) rather than treating pass 10's own "should be fine" as final. Not
a new finding — flagging because it sits squarely in the durability lens the task asked about
and the existing README already names it as unresolved.

**Recommendation:** a drop test specifically targeting this face (compass side down) before the
festival, and/or double-sided foam tape rather than plain compressible foam so the pad can't
migrate off the patch's own top face during handling.

---

## 4. Cosmetics and hand feel

### Finding 14 — Pill silhouette and hand feel are genuinely good [OK, noted]

**Evidence.** `pass15_trim_iso.png`/`pass15_trim_front.png` show a continuous, generous R10
outer fillet with no sharp transitions anywhere on the silhouette — a comfortable, pocketable
shape appropriate for a hand-held/lanyard-worn device. The wordmark deboss (KANDI WOOKS,
`pass14_bottom_logo.png`) reads cleanly with all 4 counters open (a real pass-14 fix, confirmed)
and sits centered on the back face without crowding the screw holes. No changes suggested here.

### Finding 15 — Single-point lanyard lug means the puck hangs long-axis-vertical, screen at the bottom [nice, confirm intent]

**Evidence.** See Finding 12 above. `lug_relief_box` is centered on x=0 (`x: (−8.5, 8.5)`), so
the puck should hang without a left-right tilt (assuming the internal mass — battery, 3-board
stack — is also roughly x-centered, which the params suggest). But being a single attachment
point at the extreme −y tip, ~75mm from the case's own centroid, means the whole 103.8mm length
hangs essentially vertically below the neck, display down, rather than sitting flat against the
chest the way a wide lanyard badge with a centered hole would.

**Recommendation:** confirm with Jake this is the intended wearing pose for a "glance at your
friend-compass" device — if a flatter, more badge-like hang (screen facing up/out at an angle
rather than dangling straight down) is preferred, that's a lug-position or dual-point-lanyard
question, not a case-shell question, and worth deciding before the festival rather than
discovering it on-body on day one.

### Finding 16 — Five exposed screw heads on the back is a real prototype-grade cosmetic tradeoff [nice]

**Evidence.** `pass14_bottom_logo.png` shows all 5 case-halves screws (A/B1/B2/C clustered +D
alone) as visible heads on the same face as the wordmark. For a small-batch, field-tested
prototype this is a reasonable, low-risk choice (visible fasteners are easy to service, cheap to
print, and match the "case as code" prototyping ethos this whole file is built around) — flagged
only as a known cosmetic limitation, not a defect, in case a future finish pass wants
countersunk/flush heads or color-matched screws for a more finished look.

---

## 5. Notes for the other two review lenses (lip chamfer, button-ring printability)

Nothing found in this pass materially contradicts or duplicates those reviews from the ID/
assembly angle, with one cross-cutting note: **Finding 10 (button hole on the parting seam)**
touches the button-ring reviewer's territory directly — the ring's printability and the seam's
weather-sealing are two faces of the same opening, worth comparing notes before either is
"fixed" independently. Nothing observed here bears on the lip's 45° chamfer question beyond what
README pass-9c/15-Item-7 already documents.

---

## Prioritized list

| # | Finding | Severity | Area |
|---|---|---|---|
| 2 | Candidate-5 crossbar overlaps the battery-plug clearance window by ~1.7mm, unresolved | **Blocker** | Assembly / space |
| 10 | Both button holes sit exactly on the parting seam — dust/rain path | Should-fix | Durability |
| 3 | Candidate 5 has no section/interference verification (unlike candidates 1–4) | Should-fix | Assembly |
| 4 | No plate = no separator between display and comms-bay cavity; re-check clearance | Should-fix | Assembly / durability |
| 9 | Real freed-volume opportunity for a bigger battery is the plate footprint (y 32–70), not the lanyard bosses | Should-fix | Space |
| 8 | 4-screw lanyard cluster already wall-integrated (pass 14) but not volume-freeing; 2-screw reduction plausible, unverified | Should-fix | Space |
| 6 | Compass/XIAO wiring is hand-soldered, no connector — field-service friction for 2 pucks | Should-fix | Assembly |
| 5 | Antenna cable routing is order-sensitive and tight; needs an explicit warning in the build sheet | Should-fix | Assembly |
| 13 | GPS/compass foam-pad retention across the drop spec is unmodeled and untested | Should-fix | Durability |
| 7 | Button-cap-from-inside step needs its place in candidate 5's revised order confirmed | Nice | Assembly |
| 15 | Single-point lug → long-axis-vertical hang, screen down — confirm intended wearing pose | Nice | Cosmetics |
| 16 | Five exposed screw heads on the back — acceptable for a prototype, flagged for a future finish pass | Nice | Cosmetics |
| 12 | USB-C and window share the dome cap; USB faces down when worn | Nice | Durability / cosmetics |
| 1 | Candidate 5 cuts screw count 12→9 and removes a part | Positive (nice) | Assembly / space |
| 11 | Window bezel flush with a protective R10 shoulder | Confirmed OK | Durability |
| 14 | Pill silhouette and wordmark read well | Confirmed OK | Cosmetics |

**Bottom line recommendation:** close Finding 2 (blocker) before candidate 5 touches
`firefly_case.py`; do Finding 3's section/interference pass at the same time since it's the
same modeling session. Findings 8/9 together answer Jake's space question honestly: the lanyard
bosses were never blocking a bigger battery, and integrating them further buys strength/
cosmetics, not volume — the real headroom is under the display, which candidate 5 already opens
up by removing the plate. Finding 10 (button seam) is the one durability item I'd fix before the
field test regardless of what happens with candidate 5.
