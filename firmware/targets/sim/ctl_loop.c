/**
 * ctl_loop.c — see ctl_loop.h.
 */
#include "ctl_loop.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ctl_out_path.h"
#include "face_dispatch.h"
#include "ff_intent.h"
#include "ff_proto.h"
#include "fixture.h"
#include "screenshot.h"

/* Matches the device panel / sim window (412x412, S15c — ff_theme.h's
 * FF_THEME_WINDOW_PX). Every ctl pointer command discovers its target's real
 * on-screen coordinates from the live LVGL tree (see
 * test_ctl_flare_sequence.c's ctl_tap_button), so this size only sets where
 * the puck renders, not any hard-coded tap coordinate. */
#define FF_CTL_LOOP_WINDOW_W 412
#define FF_CTL_LOOP_WINDOW_H 412

/* Full-frame render mode: the whole buffer is the flushed frame, so the
 * flush callback only needs to signal completion. */
static void ctl_loop_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

/* Monotonic milliseconds (POSIX). Used whenever --mock-clock wasn't
 * passed, so mc_client heartbeats/reconnect backoff and the crew
 * roster's freshness math see real elapsed time even though this
 * process never opens an SDL window. */
static uint32_t ctl_loop_wall_clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000));
}

/* Single-instance: lv_tick_set_cb's signature takes no user pointer (see
 * ctl_loop.h's ff_ctl_loop_open doc comment). */
static ff_ctl_loop_ctx_t *g_ctl_loop_ctx = NULL;

uint32_t ff_ctl_loop_tick_cb(void)
{
    if (g_ctl_loop_ctx != NULL && g_ctl_loop_ctx->mock_clock) return g_ctl_loop_ctx->mock_clock_ms;
    return ctl_loop_wall_clock_ms();
}

static uint32_t ctl_loop_clock_now_ms(void *user)
{
    (void)user;
    return ff_ctl_loop_tick_cb();
}

/* ---------------------------------------------------------------------
 * --ctl-out DIR resolution (S13c review fixup, PR #19 finding #2).
 * ------------------------------------------------------------------- */

/* Priority: --ctl-out DIR (created if missing) > --screenshot DIR (must
 * already exist — same "caller creates it" contract --screenshot always
 * had) > a fresh mkdtemp() temp directory (created here). Writes the
 * canonicalized (realpath()'d) root into `out` (capacity `out_sz`).
 * Returns true on success; on failure prints a diagnostic to stderr
 * itself (this only ever runs once, at startup). */
