"""Pass 12b: standalone (no-Fusion-needed) derivation of the minimum
`usb_end_extension_mm` that recovers >=1.2mm of FPC-relief skin (+0.3mm
to spare, i.e. >=1.5mm) at the pocket's own worst, unprotected SPEC-box
corner (7.02, 73.12) -- see hardware/case/README.md's pass-12b section
and firefly_case.py's FPC_RELIEF_MIN_WALL module comment for the full
context this supports.

`rho_at_z`/`rho_from_spine` below are verbatim copies of the functions of
the same name in firefly_case.py (that module can't be imported outside
Fusion -- it does `import adsk.core` at module scope). Run with:
    python3 tools/pass12b_ext_calc.py
from the hardware/case/ directory (params_current.py/params_trim.py have
no Fusion dependency and can be imported directly).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import params_current
import params_trim


def _profile_geometry(p):
    flat_rho = p['flat_rho']
    fillet_r = p['fillet_r']
    fc_rho = p['fillet_center_rho']
    outer_r = p['outer_radius']
    top_c_z = p['top_fillet_center_z']
    bot_c_z = p['bottom_fillet_center_z']
    top_z = p['top_z']
    bot_z = p['bottom_z']
    assert abs(fc_rho - (outer_r - fillet_r)) < 1e-6, (fc_rho, outer_r, fillet_r)
    tangent_rho = fc_rho + fillet_r * math.cos(math.radians(45))
    dz_tangent = fillet_r * math.sin(math.radians(45))
    top_tangent_z = top_c_z + dz_tangent
    bot_tangent_z = bot_c_z - dz_tangent
    return {
        'flat_rho': flat_rho, 'fillet_r': fillet_r, 'fc_rho': fc_rho,
        'outer_r': outer_r, 'top_c_z': top_c_z, 'bot_c_z': bot_c_z,
        'top_z': top_z, 'bot_z': bot_z,
        'tangent_rho': tangent_rho,
        'top_tangent_z': top_tangent_z, 'bot_tangent_z': bot_tangent_z,
    }


def rho_at_z(p, z):
    """Verbatim copy of firefly_case.rho_at_z."""
    g = _profile_geometry(p)
    top_z, bot_z = g['top_z'], g['bot_z']
    flat_rho, fc_rho, r = g['flat_rho'], g['fc_rho'], g['fillet_r']
    if z >= g['top_tangent_z']:
        return flat_rho + (top_z - z)
    if z >= g['top_c_z']:
        return fc_rho + math.sqrt(max(r * r - (z - g['top_c_z']) ** 2, 0.0))
    if z >= g['bot_c_z']:
        return g['outer_r']
    if z >= g['bot_tangent_z']:
        return fc_rho + math.sqrt(max(r * r - (z - g['bot_c_z']) ** 2, 0.0))
    return flat_rho + (z - bot_z)


def rho_from_spine(p, x, y):
    """Verbatim copy of firefly_case.rho_from_spine."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    if ay <= y <= by:
        return abs(x)
    center_y = ay if y < ay else by
    return math.hypot(x, y - center_y)


def worst_corner_skin(p, ext, eps=0.0):
    """Skin remaining above the FPC relief pocket's literal SPEC box (the
    part add_fpc_relief cuts in full, UNCLIPPED by the skin-safe tool --
    see that function's own docstring) at its worst corner, for a given
    candidate `usb_end_extension_mm` (`ext`) applied to spine_b. Returns
    (worst_skin, worst_corner, rho_at_corner)."""
    p = dict(p)
    p['spine_b'] = (0.0, p['spine_b'][1] - p['usb_end_extension_mm'] + ext)
    fr = p['fpc_relief']
    xs = (fr['x'][0] + eps, fr['x'][1] - eps)
    ys = (fr['y'][0] + eps, fr['y'][1] - eps)
    z1 = fr['z'][1]
    worst = None
    for x in xs:
        for y in ys:
            r = rho_from_spine(p, x, y)
            top_z = p['top_z']
            if r <= rho_at_z(p, top_z) + 1e-9:
                z_surface = top_z  # under the flat bed -- full height survives
            else:
                lo, hi = p['bottom_z'], top_z
                for _ in range(80):
                    mid = (lo + hi) / 2.0
                    if rho_at_z(p, mid) > r:
                        lo = mid
                    else:
                        hi = mid
                z_surface = (lo + hi) / 2.0
            skin = z_surface - z1
            if worst is None or skin < worst[0]:
                worst = (skin, (round(x, 3), round(y, 3)), round(r, 3))
    return worst


def find_min_ext(p, target_skin=1.5, eps=0.0):
    lo, hi = 0.0, 10.0
    for _ in range(60):
        mid = (lo + hi) / 2.0
        skin = worst_corner_skin(p, mid, eps=eps)[0]
        if skin >= target_skin:
            hi = mid
        else:
            lo = mid
    return hi


if __name__ == '__main__':
    for name, P in (('current', params_current.PARAMS), ('trim', params_trim.PARAMS)):
        print(f'--- {name}: top_z={P["top_z"]}, flat_rho={P["flat_rho"]}, '
              f'outer_radius={P["outer_radius"]}, usb_end_extension_mm={P["usb_end_extension_mm"]} ---')
        for eps in (0.0, 0.05):
            for ext in (0.0, 1.0, 1.456, 1.522, 1.6, 1.8, 2.0, 3.0):
                skin, corner, rho = worst_corner_skin(P, ext, eps=eps)
                print(f'  eps={eps} ext={ext:>5.3f}  worst_corner={corner}  rho={rho:.3f}  '
                      f'skin={skin:.3f}mm  margin_over_1.2={skin - 1.2:+.3f}mm')
            min_ext = find_min_ext(P, target_skin=1.5, eps=eps)
            print(f'  eps={eps} -> exact min ext for skin>=1.5mm (1.2 required + 0.3 spare): {min_ext:.4f}mm')
        # USB tunnel recess depth sanity: recess = (spine_b.y + outer_radius) - usb_tunnel_y_start.
        # A standard USB-C plug's overmold needs to reach within ~6.5mm of the
        # receptacle face to seat -- see add_usb_tunnel's docstring.
        recess = P['spine_b'][1] + P['outer_radius'] - P['usb_tunnel_y_start']
        print(f'  USB tunnel recess depth at ext={P["usb_end_extension_mm"]}: {recess:.2f}mm '
              f'({"OK" if recess <= 6.5 else "EXCEEDS 6.5mm plug-overmold limit"})')
        print()
