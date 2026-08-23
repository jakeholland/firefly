# app — LVGL screens: render core state, forward input. No domain logic.
#
# ff-app itself stays INTERFACE (header-only: ff_app_state.h, no domain
# logic per CLAUDE.md). Real screens are a separate library, ff-app-ui
# (added under the # S06b marker below) — see that section for why.

add_library(ff-app INTERFACE)

# S13b
# Publishes ff_app_state.h — the plain view-state struct screens will
# render (docs/ARCHITECTURE.md principle 3: "the UI is a projection").
# Header-only: no .c sources belong here until a real screen lands (no
# domain logic allowed in app/, per CLAUDE.md).
target_include_directories(ff-app INTERFACE include)

# S06 — ff_app_state.h's `radar` field is the real core/include/ff_radar.h
# ff_radar_view_t (DRIFT GUARD resolution, see that header's comment), so
# anything consuming ff_app_state.h now needs ff-core's include path too.
# INTERFACE, not PRIVATE: ff-app itself has no sources to compile, only
# this header to propagate.
target_link_libraries(ff-app INTERFACE ff-core)

# S06b (app/theme, app/screens — shell/nav + radar-face rendering;
# docs/specs/S06-radar-face.md slices b/c/d)
# ---------------------------------------------------------------------------
# The first real screen code in this repo: a genuine STATIC library (not
# header-only like ff-app above), since scr_nav.c/scr_radar.c are .c
# sources that need compiling and linking, not just a header to publish.
# ---------------------------------------------------------------------------
add_library(ff-app-ui STATIC
    screens/scr_radar.c
    screens/scr_nav.c
)
target_include_directories(ff-app-ui PUBLIC theme screens)
target_link_libraries(ff-app-ui PUBLIC ff-app lvgl::lvgl)
# scr_radar.c's arrow/dot trigonometry (sinf/cosf) needs libm — same
# rationale as ff-core's CMakeLists.txt note on why this is missed
# locally (AppleClang resolves it via libSystem) but required on
# glibc/Linux CI.
if(UNIX)
    target_link_libraries(ff-app-ui PRIVATE m)
endif()
ff_apply_warnings(ff-app-ui)
