"""Compass module (GY-273/QMC5883P) mount -- build123d port of
firefly_case.py's pass-10-REDO/pass-11/pass-15 `mag_*` family (:5143-
:5296 `add_mag_module`) and `verify_mag_pocket` (:5434, ported in
gates.py). Modeled as a plain box from PARAMS (`PARAMS['mag_module']`),
per the port brief -- the module's own `.f3d` reference is a Fusion
archive, not importable headlessly (see docs/hardware/headless-port-
plan.md). Hangs from Top's own inner ceiling directly above the GPS
patch frame's open chimney (`features/comms_bay.build_gps_frame_body`) --
entirely open cavity, no outer-wall interaction, no cut. Pass 15, item 4
removed the old 4-wall retaining fence outright (the two Ø2.7 pegs
through their own Ø3.0 mounting holes already fix XY translation AND
rotation, the same way a 2-pin polarized connector does) in favor of a
single low (<=2mm) south-side stop -- ported here as the only retaining
wall this module builds."""
import math

import build123d as bd

from .. import geometry as geo
from . import corner_blocks as cb

MAG_DISPLAY_RING_MIN_CLEAR = 3.0  # mm -- firefly_case.py:5157, pass 11 defect 2
PEG_COLLAR_RISE = 1.1             # firefly_case.py:1575
MAG_STOP_H = 2.0                  # mm -- pass 15's own brief ceiling ("<=2mm tall")


def mag_world_y(p, local_x):
    """Port of mag_world_y (:5164) -- DECREASING mapping (pass 11)."""
    return -local_x + p['mag_module']['world_y_from_local_x_offset']


def mag_world_x(p, local_y):
    """Port of mag_world_x (:5174)."""
    return local_y + p['mag_module']['world_x_from_local_y_offset']


def mag_pcb_bottom_world_z(p):
    """Port of mag_pcb_bottom_world_z (:5180)."""
    return p['top_ceiling_underside_z'] - p['mag_module']['standoff_h']


def mag_world_z(p, local_z):
    """Port of mag_world_z (:5191) -- local +z (component/top face) ->
    world -Z (module mounted components-down)."""
    return mag_pcb_bottom_world_z(p) - local_z


def mag_module_clearance(p):
    """Port of mag_module_clearance (:5199)."""
    mm = p['mag_module']
    lowest_world_z = mag_world_z(p, 1.0 + mm['local_component_h'])
    patch_top = p['bay']['gps_patch']['z'][1]
    return lowest_world_z - patch_top


def mag_module_fits(p):
    """Port of mag_module_fits (:5219) -- False for 'current' (ceiling
    never grew the 3mm trim did, see params_current.py's own comment)."""
    return mag_module_clearance(p) >= p['mag_module']['min_patch_clearance']


def mag_pcb_world_footprint(p):
    """Port of mag_pcb_world_footprint (:5228)."""
    mm = p['mag_module']
    lx0, lx1 = mm['local_pcb']['x']
    ly0, ly1 = mm['local_pcb']['y']
    wy_a, wy_b = mag_world_y(p, lx0), mag_world_y(p, lx1)
    y0, y1 = min(wy_a, wy_b), max(wy_a, wy_b)
    x0, x1 = mag_world_x(p, ly0), mag_world_x(p, ly1)
    return x0, x1, y0, y1


def mag_fence_world_footprint(p):
    """Port of mag_fence_world_footprint (:5245) -- kept under its
    original name even though pass 15 replaced the 4-wall fence with a
    single stop (see add_mag_module's own docstring); still the correct
    "outer footprint expanded by fence_clear+fence_wall" used by the
    window-bore/display keep-out checks."""
    mm = p['mag_module']
    x0, x1, y0, y1 = mag_pcb_world_footprint(p)
    margin = mm['fence_clear'] + mm['fence_wall']
    return x0 - margin, x1 + margin, y0 - margin, y1 + margin


