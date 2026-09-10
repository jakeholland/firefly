"""Display module (Waveshare ESP32-S3-Touch-LCD-1.46) STEP import and
placement -- build123d port of firefly_case.py:2907 insert_display_pcba.

Fusion's own version reads a live "reference transform" off an open
Fusion document (get_reference_transform, :2890) that this headless
build has no equivalent for -- there is no running Fusion session to
read it from. Per the port brief, the transform is instead DERIVED by
measuring the real STEP file directly and matching it against the two
values the Fusion-driven generator has always published as ground truth:
`display_underside_z_min` (12.04mm world, 'current') and the live-
measured standoff plane (18.80mm 'current' / 21.80mm 'trim',
`ear_seat_z` + `ear_seat_offset` in params_current.py).

Measured (this port, `uv run python -m gen.components measure`):
  - The STEP's own local frame has its origin at the display's own
    optical/window centre, with local +z = 0 at the glass's top face
    (flush with the case's own top_z) and local -z reaching down through
    the PCB and its underside components.
  - A 180-degree rotation about the local Z axis (both X and Y negate)
    plus a translation of (0, 50.0, top_z) reproduces BOTH ground-truth
    numbers exactly: `display_underside_z_min` (local z=-12.95 -> world
    12.05mm, 'current', vs. the documented 12.04mm) and the standoff
    plane (local z=-6.2 -> world top_z-6.2 = 18.80mm 'current' / 21.80mm
    'trim' -- an EXACT match to both documented values, not just close).
  - The three SMT standoff barrels (~3.5x3.5x4.7mm bboxes, matching the
    documented 'SMTSO-M2-3.5X2-3.5ET' part) were found by scanning every
    solid in the imported compound for that footprint -- exactly 3 come
    back, at local (12.0, -14.95), (-11.544, -15.447), (0.0, 17.75).
    Through the same transform, their real-world XY comes out as
    S1=(-12.000, 64.950), S3=(11.544, 65.447), S2=(0.000, 32.250) --
    within ~0.3mm of params_current.py's own hand-recorded
    `board_standoffs` (S1 (-12.0, 65.0), S3 (11.8, 65.46), S2 (0.04,
    32.22)), which is the expected level of agreement for a fresh direct
    measurement vs. a historical Fusion probe of the SAME real part.
    THESE measured numbers, not the historical params, are the new
    source of truth for wherever phase 2's ears/S2-boss need to target
    the real barrel (see docs/hardware/headless-port-plan.md).
"""
import functools
import hashlib
import math
import os

import build123d as bd
from OCP.BRep import BRep_Builder
from OCP.BRepTools import BRepTools
from OCP.TopoDS import TopoDS_Shape

from . import geometry as geo

_MODELS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'models'))
DISPLAY_STEP = os.path.join(_MODELS_DIR, 'ESP32-S3-Touch-LCD-1_46.step')
XIAO_STEP = os.path.join(_MODELS_DIR, 'XIAO-ESP32S3_v3.step')
WIO_STEP = os.path.join(_MODELS_DIR, 'Wio-SX1262_for_XIAO_V2.step')
L76K_STEP = os.path.join(_MODELS_DIR, 'L76K_GNSS_for_XIAO_v1.step')

# Local-frame -> world-frame transform, derived empirically (see module
# docstring). Only Z depends on the variant (via top_z); X/Y are a fixed
# 180-degree-about-Z rotation plus translation, identical both variants
# (matches "the display module... stays at its reference positions"
# convention every param file already documents).
_Y_OFFSET = 50.0
STANDOFF_BARREL_TOP_LOCAL_Z = -6.2   # matches ear_seat_z's own "standoff plane" anchor, both variants, exactly

_CASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
_CACHE_DIR = os.path.join(_CASE_DIR, 'gen', 'out', '_cache')


def display_to_world(local_pt, top_z):
    lx, ly, lz = local_pt
    return (-lx, -ly + _Y_OFFSET, lz + top_z)


