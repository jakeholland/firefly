"""Firefly case parameters -- 'trim' variant (56 x 105.8 x 28, pass 12b).

Per Jake's decision (2026-09-04): trim is the DEFAULT variant. It keeps the
SAME spine_a and the SAME reference positions for the display module,
screen plate, top posts, board standoffs, USB tunnel and buttons. Only the
outer envelope shrinks (outer_radius 30 -> 28) and the shoulder profile's
radial placement shrinks by the same 2mm (fillet_center_rho 20 -> 18), plus
the lip/anchor rings, lanyard lug, and comms-bay layout are re-derived for
the narrower cavity (52mm wide instead of 56mm).

2026-09-12/13 pass 12b: spine_b's own y is no longer a flat 50.0 -- it is
50.0 + PARAMS['usb_end_extension_mm'] (see that param's comment in
params_current.py, and the README's pass-12b section for the full
derivation/live numbers). This lengthens the outer envelope AND the inner
cavity at the +y (USB) dome end only -- every other reference position in
this file (window, FPC relief, plate, posts, buttons, screw_D) is an
ABSOLUTE mm coordinate, untouched by spine_b, exactly as it was before.

wall_x (the -x outer wall the buttons sit against) and anything else that is
a pure function of outer_radius/wall are computed at BUILD TIME in
firefly_case.py from PARAMS['outer_radius'] -- not duplicated here.
"""

import copy
from params_current import PARAMS as _BASE

PARAMS = copy.deepcopy(_BASE)
PARAMS['variant'] = 'trim'

# --- envelope: same spine, smaller outer radius ---
PARAMS['spine_a'] = (0.0, 0.0)

# --- PASS 12b (2026-09-12/13, Jake: "we probably need to move the top to
# be longer" -- meaning the case needs to be LONGER at the USB end, not
# taller; see README's pass-12b section for the full derivation this
# comment summarizes): `usb_end_extension_mm` pushes spine_b's own y
# outward, growing the outer envelope + inner cavity + USB tunnel +
# lip/anchor ring ends at the +y (dome/USB) end ONLY -- see
# params_current.py's comment on this param for the single mechanism
# (rho_from_spine/rho_at_z/wall_y all key off spine_b) that makes this
# work with no other code change, and why every absolute-mm feature
# (window, FPC relief, plate, posts, buttons, screw_D) is untouched.
#
# Exact minimum computed by bisecting a standalone reuse of
# rho_at_z/rho_from_spine (tools/pass12b_ext_calc.py) for the smallest
# ext giving >=1.5mm of real skin (1.2mm required + 0.3mm to spare) at
# the FPC relief pocket's own worst (unprotected, literal-SPEC-box)
# corner (7.02, 73.12): 1.522mm using the corner's literal coordinates,
# 1.456mm using the same 0.05mm inset add_fpc_relief's own probe uses --
# both far under the 3.0mm Jake asked about, so the smaller number wins
# per his own instruction. Picked 1.8mm, not the bare minimum: it clears
# the 1.5mm skin target with an extra ~0.26mm on top (skin 1.764mm, see
# the README table) for the same ~0.25-0.3mm flat-ray-vs-true-curvature
# tessellation slack this file already documents in half a dozen other
# probes (verify_wall_integrity, the pass-12 button footprint gates), and
# it stays under the 2.0mm ceiling where the USB tunnel's own recess
# depth (wall_y - usb_tunnel_y_start, 4.5mm at ext=0) would exceed 6.5mm
# -- the point past which a standard USB-C plug's overmold no longer
# reaches the receptacle (see add_usb_tunnel's docstring / the README's
# pass-12b section for the live check). At 1.8mm the recess is 6.3mm --
# 0.2mm under that ceiling, no tunnel/counterbore change needed.
PARAMS['usb_end_extension_mm'] = 1.8
PARAMS['spine_b'] = (0.0, 50.0 + PARAMS['usb_end_extension_mm'])
PARAMS['outer_radius'] = 28.0
PARAMS['fillet_center_rho'] = 18.0          # 20 - 2
PARAMS['flat_rho'] = 22.14                  # 24.14 - 2
# fillet_r (10.0), top/bottom fillet center z (15/10), wall (2.0) unchanged.
# Derived tangent point: rho = 18 + 10*cos(45) = 25.07 (matches Jake's spec).

# --- window / display / posts / standoffs / plate outline / USB
#     receptacle position: unchanged -- these stay at their reference
#     (current-variant) positions inside the now-narrower cavity. ---

