"""USB-C tunnel + liner -- build123d port of firefly_case.py:3717
add_usb_tunnel."""
from .. import geometry as geo


def add_usb_tunnel(bodies, p):
    wall_y = p['spine_b'][1] + p['outer_radius']
    cx, cz = 0.0, p['usb_tunnel_center_z']
    L_in, W_in = p['usb_tunnel_stadium']
    L_out, W_out = p['usb_liner_outer_stadium']
    y_start = p['usb_tunnel_y_start']

    bore_depth = (wall_y - y_start) + 1.0
    bore = geo.oriented_stadium_prism((cx, y_start, cz), (1, 0, 0), (0, 0, 1), (0, 1, 0),
                                       L_in, W_in, bore_depth)
    bodies['Top'] = bodies['Top'] - bore

    margin = 3.0
    liner_depth = (wall_y - y_start) + margin
    liner_outer = geo.oriented_stadium_prism((cx, y_start, cz), (1, 0, 0), (0, 0, 1), (0, 1, 0),
                                              L_out, W_out, liner_depth)
    liner_inner = geo.oriented_stadium_prism((cx, y_start - 0.5, cz), (1, 0, 0), (0, 0, 1), (0, 1, 0),
                                              L_in, W_in, liner_depth + 1.0)
    liner = liner_outer - liner_inner
    envelope = geo.build_outer_pill_solid(p)
    liner = liner & envelope
    bodies['Top'] = bodies['Top'] + liner
    return bodies