static bool ctl_loop_setup_ctl_out_dir(char const *ctl_out_arg, char const *screenshot_dir, char *out, size_t out_sz)
{
    char root[4096];

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

/* ---------------------------------------------------------------------
 * Open / close.
 * ------------------------------------------------------------------- */

int ff_ctl_loop_open(ff_ctl_loop_ctx_t *ctx, ff_shell_t *shell, fp_pack_t *pack, ff_shell_cfg_t *shell_cfg,
                      ff_ctl_loop_cfg_t const *cfg)
{
    if (ctx == NULL || shell == NULL || shell_cfg == NULL || cfg == NULL) return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->shell = shell;
    ctx->pack = pack;
    ctx->mock_clock = cfg->mock_clock;
    ctx->w = FF_CTL_LOOP_WINDOW_W;
    ctx->h = FF_CTL_LOOP_WINDOW_H;

    g_ctl_loop_ctx = ctx;
    lv_init();
    lv_tick_set_cb(ff_ctl_loop_tick_cb);

    uint32_t const buf_size = (uint32_t)(ctx->w * ctx->h * 4);
    ctx->xrgb_buf = malloc(buf_size);
    if (ctx->xrgb_buf == NULL) {
        fprintf(stderr, "ffsim: out of memory allocating %u byte framebuffer\n", buf_size);
        lv_deinit();
        g_ctl_loop_ctx = NULL;
        return -1;
    }

    ctx->disp = lv_display_create(ctx->w, ctx->h);
    lv_display_set_buffers(ctx->disp, ctx->xrgb_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(ctx->disp, ctl_loop_flush_cb);
    lv_display_set_default(ctx->disp);

    ctx->pointer_indev = lv_indev_create();
    lv_indev_set_type(ctx->pointer_indev, LV_INDEV_TYPE_POINTER);
    /* read_cb is installed by main.c's ff_loop_pointer_read_cb equivalent
     * — kept there rather than here since it's a one-line lambda over
     * ctx->pointer_point/state, and the handlers below (ff_ctl_loop_tap/
     * swipe) live in THIS file and already own those fields directly. */
    lv_indev_set_user_data(ctx->pointer_indev, ctx);

    ctx->clock.now_ms = ctl_loop_clock_now_ms;
    ctx->clock.user = NULL;
    shell_cfg->clock = &ctx->clock;
    shell_cfg->pack = pack;
    /* Static, singleton like g_ctl_loop_ctx above — this loop only ever
     * runs one shell at a time. See fp_pack.h's FP_MAX_TOKENS. */
    static jsmntok_t s_toks[FP_MAX_TOKENS];
    shell_cfg->toks = s_toks;
    shell_cfg->ntoks = FP_MAX_TOKENS;

    ff_live_setup_cfg_t live_cfg = {
        .connect_hostport = cfg->connect_hostport,
        .pack_path = cfg->pack_path,
        .dev_trust_all = cfg->dev_trust_all,
    };
    if (ff_live_setup(shell, shell_cfg, &live_cfg, &ctx->live) != 0) {
        free(ctx->xrgb_buf);
        lv_deinit();
        g_ctl_loop_ctx = NULL;
        return -1;
    }

    if (!ctl_loop_setup_ctl_out_dir(cfg->ctl_out_arg, cfg->screenshot_dir, ctx->ctl_out_dir_real,
                                     sizeof(ctx->ctl_out_dir_real))) {
        ff_live_setup_close(&ctx->live);
        free(ctx->xrgb_buf);
        lv_deinit();
        g_ctl_loop_ctx = NULL;
        return -1;
    }

    /* S16 slice a: FF_APP_FACE_NONE = 0 renumbered ff_app_face_t — set the
     * opening face explicitly rather than rely on a memset default, same
     * reasoning as the pre-extraction code this replaces. Overwritten by
     * the first ff_ctl_loop_pump's shell projection (whose own
     * ff_route_init also starts at RADAR) — this is purely the pre-first-
     * tick face. */
    memset(&ctx->state, 0, sizeof(ctx->state));
    ctx->state.active_face = FF_APP_FACE_RADAR;

    if (cfg->fixture_path != NULL) {
        ff_fixture_result_t fr = ff_fixture_load_file(cfg->fixture_path, &ctx->state);
        if (fr != FF_FIXTURE_OK) {
            fprintf(stderr, "ffsim: failed to load fixture %s (error %d)\n", cfg->fixture_path, (int)fr);
            ff_live_setup_close(&ctx->live);
            free(ctx->xrgb_buf);
            lv_deinit();
            g_ctl_loop_ctx = NULL;
            return -1;
        }
    }
    ff_build_face_screen(&ctx->state);
    ctx->has_screen = true;

    /* S26 slice c — see ctl_loop.h's ff_ctl_loop_ctx_t doc comment. */
    ff_idle_init(&ctx->idle);

    /* The intent seam: every wired button (FLARE, GO/DISMISS, the T9
     * keypad, ...) now reaches this shell. Unbind happens in
     * ff_ctl_loop_close, before the shell can go away — see
     * ff_intent.h's LIFETIME contract. */
    ff_intent_emit_bind(ff_shell_intent_sink, shell);

    return 0;
}

void ff_ctl_loop_pump(ff_ctl_loop_ctx_t *ctx)
{
    if (ctx == NULL) return;

    uint32_t const now_ms = ff_ctl_loop_tick_cb();
    bool const dirty = ff_shell_tick(ctx->shell, now_ms);
    if (ff_shell_take_wake(ctx->shell)) { /* S26(c)+(d) banner wakes the screen — see app_main */
        ff_idle_input(&ctx->idle, now_ms);
    }
    ctx->state = *ff_shell_view(ctx->shell);

    /* S26 slice c — the sim's own AC3 harness mirrors app_main.c's
     * keep_awake source (the sim has no blocking calibration flow, so
     * that third source is always false here) and idle tick, ticked
     * every pump regardless of dirty (same "always tick" contract every
     * FSM in this codebase follows). */
    bool const keep_awake = ff_shell_keep_awake(&ctx->state, false);
    ff_idle_state_t const idle_state = ff_idle_tick(&ctx->idle, now_ms, keep_awake);

    /* S16 slice d: rebuild ONLY on a dirty tick — this is the whole
     * point (closes #17/#29's "rebuild every frame regardless" cost, and
     * AC4(b)'s dirty bit is what drives it end to end, not just at the
     * ff_shell_tick unit level). S26 slice c ADDS a second gate: OFF
     * skips the rebuild entirely (no face rebuilds while the screen is
     * off), latching a dirty tick that lands during OFF into
     * `rebuild_pending` so it drains on the first non-OFF pump after a
     * wake — "a dirty view is rebuilt on wake" (never lost, only
     * deferred, mirroring app_main.c's finger-down defer latch in
     * spirit).
     *
     * lv_obj_clean() BEFORE rebuilding is what makes issue #17's static
     * point pools (scr_radar.c, scr_flare.c) safe under repeated builds:
     * every lv_line/triangle-descriptor object from the PREVIOUS build is
     * deleted here, so by the time ff_build_face_screen resets those
     * pools' indices, nothing still-alive references the points about to
     * be overwritten. It also closes #17's OTHER half (unbounded object
     * growth): the same screen object is reused forever, its children
     * torn down and rebuilt, never accumulated. */
    if (dirty) {
        ctx->rebuild_pending = true;
    }
    if (ctx->rebuild_pending && ctx->has_screen && idle_state != FF_IDLE_STATE_OFF) {
        lv_obj_clean(lv_screen_active());
        ff_build_face_screen(&ctx->state);
        ctx->rebuild_pending = false;
        ctx->rebuild_count++;
    }
}

void ff_ctl_loop_close(ff_ctl_loop_ctx_t *ctx)
{
    if (ctx == NULL) return;
    ff_intent_emit_bind(NULL, NULL); /* before the shell can go away — ff_intent.h LIFETIME */
    ff_live_setup_close(&ctx->live);
    free(ctx->xrgb_buf);
    ctx->xrgb_buf = NULL;
    if (g_ctl_loop_ctx == ctx) g_ctl_loop_ctx = NULL;
}

/* ---------------------------------------------------------------------
 * ff_ctl_handlers_t — tap / swipe / clock / state / screenshot / flare / quit.
 * ------------------------------------------------------------------- */

static void ctl_loop_pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ff_ctl_loop_ctx_t *ctx = lv_indev_get_user_data(indev);
    data->point = ctx->pointer_point;
    data->state = ctx->pointer_state;
}

/* Advance time between the synthetic pointer steps below, so LVGL's
 * indev read timer (33 ms period — the exact constraint
 * test_ctl_flare_sequence.c's top comment documents for `tap`) actually
 * polls each step instead of at most the first one. Without this the
 * whole press→moves→release ran in zero elapsed time, LVGL saw at most
 * a single poll, no gesture was ever recognized, and `swipe` returned
 * {"ok":true} while the face never changed — found the day someone
 * finally drove it end to end (the AC10 test had deliberately avoided
 * it). Proxy: the command replied ok. Property: the face changed. */
static void ctl_loop_pointer_step_delay(ff_ctl_loop_ctx_t *ctx)
{
    if (ctx->mock_clock) {
        ctx->mock_clock_ms += 40u; /* > the 33 ms indev period */
    } else {
        /* Untested branch: every test runs --mock-clock. Structurally
         * identical to the proven mock path; acceptable for a dev tool,
         * noted per PR #62's review rather than implied covered. */
        usleep(40 * 1000);
    }
}

/**
 * ctl_loop_pointer_gesture — the shared press -> N-step -> release
 * primitive `tap`/`swipe`/`hold` all reduce to. Issue #70's own larger
 * point: every ctl pointer command (tap and swipe, PR #62; hold, here)
 * has needed the exact same time choreography rediscovered —
 * ctl_loop_pointer_step_delay between every step so LVGL's indev read
 * timer (33ms period) actually polls at each one, matching the ordering
 * ctl_loop_tap's original fix established: point set, THEN step_delay,
 * THEN the state transition + lv_timer_handler, on both the press end
 * and the release end.
 *
 * `n_steps` intermediate steps run between press and release, calling
 * `step_cb(ctx, step_user, i, n_steps)` (if non-NULL) before each one's
 * own step_delay+lv_timer_handler to update ctx->pointer_point:
 *   - tap:   n_steps = 0, step_cb = NULL — nothing moves, nothing runs
 *            between the press call and the release call, exactly
 *            ctl_loop_tap's original two-call shape.
 *   - swipe: n_steps = STEPS (6), step_cb interpolates x from start to
 *            end — exactly ctl_loop_swipe's original press+6+release
 *            shape.
 *   - hold:  n_steps = ceil(ms/40), step_cb = NULL — the point never
 *            moves; the steps exist purely to keep pumping
 *            lv_timer_handler while enough mock-clock/wall time passes
 *            for LVGL's long_press_time to elapse, the same "at most one
 *            poll, gesture never recognized" failure mode PR #62 already
 *            found and fixed for tap/swipe, now avoided here too.
 */
typedef void (*ctl_loop_gesture_step_fn)(ff_ctl_loop_ctx_t *ctx, void *step_user, int step_i, int n_steps);

static void ctl_loop_pointer_gesture(ff_ctl_loop_ctx_t *ctx, int32_t x0, int32_t y0, int n_steps,
                                      ctl_loop_gesture_step_fn step_cb, void *step_user)
{
    /* S26 slice c — every real pointer gesture (tap/swipe/hold) is a
     * touch, so it wakes the idle FSM exactly as the device's touch
     * indev does (app_main.c). */
    ff_idle_input(&ctx->idle, ff_ctl_loop_tick_cb());

    ctx->pointer_point.x = (lv_coord_t)x0;
    ctx->pointer_point.y = (lv_coord_t)y0;
    ctl_loop_pointer_step_delay(ctx);
    ctx->pointer_state = LV_INDEV_STATE_PRESSED;
    lv_timer_handler();

    for (int i = 1; i <= n_steps; i++) {
        if (step_cb != NULL) step_cb(ctx, step_user, i, n_steps);
        ctl_loop_pointer_step_delay(ctx);
        lv_timer_handler();
    }

    ctx->pointer_state = LV_INDEV_STATE_RELEASED;
    ctl_loop_pointer_step_delay(ctx);
    lv_timer_handler();
}

static void ctl_loop_tap(void *user, double x, double y)
{
    ff_ctl_loop_ctx_t *ctx = (ff_ctl_loop_ctx_t *)user;
    /* Safe to narrow unconditionally: ctl_server.c's tap handler already
     * rejected non-finite values and anything outside
     * [FF_CTL_TAP_COORD_MIN, FF_CTL_TAP_COORD_MAX] — see ctl_server.h's
     * tap handler doc comment for the contract this relies on. */
    ctl_loop_pointer_gesture(ctx, (int32_t)x, (int32_t)y, 0, NULL, NULL);
}

typedef struct {
    int32_t start_x, end_x, y;
} ctl_loop_swipe_step_ctx_t;

static void ctl_loop_swipe_step_cb(ff_ctl_loop_ctx_t *ctx, void *step_user, int step_i, int n_steps)
{
    ctl_loop_swipe_step_ctx_t const *s = (ctl_loop_swipe_step_ctx_t const *)step_user;
    ctx->pointer_point.x = (lv_coord_t)(s->start_x + (s->end_x - s->start_x) * step_i / n_steps);
    ctx->pointer_point.y = (lv_coord_t)s->y;
}

static void ctl_loop_swipe(void *user, char const *dir)
{
    ff_ctl_loop_ctx_t *ctx = (ff_ctl_loop_ctx_t *)user;
    bool left = (strcmp(dir, "left") == 0);
    int32_t start_x = left ? (ctx->w - 60) : 60;
    int32_t end_x = left ? 60 : (ctx->w - 60);
    int32_t y = ctx->h / 2;

    enum { STEPS = 6 };
    ctl_loop_swipe_step_ctx_t step_ctx = {start_x, end_x, y};
    ctl_loop_pointer_gesture(ctx, start_x, y, STEPS, ctl_loop_swipe_step_cb, &step_ctx);
}

/**
 * ctl_loop_hold — issue #70: press at (x, y), hold in place (no
 * movement — step_cb is NULL) for at least `ms`, then release. `ms` is
 * turned into ceil(ms / 40) steps of ctl_loop_pointer_gesture, 40 being
 * the exact per-call advance ctl_loop_pointer_step_delay itself makes
 * (see that function's comment) — so the total elapsed time the press
 * has been held by the time release runs is always >= `ms`, comfortably
 * past LVGL's long_press_time (LV_INDEV_DEF_LONG_PRESS_TIME, 400ms) for
 * ctl_server.c's default `ms` (FF_CTL_HOLD_DEFAULT_MS, 600). A short
 * `ms` (below the threshold) intentionally does NOT open Settings —
 * that's what test_ctl_flare_sequence.c's negative case pins. `x`/`y`/
 * `ms` are already validated by ctl_server.c before this is called (see
 * ctl_server.h's `hold` doc comment) — safe to narrow/use directly. */
static void ctl_loop_hold(void *user, double x, double y, uint32_t ms)
{
    ff_ctl_loop_ctx_t *ctx = (ff_ctl_loop_ctx_t *)user;
    int n_steps = (int)((ms + 39u) / 40u); /* ceil(ms/40), matching step_delay's own 40ms advance */
    ctl_loop_pointer_gesture(ctx, (int32_t)x, (int32_t)y, n_steps, NULL, NULL);
}

static bool ctl_loop_clock_advance(void *user, uint32_t advance_ms, char const **err)
{
    ff_ctl_loop_ctx_t *ctx = (ff_ctl_loop_ctx_t *)user;
    if (!ctx->mock_clock) {
        *err = "clock control requires --mock-clock";
        return false;
    }
    ctx->mock_clock_ms += advance_ms;
    return true;
}

/** "connected" / "reconnecting" / "none" — ff_shell_link_t's own
 *  vocabulary, verbatim (ff_shell.h). */
static char const *ctl_loop_link_str(ff_shell_link_t link)
{
    switch (link) {
    case FF_SHELL_LINK_CONNECTED:
        return "connected";
    case FF_SHELL_LINK_RECONNECTING:
        return "reconnecting";
    case FF_SHELL_LINK_NONE:
    default:
        return "none";
    }
}

/**
 * The paired roster's per-member RSSI, as a JSON array — bench
 * visibility for #35's still-unmeasured question (see the PR body):
 * "across a real session, what fraction of packets from a paired peer
 * are classifiably DIRECT?" can't be answered from this desk, but it
 * can be answered the next time boards are driven IF the field is
 * observable over the ctl socket, same as `wall`/`link` already are
 * for the questions issue #49 raised. `has_rssi` mirrors
 * ff_crew_member_t's own `rssi_dbm == INT16_MIN` sentinel-as-absence
 * convention (ff_crew.h) rather than exposing the sentinel value
 * itself; `rssi_dbm`/`rssi_age_ms` are omitted entirely when absent —
 * same "absent key, not a placeholder value" contract
 * ff_fixture_dump_json's stage_color_rgb uses (fixture.c) — so a
 * consumer that reads the key without checking `has_rssi` first gets a
 * missing-field error, not a silently fabricated 0 dBm / 0 ms.
 * `rssi_age_ms` is the ELAPSED age at `now_ms` (this function's `now`
 * parameter), not the raw absolute clock stamp `ff_crew_member_t`
 * stores internally (see ff_crew.h's header note on that field) —
 * dumping the raw stamp would make every entry's number depend on
 * when the process booted, which is useless to a human reading the
 * dump live. Unpaired (merely-heard) roster slots are included too —
 * `paired` is dumped alongside so a reader can tell a merely-heard
 * node's RSSI (never fed to ff_crew_on_rssi — the standing trust rule,
 * ff_shell.c's shell_ev_rx_meta) from a trusted one apart; an unpaired
 * entry's `has_rssi` is therefore always false. */
static int ctl_loop_crew_json(ff_crew_t const *crew, uint32_t now_ms, char *buf, size_t buf_sz)
{
    if (buf == NULL || buf_sz == 0) return -1;
    size_t off = 0;
    int n = snprintf(buf + off, buf_sz - off, "[");
    if (n < 0 || (size_t)n >= buf_sz - off) return -1;
    off += (size_t)n;

    uint8_t count = (crew != NULL) ? crew->count : 0;
    for (uint8_t i = 0; i < count; i++) {
        ff_crew_member_t const *m = &crew->members[i];
        bool has_rssi = (m->rssi_dbm != INT16_MIN);
        if (has_rssi) {
            uint32_t const age_ms = now_ms - m->rssi_age_ms; /* wraparound-safe, ff_clock_t convention */
            n = snprintf(buf + off, buf_sz - off,
                         "%s{\"node_id\":%u,\"paired\":%s,\"has_rssi\":true,\"rssi_dbm\":%d,"
                         "\"rssi_age_ms\":%u}",
                         (i == 0) ? "" : ",", (unsigned)m->node_id, m->paired ? "true" : "false", (int)m->rssi_dbm,
                         (unsigned)age_ms);
        } else {
            n = snprintf(buf + off, buf_sz - off, "%s{\"node_id\":%u,\"paired\":%s,\"has_rssi\":false}",
                         (i == 0) ? "" : ",", (unsigned)m->node_id, m->paired ? "true" : "false");
        }
        if (n < 0 || (size_t)n >= buf_sz - off) return -1;
        off += (size_t)n;
    }

    n = snprintf(buf + off, buf_sz - off, "]");
    if (n < 0 || (size_t)n >= buf_sz - off) return -1;
    off += (size_t)n;
    return (int)off;
}

static int ctl_loop_state_json(void *user, char *buf, size_t buf_sz)
{
    ff_ctl_loop_ctx_t *ctx = (ff_ctl_loop_ctx_t *)user;
    int n = ff_fixture_dump_json(&ctx->state, buf, buf_sz);
    if (n <= 0) return n;

    /* Append what the wall clock thinks as a "wall" object, the mesh
     * link state as a "link" string (S16 slice e), and the paired
     * roster's per-member RSSI as a "crew" array (#35 remainder) —
     * ff_shell_wall() / ff_shell_link() / ff_shell_crew() are the only
     * honest sources, and the hardware bench work (issues #49, #35)
     * needs to SEE all three rather than infer them. Spliced over the
     * dump's closing '}' rather than added to the fixture schema: all
     * three are derived live state, not renderable view state, and the
     * fixture loader ignores unknown keys, so a saved state dump still
     * loads as a fixture (see CTL.md). */
    ff_wall_t const w = ff_shell_wall(ctx->shell);
    char const *link = ctl_loop_link_str(ff_shell_link(ctx->shell));
    uint32_t const now_ms = ff_ctl_loop_tick_cb();
    char crew_buf[8 /* FF_CREW_MAX */ * 96 + 16];
    int cn = ctl_loop_crew_json(ff_shell_crew(ctx->shell), now_ms, crew_buf, sizeof(crew_buf));
    if (cn < 0) return -1;

    char extra[sizeof(crew_buf) + 200];
    int en;
    char const *host = ctx->live.wall_host_observed ? "true" : "false";
    /* S18 slice a, AC5: the trust gate's rejection count, so a stranger
     * trying to move the clock is bench-visible from the bare ctl socket,
     * not something only a unit test can see (issue #49). */
    unsigned const rejected = (unsigned)ff_shell_wall_rejected_relatches(ctx->shell);
    /* S18 slice b, AC4: the cold-boot replay settle buffer's overflow-drop
     * count, surfaced next to rejected_relatches so a dropped freshness
     * recovery is visible from the bare ctl socket, not only a unit test
     * (issue #50). Like rejected_relatches it is not an ff_wall_t field and
     * survives an UNKNOWN latch, so it is dumped in both branches. */
    unsigned const replay_overflow = (unsigned)ff_shell_replay_overflow_count(ctx->shell);
    if (w.src == FF_WALL_MESH) {
        en = snprintf(extra, sizeof(extra),
                      ",\"wall\":{\"src\":\"mesh\",\"host_observed\":%s,\"day_doy\":%u,\"now_min\":%d,"
                      "\"offset_assumed\":%s,\"rejected_relatches\":%u,\"replay_overflow\":%u},"
                      "\"link\":\"%s\",\"crew\":%s}",
                      host, (unsigned)w.day_doy, (int)w.now_min, w.offset_assumed ? "true" : "false", rejected,
                      replay_overflow, link, crew_buf);
    } else {
        /* UNKNOWN: every other ff_wall_t field is meaningless and is
         * deliberately not dumped — absent, not zero (CLAUDE.md).
         * rejected_relatches is not an ff_wall_t field (it survives even
         * an expired/UNKNOWN latch) so it is dumped in both branches. */
        en = snprintf(extra, sizeof(extra),
                      ",\"wall\":{\"src\":\"unknown\",\"host_observed\":%s,\"rejected_relatches\":%u,"
                      "\"replay_overflow\":%u},\"link\":\"%s\",\"crew\":%s}",
                      host, rejected, replay_overflow, link, crew_buf);
    }
    if (en < 0 || (size_t)en >= sizeof(extra)) return -1;
    if ((size_t)n + (size_t)en > buf_sz) return -1; /* n-1 kept + en + NUL <= buf_sz */
    memcpy(buf + n - 1, extra, (size_t)en + 1u);     /* overwrite trailing '}' */
    return n - 1 + en;
}

static bool ctl_loop_screenshot(void *user, char const *path, char const **err)
{
    ff_ctl_loop_ctx_t *ctx = (ff_ctl_loop_ctx_t *)user;

    /* `path` is untrusted ctl-socket input, requested a RELATIVE name —
     * confine it under ctx->ctl_out_dir_real (rejects absolute paths,
     * "..", and symlink escapes; see ctl_out_path.h) before it ever
     * reaches a filesystem write. */
    char resolved[4096];
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

/**
 * S16 slice d / AC10: inject a synthetic inbound FLARE, reaching past
 * the transport the same way `clock`/`tap`/`swipe` already do. Pairs
 * `from` first (`ff_shell_pair`) so the roster trust policy doesn't drop
 * it — see ctl_server.h's `flare` field doc comment.
 */
static bool ctl_loop_flare(void *user, uint32_t from, uint16_t dur_s, char const **err)
{
    ff_ctl_loop_ctx_t *ctx = (ff_ctl_loop_ctx_t *)user;

    (void)ff_shell_pair(ctx->shell, from, true);

    uint8_t buf[FF_PROTO_ENVELOPE_LEN + 2u];
    int n = ff_proto_encode_flare(buf, sizeof(buf), dur_s);
    if (n <= 0) {
        *err = "flare encode failed";
        return false;
    }

    mc_events_t const ev = ff_shell_events(ctx->shell);
    if (ev.on_private == NULL) {
        *err = "flare injection unavailable";
        return false;
    }
    /* `to` = broadcast: a flare is the crew-wide SOS, and that is the
     * packet this seam simulates (the ctl protocol carries no destination
     * of its own — issue #123). */
    ev.on_private(ev.user, from, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
    return true;
}

/* Bench time-travel: hand an arbitrary unix time to the sim-only dev wall
 * observation (the same entry --dev-trust-all's host pre-latch uses, so it
 * carries the same honest provenance — the ctl state dump's
 * host_observed:true). Exists so real festpacks whose dates are not "now"
 * (a past Bass Canyon, a future Lost Lands) can be tested live end to end:
 * the wall latch itself only moves forward from genuine observations, so
 * without this a finished festival's schedule is unreachable on the bench.
 * The plausibility gate still applies — an out-of-window time is rejected
 * by ff_wall, not silently accepted here, and the reply says so. */
static bool ctl_loop_wall(void *user, int64_t unix_s, char const **err)
{
    ff_ctl_loop_ctx_t *ctx = (ff_ctl_loop_ctx_t *)user;
    if (!ff_shell_dev_wall_observe(ctx->shell, unix_s)) {
        /* The gate's verdict, from the observe's own return — NOT from
         * "is the wall resolvable afterwards", which stays true off any
         * earlier latch and would read ok for exactly the values the
         * gate refused. */
        *err = "wall time rejected by ff_wall's plausibility window";
        return false;
    }
    if (ff_shell_wall(ctx->shell).src == FF_WALL_UNKNOWN) {
        *err = "wall latched but unresolvable: no UTC offset (load a pack or set settings utc_offset)";
        return false;
    }
    ctx->live.wall_host_observed = true;
    return true;
}

static bool *g_ctl_loop_quit_flag = NULL;

static void ctl_loop_quit(void *user)
{
    (void)user;
    if (g_ctl_loop_quit_flag != NULL) *g_ctl_loop_quit_flag = true;
}

ff_ctl_handlers_t ff_ctl_loop_handlers(ff_ctl_loop_ctx_t *ctx, bool *quit_flag)
{
    /* Installed here rather than in ff_ctl_loop_open: the pointer read
     * callback and the quit flag both belong to "how this session's
     * handlers behave", which a caller (a test) may want to rebind
     * against a fresh quit_flag without reopening the whole session. */
    if (ctx != NULL && ctx->pointer_indev != NULL) {
        lv_indev_set_read_cb(ctx->pointer_indev, ctl_loop_pointer_read_cb);
    }
    g_ctl_loop_quit_flag = quit_flag;

    ff_ctl_handlers_t h = {0};
    h.user = ctx;
    h.tap = ctl_loop_tap;
    h.swipe = ctl_loop_swipe;
    h.hold = ctl_loop_hold;
    h.clock_advance = ctl_loop_clock_advance;
    h.state_json = ctl_loop_state_json;
    h.screenshot = ctl_loop_screenshot;
    h.flare = ctl_loop_flare;
    h.wall = ctl_loop_wall;
    h.quit = ctl_loop_quit;
    return h;
}