# --- case screws A/C: NO override needed here anymore (2026-09-08 pass
#     9, finding 2). They used to be re-derived per-variant as
#     x = +/-(outer_radius - wall - 3.0), y unchanged from the reference --
#     but that rule put A/C partly ON the 45-degree shoulder for BOTH
#     variants (the boss's OD reached past `flat_rho`, the true limit of
#     the flat bed at that y), confirmed as the root cause of the printed
#     part's holes/bumps there. A/C are now ABSOLUTE mm positions at the
#     dome-tip end (like B1/B2/D), defined once in params_current.py and
#     inherited unchanged by this deepcopy -- see that file's comment for
#     the full reasoning (flat_rho containment + clearing the battery/GPS/
#     display footprints that make y~25 unusable at either variant's
#     flat_rho). rho=17.44 from spine_a clears trim's tighter flat_rho
#     (22.14) with ~0.7mm to spare on top of the 1mm margin already baked
#     into that position, so current (flat_rho=24.14) inherits even more. ---

# --- alignment lip / anchor (shrink by the same 2mm as the shoulder) ---
# 2026-09-08 pass 9 (finding 5, ring thickened 0.8mm -> 1.8mm): OUTER
# edges (25.75 lip / 26.40 anchor) unchanged -- same reasoning as
# params_current.py (0.25mm nesting clearance / deliberate anchor-fuse
# reach past trim's true wall, outer_radius-wall=26) -- only the INNER
# edge moves in by 2.0mm to grow the band from 0.8mm to 1.8mm wide,
# exactly mirroring current's 27.75->25.95 (also -1.8mm).
PARAMS['lip_r'] = (23.95, 25.75)
PARAMS['anchor_r'] = (23.95, 26.40)

# --- lanyard lug/ear: no override needed (2026-09-06 pass 6) ---
# The ear's position is now derived at BUILD TIME from the shell's TRUE
# curved surface (true_wall_distance_along_ray, in lug_ear_geometry) from
# the variant-independent width/protrusion/hole_dia/hole_from_tip/
# fillet_r/hole_chamfer in PARAMS['lug'] (inherited unmodified from
# params_current.py) -- it can never drift out of sync with outer_radius
# the way the old hand-picked y_root/y_tip constants could.
_OLD_WALL_Y = _BASE['spine_a'][1] - _BASE['outer_radius']   # -30 (current)
_NEW_WALL_Y = PARAMS['spine_a'][1] - PARAMS['outer_radius']  # -28 (trim)

# --- lug relief box in the Top lip/anchor rings: same offsets from the wall
#     as the current variant ---
_box = dict(PARAMS['lug_relief_box'])
_old_off_lo = _BASE['lug_relief_box']['y'][0] - _OLD_WALL_Y
_old_off_hi = _BASE['lug_relief_box']['y'][1] - _OLD_WALL_Y
_box['y'] = (_NEW_WALL_Y + _old_off_lo, _NEW_WALL_Y + _old_off_hi)
PARAMS['lug_relief_box'] = _box

# --- comms bay v2 (2026-09-04 redesign) ---
# Deliberately UNCHANGED from params_current: the bay's half-disc layout
# (see params_current.py's 'bay' comment) uses ABSOLUTE mm positions fit to
# the tighter trim cavity_r=26; current's cavity_r=28 just has 2mm more
# slack everywhere the layout doesn't already use. Nothing to override here.

# --- PASS 7 (2026-09-06/07): case height grows 25 -> 28mm to fit the real
# 18mm-tall 3-board comms stack (measured L76K-underside-to-SX1262-top).
# Bottom/parting-plane geometry is UNCHANGED (Bottom z 0..10, split_z=10);
# only the Top grows (10..28, 18mm tall instead of 10..25/15mm). Everything
# tied to the display module shifts +3mm in Z (the same amount top_z
# grows) so the glass stays flush with the new top face; everything
# anchored to the PARTING PLANE (lip/anchor, Top case-screw bosses' pilot
# depth, the lug/ear, Bottom itself) is untouched -- see README pass-7
# section for the full rationale/z-table.
#
# --- PASS 12 (2026-09-11/12, "move the top to be longer" -- FIRST
# attempt, REVERTED in pass 12b): raised top_z 28 -> 30 on the theory a
# taller Top leaves more skin above the FPC relief pocket. Proved both
# analytically and live that this cannot work under this file's z-shift
# convention -- `rho_at_z(p, z) = flat_rho + (top_z - z)` in the flat-
# chamfer band, and PARAMS['fpc_relief']['z'] shifts by the exact same
# _DZ_TOP as top_z itself, so `top_z - z` at the pocket's own z1 is
# algebraically INVARIANT to top_z (confirmed: the SPEC box's worst
# corner (7.02, 73.12) has the identical 0.048mm of margin at top_z=28,
# 30, and 40) -- and it cost real M2x16-vs-M2x12 screw-D engagement for
# nothing. See the README's pass-12 section for that whole investigation
# (kept for the record) and pass-12b section for why the fix is a LONGER
# case at the USB end (`usb_end_extension_mm`, above), not a taller one.
_DZ_TOP = 3.0  # 28 - 25 (pass 7's number, reinstated pass 12b)

