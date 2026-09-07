"""Firefly case parameters -- 'trim' variant (56 x 102 x 25).

Per Jake's decision (2026-09-04): trim is the DEFAULT variant. It keeps the
SAME spine as 'current' -- (0,0)-(0,50) -- and the SAME reference positions
for the display module, screen plate, top posts, board standoffs, USB
tunnel and buttons. Only the outer envelope shrinks (outer_radius 30 -> 28,
so the pill ends land at y -28 and y 78) and the shoulder profile's radial
placement shrinks by the same 2mm (fillet_center_rho 20 -> 18), plus the
lip/anchor rings, lanyard lug, and comms-bay layout are re-derived for the
narrower cavity (52mm wide instead of 56mm).

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
PARAMS['spine_b'] = (0.0, 50.0)
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
_DZ_TOP = 3.0  # 28 - 25

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
