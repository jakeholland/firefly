/**
 * ctl_loop.h — S16 slice d: `ffsim --headless --ctl PORT`'s live session,
 * extracted out of main.c.
 *
 * Before this slice, `ff_run_ctl_loop` in main.c both OWNED this state
 * and was the only thing that could exercise it — proving the ctl
 * socket's command-processing layer actually drives a live shell
 * (AC10's sequence test) meant either duplicating ~150 lines of setup
 * here a second time in test code, or spinning up a real ffsim
 * subprocess and a real TCP client (this repo's e2e tier, which needs
 * docker and is deliberately skip-clean without it — not a fit for a
 * `ctest`-gated acceptance criterion). Extracted instead: main.c's ctl
 * loop and `targets/sim/tests/test_ctl_flare_sequence.c` both drive the
 * exact same `ff_ctl_loop_*` functions, in-process, over
 * `ff_ctl_process_line` — the same "pure, socket-free core" layer
 * `ctl_server.h` was already designed to make testable this way.
 *
 * Render lifecycle (S16 slice d, closes #17/#29): `ff_ctl_loop_pump`
 * only rebuilds the LVGL tree on a DIRTY `ff_shell_tick` — the first
 * tick always counts — and always `lv_obj_clean()`s the active screen
 * first. That is what makes issue #17's static line-point pool safe
 * under repeated builds: every `lv_line`/triangle-descriptor object from
 * the PREVIOUS build is deleted before the pool's index resets, so
 * nothing still-alive ever references overwritten points. See
 * `app/screens/scr_radar.c`'s and `scr_flare.c`'s point-pool comments —
 * this file is the "caller" contract those comments now depend on.
 */
#ifndef FF_CTL_LOOP_H
#define FF_CTL_LOOP_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "ctl_server.h"
#include "ff_app_state.h"
#include "ff_shell.h"
#include "live_setup.h"

#include "fp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char const *fixture_path;     /* NULL = boot screen until the first tick rebuilds it */
    bool        mock_clock;
    char const *connect_hostport; /* passed straight through to ff_live_setup_cfg_t */
    char const *pack_path;
    bool        dev_trust_all;
    char const *ctl_out_arg;      /* --ctl-out DIR, or NULL */
    char const *screenshot_dir;   /* --screenshot DIR fallback for --ctl-out, or NULL */
} ff_ctl_loop_cfg_t;

/** Treat every field as private — only ff_ctl_loop_* functions touch
 *  these (same convention as ff_ctl_server_t / mc_client_t). Caller
 *  allocates (a plain local or static, no heap), passes `&it`. */
typedef struct {
    ff_shell_t *shell; /* caller-owned storage; must outlive the ctx */
    fp_pack_t  *pack;  /* caller-owned storage; NULL = no pack support */

    lv_display_t *disp;
    uint8_t      *xrgb_buf;
    int32_t       w, h;

    lv_indev_t       *pointer_indev;
    lv_point_t        pointer_point;
    lv_indev_state_t  pointer_state;

    bool     mock_clock;
    uint32_t mock_clock_ms;
    ff_clock_t clock;

    ff_live_setup_t live;

    /* Mirrors ff_shell_view() after every ff_ctl_loop_pump — what the
     * `state`/`screenshot` ctl handlers read. */
    ff_app_state_t state;

    /* Whether the active screen has been built at least once (so
     * ff_ctl_loop_pump knows to build-not-clean on its very first call —
     * lv_obj_clean on a screen nothing has ever populated is harmless,
     * but this makes the "build once, then update in place" shape
     * explicit rather than relying on that harmlessness). */
    bool has_screen;

    char ctl_out_dir_real[4096]; /* PATH_MAX-sized; see ctl_out_path.h */
} ff_ctl_loop_ctx_t;

