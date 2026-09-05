"""Firefly case parameters — 'current' variant (60 x 110 x 25, matches Firefly V2 v15/v16)."""

PARAMS = {
    'variant': 'current',
    'l76k_mode': 'wired',   # 'hat' | 'wired'

    # --- envelope / spine ---
    'spine_a': (0.0, 0.0),
    'spine_b': (0.0, 50.0),
    'outer_radius': 30.0,
    'wall': 2.0,
    'split_z': 10.0,
    'top_z': 25.0,
    'bottom_z': 0.0,

    # --- outer edge profile (shared top/bottom, mirrored) ---
    'flat_rho': 24.14,          # flat bed extends to this radius
    'shoulder_angle_deg': 45.0,
    'fillet_r': 10.0,           # outer fillet radius
    'fillet_center_rho': 20.0,  # fillet center offset from spine
    'top_fillet_center_z': 15.0,
    'bottom_fillet_center_z': 10.0,
    # derived tangent point: rho=27.07, dz=7.07 from fillet center (see SPEC)

    # --- inner cavity ---
    'inner_fillet_r': 8.0,      # nominal (result of 2mm shell offset of R10)

    # --- window ---
    'window_dia': 45.30,
    'window_center': (0.0, 50.0),
    'window_z_bottom': 22.4,
    'window_chamfer': 0.5,
    'window_chamfer_deg': 45.0,

    # --- display module (Waveshare ESP32-S3-Touch-LCD-1.46) ---
    'display_doc_name': 'ESP32-S3-Touch-LCD-1_46',
    'display_glass_dia': 44.79,
    'display_bbox': {'x': (-22.39, 22.39), 'y': (27.6, 73.13), 'z': (20.3, 25.0)},
    'display_pcb': {'xy': (39.2, 41.4), 'z': (17.59, 18.81)},
    'display_underside_z_min': 12.04,
    'display_header': {'x': (11.5, 17.0), 'y': (43.7, 56.1)},
    'top_ceiling_underside_z': 23.0,
    'fpc_relief': {'x': (-6.2, 7.02), 'y': (71.44, 73.12), 'z': (21.83, 22.93)},

    # --- case screws (Bottom -> Top), M2 socket head ---
    'screw_head_dia': 3.8,
    'screw_head_h': 2.0,
    'screws_ABC': [
        {'name': 'A', 'xy': (-22.97, 25.04)},
        {'name': 'B', 'xy': (0.0, -24.0)},
        {'name': 'C', 'xy': (23.74, 25.2)},
    ],
    'boss_dia': 6.0,
    'screw_hole_dia': 2.4,
    'counterbore_ABC_dia': 4.5,
    'counterbore_ABC_h': 2.2,
    'top_pilot_dia': 1.62,
    'top_pilot_z': (10.0, 19.1),   # -> M2x12

    'screw_D': {'name': 'D', 'xy': (0.0, 65.0)},
    'counterbore_D_h': 4.0,
    'plate_post_D_z': (10.0, 13.1),  # post on Screen Plate, hole Ø1.62 -> M2x10
    'usb_shell_z': 14.35,             # screw tip must stay <= 14.1

    # --- alignment lip / anchor (on Top) ---
    'lip_r': (26.95, 27.75),
    'lip_z': (9.2, 10.0),
    'anchor_r': (26.95, 28.40),
    'anchor_z': (10.0, 11.0),
    'boss_relief_dia': 6.6,
    'lug_relief_box': {'x': (-5.6, 5.6), 'y': (-29.5, -26.0)},

    # --- screen plate ---
    'plate_z': (13.1, 14.1),
    'plate_outline': {'x': (-26.63, 22.89), 'y': (28.8, 69.6)},
    'plate_header_cutout': {'x': (11.5 - 1.0, 17.0 + 1.0), 'y': (43.7 - 1.0, 56.1 + 1.0)},
    'plate_hole_dia': 2.4,
    'plate_pad_dia': 6.0,
    'top_posts': {
        'P1': (-23.63, 58.84),
        'P2': (-17.45, 31.8),
        'P3': (17.0, 32.0),
        'P4': (19.89, 65.47),
    },
    'board_standoffs': {
        'S1': (-12.0, 65.0),
        'S2': (0.04, 32.22),
        'S3': (11.6, 65.46),
    },
    'top_post_dia': 4.0,
    'top_post_z': (14.1, 23.0),
    'top_post_pilot_dia': 1.62,
    'top_post_pilot_z': (14.1, 20.6),  # -> M2x6

    # --- buttons ---
    'switch_power_bbox': {'x': (-17.46, -11.83), 'y': (36.82, 42.67), 'z': (15.79, 18.01)},
    'switch_home_bbox': {'x': (-17.87, -12.24), 'y': (57.62, 63.46), 'z': (999, 999)},  # z not given; use power's span
    'nub_protrusion': 1.2,
    'power_nub_dir': (-0.8, -0.6),
    'home_nub_dir': (-0.84, 0.54),
    'power_cap': {'stadium': (10.0, 5.8), 'z': (13.8, 19.6), 'proud': 0.45, 'y_center': 29.7},
    'home_cap': {'stadium': (8.94, 6.6), 'z': (13.4, 20.0), 'proud': 0.45},
    'wall_hole_clearance': 0.25,
    'plunger_tip_gap': 0.02,
    'nub_pocket': {'xy': (1.3, 1.6), 'depth': 0.8},
    'tab': {'w': 2.9, 'h': 2.0, 'gap': 0.60},
    # wall_x (the -x outer wall, where the buttons live) is derived at build
    # time as -PARAMS['outer_radius'] -- not duplicated here so it can never
    # drift out of sync between variants.

    # --- USB-C tunnel ---
    'usb_receptacle': {'x': (-4.48, 4.48), 'z': (14.35, 18.43), 'y': 73.0},
    'usb_tunnel_stadium': (13.0, 7.0),
    'usb_tunnel_center_z': 16.4,
    'usb_tunnel_y_start': 73.5,
    'usb_liner_thickness': 1.6,
    'usb_liner_outer_stadium': (16.2, 10.2),

    # --- lanyard lug (Bottom) ---
    'lug': {
        'x': (-5.0, 5.0), 'y_root': -26.5, 'y_tip': -38.5, 'tip_r': 5.0,
        'z': (3.0, 10.0), 'hole_dia': 3.5, 'hole_xy': (0.0, -33.5),
    },

    # --- logos (debossed 0.4mm) ---
    'logo_deboss_depth': 0.4,
    'flare_center': (0.0, 2.0),
    'flare': {
        'center_dia': 3.0, 'long_ray': 7.0, 'short_ray': 4.5,
        'bar_start_r': 2.2, 'bar_w': (1.4, 0.5),
    },
    'wordmark_center': (0.0, 25.0),
    'wordmark_width': 30.0,

    # --- comms bay (current variant) ---
    'bay': {
        'cavity_x': (-28.0, 28.0), 'cavity_y': (-28.0, 27.0), 'cavity_z': (2.0, 23.0),
        'battery': {'xyz': (8.0, 30.0, 40.0), 'x': (-27.0, 3.0), 'y': (-25.0, 15.0), 'z': (2.0, 10.0)},
        'gps_patch': {'xyz': (25.0, 25.0, 8.3), 'x': (-26.0, -1.0), 'y': (-24.0, 1.0), 'z': (10.0, 18.3)},
        'gps_frame_wall': 1.0, 'gps_opening': 25.5,
        'stack': {'x': (5.0, 22.8), 'y': (-24.0, -1.7)},
        'stack_pad_dia': 3.0, 'stack_pad_h': 1.0, 'stack_frame_clear': 0.3,
        'l76k_wired': {'x': (5.0, 23.0), 'y': (1.0, 22.0), 'z': (2.0, 6.0)},
        'fpc_keepout': {'x': (-15.0, 15.0), 'z': (3.0, 9.0)},  # on -y end wall, construction only
        'wire_channel': {'x': (11.5, 17.0), 'y': (43.7, 56.0), 'width': 4.0},
    },

    'board_docs': {
        'xiao': 'XIAO-ESP32S3 v3',
        'wio': 'Wio-SX1262_for_XIAO_3D_file V2',
        'l76k': 'L76K GNSS Module for  XIAO v1',
    },

    'clearance_min': 0.3,
}
