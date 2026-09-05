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

# --- case screws A/B/C: re-derived for the narrower trim wall (2026-09-04
#     bump fix). At their current-variant (R30) x/y, bosses A/B/C punched
#     through the trim (R28) outer shell -- caught by an outside-surface
#     probe scan. New rule: A/C at x = +/-(outer_radius - wall - 3.0), y
#     unchanged; B at (0, -(outer_radius - wall - 3.0)). D is untouched
#     (its bump, if any, is handled generically by clip_to_inner_cavity on
#     every boss/post regardless of variant). ---
_R3 = PARAMS['outer_radius'] - PARAMS['wall'] - 3.0  # 23.0
_screws = [dict(s) for s in PARAMS['screws_ABC']]
for _s in _screws:
    if _s['name'] == 'A':
        _s['xy'] = (-_R3, _s['xy'][1])
    elif _s['name'] == 'B':
        _s['xy'] = (0.0, -_R3)
    elif _s['name'] == 'C':
        _s['xy'] = (_R3, _s['xy'][1])
PARAMS['screws_ABC'] = _screws

# --- alignment lip / anchor (shrink by the same 2mm as the shoulder) ---
PARAMS['lip_r'] = (24.95, 25.75)
PARAMS['anchor_r'] = (24.95, 26.40)

# --- lanyard lug: same 8.5mm protrusion beyond the (now closer) wall ---
_OLD_WALL_Y = _BASE['spine_a'][1] - _BASE['outer_radius']   # -30 (current)
_NEW_WALL_Y = PARAMS['spine_a'][1] - PARAMS['outer_radius']  # -28 (trim)
_PROTRUSION = _BASE['lug']['y_root'] - _OLD_WALL_Y  # buried depth is negative;
# recompute using the tip, which is the quantity Jake specified directly:
_TIP_PROTRUSION = abs(_BASE['lug']['y_tip'] - _OLD_WALL_Y)  # 8.5mm
_BURIED = _BASE['lug']['y_root'] - _OLD_WALL_Y  # -3.5mm (root sits inside the wall)

lug = dict(PARAMS['lug'])
lug['y_tip'] = _NEW_WALL_Y - _TIP_PROTRUSION            # -28 - 8.5 = -36.5
lug['y_root'] = _NEW_WALL_Y + _BURIED                    # -28 - 3.5 = -31.5... see note
# Jake's note gives the root as buried in the wall; keep the same 3.5mm burial
# depth measured from the new wall position:
lug['y_root'] = _NEW_WALL_Y + (_BASE['lug']['y_root'] - _OLD_WALL_Y)
# hole sits 5mm from the tip (same offset as the current variant)
_HOLE_OFFSET_FROM_TIP = _BASE['lug']['hole_xy'][1] - _BASE['lug']['y_tip']  # +5.0
lug['hole_xy'] = (_BASE['lug']['hole_xy'][0], lug['y_tip'] + _HOLE_OFFSET_FROM_TIP)
PARAMS['lug'] = lug

# --- lug relief box in the Top lip/anchor rings: same offsets from the wall
#     as the current variant (0.5mm and 4.0mm inside the wall) ---
_box = dict(PARAMS['lug_relief_box'])
_old_off_lo = _BASE['lug_relief_box']['y'][0] - _OLD_WALL_Y   # 0.5
_old_off_hi = _BASE['lug_relief_box']['y'][1] - _OLD_WALL_Y   # 4.0
_box['y'] = (_NEW_WALL_Y + _old_off_lo, _NEW_WALL_Y + _old_off_hi)
PARAMS['lug_relief_box'] = _box

# --- comms bay v2 (2026-09-04 redesign) ---
# Deliberately UNCHANGED from params_current: the bay's half-disc layout
# (see params_current.py's 'bay' comment) uses ABSOLUTE mm positions fit to
# the tighter trim cavity_r=26; current's cavity_r=28 just has 2mm more
# slack everywhere the layout doesn't already use. Nothing to override here.
