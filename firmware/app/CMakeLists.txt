# app — LVGL screens: render core state, forward input. No domain logic.
#
# Skeleton only (S13/S14 walking skeleton). Screens arrive with S06+;
# targets/sim currently renders its own boot placeholder directly.
# This INTERFACE target reserves the module's place in the superbuild.

add_library(ff-app INTERFACE)

# S13b
# Publishes ff_app_state.h — the plain view-state struct screens will
# render (docs/ARCHITECTURE.md principle 3: "the UI is a projection").
# Header-only: no .c sources belong here until a real screen lands (no
# domain logic allowed in app/, per CLAUDE.md).
target_include_directories(ff-app INTERFACE include)
