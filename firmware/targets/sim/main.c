/**
 * ffsim — Firefly desktop sim target (S13 slice a, extended in slices b/c).
 *
 * Modes:
 *   ffsim                          window mode: opens an SDL window,
 *                                  runs LVGL's normal timer loop.
 *   ffsim --headless --screenshot DIR
 *                                  renders exactly one frame into an
 *                                  offscreen LVGL buffer (no SDL, no
 *                                  display server required) and writes
 *                                  it to DIR/boot.png, then exits 0.
 *   ffsim --headless --screenshot DIR --fixture FILE.json
 *                                  same, but loads FILE.json into an
 *                                  ff_app_state_t (fixture.h) and renders
 *                                  it instead of the boot screen, writing
 *                                  DIR/<stem>.png. A fixture whose
 *                                  active_face already has a real screen
 *                                  (radar/now/signals via the shared shell,
 *                                  scr_nav.h; compose as its own full
 *                                  screen) gets that; every other face
 *                                  still gets the S13 placeholder debug
 *                                  face (fixture_view.h) — see
 *                                  face_dispatch.h's ff_build_face_screen.
 *   ffsim --fixture FILE.json      window mode with the fixture loaded
 *                                  (interactive preview; same
 *                                  face-selection and load path as
 *                                  headless).
 *   ffsim --headless --ctl PORT [--fixture FILE.json] [--mock-clock]
 *         [--connect HOST:PORT] [--pack FILE.json] [--ctl-out DIR]
 *         [--dev-trust-all]
 *                                  S13 slice c: opens a persistent,
 *                                  headless control-socket-driven session
 *                                  instead of rendering once and exiting.
 *                                  See ctl_server.h and
 *                                  firmware/tools/dev/CTL.md for the wire
 *                                  protocol. Runs until a `{"cmd":"quit"}`
 *                                  is received. --ctl currently requires
 *                                  --headless (see this file's "ctl loop"
 *                                  section for why). --ctl-out DIR
 *                                  confines the ctl socket's "screenshot"
 *                                  command's writes under DIR (created if
 *                                  missing; defaults to --screenshot's
 *                                  DIR if that was also given, else a
 *                                  fresh temp directory) — see
 *                                  ctl_out_path.h and this file's
 *                                  ff_loop_screenshot.
 *
 * --mock-clock freezes the LVGL tick source for the one-shot headless and
 * window paths (see ff_mock_tick_cb below); ff_run_headless_once()
 * UNCONDITIONALLY calls lv_tick_set_cb(ff_mock_tick_cb) regardless of
 * whether --mock-clock was passed — headless rendering is deterministic
 * either way (lv_refr_now() does NOT skip the tick — it unconditionally
 * calls lv_anim_refr_now() internally, which reads the tick and runs one
 * animation step; this matters since S06's CLOSE-mode radar face starts a
 * real lv_anim_t for its pulsing rings, see app/screens/scr_radar.c — but
 * the unconditional freeze is what actually makes it deterministic, not
 * the flag). The flag is accepted and honored in headless mode purely so
 * callers (tests/run_goldens.sh) can pass it explicitly rather than
 * depending on undocumented default behavior. In --ctl mode --mock-clock
 * instead gates the ctl socket's `{"cmd":"clock"}` command — see
 * ff_loop_clock_advance/ff_loop_tick_cb.
 *
 * --connect/--pack (S13 slice a/b flags) drive an `ff_shell_t` — the S16
 * app shell — over the mc TCP transport (S16 slice b2; the interim
 * `live.{c,h}` wiring this file used before b2 is retired, see
 * docs/specs/S13-sim-target.md's Amendments). The ctl socket's
 * `{"cmd":"state"}` dump reads `ff_shell_view()` in live mode, plus a
 * `"wall"` object from `ff_shell_wall()` (see tools/dev/CTL.md).
 *
 * --dev-trust-all (S16 AC6, sim-only, COMPILED OUT of device builds —
 * see the #error guard at ff_run_ctl_loop and ff_shell.h's
 * dev-affordances section): auto-pairs every NodeInfo sender, suspends
 * the self filter, and latches the wall clock from the host's own clock,
 * so the single-node dev meshtasticd can play a crew member. Logs a line
 * naming itself at startup. Without it, live mode routes every inbound
 * event through the exact same shell entry points and drops unpaired
 * traffic — a node that has only ever sent NodeInfo + Position produces
 * zero feed items and no roster slot.
 *
 * LIVE MODE IS STILL --headless --ctl ONLY, and still renders a static
 * screen: nothing re-derives or repaints the LVGL tree when the shell's
 * view changes (issues #17/#29 — build-once/update-in-place is S16
 * slice d, which consumes the dirty bit ff_shell_tick already returns).
 * Until slice d lands, a live session is observed through the ctl
 * socket's `state`/`screenshot` commands, not through a window.
 *
 * The boot screen and the fixture debug face are both scaffolding: real
 * screens arrive with S06+.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "lvgl.h"

/* NOT re-defined here: STB_IMAGE_WRITE_IMPLEMENTATION lives in
 * screenshot.c (S13c extracted the XRGB8888->PNG writer out of this file
 * so the ctl socket's "screenshot" command and the one-shot
 * --headless --screenshot path share one implementation) — S10 slice b's
 * main.c (pre-extraction) had its own copy of this define/include; that
 * would now be a duplicate-symbol link error against ff-stb-image-write,
 * so it's intentionally dropped here rather than merged back in. */
