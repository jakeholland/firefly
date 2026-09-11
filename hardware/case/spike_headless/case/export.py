"""STL/3MF export -- build123d's Mesher (Lib3MF-backed) handles both from
one call, unlike Fusion which needs a separate STL-export API call plus
tools/stl_to_3mf.py's own conversion pass for 3MF."""
import build123d as bd


def export_stl(solid, path, linear_deflection=0.05, angular_deflection=0.3):
    bd.export_stl(solid, path, tolerance=linear_deflection, angular_tolerance=angular_deflection)


def export_3mf(solid, path, linear_deflection=0.05, angular_deflection=0.3):
    m = bd.Mesher()
    m.add_shape(solid, linear_deflection=linear_deflection, angular_deflection=angular_deflection)
    m.write(path)
