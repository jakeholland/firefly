#!/usr/bin/env python3
"""Headless build123d driver for the Firefly case generator port.

Usage (from hardware/case, with gen/.venv active):
    python3 -m gen.cli build --variant trim --gates --export --render

Phase 1 builds: Top/Bottom shell, window, lip/anchor ring, case screws
A/C/D (Bottom boss + Top corner block, each with its root-reinforcement
collar), the USB-C tunnel + liner, the FPC relief pocket, and the
lanyard lug. See docs/hardware/headless-port-plan.md for what is not
yet ported (ears, S2 boss, buttons, stack/GPS/battery/compass frames,
wordmark, comms boards) and docs/hardware/headless-port-parity.md for
the regression comparison against the pass-16 goldens.
"""
import argparse
import json
import os
import time

from . import export as export_mod
from . import gates as gates_mod
from . import shell
from .features import corner_blocks, fpc_relief, lug, usb_tunnel
from .params import get_params

_CASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


def build(variant='trim'):
    """Build Top/Bottom for the given variant. Returns (bodies, timings)."""
    p = get_params(variant)
    t = {}

    t0 = time.perf_counter()
    bodies = shell.build_shells(p)
    t['shell'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    bodies = shell.add_window(bodies, p)
    t['window'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    bodies = shell.add_lip_anchor_reliefs(bodies, p)
    t['lip_anchor_ring'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    bodies = corner_blocks.add_case_screws(bodies, p)
    t['case_screws_ACD'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    bodies = usb_tunnel.add_usb_tunnel(bodies, p)
    t['usb_tunnel'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    bodies = fpc_relief.add_fpc_relief(bodies, p)
    t['fpc_relief'] = time.perf_counter() - t0

    t0 = time.perf_counter()
    bodies = lug.add_lug(bodies, p)
    t['lug'] = time.perf_counter() - t0

    t['total_build'] = sum(t.values())
    return p, bodies, t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('action', choices=['build'])
    ap.add_argument('--variant', default='trim', choices=['trim', 'current'])
    ap.add_argument('--gates', action='store_true')
    ap.add_argument('--export', action='store_true')
    ap.add_argument('--render', action='store_true')
    ap.add_argument('--out-dir', default=os.path.join(_CASE_DIR, 'gen', 'out'))
    args = ap.parse_args()

    t_all0 = time.perf_counter()
    p, bodies, timings = build(args.variant)
    print(f'--- build ({args.variant}) ---')
    print(json.dumps(timings, indent=2))
    print('Top volume mm3', bodies['Top'].volume)
    print('Bottom volume mm3', bodies['Bottom'].volume)
    bb_top = bodies['Top'].bounding_box()
    bb_bot = bodies['Bottom'].bounding_box()
    print('Top bbox', (bb_top.min.X, bb_top.min.Y, bb_top.min.Z), (bb_top.max.X, bb_top.max.Y, bb_top.max.Z))
    print('Bottom bbox', (bb_bot.min.X, bb_bot.min.Y, bb_bot.min.Z), (bb_bot.max.X, bb_bot.max.Y, bb_bot.max.Z))

    stl_paths = {}
    if args.export:
        t0 = time.perf_counter()
        stl_paths = export_mod.export_all(bodies, args.variant, args.out_dir)
        print('export_s', time.perf_counter() - t0)
        print(json.dumps(stl_paths, indent=2))

    if args.gates:
        t0 = time.perf_counter()
        from .tools_bridge import top_whitelist, bottom_whitelist
        wl = top_whitelist() if 'Top' in stl_paths else None
        report = gates_mod.all_gates(bodies, p, stl_paths, whitelist_xy=wl)
        print('gates_s', time.perf_counter() - t0)
        print(json.dumps(report, indent=2, default=str))

    if args.render:
        from . import render as render_mod
        render_dir = os.path.join(_CASE_DIR, 'renders', 'gen')
        t0 = time.perf_counter()
        paths = render_mod.render_bodies(bodies, args.variant, render_dir)
        print('render_s', time.perf_counter() - t0)
        print(json.dumps(paths, indent=2))

    print('total_cycle_s', time.perf_counter() - t_all0)


if __name__ == '__main__':
    main()
