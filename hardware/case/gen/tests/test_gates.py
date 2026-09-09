"""Gates-as-tests: every phase-1 verify_* gate, both variants, against a
from-scratch build. Run with `pytest` from hardware/case (gen/.venv
active)."""
import os

import pytest

from .. import export as export_mod
from .. import gates as gates_mod
from ..cli import build
from ..tools_bridge import bottom_whitelist, top_whitelist

VARIANTS = ['trim', 'current']


@pytest.fixture(scope='module', params=VARIANTS)
def built(request, tmp_path_factory):
    variant = request.param
    p, bodies, timings = build(variant)
    out_dir = tmp_path_factory.mktemp(f'gates_{variant}')
    stl_paths = {}
    for name in ('Top', 'Bottom'):
        path = os.path.join(out_dir, f'{name}.stl')
        export_mod.export_stl(bodies[name], path)
        stl_paths[name] = path
    return variant, p, bodies, stl_paths


def test_no_interference(built):
    variant, p, bodies, stl_paths = built
    result = gates_mod.check_interference_pairs({'Top': bodies['Top'], 'Bottom': bodies['Bottom']})
    assert result == {}, f'{variant}: real interference found: {result}'


def test_post_walls(built):
    variant, p, bodies, stl_paths = built
    result = gates_mod.verify_post_walls(bodies, p)
    bad = {k: v for k, v in result.items() if v}
    assert not bad, f'{variant}: post-wall failures: {bad}'


def test_root_fillets(built):
    variant, p, bodies, stl_paths = built
    result = gates_mod.verify_root_fillets(bodies, p)
    bad = {k: v for k, v in result.items() if v}
    assert not bad, f'{variant}: root-fillet failures: {bad}'


def test_corner_blocks(built):
    variant, p, bodies, stl_paths = built
    result = gates_mod.verify_corner_blocks(bodies, p)
    bad = {k: v for k, (ok, detail) in result.items() if not ok}
    assert not bad, f'{variant}: corner-block failures: {bad}'


def test_bottom_openings(built):
    variant, p, bodies, stl_paths = built
    result = gates_mod.verify_bottom_openings(bodies, p)
    bad = {k: v for k, (ok, detail) in result.items() if not ok}
    assert not bad, f'{variant}: bottom-opening failures: {bad}'


def test_offline_manifold_and_overhang(built):
    variant, p, bodies, stl_paths = built
    down_z_bed = {'Top': (1.0, p['top_z']), 'Bottom': (-1.0, p['bottom_z'])}
    wl = {'Top': top_whitelist(), 'Bottom': bottom_whitelist()}
    for name, path in stl_paths.items():
        down_z, bed_z = down_z_bed[name]
        report = gates_mod.manifold_and_overhang_check(path, down_z, bed_z, whitelist_xy=wl[name])
        assert report['manifold']['manifold'], f'{variant} {name}: non-manifold edges {report["manifold"]}'
        assert report['body_count']['one_body'], f'{variant} {name}: body count {report["body_count"]}'
        assert report['overhang']['bad_clusters_mm2'] == [], (
            f'{variant} {name}: bad overhang clusters {report["overhang"]["bad_clusters_mm2"]}')


def test_lip_ring_profile(built):
    variant, p, bodies, stl_paths = built
    result = gates_mod.verify_lip_ring_profile(stl_paths['Top'], p)
    assert result['mid_overhang_found'][0], f'{variant}: no real taper facet found in the lip/anchor ring band'
    assert result['flat_patch_max_width_mm'][0], (
        f'{variant}: flat patch too wide: {result["flat_patch_max_width_mm"][1]}mm')