#include "ff_version.h"

#include "ctl_out_path.h"
#include "ctl_server.h"
#include "face_dispatch.h" /* PR #25 UX review follow-up — ff_build_face_screen extracted
                             * here (shared with targets/sim/tests/test_face_hit_targets.c)
                             * instead of defined locally in this file. */
#include "ff_shell.h"      /* S16b2 — live mode is the app shell now (live.{c,h} retired) */
#include "fixture.h"
#include "mc_transport_tcp.h"
#include "screenshot.h"

#ifndef PATH_MAX
#define PATH_MAX 4096 /* POSIX guarantees this exists, but fall back just in case */
#endif

#define FF_SIM_WINDOW_W 456
#define FF_SIM_WINDOW_H 456

#define FF_COLOR_BG_DARK 0x0b0b10
#define FF_COLOR_AMBER   0xffc66b

/* Builds the boot placeholder UI (dark puck + centered "FIREFLY" label)
 * on whatever the current default display's active screen is. */
static void ff_build_boot_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_SIM_WINDOW_W - 16, FF_SIM_WINDOW_H - 16);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(puck);
    lv_label_set_text(label, "FIREFLY");
    lv_obj_set_style_text_color(label, lv_color_hex(FF_COLOR_AMBER), 0);
    lv_obj_center(label);
}

/* Full-frame render mode: the whole buffer is the flushed frame, so the
 * flush callback only needs to signal completion. */
static void ff_headless_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

/* --mock-clock (one-shot headless/window paths only — see ff_loop_tick_cb
 * for the --ctl loop's clock): a frozen tick source (always reports the
 * same instant). Headless rendering is already deterministic without it
 * — a single lv_refr_now() call with no timers run and no animations
 * started never reads the tick at all — but it's accepted (and honored)
 * in headless mode too so callers (tests/run_goldens.sh) can pass it
 * explicitly rather than relying on that being true forever as a
 * coincidence. */
static uint32_t ff_mock_tick_cb(void)
{
    return 0;
}

/* Renders exactly one frame — either the fixture debug face (if
 * fixture_path is non-NULL) or the boot placeholder — to
 * DIR/<name>.png. Returns 0 on success, 1 on any failure (fixture load,
 * OOM, or PNG write). */
static int ff_run_headless_once(const char *screenshot_dir, const char *fixture_path)
{
    lv_init();
    lv_tick_set_cb(ff_mock_tick_cb);

    const int32_t w = FF_SIM_WINDOW_W;
    const int32_t h = FF_SIM_WINDOW_H;
    const uint32_t buf_size = (uint32_t)(w * h * 4);

    uint8_t *xrgb_buf = malloc(buf_size);
    if (xrgb_buf == NULL) {
        fprintf(stderr, "ffsim: out of memory allocating %u byte framebuffer\n", buf_size);
        lv_deinit();
        return 1;
    }

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, xrgb_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, ff_headless_flush_cb);
    lv_display_set_default(disp);

    char path[4096];
    if (fixture_path != NULL) {
        ff_app_state_t state;
        ff_fixture_result_t fr = ff_fixture_load_file(fixture_path, &state);
        if (fr != FF_FIXTURE_OK) {
            fprintf(stderr, "ffsim: failed to load fixture %s (error %d)\n", fixture_path, (int)fr);
            free(xrgb_buf);
            lv_deinit();
            return 1;
        }
        /* Every S10 button (GO/DISMISS/CANCEL/FLARE) emits a semantic
         * intent through the seam (S16 slice c2) — a one-shot headless
         * render never binds anything to it (see ff_intent.h's top
         * comment: unbound is a safe no-op), so buttons still render,
         * just inertly, with no special-casing needed here at all. */
        ff_build_face_screen(&state);

        char stem[256];
        ff_fixture_stem(fixture_path, stem, sizeof(stem));
        snprintf(path, sizeof(path), "%s/%s.png", screenshot_dir, stem);
    } else {
        ff_build_boot_screen();
        snprintf(path, sizeof(path), "%s/boot.png", screenshot_dir);
    }

    lv_refr_now(disp);
    int rc = ff_screenshot_write(path, xrgb_buf, w, h);

    free(xrgb_buf);
    lv_deinit();
    return rc;
}

