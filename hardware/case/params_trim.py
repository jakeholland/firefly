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

# --- case screws A/C: re-derived for the narrower trim wall (2026-09-04
#     bump fix). At their current-variant (R30) x/y, bosses A/C punched
#     through the trim (R28) outer shell -- caught by an outside-surface
#     probe scan. New rule: A/C at x = +/-(outer_radius - wall - 3.0), y
#     unchanged. D is untouched (its bump, if any, is handled generically
#     by clip_to_inner_cavity on every boss/post regardless of variant).
#     B1/B2 (2026-09-07 pass 7) are also left untouched here -- they are
#     ABSOLUTE mm positions sized against the comms stack, not the outer
#     shell, and rho=19.5 clears even the narrower trim flat bed
#     (flat_rho=22.14) with margin -- see params_current.py's comment. ---
_R3 = PARAMS['outer_radius'] - PARAMS['wall'] - 3.0  # 23.0
_screws = [dict(s) for s in PARAMS['screws_ABC']]
for _s in _screws:
    if _s['name'] == 'A':
        _s['xy'] = (-_R3, _s['xy'][1])
    elif _s['name'] == 'C':
        _s['xy'] = (_R3, _s['xy'][1])
PARAMS['screws_ABC'] = _screws

# --- alignment lip / anchor (shrink by the same 2mm as the shoulder) ---
PARAMS['lip_r'] = (24.95, 25.75)
PARAMS['anchor_r'] = (24.95, 26.40)

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
PARAMS['plate_post_D_z'] = tuple(z + _DZ_TOP for z in PARAMS['plate_post_D_z'])

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
