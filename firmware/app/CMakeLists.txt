# app — LVGL screens: render core state, forward input. No domain logic.
#
# Skeleton only (S13/S14 walking skeleton). Screens arrive with S06+;
# targets/sim currently renders its own boot placeholder directly.
# This INTERFACE target reserves the module's place in the superbuild.

add_library(ff-app INTERFACE)