static int ff_run_window(const char *fixture_path, bool mock_clock)
{
    lv_init();
    lv_tick_set_cb(mock_clock ? ff_mock_tick_cb : (lv_tick_get_cb_t)SDL_GetTicks);

    lv_display_t *disp = lv_sdl_window_create(FF_SIM_WINDOW_W, FF_SIM_WINDOW_H);
    lv_sdl_window_set_title(disp, "Firefly (ffsim)");
    lv_sdl_mouse_create();

    if (fixture_path != NULL) {
        ff_app_state_t state;
        ff_fixture_result_t fr = ff_fixture_load_file(fixture_path, &state);
        if (fr != FF_FIXTURE_OK) {
            fprintf(stderr, "ffsim: failed to load fixture %s (error %d)\n", fixture_path, (int)fr);
            return 1;
        }

        /* S16 slice c2 retired the live per-process `ff_flare_t` this
         * mode used to own (S10 slice b, PR #20 review): every S10 button
         * (GO/DISMISS/CANCEL/FLARE) now emits a semantic intent through
         * the seam instead of mutating a core struct directly, and this
         * fixture-preview window never binds anything to that seam
         * (`ff_intent_emit_bind` — see ff_intent.h's top comment for why
         * unbound is a documented, safe no-op, the ctl loop's live shell
         * being the one place a target does bind it). So where the old
         * note said "GO/DISMISS mutate the engine but the window won't
         * redraw", the honest statement now is stronger and simpler: a
         * fixture window is a STATIC preview and every button on it is
         * inert. Printed for takeover fixtures specifically because that
         * screen has no other way to leave. */
        if (state.flare.takeover_active) {
            fprintf(stderr,
                    "ffsim: NOTE — this is a STATIC single-frame preview. GO/DISMISS/CANCEL/"
                    "FLARE emit intents through the seam (S16), but this window binds nothing "
                    "to it, so every button here is inert. This takeover screen has NO "
                    "on-screen way to leave — close the window or Ctrl+C to exit.\n");
        }

        ff_build_face_screen(&state);
    } else {
        ff_build_boot_screen();
    }

    while (true) {
        uint32_t next_ms = lv_timer_handler();
        SDL_Delay(next_ms > 0 ? next_ms : 1);
    }
}

/* -----------------------------------------------------------------------
 * S13c — the --ctl PORT persistent, headless, control-socket-driven loop.
 *
 * Currently requires --headless: capturing a screenshot from window
 * mode's SDL-backed display would need querying LVGL's internal draw
 * buffer for that backend (a different code path than the FULL-mode
 * offscreen buffer this file already owns and controls directly), which
 * adds real complexity for zero benefit to this slice's actual consumers
 * (the e2e harness always drives ffsim headless — see
 * docs/specs/S14-testing-ci.md slice d). Flagged as a scope decision in
 * this slice's PR body, not silently assumed.
 * --------------------------------------------------------------------- */

typedef struct {
    ff_app_state_t *state;
    lv_display_t   *disp;
    uint8_t        *xrgb_buf;
    int32_t         w, h;

    lv_indev_t     *pointer_indev;
    lv_point_t      pointer_point;
    lv_indev_state_t pointer_state;

    bool     mock_clock;
    uint32_t mock_clock_ms;

    /* S16b2 — live mode. `shell` is non-NULL for every ctl session (the
     * shell also serves fixture-only sessions as the wall-clock source
     * for the state dump); `live` is true only when --connect was given,
     * and gates both the mc TCP transport below and the per-loop
     * view -> state copy. */
    ff_shell_t *shell;
    bool        live;    /* --connect given: state mirrors ff_shell_view() */
    bool        tcp_open;
    mc_tcp_t    tcp;
    ff_clock_t  clock;   /* injected into the shell; must outlive it */

    /* PR #56 review F1: true iff this process offered the HOST's clock
     * to the wall latch (--dev-trust-all does, at startup). ff_wall_src_t
     * only knows UNKNOWN vs MESH, so without this bit a host pre-latch
     * would dump as "src":"mesh" — an honest time under a dishonest
     * provenance label, in the one surface (#49) that exists to show
     * provenance. The dump states it; see ff_loop_state_json. */
    bool wall_host_observed;

    /* Review fixup (PR #19 finding #2): the ctl socket's "screenshot"
     * path is confined under this ALREADY-CANONICALIZED (realpath()'d at
     * startup — see ff_run_ctl_loop) root — see ctl_out_path.h. */
    char ctl_out_dir_real[PATH_MAX];
} ff_loop_ctx_t;

/* Wall-clock milliseconds (POSIX monotonic clock) — used whenever
 * --mock-clock wasn't passed, so mc_client heartbeats/reconnect backoff
 * and the crew roster's freshness math see real elapsed time even though
 * this process never opens an SDL window (so no SDL_GetTicks() source is
 * available here, unlike ff_run_window's tick cb). */
static uint32_t ff_wall_clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000));
}