def mag_window_bore_clearance(p):
    """Port of mag_window_bore_clearance (:5257)."""
    fx0, fx1, _, fy1 = mag_fence_world_footprint(p)
    wx, wy = p['window_center']
    r = p['window_dia'] / 2.0
    return min(math.hypot(x - wx, fy1 - wy) - r for x in (fx0, fx1))


def mag_peg_world_positions(p):
    """Port of mag_peg_world_positions (:5273)."""
    mm = p['mag_module']
    return [(mag_world_x(p, ly), mag_world_y(p, lx)) for lx, ly in mm['local_mount_holes']]


def mag_pad_world_positions(p):
    """Port of mag_pad_world_positions (:5279)."""
    mm = p['mag_module']
    lx_edge = mm['local_pcb']['x'][1]
    ly0, ly1 = mm['local_pcb']['y']
    inset = 1.0
    wy = mag_world_y(p, lx_edge - inset)
    return [(mag_world_x(p, ly0) + inset, wy), (mag_world_x(p, ly1) - inset, wy)]


def mag_header_notch_center_x(p):
    """Port of mag_header_notch_center_x (:5300)."""
    mm = p['mag_module']
    ys = mm['local_header']['y']
    mid_local_y = (min(ys) + max(ys)) / 2.0
    return mag_world_x(p, mid_local_y)


def add_mag_module(bodies, p):
    """Port of add_mag_module (:5308): two Ø2.7 pegs into the mounting
    holes + two header-side rest pads, all hanging from the ceiling, plus
    a single low (<=2mm) south (lanyard/header) side stop with its own
    wire-exit notch (pass 15, item 4 -- the 4-wall fence is gone, see this
    module's own docstring). Skipped entirely when `mag_module_fits(p)`
    is False ('current')."""
    if not mag_module_fits(p):
        return bodies
    top = bodies['Top']
    mm = p['mag_module']
    ceiling = p['top_ceiling_underside_z']
    pcb_bottom = mag_pcb_bottom_world_z(p)

    peg_r = mm['peg_dia'] / 2.0
    for px, py in mag_peg_world_positions(p):
        peg = geo.cylinder_solid(px, py, peg_r, pcb_bottom, ceiling)
        top = top + peg
        top = cb.add_root_reinforcement(top, px, py, peg_r, ceiling, direction='up', collar_rise=PEG_COLLAR_RISE)

    pad_r = mm['pad_dia'] / 2.0
    for px, py in mag_pad_world_positions(p):
        pad = geo.cylinder_solid(px, py, pad_r, pcb_bottom, ceiling)
        top = top + pad
        top = cb.add_root_reinforcement(top, px, py, pad_r, ceiling, direction='up', collar_rise=PEG_COLLAR_RISE)

    # pass 15, item 4: single low south stop replacing the old 4-wall
    # fence (see module docstring) -- Jake's own two complaints
    # ("do we need the walls?" / "the top wall is too close to the
    # screen") both addressed by removing the fence outright rather than
    # shaving the close (north) wall thinner: the two pegs already give
    # positive XY location, the two pads positive Z seating, and the GPS
    # patch's own foam pad the only real downward/lateral retention this
    # design ever needed from a wall in the first place.
    x0, x1, y0, y1 = mag_pcb_world_footprint(p)
    sy0 = y0 - mm['fence_clear'] - mm['fence_wall']
    sy1 = y0 - mm['fence_clear']
    stop = geo.box_solid(x0 - mm['fence_clear'], x1 + mm['fence_clear'], sy0, sy1,
                          ceiling - MAG_STOP_H, ceiling + 0.3)
    notch_cx = mag_header_notch_center_x(p)
    notch_w = mm['header_notch_w']
    notch = geo.box_solid(notch_cx - notch_w / 2.0, notch_cx + notch_w / 2.0,
                           sy0 - 0.5, sy1 + 0.5, ceiling - MAG_STOP_H - 0.5, ceiling + 0.8)
    stop = stop - notch
    top = top + stop

    bodies['Top'] = top
    return bodies
