/**
 * live_setup.h — S16 slice d: the shared "bring a live ff_shell_t up"
 * sequence.
 *
 * Before this slice, exactly one place in this file's history
 * (`ff_run_ctl_loop`) ever opened a `--connect` transport, applied
 * `--dev-trust-all` (including its host wall pre-latch), loaded `--pack`
 * and set the sim's fixed north-heading placeholder. Slice d adds a
 * SECOND live entry point — a live SDL window mode, `ff_run_window` in
 * main.c, now that build-once/update-in-place rendering (closing #17/#29)
 * makes re-rendering on a tick safe — and two call sites doing the exact
 * same seven-step dance is exactly the shape that drifts (one gets a
 * fix, the other doesn't, silently, until someone diffs them). Extracted
 * here so there is one implementation instead of two.
 *
 * Everything else about a live session (the LVGL display/buffer, the ctl
 * socket's handlers, an SDL window's real mouse indev) is genuinely
 * different between the two callers and stays where it is.
 */
#ifndef FF_LIVE_SETUP_H
#define FF_LIVE_SETUP_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_shell.h"
#include "mc_transport_tcp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char const *connect_hostport; /* "HOST:PORT", or NULL for no transport (the
                                    * documented ff_shell_cfg_t.transport test
                                    * seam — events only arrive via
                                    * ff_shell_events()/the ctl `flare` command) */
    char const *pack_path;        /* festpack JSON file to load, or NULL */
    bool dev_trust_all;           /* S16 AC6 — sim-only; see ff_shell.h's
                                    * dev-affordances section. Meaningless (and
                                    * ignored) without connect_hostport, same as
                                    * today's documented behavior. */
} ff_live_setup_cfg_t;

typedef struct {
    bool     tcp_open;
    mc_tcp_t tcp;

    /* PR #56 review F1's provenance bit, moved here verbatim: whether
     * THIS PROCESS offered the host clock to the wall latch
     * (--dev-trust-all does, at startup) — ff_wall_src_t alone can't
     * carry that distinction. See ff_shell.h's ff_shell_dev_wall_observe
     * and main.c's original ff_loop_state_json for the full reasoning. */
    bool wall_host_observed;
} ff_live_setup_t;

/**
 * ff_live_setup — opens `cfg->connect_hostport` (if given), calls
 * `ff_shell_init(shell, shell_cfg)` with `shell_cfg->transport` filled in
 * from that connection, then applies `--dev-trust-all` (host wall
 * pre-latch included), loads `cfg->pack_path` into `shell_cfg->pack`, and
 * sets the sim's fixed north-heading placeholder (no compass on this
 * target; see targets/sim/main.c's original comment on why NOT setting
 * it would make the dev radar loop useless).
 *
 * `shell_cfg` must already have `clock`/`store`/`haptic`/`haptic_user`/
 * `pack` filled in by the caller (this function only sets `transport`);
 * everything `shell_cfg` and `shell` point to must outlive the shell, per
 * `ff_shell_cfg_t`'s own contract.
 *
 * Returns 0 on success. On failure, prints its own diagnostic to stderr
 * (the same convention every call site this replaces already used) and
 * returns negative; `*out` is valid either way — `tcp_open` is only ever
 * true once a connection actually succeeded, so `ff_live_setup_close` is
 * always safe to call.
 */
int ff_live_setup(ff_shell_t *shell, ff_shell_cfg_t *shell_cfg, ff_live_setup_cfg_t const *cfg,
                   ff_live_setup_t *out);

/** ff_live_setup_close — closes the TCP transport ff_live_setup opened,
 *  if any. Does NOT call ff_shell_close: the shell's lifetime is the
 *  caller's, exactly as every other ff_shell_t consumer in this repo
 *  works. Safe on an `*out` that was zero-initialised or that
 *  ff_live_setup never successfully populated. */
void ff_live_setup_close(ff_live_setup_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FF_LIVE_SETUP_H */
