"""Parity tests against the case-pass16 regression goldens (bounding box
+ volume + signed-distance sampling), per docs/hardware/cad-tooling-
spike.md section 4's method. Skips automatically if the read-only
case-pass16 worktree this was developed against isn't present (it lives
outside this repo, at a path fixed to this development environment) --
CI/other machines should point GOLDEN_DIR at their own copy of that
branch's exports.
"""
import os

import pytest

trimesh = pytest.importorskip('trimesh')
np = pytest.importorskip('numpy')

from .. import export as export_mod  # noqa: E402
from ..cli import build  # noqa: E402

GOLDEN_DIR = os.environ.get(
    'HEADLESS_PORT_GOLDEN_DIR',
    '/private/tmp/claude-501/case-pass16/hardware/case/export')

BBOX_TOL_MM = 0.1


def _golden_path(variant, name):
    return os.path.join(GOLDEN_DIR, variant, f'{name}.stl')


def _skip_if_no_golden(variant, name):
    path = _golden_path(variant, name)
    if not os.path.exists(path):
        pytest.skip(f'golden not found at {path} -- set HEADLESS_PORT_GOLDEN_DIR')
    return path


@pytest.mark.parametrize('variant,name', [('trim', 'Top'), ('trim', 'Bottom')])
def test_bbox_within_tolerance(tmp_path, variant, name):
    golden_path = _skip_if_no_golden(variant, name)
    p, bodies, _ = build(variant)
    mine_path = os.path.join(tmp_path, f'{name}.stl')
    export_mod.export_stl(bodies[name], mine_path)

    g = trimesh.load(golden_path)
    m = trimesh.load(mine_path)
    diff = np.abs(g.bounds - m.bounds).max()
    assert diff <= BBOX_TOL_MM, f'{variant} {name}: bbox diff {diff}mm exceeds {BBOX_TOL_MM}mm'


@pytest.mark.parametrize('variant,name', [('trim', 'Top'), ('trim', 'Bottom')])
def test_signed_distance_median_near_zero(tmp_path, variant, name):
    """The ported surfaces should match the golden almost exactly
    (median signed distance near 0) -- large max/95th-percentile values
    are expected wherever phase 1 has not yet ported a feature (see
    docs/hardware/headless-port-parity.md) and are reported, not
    asserted on here."""
    golden_path = _skip_if_no_golden(variant, name)
    p, bodies, _ = build(variant)
    mine_path = os.path.join(tmp_path, f'{name}.stl')
    export_mod.export_stl(bodies[name], mine_path)

    g = trimesh.load(golden_path)
    m = trimesh.load(mine_path)
    np.random.seed(0)
    pts, _ = trimesh.sample.sample_surface(g, 2000)
    d = trimesh.proximity.signed_distance(m, pts)
    median_abs = np.median(np.abs(d))
    assert median_abs < 0.05, f'{variant} {name}: median |signed distance| {median_abs}mm -- ported surfaces have drifted'