static ff_loop_ctx_t *g_loop_ctx = NULL; /* single-instance: lv_tick_set_cb's
                                             signature takes no user pointer */

static uint32_t ff_loop_tick_cb(void)
{
    if (g_loop_ctx != NULL && g_loop_ctx->mock_clock) return g_loop_ctx->mock_clock_ms;
    return ff_wall_clock_ms();
}

static uint32_t ff_loop_clock_now_ms(void *user)
{
    (void)user;
    return ff_loop_tick_cb();
}

static void ff_loop_pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ff_loop_ctx_t *ctx = lv_indev_get_user_data(indev);
    data->point = ctx->pointer_point;
    data->state = ctx->pointer_state;
}

static void ff_loop_tap(void *user, double x, double y)
{
    ff_loop_ctx_t *ctx = (ff_loop_ctx_t *)user;
    /* Safe to narrow unconditionally: ctl_server.c's tap handler already
     * rejected non-finite values and anything outside
     * [FF_CTL_TAP_COORD_MIN, FF_CTL_TAP_COORD_MAX] (-32768..32767) before
     * ever calling this — that used to NOT be true, and an out-of-range
     * double cast to lv_coord_t here was reproducible undefined behavior
     * (PR #19 review finding #1, confirmed under -fsanitize=undefined
     * with {"cmd":"tap","x":1e300,"y":0}). See ctl_server.h's tap
     * handler doc comment for the contract this now relies on. */
    ctx->pointer_point.x = (lv_coord_t)x;
    ctx->pointer_point.y = (lv_coord_t)y;
    ctx->pointer_state = LV_INDEV_STATE_PRESSED;
    lv_timer_handler();
    ctx->pointer_state = LV_INDEV_STATE_RELEASED;
    lv_timer_handler();
}

static void ff_loop_swipe(void *user, char const *dir)
{
    ff_loop_ctx_t *ctx = (ff_loop_ctx_t *)user;
    bool left = (strcmp(dir, "left") == 0);
    int32_t start_x = left ? (ctx->w - 60) : 60;
    int32_t end_x = left ? 60 : (ctx->w - 60);
    int32_t y = ctx->h / 2;

    ctx->pointer_point.x = (lv_coord_t)start_x;
    ctx->pointer_point.y = (lv_coord_t)y;
    ctx->pointer_state = LV_INDEV_STATE_PRESSED;
    lv_timer_handler();

    enum { STEPS = 6 };
    for (int i = 1; i <= STEPS; i++) {
        ctx->pointer_point.x = (lv_coord_t)(start_x + (end_x - start_x) * i / STEPS);
        lv_timer_handler();
    }

    ctx->pointer_state = LV_INDEV_STATE_RELEASED;
    lv_timer_handler();
}

static bool ff_loop_clock_advance(void *user, uint32_t advance_ms, char const **err)
{
    ff_loop_ctx_t *ctx = (ff_loop_ctx_t *)user;
    if (!ctx->mock_clock) {
        *err = "clock control requires --mock-clock";
        return false;
    }
    ctx->mock_clock_ms += advance_ms;
    return true;
}

static int ff_loop_state_json(void *user, char *buf, size_t buf_sz)
{
    ff_loop_ctx_t *ctx = (ff_loop_ctx_t *)user;
    int n = ff_fixture_dump_json(ctx->state, buf, buf_sz);
    if (n <= 0 || ctx->shell == NULL) return n;

    /* S16b2: append what the wall clock thinks as a "wall" object —
     * ff_shell_wall() is the only honest source, and the hardware bench
     * work (issue #49) needs to SEE what latched rather than infer it.
     * Spliced over the dump's closing '}' rather than added to the
     * fixture schema: the wall is derived live state, not renderable
     * view state, and the fixture loader ignores unknown keys, so a
     * saved state dump still loads as a fixture (see CTL.md). When it
     * would not fit, fail loudly (the caller reports "state unavailable")
     * instead of silently dropping the field. */
    ff_wall_t const w = ff_shell_wall(ctx->shell);
    char wall[160];
    int wn;
    /* `host_observed` (review F1): whether THIS PROCESS offered the host
     * clock to the latch (--dev-trust-all does at startup). ff_wall_src_t
     * cannot carry that distinction — the host observation goes through
     * the same ff_wall_observe as a mesh timestamp — so "src":"mesh"
     * alone would mislabel a host pre-latch in exactly the surface #49's
     * bench work reads for provenance. It claims exactly what is known:
     * the host clock was offered. Whether a later mesh reading re-latched
     * over it is NOT claimed — distinguishing that would need latch
     * provenance inside ff_wall_state_t, a core [api] change #49 can
     * request if the bench needs it. Dumped in both branches: "offered"
     * is a fact about this process regardless of latch state. */
    char const *host = ctx->wall_host_observed ? "true" : "false";
    if (w.src == FF_WALL_MESH) {
        wn = snprintf(wall, sizeof(wall),
                      ",\"wall\":{\"src\":\"mesh\",\"host_observed\":%s,\"day_doy\":%u,\"now_min\":%d,"
                      "\"offset_assumed\":%s}}",
                      host, (unsigned)w.day_doy, (int)w.now_min, w.offset_assumed ? "true" : "false");
    } else {
        /* UNKNOWN: every other ff_wall_t field is meaningless and is
         * deliberately not dumped — absent, not zero (CLAUDE.md). */
        wn = snprintf(wall, sizeof(wall), ",\"wall\":{\"src\":\"unknown\",\"host_observed\":%s}}", host);
    }
    if (wn < 0 || (size_t)wn >= sizeof(wall)) return -1;
    if ((size_t)n + (size_t)wn > buf_sz) return -1; /* n-1 kept + wn + NUL <= buf_sz */
    memcpy(buf + n - 1, wall, (size_t)wn + 1u);     /* overwrite trailing '}' */
    return n - 1 + wn;
}