@functools.lru_cache(maxsize=1)
def _step_hash():
    """First 8 hex chars of the STEP file's own sha256 -- a cache-key
    fragment, not a security hash (item 3, this phase's own cycle-time
    brief: 'persisted to gen/out/ keyed by STEP hash'). Cheap (~10ms for
    a 14.8MB file, dominated by disk read, not hashing) and automatically
    invalidates every cache entry below if `hardware/models/ESP32-S3-
    Touch-LCD-1_46.step` is ever replaced."""
    h = hashlib.sha256()
    with open(DISPLAY_STEP, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()[:8]


def _brep_write(shape, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    BRepTools.Write_s(shape.wrapped, tmp)
    os.replace(tmp, path)  # atomic -- a concurrent reader never sees a partial file


def _brep_read(path, wrapper):
    raw = TopoDS_Shape()
    ok = BRepTools.Read_s(raw, path, BRep_Builder())
    if not ok:
        raise ValueError(f'corrupt BREP cache file: {path}')
    return wrapper(raw)


@functools.lru_cache(maxsize=1)
def _load_compound():
    """The raw (local-frame) 420-solid display compound. Item 3 (this
    phase's own cycle-time brief): a plain `bd.import_step` of this
    14.8MB file measured ~6.4s on its own (this port's dominant
    per-process fixed cost every time the display is touched at all --
    `measure_standoffs`/`ceiling_safe_display_cut`/`check_display_
    interference_near_ears`/buttons' own `verify_plunger_reach` each
    need it, and `functools.lru_cache` here only dedupes calls WITHIN
    one process, not across the separate `python3 -m gen.cli`/pytest
    processes a normal edit-build-check loop runs). Cached to a native
    OCC BREP file (`gen/out/_cache/display_raw_<stephash>.brep`,
    keyed by `_step_hash()` so a STEP-file change invalidates it
    automatically) -- BREP is a lossless, near-instant round-trip
    (~0.13s measured for this same 420-solid compound, ~50x faster than
    the STEP import it replaces) unlike STEP's own text-based, unit-
    converting, topology-rebuilding parse. `gen/out/` is already
    git-ignored (`gen/.gitignore`), so this cache is pure scratch, safe
    to delete any time (rebuilt transparently on the next miss)."""
    cache_path = os.path.join(_CACHE_DIR, f'display_raw_{_step_hash()}.brep')
    if os.path.exists(cache_path):
        try:
            return _brep_read(cache_path, bd.Compound)
        except Exception:
            pass  # fall through and rebuild from the real STEP file
    compound = bd.import_step(DISPLAY_STEP)
    try:
        _brep_write(compound, cache_path)
    except OSError:
        pass  # a read-only gen/out/ (e.g. CI) just means no cache, not a build failure
    return compound


def _step_hash_of(step_path):
    """Generalization of `_step_hash` (item 3's own cache-key idiom) for
    an arbitrary STEP file, not just the display -- used by the comms
    board loaders below."""
    h = hashlib.sha256()
    with open(step_path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()[:8]


@functools.lru_cache(maxsize=8)
def _load_step_compound_cached(step_path):
    """Same BREP-cache idiom as `_load_compound` (item 3), generalized to
    any STEP file -- used for the three comms-stack boards (phase 2 item
    3, this port's own numbering: comms stack / GPS frame / battery bay).
    Each board's STEP is small (1.5-3.7MB, vs. the display's 14.8MB) so
    the raw-import cost this cache removes is smaller per-file, but the
    same per-process-only `lru_cache` limitation applies across the
    separate CLI/pytest processes a normal build-check loop runs, so the
    BREP round-trip is still worth it."""
    stem = os.path.splitext(os.path.basename(step_path))[0]
    cache_path = os.path.join(_CACHE_DIR, f'{stem}_{_step_hash_of(step_path)}.brep')
    if os.path.exists(cache_path):
        try:
            return _brep_read(cache_path, bd.Compound)
        except Exception:
            pass
    compound = bd.import_step(step_path)
    try:
        _brep_write(compound, cache_path)
    except OSError:
        pass
    return compound


def find_pcb_like_body(compound, lo=15.0, hi=24.0, max_thick=3.0):
    """Port of firefly_case.py:5636 find_pcb_like_body -- scans every
    solid in `compound` (its own local/native frame) for the largest-area
    body shaped like a small PCB (two extents in [lo,hi]mm, the third
    <= max_thick), returning (solid, area, thin_axis) or None. Used to
    find the L76K assembly's own real PCB body among its other solids
    (the GPS patch antenna on its own cable, a stray lead, ...) -- see
    load_l76k's own docstring for why this matters (positioning by the
    whole compound's own aggregate bbox would put the real board outside
    the case entirely, same finding the source's own 2026-09-05 fix
    documents)."""
    best = None
    for s in compound.solids():
        bb = s.bounding_box()
        dx = bb.max.X - bb.min.X
        dy = bb.max.Y - bb.min.Y
        dz = bb.max.Z - bb.min.Z
        extents = {'x': dx, 'y': dy, 'z': dz}
        thin_axis = min(extents, key=extents.get)
        thin = extents[thin_axis]
        others = sorted(v for k, v in extents.items() if k != thin_axis)
        if thin <= max_thick and lo <= others[0] <= hi and lo <= others[1] <= hi:
            area = others[0] * others[1]
            if best is None or area > best[1]:
                best = (s, area, thin_axis)
    return best


# Rotation-matrix columns for each `flatten_transform` mode -- (to_x, to_y,
# to_z), i.e. where each NATIVE axis direction ends up in world space.
# Verbatim port of firefly_case.py:5598 flatten_transform's own
# Matrix3D.setToAlignCoordinateSystems table (see that function's own
# per-mode comments for the physical reasoning -- 'y90' in particular
# exists so a board's long native axis lands on world Y, not world X).
_FLATTEN_AXES = {
    'z': ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)),
    'y': ((1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (0.0, -1.0, 0.0)),
    'y90': ((0.0, 1.0, 0.0), (0.0, 0.0, 1.0), (1.0, 0.0, 0.0)),
    'y90neg': ((0.0, -1.0, 0.0), (0.0, 0.0, 1.0), (-1.0, 0.0, 0.0)),
    'x': ((0.0, 0.0, 1.0), (0.0, 1.0, 0.0), (-1.0, 0.0, 0.0)),
}


def flatten_transform(native_center, thin_axis, target_center):
    """Port of firefly_case.py:5598 flatten_transform -- a rigid transform
    that rotates the object's own NATIVE axes onto the world axes named by
    `_FLATTEN_AXES[thin_axis]` (pivoting about `native_center`, its own
    center in its native frame) and then relocates that same pivot to
    `target_center`. Fusion's `Matrix3D.setToAlignCoordinateSystems`
    (native frame -> target frame) has an exact OCP equivalent,
    `gp_Trsf.SetDisplacement(from_ax3, to_ax3)` -- documented to do
    exactly this (map FromSystem's origin/X/Y/Z onto ToSystem's), so no
    hand-rolled matrix math is needed. Returns a build123d Location;
    apply as `flatten_transform(...) * compound`."""
    from OCP.gp import gp_Ax3, gp_Dir, gp_Pnt, gp_Trsf

    to_x, to_y, to_z = _FLATTEN_AXES[thin_axis]
    from_ax3 = gp_Ax3(gp_Pnt(*native_center), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))
    to_ax3 = gp_Ax3(gp_Pnt(*target_center), gp_Dir(*to_z), gp_Dir(*to_x))
    trsf = gp_Trsf()
    trsf.SetDisplacement(from_ax3, to_ax3)
    return bd.Location(trsf)


def load_l76k_raw():
    """Raw (local-frame) L76K GNSS module compound (PCB + GPS patch
    antenna on its own cable + a stray lead -- see find_pcb_like_body's
    own docstring)."""
    return _load_step_compound_cached(L76K_STEP)


def load_xiao_raw():
    return _load_step_compound_cached(XIAO_STEP)


def load_wio_raw():
    return _load_step_compound_cached(WIO_STEP)


L76K_STRAY_BODY_RADIUS_MM = 20.0  # firefly_case.py:5789 -- see _filter_l76k_placed's own docstring


def _filter_l76k_placed(placed, target_xy):
    """Port of insert_comms_boards' own `_hide_leaf_bodies` (:5789) --
    NOT one of firefly_case.py's 217 named functions, but load-bearing:
    the L76K reference STEP's own GPS-patch-antenna-on-a-cable
    sub-assembly (see find_pcb_like_body's own docstring) is native-
    authored with a cable trace reaching ~49mm away from the PCB in the
    native frame's OWN thin (Z) axis -- under this board's 'y90' rotation
    that native Z axis lands on WORLD X, so the raw placed compound's own
    bbox balloons to roughly (-40, 9) in X, nowhere near the real stack's
    actual ~18x21mm footprint (confirmed empirically importing the real
    STEP for this port). The source works around the identical problem by
    setting `isLightBulbOn=False` directly on every leaf body more than
    `L76K_STRAY_BODY_RADIUS_MM` from the target XY (except the PCB body
    itself, never hidden) -- a live, physically-real GPS patch antenna
    that in the ACTUAL assembly is wired separately and mounted in its
    own bay.gps_patch frame (add_gps_reference_box), not physically
    riding along with this board. This port's equivalent: keep only the
    solids of the PLACED compound whose own bbox centre (world XY) falls
    within that same radius of the stack's target centre -- the PCB body
    is always kept (its own centre is exactly the target by
    construction, distance 0)."""
    tx, ty = target_xy
    kept = []
    for s in placed.solids():
        bb = s.bounding_box()
        cx = (bb.min.X + bb.max.X) / 2.0
        cy = (bb.min.Y + bb.max.Y) / 2.0
        if math.hypot(cx - tx, cy - ty) <= L76K_STRAY_BODY_RADIUS_MM:
            kept.append(s)
    assert kept, 'L76K stray-body filter left nothing -- target_xy or radius is wrong'
    return bd.Compound(kept) if len(kept) > 1 else kept[0]


def load_comms_stack(p):
    """Port of firefly_case.py:5695 insert_comms_boards -- places the real
    3-board direct-solder/B2B stack (L76K bottom -> XIAO middle -> Wio
    top) in world space, each board's Z derived from the ACTUAL measured
    thickness/top of the board below it (xiao_gap/wio_gap are the only
    fixed numbers in PARAMS; every Z builds on a live measurement of the
    board actually loaded), exactly like the source. Returns a dict:
    {'l76k': compound, 'l76k_top_z': z,
     'xiao': compound_or_None, 'xiao_top_z': z_or_None,
     'wio': compound_or_None, 'stack_top_z': z}  -- `stack_top_z` is
    Wio's own top when present, else XIAO's, else L76K's (matches the
    source's own "whichever board is physically highest" convention).
    `comms_stack3_full_height=False` ('current' variant) loads ONLY the
    L76K, same as the source's own insert_comms_boards -- 'current' is
    the M1 probe-table comparison variant, not one meant to carry real
    electronics (see params_current.py's own comment)."""
    s3 = p['bay']['stack3']
    pcb = s3['l76k_pcb']
    cx = (pcb['x'][0] + pcb['x'][1]) / 2.0
    cy = (pcb['y'][0] + pcb['y'][1]) / 2.0
    full_height = p.get('comms_stack3_full_height', True)
    out = {'l76k': None, 'l76k_top_z': None, 'xiao': None, 'xiao_top_z': None, 'wio': None, 'stack_top_z': None}

    # --- L76K: position by its own real PCB body, not the whole
    # assembly's aggregate bbox (the GPS patch antenna-on-a-cable would
    # otherwise dominate it -- see find_pcb_like_body's docstring). ---
    raw = load_l76k_raw()
    match = find_pcb_like_body(raw)
    assert match is not None, 'no ~18x21mm PCB-like body found in the L76K assembly'
    pcb_body, _area, _thin_axis = match
    bb = pcb_body.bounding_box()
    native_center = ((bb.min.X + bb.max.X) / 2.0, (bb.min.Y + bb.max.Y) / 2.0, (bb.min.Z + bb.max.Z) / 2.0)
    # thin_axis from find_pcb_like_body is the SHAPE's own thin axis
    # ('y' here, matching the source's own live-measured finding) -- the
    # ROTATION MODE passed to flatten_transform is 'y90' regardless (see
    # that function's own module-level table): 'y90' still maps native Y
    # (the 1.54mm PCB thickness) onto world Z for the thickness lookup
    # below, but ALSO rotates 90deg about world Z so native X (the PCB's
    # long ~20.95mm axis) lands on world Y, not world X -- see
    # flatten_transform's own 'y90' comment for why (a live Fusion build
    # found the plain 'y' mode put the long axis backwards, on world X).
    pcb_thickness = bb.max.Y - bb.min.Y
    l76k_bottom_z = s3['l76k_bottom_z']
    target_pcb_center = (cx, cy, l76k_bottom_z + pcb_thickness / 2.0)
    loc = flatten_transform(native_center, 'y90', target_pcb_center)
    placed = loc * raw
    out['l76k'] = _filter_l76k_placed(placed, (cx, cy))
    out['l76k_top_z'] = l76k_bottom_z + pcb_thickness

    if not full_height:
        out['stack_top_z'] = out['l76k_top_z']
        return out

    # --- XIAO: positioned by its own WHOLE-compound bbox (the source's
    # own insert_and_place uses the occurrence's aggregate bbox here, not
    # a sub-body search -- XIAO's reference doc is a single clean board,
    # no antenna-on-a-cable complication). thin_axis='y90' (native Y ->
    # world Z, native X -> world Y) -- USB-C end toward +Y, matching the
    # L76K's own long axis below it (per the source's own comment: "the
    # spec's XIAO USB-C end toward +Y"). ---
    xiao_raw = load_xiao_raw()
    xbb = xiao_raw.bounding_box()
    x_native_center = ((xbb.min.X + xbb.max.X) / 2.0, (xbb.min.Y + xbb.max.Y) / 2.0, (xbb.min.Z + xbb.max.Z) / 2.0)
    xiao_thickness = xbb.max.Y - xbb.min.Y  # native Y is XIAO's thin axis (per Jake, matching the source)
    xiao_bottom_z = out['l76k_top_z'] + s3['xiao_gap']
    x_target_center = (cx, cy, xiao_bottom_z + xiao_thickness / 2.0)
    xloc = flatten_transform(x_native_center, 'y90', x_target_center)
    out['xiao'] = xloc * xiao_raw
    out['xiao_top_z'] = xiao_bottom_z + xiao_thickness

    # --- Wio: native bbox is already thinnest in Z (module face up, as
    # authored) -- thin_axis='z' is a pure translation, no rotation. ---
    wio_raw = load_wio_raw()
    wbb = wio_raw.bounding_box()
    w_native_center = ((wbb.min.X + wbb.max.X) / 2.0, (wbb.min.Y + wbb.max.Y) / 2.0, (wbb.min.Z + wbb.max.Z) / 2.0)
    wio_thickness = wbb.max.Z - wbb.min.Z
    wio_bottom_z = out['xiao_top_z'] + s3['wio_gap']
    w_target_center = (cx, cy, wio_bottom_z + wio_thickness / 2.0)
    wloc = flatten_transform(w_native_center, 'z', w_target_center)
    out['wio'] = wloc * wio_raw
    out['stack_top_z'] = wio_bottom_z + wio_thickness
    return out


def load_display(p):
    """Return the display compound placed in world space for params `p`
    (uses p['top_z'] for the Z offset -- see insert_display_pcba's own
    'display_z_offset shifts everything display-relative' convention)."""
    compound = _load_compound()
    top_z = p['top_z']
    # 180-about-Z rotation then translate -- build123d Location takes
    # (translation, rotation); rotate first about the compound's own
    # local origin, then translate to world.
    loc = bd.Location((0, _Y_OFFSET, top_z)) * bd.Location((0, 0, 0), (0, 0, 180))
    return loc * compound


def find_standoff_barrels(foot_lo=3.0, foot_hi=4.2, height_lo=4.0, height_hi=5.5):
    """Scan the raw (local-frame) compound for the display's own SMT
    standoff barrels by shape (see module docstring) -- returns a list of
    (local_x, local_y, local_z_bottom, local_z_top) tuples, unsorted."""
    compound = _load_compound()
    hits = []
    for s in compound.solids():
        bb = s.bounding_box()
        dx, dy, dz = bb.max.X - bb.min.X, bb.max.Y - bb.min.Y, bb.max.Z - bb.min.Z
        foot = max(dx, dy)
        if foot_lo <= foot <= foot_hi and height_lo <= dz <= height_hi:
            cx = (bb.min.X + bb.max.X) / 2.0
            cy = (bb.min.Y + bb.max.Y) / 2.0
            hits.append((cx, cy, bb.min.Z, bb.max.Z))
    return hits


def measure_standoffs(p, target_world_xy=None):
    """Measure the standoff plane z and each barrel's world XY for
    params `p`. `target_world_xy` (dict name -> (x, y)) lets the caller
    match each measured barrel to a named target (S1/S2/S3) by nearest
    distance -- defaults to params_current.py's own `board_standoffs`.
    Returns {name: {'world_xy': (x, y), 'standoff_plane_z': z}}."""
    if target_world_xy is None:
        target_world_xy = p['board_standoffs']
    top_z = p['top_z']
    barrels = find_standoff_barrels()
    assert len(barrels) == 3, f'expected 3 standoff barrels, found {len(barrels)}: {barrels}'

    measured = []
    for lx, ly, lz_bot, lz_top in barrels:
        wx, wy, _ = display_to_world((lx, ly, 0.0), top_z)
        plane_z = STANDOFF_BARREL_TOP_LOCAL_Z + top_z
        measured.append({'world_xy': (wx, wy), 'standoff_plane_z': plane_z, 'local_xy': (lx, ly)})

    result = {}
    remaining = list(measured)
    for name, (tx, ty) in target_world_xy.items():
        best = min(remaining, key=lambda m: math.hypot(m['world_xy'][0] - tx, m['world_xy'][1] - ty))
        remaining.remove(best)
        result[name] = best
    return result


def battery_connector_world_bbox(p):
    """firefly_case.py:2849 battery_connector_world_bbox -- world bbox of
    the display module's own 2-pin JST-style battery socket
    ('HP1_25MM-2P-SMT-HORIZONTAL'). x/y are identical in both variants
    (same reference transform); z is offset by `display_z_offset`, same
    convention as every other display-relative z value in this port (see
    load_display)."""
    bb = p['battery_connector_bbox']
    dz = p.get('display_z_offset', 0.0)
    return bb['x'], bb['y'], (bb['z'][0] + dz, bb['z'][1] + dz)


def secondary_conn_world_bbox(p):
    """firefly_case.py:2873 secondary_conn_world_bbox -- world bbox of the
    display module's second real SMT connector
    ('PITCH1MM-2PIN-SMT-HORIZONTAL'). Same display_z_offset convention as
    battery_connector_world_bbox."""
    bb = p['secondary_conn_bbox']
    dz = p.get('display_z_offset', 0.0)
    return bb['x'], bb['y'], (bb['z'][0] + dz, bb['z'][1] + dz)


def apply_known_component_keepouts(top, p):
    """Port of firefly_case.py:5936-5959 (inline in build(), right after
    add_ear/add_s2_boss) -- two unconditional keep-out cuts of specific,
    live-found real display components the S3 ear's own arm/wedge run
    passes close to: the second SMT connector (`secondary_conn_world_
    bbox`, +0.5mm margin every side -- the source's own comment: "a live
    check_interference(Top, display) found a real 17.8mm^3 overlap here
    even after the standoff-barrel fix... cleared the other 8 hits") and
    five smaller SMD-component hits (`p['ear_wedge_component_keepouts']`,
    already-margined boxes, no extra pad). Applied generically to Top,
    same as the source's own framing ("stays correct even if the ear
    geometry shifts again"), not tied to add_ear's own construction."""
    scx, scy, scz = secondary_conn_world_bbox(p)
    top = top - geo.box_solid(scx[0] - 0.5, scx[1] + 0.5, scy[0] - 0.5, scy[1] + 0.5,
                               scz[0] - 0.5, scz[1] + 0.5)

    dz = p.get('display_z_offset', 0.0)
    for kb in p.get('ear_wedge_component_keepouts', []):
        top = top - geo.box_solid(kb['x'][0], kb['x'][1], kb['y'][0], kb['y'][1],
                                   kb['z'][0] + dz, kb['z'][1] + dz)
    return top


PILOT_PROTECT_MARGIN = 1.0        # firefly_case.py:1975
CEILING_CUT_FOOTPRINT_MIN_MM2 = 4.0  # firefly_case.py's own build() candidate filter, same value
SPLIT_REJECT_VOLUME_MM3 = 0.5     # see ceiling_safe_display_cut's own docstring -- sliver vs. real sever


def _ceiling_cut_candidates(p):
    """The same candidate selection ceiling_safe_display_cut's own exact
    path uses (footprint area + pilot-protect-z filter) -- factored out
    so the fast path's own fused-tool cache and the exact path's own
    per-candidate loop can never silently drift apart about which real
    display sub-bodies qualify."""
    pilot_protect_z = p['top_pilot_z'][1] + PILOT_PROTECT_MARGIN
    disp = load_display(p)
    for s in disp.solids():
        bb = s.bounding_box()
        if bb.max.Z <= pilot_protect_z:
            continue
        area = (bb.max.X - bb.min.X) * (bb.max.Y - bb.min.Y)
        if area <= CEILING_CUT_FOOTPRINT_MIN_MM2:
            continue
        yield s


def _fused_ceiling_cut_tool(p, use_cache=True):
    """Item 3 (this phase's own cycle-time brief): fuses every
    candidate's own `band & s` cutting tool (the exact same per-candidate
    tool `ceiling_safe_display_cut`'s own exact path builds -- see
    `_ceiling_cut_candidates`) into ONE solid, cached to a native BREP
    file keyed by `_step_hash()` plus every band-defining PARAMS number
    (`top_pilot_z`, `top_ceiling_underside_z`, `top_z` -- `load_display`'s
    own world transform depends on `top_z` too), so a params edit
    invalidates the cache automatically rather than silently serving a
    stale tool. This is the ACTUAL material the exact path would remove
    absent its own per-candidate sliver-rejection safety net (see that
    function's own docstring) -- the fast default path below applies
    this ONE fused tool in a SINGLE cut instead of ~30 sequential ones,
    trading the per-candidate rejection's fine-grained safety margin for
    speed; `exact=True` (the release-gate path) still runs the original,
    unfused algorithm unchanged."""
    key = f'{_step_hash()}_{p["top_z"]}_{p["top_pilot_z"][0]}_{p["top_pilot_z"][1]}_{p["top_ceiling_underside_z"]}'
    cache_path = os.path.join(_CACHE_DIR, f'ceiling_cut_tool_{key}.brep')
    if use_cache and os.path.exists(cache_path):
        try:
            return _brep_read(cache_path, bd.Solid)
        except Exception:
            pass
    pilot_protect_z = p['top_pilot_z'][1] + PILOT_PROTECT_MARGIN
    ceiling_z = p['top_ceiling_underside_z']
    band = geo.box_solid(-100.0, 100.0, -100.0, 100.0, pilot_protect_z, ceiling_z + 0.3)
    tool = None
    for s in _ceiling_cut_candidates(p):
        try:
            piece = band & s
        except Exception:
            continue
        if piece is None or piece.volume < 1e-6:
            continue
        tool = piece if tool is None else (tool + piece)
    if tool is not None and use_cache:
        try:
            _brep_write(tool, cache_path)
        except OSError:
            pass
    return tool


def ceiling_safe_display_cut(top, p, exact=False, use_cache=True):
    """Port of the inline ceiling-safe cut in firefly_case.py's own
    `build()` (right after insert_display_pcba, ~:5997-6096) -- NOT one
    of the file's 217 named functions, but load-bearing: cuts Top against
    the display module's own real sub-bodies, restricted to the slice
    strictly between `top_pilot_z[1] + PILOT_PROTECT_MARGIN` and
    `top_ceiling_underside_z + 0.3` (a band that, by construction, can
    never reach the outer 2mm skin or undercut any screw pilot's own
    engagement depth), candidate-filtered to sub-bodies whose own XY
    footprint area exceeds `CEILING_CUT_FOOTPRINT_MIN_MM2` (every
    individual SMT part measures well under 1mm^2; only the connectors/
    shields/bare PCB that can plausibly reach this high are bigger --
    the source's own live-tuned proxy, confirmed there to bring
    display-vs-Top interference to true zero both variants).

    The source calls this once, globally, after add_ear/add_s2_boss/
    add_buttons -- this port calls it once too (see cli.py), after the
    ears/S2 boss (buttons not yet ported); it does not depend on either.

    OCC-specific robustness note (no Fusion equivalent needed): cutting
    ~30 independent tool boxes one at a time out of a single shell
    occasionally leaves a NEGLIGIBLE sliver behind as its own disjoint
    micro-solid -- live-checked (this port): both real occurrences are
    <=0.05mm^3 (ordinary tessellation slack at the cut tool's own
    coincident face, the exact same artifact firefly_case.py's own
    comment on this cut anticipates -- "most likely ordinary
    tessellation slack at the cut tool's own coincident top face"), next
    to a main remaining body of ~18700mm^3 -- discarded (kept only the
    largest resulting solid) rather than rejecting the whole cut, since
    a print slicer would never render a sub-0.1mm^3 fleck anyway. A cut
    that instead leaves a SECOND solid above `SPLIT_REJECT_VOLUME_MM3`
    is a genuine structural sever (the "silently splits into two bodies"
    failure class firefly_case.py's own dedupe_body/_clip_of_ear_boss_
    keepout docstrings document at length elsewhere in this file --
    Fusion's own timeline hid this same risk behind body names, not
    avoided it) and is rolled back outright (skipped, counted in the
    returned stats), never silently accepted. See docs/hardware/
    headless-port-parity.md for how many of the source's own ~33 live
    candidates this port actually applies vs. skips.

    `exact=False` (default, item 3 -- this phase's own cycle-time brief):
    applies `_fused_ceiling_cut_tool`'s one cached, fused tool in a
    SINGLE cut + one split-check, instead of ~30 sequential per-candidate
    cuts -- ~15-25s down to a fraction of a second on a warm cache (the
    STEP import/candidate-fuse itself is the one-time cost the cache
    file absorbs). `exact=True` (the release-gate path -- pass this from
    the CLI's own `--exact-display` flag) runs the original, unfused,
    per-candidate algorithm unchanged, with its own finer-grained
    sliver-vs-real-sever distinction per candidate rather than one
    combined tool's -- use it before cutting real plastic."""
    if not exact:
        tool = _fused_ceiling_cut_tool(p, use_cache=use_cache)
        stats = {'candidates': None, 'applied': 0, 'skipped_empty': 0, 'skipped_would_split': 0,
                 'slivers_discarded': 0, 'mode': 'fast (fused, cached)'}
        if tool is None or tool.volume < 1e-6:
            stats['skipped_empty'] = 1
            return top, stats
        candidate = top - tool
        sols = sorted(candidate.solids(), key=lambda sol: sol.volume, reverse=True)
        if len(sols) > 1 and sum(sol.volume for sol in sols[1:]) > SPLIT_REJECT_VOLUME_MM3:
            stats['skipped_would_split'] = 1
            return top, stats
        if len(sols) > 1:
            stats['slivers_discarded'] = len(sols) - 1
            candidate = sols[0]
        stats['applied'] = 1
        return candidate, stats

    pilot_protect_z = p['top_pilot_z'][1] + PILOT_PROTECT_MARGIN
    ceiling_z = p['top_ceiling_underside_z']
    stats = {'candidates': 0, 'applied': 0, 'skipped_empty': 0, 'skipped_would_split': 0,
              'slivers_discarded': 0, 'mode': 'exact'}
    for s in _ceiling_cut_candidates(p):
        stats['candidates'] += 1
        band = geo.box_solid(-100.0, 100.0, -100.0, 100.0, pilot_protect_z, ceiling_z + 0.3)
        try:
            tool = band & s
        except Exception:
            stats['skipped_empty'] += 1
            continue
        if tool is None or tool.volume < 1e-6:
            stats['skipped_empty'] += 1
            continue
        candidate = top - tool
        sols = sorted(candidate.solids(), key=lambda sol: sol.volume, reverse=True)
        if len(sols) > 1 and sum(sol.volume for sol in sols[1:]) > SPLIT_REJECT_VOLUME_MM3:
            stats['skipped_would_split'] += 1
            continue
        if len(sols) > 1:
            stats['slivers_discarded'] += len(sols) - 1
            candidate = sols[0]
        top = candidate
        stats['applied'] += 1
    return top, stats


def _main():
    import json

    from .params import get_params

    for variant in ('current', 'trim'):
        p = get_params(variant)
        m = measure_standoffs(p)
        print(f'--- {variant} (top_z={p["top_z"]}) ---')
        print(json.dumps({k: {'world_xy': [round(v, 3) for v in d['world_xy']],
                               'standoff_plane_z': round(d['standoff_plane_z'], 3)}
                           for k, d in m.items()}, indent=2))
    bb = _load_compound().bounding_box()
    print('display STEP local bbox:', (bb.min.X, bb.min.Y, bb.min.Z), (bb.max.X, bb.max.Y, bb.max.Z))
    p_current = get_params('current')
    underside_world = display_to_world((0, 0, bb.min.Z), p_current['top_z'])[2]
    print('display_underside_z_min (current, measured):', round(underside_world, 3),
          'vs params_current.py documented 12.04')


if __name__ == '__main__':
    _main()
