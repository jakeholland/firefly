#!/usr/bin/env python3
"""Spike driver: build the ported Top-shell slice, export it, run the
offline + headless gates, and print timings -- see
docs/hardware/cad-tooling-spike.md for the full writeup this feeds.

Usage: .venv/bin/python3 build_slice.py [--repeat N]
"""
import argparse
import json
import sys
import time

sys.path.insert(0, '.')

from case.params import PARAMS   # noqa: E402
from case import shell, features, gates, export  # noqa: E402


def build_once(p):
    t = {}
    t0 = time.perf_counter()
    top = shell.build_top_shell(p)
    t['shell'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    top = shell.add_window(p, top)
    t['window'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    top = shell.add_lip_anchor_reliefs(p, top)
    t['lip_anchor'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    clip_tool = features.build_inner_cavity_clip_tool(p)
    t['clip_tool'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    near1, far1 = features.lanyard_corner_pair(p, side=-1)
    top = features.add_corner_block(p, top, near1, far1, clip_tool)
    t['corner_block_lanyard_end'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    near2, far2 = features.lanyard_corner_pair(p, side=1)
    top_before_ear = top
    top = features.add_corner_block(p, top, near2, far2, clip_tool)
    t['corner_block_candidate5_ear_standin'] = time.perf_counter() - t0

    return top, top_before_ear, (near1, far1), (near2, far2), t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repeat', type=int, default=1, help='full rebuild repeat count for timing')
    ap.add_argument('--out-dir', default='out')
    args = ap.parse_args()

    import os
    os.makedirs(args.out_dir, exist_ok=True)

    all_timings = []
    top = None
    for i in range(args.repeat):
        top, top_before_ear, pair1, pair2, t = build_once(PARAMS)
        t['total_build'] = sum(t.values())
        all_timings.append(t)
        print(f'--- build {i + 1}/{args.repeat} ---')
        print(json.dumps(t, indent=2))

    stl_path = os.path.join(args.out_dir, 'Top_slice.stl')
    threemf_path = os.path.join(args.out_dir, 'Top_slice.3mf')

    t0 = time.perf_counter()
    export.export_stl(top, stl_path)
    t_export_stl = time.perf_counter() - t0

    t0 = time.perf_counter()
    export.export_3mf(top, threemf_path)
    t_export_3mf = time.perf_counter() - t0

    print('export_stl_s', t_export_stl)
    print('export_3mf_s', t_export_3mf)

    # --- gates -----------------------------------------------------------
    t0 = time.perf_counter()
    p = PARAMS
    report = gates.manifold_and_overhang_check(
        stl_path, down_z=1.0, bed_z=p['top_z'],
        whitelist_xy=gates.offline_stl_check.TOP_WL)
    t_gate_offline = time.perf_counter() - t0
    print('gate_offline_stl_check_s', t_gate_offline)
    print(json.dumps({k: v for k, v in report.items() if k != 'overhang'}, indent=2, default=str))
    print('overhang bad_clusters_mm2:', report['overhang']['bad_clusters_mm2'])

    # live-geometry pilot-wall probe on the ACTUAL corner-block pilots
    # (top_pilot_dia / top_pilot_z), the ported stand-in for
    # verify_post_walls (which is written against top_posts, not ported
    # in this slice -- see the spike doc).
    t0 = time.perf_counter()
    wall_min = 0.6  # firefly_case.py:6437's skin_min for the analogous shell-skin check
    z0, z1 = p['top_pilot_z']
    z_samples = [z0 + 0.5, (z0 + z1) / 2.0, z1 - 0.5]
    probe_results = {}
    for name, (cx, cy) in (('A', pair1[0]), ('B1', pair1[1]), ('C', pair2[0]), ('B2', pair2[1])):
        bad = gates.pilot_wall_probe(top, cx, cy, p['top_pilot_dia'], wall_min, z_samples)
        probe_results[name] = bad
    t_gate_probe = time.perf_counter() - t0
    print('gate_pilot_wall_probe_s', t_gate_probe)
    print('pilot_wall_probe bad angles/z (empty = pass):', json.dumps(probe_results))

    # interference-volume gate: intersect the two corner blocks' own
    # ADDED material against each other -- headless stand-in for a live
    # Fusion checkInterferenceInput call.
    t0 = time.perf_counter()
    block1_only = top_before_ear  # top BEFORE the 2nd block was added -- proxy for "block 1's material"
    added_by_block2 = top - block1_only  # the material the 2nd add_corner_block call actually contributed
    overlap_vol = gates.interference_volume(block1_only, added_by_block2)
    t_gate_interference = time.perf_counter() - t0
    print('gate_interference_s', t_gate_interference)
    print('interference_volume_mm3 (0 = no overlap):', overlap_vol)

    print()
    print('=== SUMMARY ===')
    print(json.dumps({
        'build_timings': all_timings,
        'export_stl_s': t_export_stl,
        'export_3mf_s': t_export_3mf,
        'gate_offline_stl_check_s': t_gate_offline,
        'gate_pilot_wall_probe_s': t_gate_probe,
        'gate_interference_s': t_gate_interference,
        'final_volume_mm3': top.volume,
        'final_bbox': {
            'min': [top.bounding_box().min.X, top.bounding_box().min.Y, top.bounding_box().min.Z],
            'max': [top.bounding_box().max.X, top.bounding_box().max.Y, top.bounding_box().max.Z],
        },
    }, indent=2))


if __name__ == '__main__':
    main()