static bool ff_loop_screenshot(void *user, char const *path, char const **err)
{
    ff_loop_ctx_t *ctx = (ff_loop_ctx_t *)user;

    /* Review fixup (PR #19 finding #2): `path` is untrusted ctl-socket
     * input, requested a RELATIVE name — confine it under
     * ctx->ctl_out_dir_real (rejects absolute paths, "..", and symlink
     * escapes; see ctl_out_path.h) before it ever reaches a filesystem
     * write. This used to go straight to ff_screenshot_write() with only
     * a length check — a reproducible arbitrary-file-write via e.g.
     * {"cmd":"screenshot","path":"/tmp/anything.png"}. */
    char resolved[PATH_MAX];
    if (!ff_ctl_out_resolve_path(path, ctx->ctl_out_dir_real, resolved, sizeof(resolved), err)) {
        return false; /* *err already set by ff_ctl_out_resolve_path */
    }

    lv_refr_now(ctx->disp);
    if (ff_screenshot_write(resolved, ctx->xrgb_buf, ctx->w, ctx->h) != 0) {
        *err = "screenshot write failed";
        return false;
    }
    return true;
}

static bool g_loop_quit_requested = false;

static void ff_loop_quit(void *user)
{
    (void)user;
    g_loop_quit_requested = true;
}

/* 256 KiB read budget for --pack: generous for any real festpack.json
 * (schedules included), same "fixed, documented budget" discipline as
 * fixture.c's FIX_MAX_JSON_LEN — and the same budget the retired
 * live.c's ff_live_load_pack enforced. ff_shell_load_pack takes bytes,
 * not a path (the device has no filesystem the way the sim does), so
 * reading the file under a stated budget is this target's job. */
#define FF_SIM_PACK_MAX (256 * 1024)

/* Reads `path` (under FF_SIM_PACK_MAX) and parses it into the shell's
 * pack storage via ff_shell_load_pack. Returns 0 on success, negative on
 * I/O failure, over-budget, or a parse error. */
static int ff_sim_load_pack_file(ff_shell_t *shell, char const *path)
{
    if (path == NULL) return -1;

    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > FF_SIM_PACK_MAX) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    char *buf = malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return -1;
    }

    int rc = ff_shell_load_pack(shell, buf, n);
    free(buf);
    return rc;
}

/* Splits "HOST:PORT" (last ':' is the separator, so an IPv6 literal host
 * isn't supported — meshtasticd's client API is addressed by hostname or
 * IPv4 in every deployment this repo targets: firmware/tools/dev/
 * compose.yml's service name, or 127.0.0.1). Returns true and fills
 * *host_out (truncated to host_sz) and *port_out on success. */
static bool ff_parse_host_port(char const *hostport, char *host_out, size_t host_sz, uint16_t *port_out)
{
    char const *colon = strrchr(hostport, ':');
    if (colon == NULL || colon == hostport || colon[1] == '\0') return false;

    size_t host_len = (size_t)(colon - hostport);
    if (host_len >= host_sz) return false;
    memcpy(host_out, hostport, host_len);
    host_out[host_len] = '\0';

    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || port <= 0 || port > 65535) return false;

    *port_out = (uint16_t)port;
    return true;
}

/* Review fixup (PR #19 finding #2): determines and canonicalizes the
 * root the ctl socket's "screenshot" command confines writes to.
 * Priority: --ctl-out DIR (created if missing) > --screenshot DIR (must
 * already exist — same "caller creates it" contract --screenshot always
 * had) > a fresh mkdtemp() temp directory (created here). Writes the
 * canonicalized (realpath()'d) root into `out` (capacity `out_sz`).
 * Returns true on success; on failure prints a diagnostic to stderr
 * itself (this only ever runs once, at startup, so an inline message is
 * simpler than threading an error string back through main()). */
