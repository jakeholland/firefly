"""FPC-tab relief pocket -- build123d port of firefly_case.py:1285
fpc_relief_footprint / :1301 add_fpc_relief (pass-12b, no brow)."""
import build123d as bd

from .. import geometry as geo

FPC_RELIEF_MIN_WALL = 1.2  # firefly_case.py's own constant, same name


def fpc_relief_footprint(p):
    fr = p['fpc_relief']
    x0 = min(fr['x'][0], -14.0)
    x1 = max(fr['x'][1], 14.0)
    y0 = min(fr['y'][0], 65.0)
    y1 = fr['y'][1]
    z0 = min(fr['z'][0], 21.0)
    z1 = fr['z'][1] + 0.1
    return x0, x1, y0, y1, z0, z1


def add_fpc_relief(bodies, p):
    fr = p['fpc_relief']
    x0, x1, y0, y1, z0, z1 = fpc_relief_footprint(p)
    pocket = geo.box_solid(x0, x1, y0, y1, z0, z1)

    skin_safe_tool = geo.build_outer_pill_solid(p)
    skin_safe_tool = bd.offset(skin_safe_tool, amount=-FPC_RELIEF_MIN_WALL)

    pocket = pocket & skin_safe_tool
    bodies['Top'] = bodies['Top'] - pocket

    spec_box = geo.box_solid(fr['x'][0], fr['x'][1], fr['y'][0], fr['y'][1], z0, z1)
    bodies['Top'] = bodies['Top'] - spec_box

    top = bodies['Top']
    eps = 0.05
    fx = (fr['x'][0] + eps, fr['x'][1] - eps)
    fy = (fr['y'][0] + eps, fr['y'][1] - eps)
    fz = fr['z'][1] - eps
    uncleared = []
    for cx in fx:
        for cy in fy:
            pt = (cx, cy, fz)
            if geo.probe_point_solid(top, pt):
                uncleared.append((round(cx, 3), round(cy, 3), round(fz, 3)))
    assert not uncleared, (
        f'add_fpc_relief: SPEC box corner(s) not cleared after the skin-safe clip: '
        f'{uncleared} -- FPC tab would not fit')

    return bodies