/**
 * ff_ctl_loop_open — brings a headless ctl session up: `lv_init()` with
 * this module's own tick callback (`ff_ctl_loop_tick_cb`), an offscreen
 * full-frame buffer sized `FF_CTL_LOOP_W x FF_CTL_LOOP_H`, a synthetic
 * pointer indev (what the ctl socket's `tap`/`swipe` commands drive),
 * `ff_live_setup` (transport/dev-trust-all/pack/heading), `--ctl-out`
 * resolution, and the FIRST screen build. Binds the intent seam to
 * `shell` (`ff_intent_emit_bind(ff_shell_intent_sink, shell)`) — the
 * caller must unbind (`ff_intent_emit_bind(NULL, NULL)`) before the
 * shell goes away, per `ff_intent.h`'s LIFETIME contract;
 * `ff_ctl_loop_close` does this for you.
 *
 * `shell`/`pack` are caller-owned storage (static, or a host stack in
 * tests) that must outlive `*ctx`. `shell_cfg` must already have
 * `clock`/`store`/`haptic`/`haptic_user` filled in by the caller — this
 * function fills in `transport`/`pack` and calls `ff_shell_init` itself
 * (see `ff_live_setup`).
 *
 * Only ONE ctx may be open at a time per process: `lv_tick_set_cb`
 * carries no user pointer, so `ff_ctl_loop_tick_cb` reads a single
 * process-global (mirrors the pre-extraction `g_loop_ctx` constraint).
 *
 * Returns 0 on success (diagnostic already printed to stderr on
 * failure, same convention `ff_live_setup` uses).
 */
int ff_ctl_loop_open(ff_ctl_loop_ctx_t *ctx, ff_shell_t *shell, fp_pack_t *pack, ff_shell_cfg_t *shell_cfg,
                      ff_ctl_loop_cfg_t const *cfg);

/** ff_ctl_loop_tick_cb — this session's `lv_tick_get_cb_t`: real
 *  monotonic milliseconds, or the frozen/advanced mock clock under
 *  `--mock-clock`. Exposed (not just installed internally) because
 *  `ff_ctl_loop_pump` also uses it to read "now" for `ff_shell_tick`. */
uint32_t ff_ctl_loop_tick_cb(void);

/**
 * ff_ctl_loop_handlers — the `ff_ctl_handlers_t` wired to `*ctx`:
 * tap/swipe/hold/clock/state/screenshot/flare/wall/quit. `quit_flag`, if non-NULL,
 * is set true the moment a "quit" command is fully processed — exposed
 * because a caller driving `ff_ctl_process_line` directly (a test) has
 * no `ff_ctl_poll` return value to read that from; main.c's real loop
 * can pass NULL and rely on `ff_ctl_poll`'s own return instead.
 */
ff_ctl_handlers_t ff_ctl_loop_handlers(ff_ctl_loop_ctx_t *ctx, bool *quit_flag);

/**
 * ff_ctl_loop_pump — one tick of the live session: `ff_shell_tick` at
 * the current clock reading, mirror the projection into `ctx->state`,
 * and — ONLY when that tick was dirty (S16 slice d) — `lv_obj_clean()`
 * the active screen and rebuild it (`ff_build_face_screen`). Does NOT
 * call `lv_timer_handler()` or poll the ctl socket; the caller does both
 * (main.c's real loop, or a test driving the sequence by hand) so this
 * function stays a single, testable unit: tick-and-maybe-rebuild.
 */
void ff_ctl_loop_pump(ff_ctl_loop_ctx_t *ctx);

/** ff_ctl_loop_close — unbinds the intent seam, tears down what
 *  ff_live_setup opened, and frees the framebuffer. Does NOT call
 *  `ff_shell_close`/`lv_deinit` — the caller owns those, same as every
 *  other layer here. Safe on a `*ctx` that ff_ctl_loop_open never
 *  successfully populated. */
void ff_ctl_loop_close(ff_ctl_loop_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FF_CTL_LOOP_H */