static bool ff_loop_setup_ctl_out_dir(char const *ctl_out_arg, char const *screenshot_dir, char *out, size_t out_sz)
{
    char root[PATH_MAX];

    if (ctl_out_arg != NULL) {
        if (mkdir(ctl_out_arg, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "ffsim: --ctl-out %s: %s\n", ctl_out_arg, strerror(errno));
            return false;
        }
        (void)snprintf(root, sizeof(root), "%s", ctl_out_arg);
    } else if (screenshot_dir != NULL) {
        (void)snprintf(root, sizeof(root), "%s", screenshot_dir);
    } else {
        char const *tmp = getenv("TMPDIR");
        if (tmp == NULL || tmp[0] == '\0') tmp = "/tmp";
        int n = snprintf(root, sizeof(root), "%s/ffsim-ctl-XXXXXX", tmp);
        if (n < 0 || (size_t)n >= sizeof(root) || mkdtemp(root) == NULL) {
            fprintf(stderr, "ffsim: failed to create a temp dir for --ctl screenshots under %s\n", tmp);
            return false;
        }
    }

    if (!ff_ctl_out_resolve_root(root, out, out_sz)) {
        fprintf(stderr, "ffsim: --ctl-out directory %s does not exist or could not be resolved\n", root);
        return false;
    }
    printf("ffsim: ctl screenshot writes confined to %s\n", out);
    return true;
}

