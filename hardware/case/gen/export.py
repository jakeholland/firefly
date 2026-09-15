"""STL/STEP/3MF export -- build123d's own export_stl/export_step/Mesher
handle all three natively, unlike the Fusion generator which needs a
separate STL-export API call plus tools/stl_to_3mf.py's own conversion
pass for 3MF (see docs/hardware/cad-tooling-spike.md section 1)."""
import os

import build123d as bd


def export_stl(solid, path, linear_deflection=0.05, angular_deflection=0.3):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    bd.export_stl(solid, path, tolerance=linear_deflection, angular_tolerance=angular_deflection)


def export_step(solid, path):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    bd.export_step(solid, path)


def export_3mf(bodies_by_name, path, linear_deflection=0.05, angular_deflection=0.3):
    """Pack every named body into one native 3MF, mirroring
    export_native_3mf_case's own "one file, per-object structure" shape."""
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    m = bd.Mesher()
    for name, solid in bodies_by_name.items():
        m.add_shape(solid, linear_deflection=linear_deflection, angular_deflection=angular_deflection)
    m.write(path)


def export_all(bodies, variant, base_dir):
    """Write one STL per body plus a packed 3MF, mirroring export_stls'
    own per-variant directory layout."""
    out_dir = os.path.join(base_dir, variant)
    paths = {}
    for name, solid in bodies.items():
        stl_path = os.path.join(out_dir, f'{name}.stl')
        export_stl(solid, stl_path)
        paths[name] = stl_path
    pack_path = os.path.join(out_dir, f'firefly_{variant}_pack.3mf')
    export_3mf(bodies, pack_path)
    paths['pack_3mf'] = pack_path
    return paths