PARAMS['top_z'] = 28.0
# top_fillet_center_z = top_z - fillet_r (10.0) -- keeps the outer R10
# fillet tangent to the (now higher) flat top face, same derivation as the
# 'current'-variant number (15.0 = 25 - 10).
PARAMS['top_fillet_center_z'] = PARAMS['top_z'] - PARAMS['fillet_r']
# top_ceiling_underside_z = top_z - wall (2.0mm skin) -- was 23.0 (25-2).
PARAMS['top_ceiling_underside_z'] = PARAMS['top_z'] - PARAMS['wall']

# display PCBA occurrence: +3mm Z offset vs the reference transform (read
# from "Firefly V2 v16", built for the 25mm-tall case) so the glass lands
# flush at the new top_z=28 instead of 3mm below it. See
# insert_display_pcba()'s use of PARAMS['display_z_offset'].
PARAMS['display_z_offset'] = _DZ_TOP

# --- everything else tied to the display/Top ceiling: shift +3mm ---
PARAMS['fpc_relief'] = dict(PARAMS['fpc_relief'])
PARAMS['fpc_relief']['z'] = tuple(z + _DZ_TOP for z in PARAMS['fpc_relief']['z'])

PARAMS['plate_z'] = tuple(z + _DZ_TOP for z in PARAMS['plate_z'])
PARAMS['top_post_z'] = tuple(z + _DZ_TOP for z in PARAMS['top_post_z'])
PARAMS['top_post_pilot_z'] = tuple(z + _DZ_TOP for z in PARAMS['top_post_pilot_z'])
# 2026-09-07 pass 7 fix: plate_post_D_z is (split_z, plate_z[0]) by
# construction (build_screen_plate's own comment: "post for screw D
# (fuses onto the plate's underside, z (10, plate_z0))") -- Bottom's own
# boss D pillar (add_case_boss) stops at the FIXED p['split_z'], not at
# plate_z[0], so the post's lower bound must stay pinned to split_z
# (10.0, unchanged by the case-height bump) for the two pieces to stay
# contiguous. Blindly shifting BOTH tuple elements by _DZ_TOP (as every
# other plate-anchored z-range above correctly does) instead moved the
# lower bound to 13.0, leaving a 3mm GAP of missing boss material between
# Bottom's boss D (still ending at z=10) and the Screen Plate's post
# (now starting at z=13) -- a real "missing screw post" defect, caught by
# re-deriving this from first principles rather than trusting the
# uniform-shift pattern used everywhere else in this file.
PARAMS['plate_post_D_z'] = (PARAMS['split_z'], PARAMS['plate_z'][0])

PARAMS['usb_receptacle'] = dict(PARAMS['usb_receptacle'])
PARAMS['usb_receptacle']['z'] = tuple(z + _DZ_TOP for z in PARAMS['usb_receptacle']['z'])
PARAMS['usb_tunnel_center_z'] = PARAMS['usb_tunnel_center_z'] + _DZ_TOP  # 16.4 -> 19.4
PARAMS['usb_shell_z'] = PARAMS['usb_shell_z'] + _DZ_TOP  # documentation only, not read by code

PARAMS['window_z_bottom'] = PARAMS['window_z_bottom'] + _DZ_TOP

PARAMS['power_cap'] = dict(PARAMS['power_cap'])
PARAMS['power_cap']['z'] = tuple(z + _DZ_TOP for z in PARAMS['power_cap']['z'])
PARAMS['home_cap'] = dict(PARAMS['home_cap'])
PARAMS['home_cap']['z'] = tuple(z + _DZ_TOP for z in PARAMS['home_cap']['z'])
# switch bboxes ride up with the board (+3): switch_home_bbox's own 'z' is
# a (999,999) placeholder never read (add_buttons/verify always reuse
# switch_power_bbox['z'] for both buttons -- see firefly_case.py), so only
# switch_power_bbox needs the real shift.
PARAMS['switch_power_bbox'] = dict(PARAMS['switch_power_bbox'])
PARAMS['switch_power_bbox']['z'] = tuple(z + _DZ_TOP for z in PARAMS['switch_power_bbox']['z'])

# top_pilot_z / top_pilot_dia (Top case-screw boss pilots, M2x12): UNCHANGED
# -- parting-plane anchored (depth 9.1mm from z=10 regardless of case
# height); lip_r/lip_z/anchor_r/anchor_z/boss_relief_dia/lug_relief_box/
# lug: also unchanged, same reasoning.

# --- comms stack (pass 7): trim's +3mm case-height bump gives the real
# 18mm-tall 3-board stack the room 'current' (frozen at height 25, "for
# the probe comparison") does not have -- see params_current.py's comment
# on 'comms_stack3_full_height'. Trim inserts all three boards.
PARAMS['comms_stack3_full_height'] = True
