"""Headless PNG review renders -- plain trimesh + matplotlib (Agg
backend), per the spike's own finding (docs/hardware/cad-tooling-spike.md
section 1): sufficient for a static review render, no ocp_vscode/
ocp-tessellate dependency needed."""
import os

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Poly3DCollection  # noqa: E402
import trimesh  # noqa: E402

from . import export as export_mod


def render_stl(stl_path, out_png, elev=25, azim=-60, title=None):
    mesh = trimesh.load(stl_path)
    fig = plt.figure(figsize=(6, 6))
    ax = fig.add_subplot(111, projection='3d')
    poly = Poly3DCollection(mesh.triangles, alpha=0.9)
    poly.set_facecolor((0.75, 0.78, 0.82, 1.0))
    poly.set_edgecolor((0.2, 0.2, 0.2, 0.15))
    ax.add_collection3d(poly)
    bounds = mesh.bounds
    span = (bounds[1] - bounds[0]).max() / 2.0
    center = (bounds[0] + bounds[1]) / 2.0
    ax.set_xlim(center[0] - span, center[0] + span)
    ax.set_ylim(center[1] - span, center[1] + span)
    ax.set_zlim(center[2] - span, center[2] + span)
    ax.set_box_aspect((1, 1, 1))
    ax.view_init(elev=elev, azim=azim)
    ax.set_axis_off()
    if title:
        ax.set_title(title)
    os.makedirs(os.path.dirname(out_png) or '.', exist_ok=True)
    fig.savefig(out_png, dpi=150, bbox_inches='tight')
    plt.close(fig)
    return out_png


def render_bodies(bodies, variant, out_dir):
    """Export each body to a scratch STL (if not already on disk) and
    render one isometric PNG per body."""
    paths = {}
    for name, solid in bodies.items():
        stl_path = os.path.join(out_dir, f'{name}.stl')
        export_mod.export_stl(solid, stl_path)
        png_path = os.path.join(out_dir, f'{variant}_{name}_iso.png')
        render_stl(stl_path, png_path, title=f'{variant} {name}')
        paths[name] = png_path
    return paths