static int ff_run_ctl_loop(uint16_t ctl_port, const char *fixture_path, bool mock_clock, const char *connect_hostport,
                            const char *pack_path, const char *ctl_out_arg, const char *screenshot_dir,
                            bool dev_trust_all)
{
    lv_init();
    lv_tick_set_cb(ff_loop_tick_cb);

    ff_loop_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mock_clock = mock_clock;
    g_loop_ctx = &ctx;

    ctx.w = FF_SIM_WINDOW_W;
    ctx.h = FF_SIM_WINDOW_H;
    uint32_t const buf_size = (uint32_t)(ctx.w * ctx.h * 4);
    ctx.xrgb_buf = malloc(buf_size);
    if (ctx.xrgb_buf == NULL) {
        fprintf(stderr, "ffsim: out of memory allocating %u byte framebuffer\n", buf_size);
        lv_deinit();
        return 1;
    }

    ctx.disp = lv_display_create(ctx.w, ctx.h);
    lv_display_set_buffers(ctx.disp, ctx.xrgb_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(ctx.disp, ff_headless_flush_cb);
    lv_display_set_default(ctx.disp);

    ctx.pointer_indev = lv_indev_create();
    lv_indev_set_type(ctx.pointer_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ctx.pointer_indev, ff_loop_pointer_read_cb);
    lv_indev_set_user_data(ctx.pointer_indev, &ctx);

    static ff_app_state_t state; /* static: outlives this function via ctx.state
                                     (screens built below hold no pointer, but the
                                     ctl handlers dump it between loop turns) */
    memset(&state, 0, sizeof(state));
    /* S16 slice a: FF_APP_FACE_NONE = 0 renumbered ff_app_face_t, so the
     * memset above no longer leaves active_face on RADAR the way it did
     * when RADAR was the zero value. Set it explicitly to keep a
     * fixture-less ctl session's opening face exactly what it has always
     * been: without this, its state would report NONE, and the ctl
     * `dump` command's fx_enum_name() would quietly write it out as
     * "radar" anyway (its unknown-value fallback) — a state and its own
     * dump disagreeing, with nothing to notice. (In live mode the
     * per-loop ff_shell_view() copy overwrites this with the route's
     * base face, which ff_route_init also starts at RADAR.) */
    state.active_face = FF_APP_FACE_RADAR;
    ctx.state = &state;

    if (fixture_path != NULL) {
        ff_fixture_result_t fr = ff_fixture_load_file(fixture_path, &state);
        if (fr != FF_FIXTURE_OK) {
            fprintf(stderr, "ffsim: failed to load fixture %s (error %d)\n", fixture_path, (int)fr);
            free(ctx.xrgb_buf);
            lv_deinit();
            return 1;
        }
        ff_build_face_screen(&state);
    } else {
        ff_build_boot_screen();
    }

    /* --------------------------------------------------------------
     * S16b2 — the app shell replaces live.{c,h}. Static storage: the
     * shell is 16 KB and the pack ~48 KB of budget (fp_pack.h), neither
     * of which belongs on this stack frame; both are single-instance
     * per process, same as `state` above.
     * ------------------------------------------------------------ */
    static ff_shell_t s_shell;
    static fp_pack_t s_pack;

    ctx.clock.now_ms = ff_loop_clock_now_ms;
    ctx.clock.user = NULL;
    ctx.tcp.fd = -1;

    if (connect_hostport != NULL) {
        char host[256];
        uint16_t port;
        if (!ff_parse_host_port(connect_hostport, host, sizeof(host), &port)) {
            fprintf(stderr, "ffsim: --connect expects HOST:PORT, got \"%s\"\n", connect_hostport);
            free(ctx.xrgb_buf);
            lv_deinit();
            return 1;
        }
        if (mc_tcp_open(&ctx.tcp, host, port) != 0) {
            fprintf(stderr, "ffsim: failed to connect to %s:%u\n", host, (unsigned)port);
            free(ctx.xrgb_buf);
            lv_deinit();
            return 1;
        }
        ctx.tcp_open = true;
        ctx.live = true;
        printf("ffsim: connected to %s:%u\n", host, (unsigned)port);
    }

    ff_shell_cfg_t shell_cfg;
    memset(&shell_cfg, 0, sizeof(shell_cfg));
    shell_cfg.clock = &ctx.clock;
    shell_cfg.store = NULL; /* settings persistence is S16 slice e */
    shell_cfg.pack = &s_pack;
    if (ctx.tcp_open) {
        shell_cfg.transport = mc_tcp_transport(&ctx.tcp);
    } /* else: the documented "no transport" cfg — the shell still owns
       * the wall clock the ctl `state` dump reports, and --pack is
       * still validated, but no handshake ever starts. */

    if (ff_shell_init(&s_shell, &shell_cfg) != 0) {
        fprintf(stderr, "ffsim: ff_shell_init failed\n");
        if (ctx.tcp_open) mc_tcp_close(&ctx.tcp);
        free(ctx.xrgb_buf);
        lv_deinit();
        return 1;
    }
    ctx.shell = &s_shell;

    if (dev_trust_all) {
        /* S16 AC6's compile-time assertion: this affordance must not
         * exist in a non-sim build. targets/sim only ever builds with
         * FF_TARGET=sim, so this can only fire if the build system stops
         * defining FF_TARGET_SIM — in which case the flag would parse
         * while the shell-side branch was silently compiled away, which
         * is exactly the failure this makes loud. */
#if !defined(FF_TARGET_SIM)
#error "--dev-trust-all is sim-only (S16 AC6): FF_TARGET_SIM must be defined for every build of this file"
#else
        ff_shell_dev_trust_all(&s_shell, true);
        /* The host genuinely knows what time it is (see
         * ff_shell_dev_wall_observe's header comment for why this is
         * honest and why the single-node dev daemon needs it), and the
         * observation is paired with the CURRENT tick source reading —
         * under --mock-clock, later `clock` commands advance the derived
         * wall time from here. */
        ff_shell_dev_wall_observe(&s_shell, (int64_t)time(NULL));
        ctx.wall_host_observed = true; /* stated in the `state` dump's wall object (review F1) */
        printf("ffsim: --dev-trust-all: auto-pairing every NodeInfo sender, treating the daemon's "
               "own node as inbound, and latching the wall clock from the host clock "
               "(sim-only dev affordance, compiled out of device builds — S16 AC6)\n");
#endif
    }

    if (pack_path != NULL) {
        if (ff_sim_load_pack_file(&s_shell, pack_path) != 0) {
            fprintf(stderr, "ffsim: failed to load festpack %s\n", pack_path);
            ff_shell_close(&s_shell); /* same teardown shape as every exit path below */
            if (ctx.tcp_open) mc_tcp_close(&ctx.tcp);
            free(ctx.xrgb_buf);
            lv_deinit();
            return 1;
        }
        /* The shell deliberately does NOT adopt a pack's venue origin as
         * "my position" (the venue centre is not where the wearer is
         * standing — see ff_shell_load_pack's doc). This dev target DOES
         * want the old live.c behaviour for the radar dev loop, so it
         * does it itself, visibly, exactly as that doc prescribes. */
        if (s_pack.origin_known) {
            ff_shell_set_my_pos(&s_shell, s_pack.origin);
            printf("ffsim: --pack: adopting the pack's venue origin (%.6f, %.6f) as my position "
                   "(dev affordance — the sim has no GPS)\n",
                   s_pack.origin.lat, s_pack.origin.lon);
        }
    }

    /* Fixed north heading placeholder — the sim has no compass, same
     * documented stand-in the retired live.c used. Without it the radar
     * honestly reports arrow_valid=false for every member, which is
     * correct on device but makes the dev loop's radar face useless. */
    ff_shell_set_heading(&s_shell, 0.0f);

    if (!ff_loop_setup_ctl_out_dir(ctl_out_arg, screenshot_dir, ctx.ctl_out_dir_real, sizeof(ctx.ctl_out_dir_real))) {
        ff_shell_close(&s_shell);
        if (ctx.tcp_open) mc_tcp_close(&ctx.tcp);
        free(ctx.xrgb_buf);
        lv_deinit();
        return 1;
    }

    ff_ctl_server_t ctl_srv;
    if (ff_ctl_open(&ctl_srv, ctl_port, FF_CTL_DEFAULT_IDLE_TIMEOUT_MS) != 0) {
        fprintf(stderr, "ffsim: failed to open ctl socket on 127.0.0.1:%u\n", (unsigned)ctl_port);
        ff_shell_close(&s_shell);
        if (ctx.tcp_open) mc_tcp_close(&ctx.tcp);
        free(ctx.xrgb_buf);
        lv_deinit();
        return 1;
    }
    printf("ffsim: ctl socket listening on 127.0.0.1:%u\n", (unsigned)ctl_port);

    ff_ctl_handlers_t handlers = {0};
    handlers.user = &ctx;
    handlers.tap = ff_loop_tap;
    handlers.swipe = ff_loop_swipe;
    handlers.clock_advance = ff_loop_clock_advance;
    handlers.state_json = ff_loop_state_json;
    handlers.screenshot = ff_loop_screenshot;
    handlers.quit = ff_loop_quit;

    g_loop_quit_requested = false;
    while (!g_loop_quit_requested) {
        /* Every session ticks the shell (it is the wall-clock source for
         * the `state` dump); its dirty-bit return is deliberately unused
         * until S16 slice d wires the build-once/update-in-place render
         * lifecycle (issues #17/#29 — see this file's top comment). */
        (void)ff_shell_tick(&s_shell, ff_loop_tick_cb());
        if (ctx.live) {
            /* Live mode: the ctl `state` dump IS the shell's view (S16
             * AC6). Copied rather than aliased so the fixture path above
             * keeps owning `state` when --connect wasn't given; with
             * both --fixture and --connect, the fixture only seeds the
             * initial screen and the dump reflects the live view. */
            state = *ff_shell_view(&s_shell);
        }
        lv_timer_handler();
        if (ff_ctl_poll(&ctl_srv, &handlers)) break;
        usleep(5000); /* ~200 Hz: responsive without busy-spinning a CPU core */
    }

    ff_ctl_close(&ctl_srv);
    ff_shell_close(&s_shell);
    if (ctx.tcp_open) mc_tcp_close(&ctx.tcp); /* the target opened it, the target closes it */
    free(ctx.xrgb_buf);
    lv_deinit();
    g_loop_ctx = NULL;
    return 0;
}

int main(int argc, char **argv)
{
    bool headless = false;
    bool mock_clock = false;
    const char *screenshot_dir = NULL;
    const char *fixture_path = NULL;
    const char *ctl_port_str = NULL;
    const char *connect_hostport = NULL;
    const char *pack_path = NULL;
    const char *ctl_out_arg = NULL;
    bool dev_trust_all = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_dir = argv[++i];
        } else if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
            fixture_path = argv[++i];
        } else if (strcmp(argv[i], "--mock-clock") == 0) {
            mock_clock = true;
        } else if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
            ctl_port_str = argv[++i];
        } else if (strcmp(argv[i], "--ctl-out") == 0 && i + 1 < argc) {
            ctl_out_arg = argv[++i];
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            connect_hostport = argv[++i];
        } else if (strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
            pack_path = argv[++i];
        } else if (strcmp(argv[i], "--dev-trust-all") == 0) {
            dev_trust_all = true; /* sim-only by construction — see ff_run_ctl_loop */
        }
    }

    printf("ffsim: %s\n", ff_version_string());

    if (ctl_port_str != NULL) {
        if (!headless) {
            fprintf(stderr, "ffsim: --ctl currently requires --headless (see main.c's ctl-loop comment)\n");
            return 1;
        }
        char *end = NULL;
        long port = strtol(ctl_port_str, &end, 10);
        if (end == ctl_port_str || *end != '\0' || port <= 0 || port > 65535) {
            fprintf(stderr, "ffsim: --ctl expects a port number, got \"%s\"\n", ctl_port_str);
            return 1;
        }
        return ff_run_ctl_loop((uint16_t)port, fixture_path, mock_clock, connect_hostport, pack_path, ctl_out_arg,
                                screenshot_dir, dev_trust_all);
    }

    if (dev_trust_all) {
        /* Meaningful only where a live shell exists (the --ctl loop, the
         * only home of --connect). Fail loud rather than silently accept
         * a flag that would do nothing (CLAUDE.md: honest over pretty). */
        fprintf(stderr, "ffsim: --dev-trust-all requires --ctl (live mode is --headless --ctl only)\n");
        return 1;
    }

    if (headless) {
        if (screenshot_dir == NULL) {
            fprintf(stderr, "ffsim: --headless requires --screenshot DIR (or --ctl PORT)\n");
            return 1;
        }
        /* mock_clock is unconditionally honored in headless mode (see
         * ff_run_headless_once's tick setup) — accepted here without a
         * "not meaningful" warning since passing it explicitly is the
         * documented, supported way callers (tests/run_goldens.sh) opt
         * into that guarantee rather than depending on an undocumented
         * default. */
        (void)mock_clock;
        return ff_run_headless_once(screenshot_dir, fixture_path);
    }

    return ff_run_window(fixture_path, mock_clock);
}
