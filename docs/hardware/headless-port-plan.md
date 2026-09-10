# Headless case generator port plan

Tracks the feature-by-feature port of `hardware/case/firefly_case.py`
(Fusion-via-MCP) to `hardware/case/gen/` (headless
[build123d](https://github.com/gumyr/build123d)), per
`docs/hardware/cad-tooling-spike.md`'s recommendation and migration plan.
Every function in the source generator is listed below with its port
status so a later phase can pick up exactly where this one left off.

**Source generator note:** this port targets the **case-pass16** branch's
`firefly_case.py`/`params_current.py`/`params_trim.py` (5588a2b), not
`origin/main`'s own (pre-pass-16) copy -- the phase-1 brief's own
architecture (single-pilot corner blocks at A/C/D, D relocated to
(18, 58), ears/S2-boss deferred to phase 2) is pass-16's "candidate 5"
display-mount design, which has not merged to `main` yet. `params_
current.py`/`params_trim.py`/`tools/offline_stl_check.py` were pulled
forward from `case-pass16` into this branch (pure data + a pure-Python
tool, no Fusion dependency in either) so `gen/params.py` can import them
unchanged, per the port brief. `firefly_case.py` itself (the Fusion
generator) is untouched -- it stays the historical/legacy driver; `gen/`
is the new, parallel package.

## Status legend

- **ported** -- a build123d equivalent exists in `gen/` and is exercised
  by `gen/cli.py build`.
- **deleted-as-quirk** -- a Fusion-API/timeline workaround with no OCC
  equivalent needed (see `gen/geometry.py`'s own module docstring and
  the spike's "19.4% pure Fusion-quirk workaround" finding). Not
  translated, not planned.
- **deferred** -- a real feature, out of phase-1's scope, tracked for
  phase 2 (ears, S2 boss, buttons, comms stack/GPS/battery/compass) or
  phase 3 (wordmark, coupons, packed exports, min-clearance checks that
  need board occurrences placed).
- **todo** -- a straightforward gate/helper not yet ported, with no
  phase-2/3 feature dependency blocking it; a good pickup for whoever
  continues this work, in no particular priority order beyond what's
  noted per row.

## Phase 1 (this PR): what actually got built

`gen/shell.py` -- Top/Bottom shell (pill outline, R8 ceiling fillet, 2mm
wall, both variants), the window bore + pass-15 print-orientation cone
chamfer + pass-16 glass-seat chamfer, and the pass-16 continuous-taper
lip/anchor ring (with its own per-boss/lug reliefs). `gen/features/
corner_blocks.py` -- case screws A/C/D: Bottom boss + counterbore, Top
single-pilot wall-anchored corner block, the unconditional 45-degree
root-reinforcement collar (with pass-16's wedge-overlap and trim-then-
join fixes) on every one of the 6 boss/block roots. `gen/features/
usb_tunnel.py`, `gen/features/fpc_relief.py`, `gen/features/lug.py` --
the USB-C tunnel + liner, the FPC relief pocket (skin-safe clip + SPEC
box, with its own build-time clearance assert), and the lanyard lug
(with its own best-effort corner/root fillets and hole chamfer).
`gen/components.py` -- STEP import + empirically-derived placement of
the display module (see its own module docstring); `gen/gates.py` --
`verify_post_walls`/`verify_root_fillets`/`verify_corner_blocks`/
`verify_bottom_openings`/`verify_lip_ring_profile` plus a generic
all-pairs interference check, all against live OCC solids or the
exported STL, wired to `tools/offline_stl_check.py` reused unchanged.
`gen/export.py`/`gen/render.py`/`gen/cli.py` round out the driver. See
`docs/hardware/headless-port-parity.md` for the regression numbers this
produced against the pass-16 goldens.

## Phase 2 plan

In roughly the order `firefly_case.py` itself builds them:

1. ~~**Ears + S2 boss**~~ **DONE, this revision** (`features/ears.py`:
   `add_ear`, `add_s2_boss`, `ear_root_cap_z1`, `_ear_wedge_wall_
   touch_z1`; `components.py`: `battery_connector_world_bbox`,
   `secondary_conn_world_bbox`, `ceiling_safe_display_cut`,
   `apply_known_component_keepouts`; `gates.py`: `verify_seat_heights`,
   `verify_ear_root_material`, `verify_s2_boss_clearance`,
   `verify_display_to_stack_clearance`, `check_display_interference_
   near_ears`) -- the display mount proper, targeting the real measured
   standoff plane/XY (`components.measure_standoffs`), not the typed
   `board_standoffs`/`ear_seat_z`. Found and fixed three real bugs (a
   gate sign error, a genuine Top-vs-Bottom interference at `split_z`,
   and ~600mm³ of real display interference from two not-yet-ported
   inline cuts in the source's own `build()`) -- see
   `docs/hardware/headless-port-parity.md`'s own "Phase 2 update"
   section for the full account, including the two known, narrow,
   live-found trade-offs (`corner_block_D_top`, `S3_riser_solid`) still
   open on `trim`, and `current`'s several additional open findings
   (not root-caused this pass, `current` is not the printed variant).
2. **Buttons** (`button_geometry`, `add_button`, `add_buttons`, the
   guide-rib/collar mechanism) -- needs the real switch body position;
   plan to import a switch reference STEP or keep the existing
   params-derived `switch_power_bbox`/`switch_home_bbox` boxes as a
   stand-in the way this phase-1 build already does for the display's
   own sub-components it doesn't yet probe individually.
3. **Comms stack / GPS frame / battery bay** (`add_comms_bay` and its
   `add_battery_bay`/`add_comms_stack_frame`/`build_gps_frame_body`
   dependents) -- the L76K/XIAO/Wio STEP files are already staged at
   `hardware/models/` for this; `components.py`'s STEP-import + shape-
   search pattern generalizes directly.
4. **Compass module** -- per the port brief, model as a plain box from
   params for now (the `.f3d` reference is not headlessly importable);
   `mag_module_fits`/`mag_*_world_*` port as pure analytic functions
   first, the box geometry itself is a small addition to `gen/features/`.
5. **Antenna channels** -- once the comms stack/GPS frame exist to route
   between.
6. Re-run the full parity harness (`docs/hardware/headless-port-parity.md`)
   after each feature lands, same methodology as phase 1.

## Phase 3 plan (after phase 2)

Wordmark/logo deboss (`deboss_loops` and the whole `_wordmark_*`/
`load_wordmark_loops`/`add_wordmark_logo` family, reading
`kandiwooks_logo.json` unchanged), button coupons (`build_button_coupon`,
`export_coupons`, the pass-16 mount coupon), the remaining `todo`-status
analytic gates below (`verify_m1_probe_table`, `verify_envelope`,
`verify_export_envelope`, `verify_wall_integrity`, `verify_openings_open`,
`assert_export_body_size`), and `verify_min_clearances`/
`verify_stack3_clearance`/`verify_display_to_stack_clearance` once the
comms boards are actually placed (phase 2, item 3) rather than just
referenced by bbox.

## Full function table (case-pass16 `firefly_case.py`, 217 functions)

69 ported, 51 deleted-as-quirk, 75 deferred, 22 todo (phase 2, this
revision: +8 -- `ear_root_cap_z1`, `add_ear`, `add_s2_boss`,
`battery_connector_world_bbox`, `secondary_conn_world_bbox`,
`verify_ear_root_material`, `verify_s2_boss_clearance`,
`verify_display_to_stack_clearance`).

| firefly_case.py line | function | status | port note |
|---|---|---|---|
| 82 | `P` | deleted-as-quirk | Fusion cm<->mm Point3D helper -- build123d takes plain mm tuples |
| 86 | `V` | deleted-as-quirk | Fusion cm<->mm ValueInput helper -- unneeded, OCP works in mm directly |
| 90 | `new_sketch` | deleted-as-quirk | Fusion sketch-on-plane boilerplate -- build123d's Line/ThreePointArc take plain 3D points |
| 94 | `_sk` | deleted-as-quirk | Fusion sketch-point helper, same reason as new_sketch |
| 101 | `add_line` | deleted-as-quirk | replaced by bd.Line(...) directly |
| 105 | `add_arc3` | deleted-as-quirk | replaced by bd.ThreePointArc(...) directly |
| 109 | `extrude_new_body` | deleted-as-quirk | replaced by bd.extrude(...) directly |
| 118 | `revolve_new_body` | deleted-as-quirk | replaced by bd.revolve(...) directly |
| 126 | `combine_join` | deleted-as-quirk | replaced by Python's + operator on OCC solids |
| 130 | `combine_cut` | deleted-as-quirk | replaced by Python's - operator |
| 134 | `combine_cut_keep` | deleted-as-quirk | no Fusion isKeepToolBodies flag needed -- OCC operators never consume operands |
| 149 | `combine_intersect` | deleted-as-quirk | replaced by Python's & operator |
| 153 | `_combine` | deleted-as-quirk | shared Fusion CombineFeatureInput builder -- superseded by +/-/& operators |
| 165 | `plane_at_z` | deleted-as-quirk | Fusion named-construction-plane helper -- build123d sketches in free 3D space |
| 172 | `plane_at_x` | deleted-as-quirk | ditto |
| 179 | `plane_at_y` | deleted-as-quirk | ditto |
| 186 | `revolve_taper_wedge` | ported | geometry.py |
| 217 | `extrude_taper_wedge_along_y` | ported | geometry.py |
| 241 | `extrude_taper_cut_along_y` | ported | geometry.py |
| 268 | `build_wedge_along_x` | deferred | phase 2 -- button guide-rib self-supporting ledge |
| 294 | `add_stadium_loop` | ported | folded into geometry.py `_stadium_face` |
| 309 | `stadium_solid` | ported | geometry.py |
| 317 | `stadium_ring_solid` | ported | geometry.py |
| 323 | `cylinder_solid` | ported | geometry.py |
| 333 | `box_solid` | ported | geometry.py |
| 343 | `vadd` | ported | geometry.py `_vadd` |
| 348 | `move_body_to_frame` | deleted-as-quirk | Fusion's Matrix3D canonical-build-then-move step -- build123d builds oriented prisms directly in 3D (see geometry.py module docstring) |
| 378 | `oriented_stadium_loop` | ported | folded into geometry.py `oriented_stadium_prism` |
| 403 | `_cross` | deleted-as-quirk | only existed to recompute move_body_to_frame’s z-axis |
| 407 | `_dot` | deleted-as-quirk | ditto, unused once move_body_to_frame is gone |
| 411 | `oriented_stadium_prism` | ported | geometry.py |
| 425 | `oriented_box_loop` | ported | folded into geometry.py `oriented_box_prism` |
| 439 | `oriented_box_prism` | ported | geometry.py |
| 447 | `bbox_of` | deleted-as-quirk | replaced by build123d's native `.bounding_box()` |
| 456 | `count_sliver_faces` | todo | diagnostic only, not gating -- low priority |
| 474 | `_profile_geometry` | ported | geometry.py `profile_geometry` |
| 503 | `rho_at_z` | ported | geometry.py |
| 520 | `build_outer_half_profile_points` | ported | folded into geometry.py `_outer_half_points` |
| 554 | `sketch_half_profile` | deleted-as-quirk | Fusion sketch wrapper -- folded into geometry.py `_outer_half_face` |
| 568 | `sketch_full_profile` | deleted-as-quirk | folded into geometry.py `_outer_full_face` |
| 598 | `_inner_profile_geometry` | ported | geometry.py `inner_profile_geometry` |
| 625 | `build_inner_half_profile_points` | ported | folded into geometry.py `_inner_half_points` |
| 648 | `sketch_inner_half_profile` | deleted-as-quirk | folded into geometry.py `_inner_half_face` |
| 660 | `sketch_inner_full_profile` | deleted-as-quirk | folded into geometry.py `_inner_full_face` |
| 682 | `build_inner_pill_solid` | ported | geometry.py |
| 707 | `combine_intersect_keep` | deleted-as-quirk | duplicate of combine_intersect_keep at :707 -- see combine_intersect |
| 719 | `build_inner_cavity_clip_tool` | ported | geometry.py |
| 734 | `clip_to_inner_cavity` | ported | inlined as `& build_inner_cavity_clip_tool(p)` at each call site (corner_blocks.py) |
| 747 | `clipped_pillar_with_reach` | ported | kept defensively in corner_blocks.add_case_boss's wide+core pattern -- the Fusion silent-no-op-join this exists for was not reproduced under OCC in phase-1 testing, so the extra core may be redundant here; not re-verified |
| 794 | `build_thickened_envelope` | ported | geometry.py |
| 810 | `build_outer_pill_solid` | ported | geometry.py |
| 851 | `hollow_and_split` | ported | shell.py `build_shells` |
| 893 | `add_lip_anchor_reliefs` | ported | shell.py |
| 1122 | `add_window` | ported | shell.py |
| 1285 | `fpc_relief_footprint` | ported | features/fpc_relief.py |
| 1301 | `add_fpc_relief` | ported | features/fpc_relief.py |
| 1451 | `chamfer_stadium_edge_at` | deleted-as-quirk | superseded by shell.py add_lip_anchor_reliefs pass-16 boolean taper -- the pass-16 design no longer calls this for the ring seam |
| 1515 | `chamfer_edge_at` | ported | inlined as OCC bd.chamfer with geometric edge-selection in features/lug.py's `_best_effort_hole_chamfer` |
| 1596 | `_log_root_fillet` | deleted-as-quirk | ROOT_FILLET_REPORT bookkeeping only, no geometry |
| 1604 | `cone_frustum_solid` | ported | geometry.py |
| 1636 | `add_root_reinforcement` | ported | features/corner_blocks.py |
| 1776 | `_best_effort_fillet_at_z` | todo | GPS/stack-frame cosmetic fillet helper -- port alongside those frames |
| 1802 | `dedupe_body` | deleted-as-quirk | Fusion orphaned-duplicate-body workaround -- not observed in OCC (see geometry.py module docstring) |
| 1968 | `_ear_boss_keepout_points` | deferred | phase 2 -- ears/S2-boss vs. button cut interaction |
| 2041 | `_clip_of_ear_boss_keepout` | deferred | phase 2 |
| 2125 | `_ear_wedge_wall_touch_z1` | deferred | phase 2 -- ear-vs-button height cap |
| 2137 | `_ear_root_z1` | ported | features/corner_blocks.py -- needed by A/C/D’s display keepout, not just ears |
| 2154 | `ear_root_cap_z1` | ported | features/ears.py -- also relies on the new `add_root_reinforcement(..., z_floor=...)` param (phase 2 fix, no source equivalent needed, see headless-port-parity.md) so the collar it caps can never dip below `split_z` |
| 2184 | `_wall_outward_axes` | ported | geometry.py `wall_outward_axes` |
| 2213 | `_nearer_spine_y` | ported | geometry.py |
| 2232 | `_corner_block_ring_limit_r` | ported | features/corner_blocks.py |
| 2293 | `add_single_corner_block` | ported | features/corner_blocks.py |
| 2381 | `add_ear` | ported | features/ears.py -- target xy/seat_z come from `components.measure_standoffs` (measured), not the typed `board_standoffs`/`ear_seat_z`, per the port brief |
| 2536 | `add_s2_boss` | ported | features/ears.py -- same measured-standoff sourcing as add_ear; adds a best-effort underside edge chamfer (no source equivalent, see its own docstring for the honest "still needs support" finding) |
| 2681 | `_refetch_by_name` | deleted-as-quirk | Fusion stale-reference workaround -- OCC's +/-/& return the real result directly, no name-based re-fetch needed |
| 2702 | `add_case_boss` | ported | features/corner_blocks.py |
| 2813 | `add_case_screws` | ported | features/corner_blocks.py |
| 2849 | `battery_connector_world_bbox` | ported | components.py -- used by add_s2_boss's own hard keep-out |
| 2873 | `secondary_conn_world_bbox` | ported | components.py -- used by the new `apply_known_component_keepouts` (see headless-port-parity.md) |
| 2883 | `get_open_doc` | deleted-as-quirk | Fusion multi-document lookup -- no live Fusion session in the headless build |
| 2890 | `get_reference_transform` | deleted-as-quirk | superseded by components.py's empirically-derived transform (see its module docstring) |
| 2901 | `insert_referenced_component` | deleted-as-quirk | Fusion occurrence-insert API |
| 2907 | `insert_display_pcba` | ported | components.py `load_display` -- transform derived empirically, not read live from Fusion (see module docstring) |
| 2930 | `normalize2` | deferred | phase 2 -- button geometry helper |
| 2935 | `ray_box_exit_2d` | deferred | phase 2 -- button geometry helper |
| 2954 | `button_geometry` | deferred | phase 2 -- buttons |
| 3129 | `add_button` | deferred | phase 2 -- buttons |
| 3639 | `add_buttons` | deferred | phase 2 -- buttons |
| 3717 | `add_usb_tunnel` | ported | features/usb_tunnel.py |
| 3747 | `lug_ear_geometry` | ported | features/lug.py |
| 3792 | `add_lug` | ported | features/lug.py |
| 3970 | `_polyline_loop_lines` | deferred | phase 3 -- wordmark |
| 3980 | `_loop_bbox_mm` | deferred | phase 3 |
| 3986 | `_profile_bbox_mm` | deferred | phase 3 |
| 3991 | `_bbox_matches` | deferred | phase 3 |
| 3995 | `deboss_loops` | deferred | phase 3 |
| 4056 | `flare_glyph_loops` | deferred | phase 3 |
| 4083 | `add_flare_logo` | deferred | phase 3 |
| 4119 | `_wordmark_word_raw_loops` | deferred | phase 3 |
| 4131 | `_wordmark_word_loops_by_flag` | deferred | phase 3 |
| 4150 | `_point_in_poly` | deferred | phase 3 |
| 4171 | `_poly_area` | deferred | phase 3 |
| 4181 | `_poly_centroid` | deferred | phase 3 |
| 4187 | `_wordmark_counter_probes` | deferred | phase 3 |
| 4223 | `_wordmark_split_sprout` | deferred | phase 3 |
| 4258 | `_wordmark_local_bbox` | deferred | phase 3 |
| 4264 | `_wordmark_place_word` | deferred | phase 3 |
| 4295 | `wordmark_vertical_span` | deferred | phase 3 |
| 4317 | `wordmark_layout` | deferred | phase 3 |
| 4426 | `load_wordmark_loops` | deferred | phase 3 |
| 4445 | `add_wordmark_logo` | deferred | phase 3 |
| 4462 | `safe_half_width` | deferred | phase 3 |
| 4486 | `clip_box_x_to_cavity` | deferred | phase 3 |
| 4497 | `add_battery_bay` | deferred | phase 2 -- battery |
| 4567 | `add_battery_reference_box` | deferred | phase 2 |
| 4588 | `build_comms_stack_frame` | deferred | phase 2 -- comms stack |
| 4638 | `add_comms_stack_frame` | deferred | phase 2 |
| 4698 | `build_hanging_frame` | deferred | phase 2 -- GPS frame |
| 4752 | `build_gps_frame_body` | deferred | phase 2 |
| 4788 | `add_gps_reference_box` | deferred | phase 2 |
| 4797 | `add_fpc_keepout_marker` | deferred | phase 2 -- reference-only marker |
| 4808 | `add_comms_bay` | deferred | phase 2 -- battery+GPS+stack driver |
| 4849 | `_antenna_skin_safe_channel` | deferred | phase 2 -- antenna channels |
| 4869 | `_best_effort_fillet` | deferred | phase 2/3 -- cosmetic channel fillet |
| 4894 | `add_antenna_channels` | deferred | phase 2 |
| 5030 | `antenna_channel_geometry` | deferred | phase 2 |
| 5062 | `verify_antenna_channels` | deferred | phase 2 |
| 5143 | `mag_world_y` | deferred | phase 2 -- compass module (brief: model as a box from params for now) |
| 5154 | `mag_world_x` | deferred | phase 2 |
| 5160 | `mag_pcb_bottom_world_z` | deferred | phase 2 |
| 5171 | `mag_world_z` | deferred | phase 2 |
| 5180 | `mag_module_clearance` | deferred | phase 2 |
| 5200 | `mag_module_fits` | deferred | phase 2 |
| 5210 | `mag_pcb_world_footprint` | deferred | phase 2 |
| 5227 | `mag_fence_world_footprint` | deferred | phase 2 |
| 5239 | `mag_window_bore_clearance` | deferred | phase 2 |
| 5260 | `mag_peg_world_positions` | deferred | phase 2 |
| 5266 | `mag_pad_world_positions` | deferred | phase 2 |
| 5287 | `mag_header_notch_center_x` | deferred | phase 2 |
| 5296 | `add_mag_module` | deferred | phase 2 |
| 5434 | `verify_mag_pocket` | deferred | phase 2 |
| 5551 | `_collect_occ_bodies` | deleted-as-quirk | Fusion occurrence-tree body walk |
| 5567 | `_safe_visible` | deleted-as-quirk | Fusion visibility-flag guard for the walk above |
| 5580 | `_bbox_extents` | deleted-as-quirk | Fusion occurrence bbox helper, superseded by build123d bounding_box() |
| 5598 | `flatten_transform` | deleted-as-quirk | Fusion Matrix3D construction for board placement |
| 5636 | `find_pcb_like_body` | deleted-as-quirk | Fusion occurrence body-shape search -- components.py finds parts by solid shape directly on the imported STEP, no occurrence tree involved |
| 5668 | `insert_and_place` | deleted-as-quirk | Fusion insert + snapshot placement |
| 5695 | `insert_comms_boards` | deferred | phase 2/3 -- XIAO/Wio/L76K STEP placement, once needed for a gate (components.py already establishes the STEP-measurement pattern to reuse) |
| 5879 | `build` | todo | the CLI driver (cli.py `build()`) is phase 1's own analog; this row tracks the ORIGINAL Fusion build()'s remaining un-ported feature calls, which land as each feature is ported |
| 6119 | `remove_stray_generic_bodies` | deleted-as-quirk | Fusion stray-body cleanup after a live rebuild |
| 6147 | `probe_point_solid` | ported | geometry.py |
| 6153 | `find_outer_x_at` | todo | verify_m1_probe_table helper |
| 6170 | `find_first_solid_x` | todo | unused helper in the source itself beyond find_outer_x_at’s neighbor |
| 6182 | `find_ceiling_z_at` | ported | superseded by components.py's direct solid-shape measurement of the imported STEP (no live downward scan needed -- the geometry is already in memory) |
| 6199 | `check_interference` | ported | gates.py `check_interference_pairs` -- simplified to body-pairs only, no Fusion occurrence-tree walk (board occurrences are not yet inserted, see components.py) |
| 6297 | `inner_rho_at_z` | ported | geometry.py |
| 6310 | `verify_m1_cavity_probes` | todo | M1 cavity probe-table gate -- shell already parity-checked against the golden STL in phase 1; porting this analytic gate is straightforward, just not yet wired |
| 6348 | `envelope_bounds` | todo | verify_envelope helper |
| 6369 | `verify_envelope` | todo | gross envelope bounds check |
| 6384 | `verify_no_outer_bumps` | todo | outer-bump regression probe |
| 6406 | `rho_from_spine` | ported | geometry.py |
| 6418 | `check_body_envelope_vertices` | todo | export-vertex envelope scan |
| 6449 | `verify_export_envelope` | todo | driver for check_body_envelope_vertices |
| 6462 | `verify_m1_probe_table` | todo | SPEC.md profile probe table |
| 6481 | `true_wall_distance_along_ray` | ported | geometry.py |
| 6509 | `find_outermost_s` | todo | ray-scan helper, currently only used by button/wall checks |
| 6522 | `find_innermost_s` | todo | ditto |
| 6539 | `find_switch_body` | deferred | phase 2 -- buttons |
| 6560 | `verify_plunger_reach` | deferred | phase 2 -- buttons |
| 6624 | `verify_button_insertion` | deferred | phase 2 -- buttons |
| 6684 | `verify_button_retention` | deferred | phase 2 -- buttons |
| 6758 | `verify_m2` | todo | dimensional gate -- the USB tunnel/lug portions are covered by phase-1’s own build-time asserts; the button portion is deferred with buttons |
| 6918 | `find_component_occurrence` | deleted-as-quirk | Fusion component-tree walk |
| 6929 | `organize_components` | deleted-as-quirk | Fusion outline-folder organization, no headless analog needed |
| 6983 | `verify_structure` | deleted-as-quirk | checks the Fusion outline-folder organization organize_components builds |
| 7049 | `collect_interference_entities` | deleted-as-quirk | Fusion occurrence-tree walk feeding the live analyzeInterference call |
| 7107 | `verify_min_clearances` | deferred | phase 2/3 -- needs board occurrences placed |
| 7135 | `_rect_perimeter_points` | todo | verify_skin_intact helper, deferred with buttons |
| 7150 | `verify_skin_intact` | deferred | phase 2 -- button tab holes |
| 7246 | `verify_wall_integrity` | todo | general dome-wall probe -- not button-specific, worth porting standalone |
| 7358 | `verify_posts_and_bosses` | deleted-as-quirk | targets the retired P1-P4 Screen-Plate posts (pass 16 removed them) -- superseded by verify_root_fillets/verify_corner_blocks |
| 7431 | `verify_post_walls` | ported | gates.py -- targets screws_D12 (D), same as the pass-16 source |
| 7497 | `verify_root_fillets` | ported | gates.py -- restricted to case-screw bosses A/C/D; ear/S2/mag entries deferred with those features |
| 7585 | `verify_corner_blocks` | ported | gates.py -- A/C/D only |
| 7649 | `verify_bottom_openings` | ported | gates.py -- A/C/D + the lug cord hole |
| 7728 | `probe_bodies_interference_volume` | ported | gates.py `interference_volume` |
| 7758 | `find_display_occurrence` | deleted-as-quirk | Fusion occurrence lookup -- components.py holds the compound directly |
| 7778 | `verify_display_insertion_path` | deferred | phase 2/3 -- diagnostic-only in the source too |
| 7826 | `verify_stack3_clearance` | deferred | phase 2 -- comms stack |
| 7873 | `verify_fpc_relief` | todo | the SPEC-box corner clearance is already a build-time assert in features/fpc_relief.py; the fuller multi-probe gate is not yet a standalone function |
| 7992 | `verify_wordmark` | deferred | phase 3 |
| 8068 | `verify_wordmark_counters` | deferred | phase 3 |
| 8107 | `verify_ear_root_material` | ported | gates.py -- S1/S3 only; one known, live-found, narrow finding (`S3_riser_solid` at 1 of 4 angles), see headless-port-parity.md |
| 8187 | `verify_s2_boss_clearance` | ported | gates.py -- clean both variants |
| 8229 | `verify_seat_heights` | ported | gates.py, superseded in method (not just referenced) by components.py's `measure_standoffs` -- re-derives each barrel's OWN measured local top-z (not the shared constant) and checks the seat gap against it directly, rather than only reproducing the documented 18.80/21.80mm plane number |
| 8300 | `_xy_overlap` | ported | features/corner_blocks.py |
| 8308 | `verify_display_to_stack_clearance` | ported | gates.py -- purely analytic (bay params only); the `stack3`/GPS-patch checks are no-ops until phase 2 item 3 (comms stack) places real geometry, same convention `verify_corner_blocks` already uses |
| 8357 | `window_column_probe_points` | todo | verify_openings_open helper |
| 8376 | `_in_stadium` | todo | ditto |
| 8391 | `verify_openings_open` | todo | general opening/closed regression gate |
| 8648 | `verify` | deleted-as-quirk | Fusion top-level verify driver -- superseded by gates.all_gates |
| 8955 | `build_button_coupon` | deferred | phase 2/3 -- coupons |
| 9058 | `assert_export_body_size` | todo | export size sanity guard -- easy, not yet wired into export.py |
| 9074 | `export_stls` | deleted-as-quirk | Fusion STL-export API call -- superseded by export.py |
| 9091 | `read_stl_triangles` | ported | reused unchanged via tools/offline_stl_check.py (pure Python, needs no porting) |
| 9109 | `_tri_area` | ported | ditto |
| 9116 | `scan_stl_overhangs` | ported | ditto |
| 9224 | `verify_lip_ring_profile` | ported | gates.py, verbatim algorithm |
| 9331 | `export_coupons` | deferred | phase 2/3 |
| 9385 | `build_mount_coupon_point` | deferred | phase 2 -- new, not-yet-built-upstream mount coupon |
| 9483 | `export_mount_coupon` | deferred | phase 2 |
| 9521 | `export_native_3mf_case` | deleted-as-quirk | Fusion-specific 3MF packer -- build123d's Mesher does STL+3MF from one call (export.py) |
| 9557 | `export_native_3mf_coupons` | deferred | phase 2/3 |
| 9569 | `_set_ortho_camera` | deleted-as-quirk | Fusion camera API -- superseded by render.py's trimesh/matplotlib renderer |
| 9581 | `take_orthographic_screenshots` | ported | superseded by render.py's headless trimesh/matplotlib renderer |
| 9606 | `_find_or_create_doc` | deleted-as-quirk | Fusion scratch-document reuse -- no live Fusion session |
| 9616 | `run` | deleted-as-quirk | Fusion top-level entry point -- superseded by cli.py |
