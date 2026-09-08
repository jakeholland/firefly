/**
 * ff_bench_main.c — S14 hardening pass, item 3: sim micro-benchmark
 * target ("ff_bench"). Times the hot paths named in this pass's brief,
 * measured (POSIX CLOCK_MONOTONIC, not guessed) rather than reasoned
 * about from reading the source:
 *
 *   1. shell_render_key + the render-key memcmp (via a real ctl-loop
 *      session's steady-state ff_ctl_loop_pump — shell_render_key
 *      itself is `static` in app/ff_shell.c, which is a file another
 *      agent is concurrently editing (fix/render-key-churn) in this
 *      hardening pass's environment, so this deliberately does NOT
 *      export or otherwise touch that function; a no-op pump still
 *      calls it and the render-key memcmp exactly once per tick, which
 *      is what actually matters for a live device's per-frame budget).
 *   2. ff_face_build (ff_build_face_screen, targets/sim/face_dispatch.c)
 *      for each of the app's faces, via the same real ctl-loop session
 *      used for (1) — a real, fully-populated ff_app_state_t, not a
 *      hand-built minimal one.
 *   3. ff_radar_compute with 8 (== FF_CREW_MAX) paired, positioned crew
 *      members.
 *   4. Map draw-op generation: ff_map_place_labels + ff_map_triangulate
 *      on a representative point set (the real, currently-merged Lost
 *      Lands "Venue extent" feature's 9-point concave polygon —
 *      ff_map_triangulate's own doc comment names this exact fixture as
 *      the reason its ear-clipping exists instead of a plain fan).
 *   5. T9 prediction per keypress: ff_t9pred_match, incrementally, over
 *      a real word from the shipped dictionary.
 *   6. festpack lookup: ff_sched_now_playing over the real, vendored
 *      Lost Lands 2026 pack (27+ sets — see festpack/tests/fixtures/
 *      lost-lands-2026.festpack.json).
 *
 * NOT a ctest (see targets/sim/CMakeLists.txt's comment on this target):
 * wall-clock timings are inherently machine-dependent, so this is a
 * `cmake --build ... --target ff_bench && ./ff_bench` tool a human (or
 * this PR's own before/after report) runs deliberately, not a build
 * gate. It still has to actually build clean under both compilers'
 * -Wall -Wextra -Werror, same as everything else in this tree.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ctl_loop.h"
#include "face_dispatch.h"
#include "ff_app_state.h"
#include "ff_crew.h"
#include "ff_intent.h"
#include "ff_map.h"
#include "ff_radar.h"
#include "ff_sched.h"
#include "ff_shell.h"
#include "ff_t9pred.h"
#include "fp_pack.h"

/* ---------------------------------------------------------------------
 * Timing + reporting.
 * ------------------------------------------------------------------- */

static double bench_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void bench_report(char const *name, long iters, double total_ms, char const *extra)
{
    double per_iter_us = (iters > 0) ? (total_ms * 1000.0 / (double)iters) : 0.0;
    printf("%-46s %8ld iters  %10.3f ms total  %10.3f us/iter", name, iters, total_ms, per_iter_us);
    if (extra != NULL && extra[0] != '\0') {
        printf("  (%s)", extra);
    }
    printf("\n");
}

/* ---------------------------------------------------------------------
 * 1 + 2. shell_render_key/memcmp + ff_face_build, via a real ctl-loop
 * session (see this file's header comment for why shell_render_key
 * itself is only reached indirectly, through ff_ctl_loop_pump).
 * ------------------------------------------------------------------- */

/* Populates FF_CREW_MAX (8) paired crew members with fresh positions —
 * shared by the render-key/face-build session below and the standalone
 * ff_radar_compute benchmark further down, so both exercise the same
 * "8 members" scenario the task specifies. */
static void seed_eight_crew_members(mc_events_t const *ev)
{
    ev->on_my_info(ev->user, 0xF00Du);
    for (uint32_t i = 0; i < FF_CREW_MAX; i++) {
        uint32_t node = 0x1000u + i;
        mc_nodeinfo_t n;
        memset(&n, 0, sizeof(n));
        n.node_num = node;
        n.has_short_name = true;
        snprintf(n.short_name, sizeof(n.short_name), "C%02u", (unsigned)i);
        n.has_long_name = true;
        snprintf(n.long_name, sizeof(n.long_name), "Crew Member %02u", (unsigned)i);
        n.last_heard = 1000u;
        n.has_position = true;
        /* Spread around a real-ish festival-sized area (~1km across) so
         * ff_radar_compute's bearing/distance math sees varied, non-
         * degenerate inputs rather than 8 coincident points. */
        n.position.lat = 41.000 + (double)i * 0.0005;
        n.position.lon = -84.000 + (double)i * 0.0007;
        n.position.has_rx_time = false;
        ev->on_node(ev->user, &n);
    }
}

typedef struct {
    ff_ctl_loop_ctx_t ctx;
    ff_shell_t shell;
    fp_pack_t pack;
    ff_ctl_handlers_t h;
    bool quit_flag;
} bench_session_t;

static void bench_settle(bench_session_t *s)
{
    s->ctx.mock_clock_ms += 50u; /* plain public field — see
                                   * targets/sim/tests/test_ctl_flare_sequence.c's own comment
                                   * pinning this as an approved direct-mutation pattern. */
    lv_timer_handler();
    ff_ctl_loop_pump(&s->ctx);
    lv_timer_handler();
}

static int bench_session_open(bench_session_t *s)
{
    memset(s, 0, sizeof(*s));
    ff_shell_cfg_t shell_cfg;
    memset(&shell_cfg, 0, sizeof(shell_cfg));

    ff_ctl_loop_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mock_clock = true;

    if (ff_ctl_loop_open(&s->ctx, &s->shell, &s->pack, &shell_cfg, &cfg) != 0) {
        return -1;
    }
    s->h = ff_ctl_loop_handlers(&s->ctx, &s->quit_flag);
    bench_settle(s);

    mc_events_t ev = ff_shell_events(&s->shell);
    seed_eight_crew_members(&ev);
    for (uint32_t i = 0; i < FF_CREW_MAX; i++) {
        ff_shell_pair(&s->shell, 0x1000u + i, true);
    }
    bench_settle(s);
    return 0;
}

static void bench_session_close(bench_session_t *s)
{
    ff_ctl_loop_close(&s->ctx);
    lv_deinit();
}

/* Navigate to `face`; RADAR/LINEUP/INBOX/MAP/SETTINGS go through the
 * launcher's fixed circle order (app/ff_shell.c's FF_INTENT_LAUNCHER_
 * SELECT case: 0=Radar,1=Lineup,2=Inbox,3=Map,4=Settings); LAUNCHER is
 * FF_INTENT_HOME; COMPOSE and POWER_MENU have their own dedicated
 * intents. Returns false if `face` isn't one this bench knows how to
 * reach (never actually hit, given the fixed face_names[] table below,
 * but keeps this function honest rather than silently mis-navigating). */
static bool bench_navigate(bench_session_t *s, ff_app_face_t face)
{
    ff_intent_t in;
    memset(&in, 0, sizeof(in));

    switch (face) {
    case FF_APP_FACE_LAUNCHER:
        in.kind = FF_INTENT_HOME;
        break;
    case FF_APP_FACE_RADAR:
        in.kind = FF_INTENT_LAUNCHER_SELECT;
        in.u.launcher_idx = 0u;
        break;
    case FF_APP_FACE_LINEUP:
        in.kind = FF_INTENT_LAUNCHER_SELECT;
        in.u.launcher_idx = 1u;
        break;
    case FF_APP_FACE_INBOX:
        in.kind = FF_INTENT_LAUNCHER_SELECT;
        in.u.launcher_idx = 2u;
        break;
    case FF_APP_FACE_MAP:
        in.kind = FF_INTENT_LAUNCHER_SELECT;
        in.u.launcher_idx = 3u;
        break;
    case FF_APP_FACE_SETTINGS:
        in.kind = FF_INTENT_LAUNCHER_SELECT;
        in.u.launcher_idx = 4u;
        break;
    case FF_APP_FACE_COMPOSE:
        in.kind = FF_INTENT_OPEN_COMPOSE;
        break;
    case FF_APP_FACE_POWER_MENU:
        in.kind = FF_INTENT_POWER_MENU_OPEN;
        break;
    default:
        return false;
    }

    /* Always via HOME first, so every navigation starts from the same
     * known state regardless of what the previous face left routed
     * (e.g. COMPOSE/POWER_MENU are modals over whatever was under
     * them). */
    ff_intent_t const home = {.kind = FF_INTENT_HOME, .u = {0}};
    ff_shell_intent(&s->shell, &home);
    bench_settle(s);

    ff_shell_intent(&s->shell, &in);
    bench_settle(s);
    return s->ctx.state.active_face == face;
}

static void bench_render_key_and_face_build(void)
{
    bench_session_t s;
    if (bench_session_open(&s) != 0) {
        fprintf(stderr, "ff_bench: could not open ctl-loop session — skipping shell_render_key/"
                         "ff_face_build benches\n");
        return;
    }

    char size_note[64];
    snprintf(size_note, sizeof(size_note), "sizeof(ff_app_state_t) = %zu bytes", sizeof(ff_app_state_t));

    /* --- 1. shell_render_key + memcmp, steady state (no dirty bit): -- */
    {
        long const iters = 2000;
        /* One settle to reach steady state (RADAR, nothing changing
         * tick to tick) before the timed loop — the first tick after
         * any navigation is unconditionally dirty (S16 slice d) and
         * would skew the average toward the (much rarer) rebuild path. */
        bench_settle(&s);
        double t0 = bench_now_ms();
        for (long i = 0; i < iters; i++) {
            s.ctx.mock_clock_ms += 50u;
            ff_ctl_loop_pump(&s.ctx); /* dirty bit false every call here: no rebuild, pure tick+key+memcmp */
        }
        double t1 = bench_now_ms();
        bench_report("shell_render_key+memcmp (steady RADAR)", iters, t1 - t0, size_note);
    }

    /* --- 2. ff_face_build (ff_build_face_screen) per face: ----------- */
    static struct {
        ff_app_face_t face;
        char const *name;
    } const faces[] = {
        {FF_APP_FACE_LAUNCHER, "ff_face_build LAUNCHER"},
        {FF_APP_FACE_RADAR, "ff_face_build RADAR"},
        {FF_APP_FACE_LINEUP, "ff_face_build LINEUP"},
        {FF_APP_FACE_INBOX, "ff_face_build INBOX"},
        {FF_APP_FACE_MAP, "ff_face_build MAP"},
        {FF_APP_FACE_SETTINGS, "ff_face_build SETTINGS"},
        {FF_APP_FACE_COMPOSE, "ff_face_build COMPOSE"},
        /* FF_APP_FACE_POWER_MENU deliberately excluded: it's reachable
         * only via FF_INTENT_POWER_MENU_OPEN, which ff_shell.c's own
         * doc comment marks "NOT screen-originated" (device-only, the
         * esp32s3 target's power-button FSM dispatches it directly) —
         * not a face this bench's plain intent-based navigation can
         * reach the same way as the others; the other 7 faces are a
         * representative enough set. */
    };
    for (size_t f = 0; f < sizeof(faces) / sizeof(faces[0]); f++) {
        if (!bench_navigate(&s, faces[f].face)) {
            fprintf(stderr, "ff_bench: could not navigate to %s — skipping\n", faces[f].name);
            continue;
        }
        /* ff_build_face_screen is called directly here (bypassing
         * ff_ctl_loop_pump's dirty-bit gate entirely) so this measures
         * ONLY the LVGL tree build, not tick/idle/rebuild-gate overhead
         * — matching "ff_face_build for each face" precisely. Each call
         * still needs lv_obj_clean() first (mirrors ff_ctl_loop_pump's
         * own contract, ctl_loop.h's doc comment on ff_ctl_loop_pump) so
         * repeated calls don't just keep appending children forever. */
        long const iters = 300;
        double t0 = bench_now_ms();
        for (long i = 0; i < iters; i++) {
            lv_obj_clean(lv_screen_active());
            ff_build_face_screen(&s.ctx.state);
        }
        double t1 = bench_now_ms();
        bench_report(faces[f].name, iters, t1 - t0, NULL);
    }

    bench_session_close(&s);
}

/* ---------------------------------------------------------------------
 * 3. ff_radar_compute, 8 members.
 * ------------------------------------------------------------------- */

static void bench_radar_compute(void)
{
    ff_crew_t crew;
    ff_crew_init(&crew, NULL);
    for (uint32_t i = 0; i < FF_CREW_MAX; i++) {
        uint32_t node = 0x1000u + i;
        ff_crew_member_t *m = ff_crew_upsert(&crew, node);
        snprintf(m->name, sizeof(m->name), "C%02u", (unsigned)i);
        m->initial = (char)('A' + i);
        m->color_idx = (uint8_t)(i % 8u);
        ff_crew_set_paired(&crew, node, true);
        m->has_pos = true;
        m->pos.lat = 41.000 + (double)i * 0.0005;
        m->pos.lon = -84.000 + (double)i * 0.0007;
        m->pos_age_ms = 1000u;
    }
    ff_crew_select_node(&crew, 0x1000u);

    ff_radar_view_t v;
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {41.0, -84.0};

    long const iters = 20000;
    double t0 = bench_now_ms();
    for (long i = 0; i < iters; i++) {
        memset(&v, 0, sizeof(v));
        ff_radar_compute(&v, &sm, &crew, (float)(i % 360), my_pos, true, false, 1000u + (uint32_t)i);
    }
    double t1 = bench_now_ms();
    bench_report("ff_radar_compute (8 members)", iters, t1 - t0, NULL);
}

/* ---------------------------------------------------------------------
 * 4. Map draw-op generation: label placement + polygon triangulation.
 * ------------------------------------------------------------------- */

static void bench_map_draw_ops(void)
{
    /* Label placement: FF_MAP_LABEL_MAX_ITEMS (24) requests spread
     * around the circle — a busy Map face (every stage/landmark/feature
     * label on screen at once), the realistic worst case this function
     * actually has to handle. */
    {
        ff_map_label_request_t req[FF_MAP_LABEL_MAX_ITEMS];
        ff_map_label_result_t out[FF_MAP_LABEL_MAX_ITEMS];
        for (int i = 0; i < FF_MAP_LABEL_MAX_ITEMS; i++) {
            float angle = (float)i * (6.2831853f / (float)FF_MAP_LABEL_MAX_ITEMS);
            req[i].x = 150.0f * cosf(angle);
            req[i].y = 150.0f * sinf(angle);
            req[i].priority = (ff_map_label_priority_t)(i % 3);
            req[i].half_w = 24.0f;
            req[i].half_h = 8.0f;
        }
        long const iters = 5000;
        double t0 = bench_now_ms();
        for (long i = 0; i < iters; i++) {
            (void)ff_map_place_labels(req, FF_MAP_LABEL_MAX_ITEMS, 4.0f, 6, 190.0f, out);
        }
        double t1 = bench_now_ms();
        bench_report("ff_map_place_labels (24 labels)", iters, t1 - t0, NULL);
    }

    /* Triangulation: the real, currently-merged Lost Lands "Venue
     * extent" feature's 9-point concave polygon shape (see
     * ff_map_triangulate's own doc comment) — a representative,
     * non-trivial (concave) real-world input rather than a convex
     * synthetic one a plain fan could also handle. Coordinates are a
     * simple concave "arrow" shape at the same scale (meters), not a
     * copy of the actual vendored fixture (no need for this bench to
     * depend on that exact file's contents surviving future edits). */
    {
        float const pts[9][2] = {
            {0.0f, 0.0f},     {100.0f, 0.0f},  {100.0f, 60.0f}, {60.0f, 60.0f},
            {60.0f, 30.0f},   {40.0f, 30.0f},  {40.0f, 60.0f},  {0.0f, 60.0f},
            {0.0f, 0.0f},
        };
        uint8_t tris[7][3]; /* n - 2 = 7 */
        long const iters = 20000;
        double t0 = bench_now_ms();
        int last_n = 0;
        for (long i = 0; i < iters; i++) {
            last_n = ff_map_triangulate(pts, 9, tris, 7);
        }
        double t1 = bench_now_ms();
        char extra[64];
        snprintf(extra, sizeof(extra), "triangulate() returned %d", last_n);
        bench_report("ff_map_triangulate (9-pt concave)", iters, t1 - t0, extra);
    }
}

/* ---------------------------------------------------------------------
 * 5. T9 prediction per keypress.
 * ------------------------------------------------------------------- */

static void bench_t9_prediction(void)
{
    /* "festival" typed key by key on a real keypad — 2=abc 3=def 4=ghi
     * 5=jkl 6=mno 7=pqrs 8=tuv 9=wxyz — f=3,e=3,s=7,t=8,i=4,v=8,a=2,l=5. */
    uint8_t const digits_full[] = {3, 3, 7, 8, 4, 8, 2, 5};
    size_t const n_full = sizeof(digits_full) / sizeof(digits_full[0]);
    char const *out[16];

    long const iters_per_len = 5000;
    double total_ms = 0.0;
    long total_iters = 0;
    for (size_t len = 1; len <= n_full; len++) {
        double t0 = bench_now_ms();
        for (long i = 0; i < iters_per_len; i++) {
            (void)ff_t9pred_match(digits_full, len, out, 16);
        }
        double t1 = bench_now_ms();
        total_ms += (t1 - t0);
        total_iters += iters_per_len;
    }
    bench_report("ff_t9pred_match (per keypress, 1..8 digits of \"festival\")", total_iters, total_ms, NULL);
}

/* ---------------------------------------------------------------------
 * 6. festpack lookup: ff_sched_now_playing over the real Lost Lands
 * pack.
 * ------------------------------------------------------------------- */

static void bench_festpack_lookup(char const *fixture_path)
{
    static char json_buf[256u * 1024u];
    FILE *f = fopen(fixture_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "ff_bench: could not open %s — skipping festpack lookup bench\n", fixture_path);
        return;
    }
    size_t len = fread(json_buf, 1, sizeof(json_buf), f);
    fclose(f);
    if (len == 0) {
        fprintf(stderr, "ff_bench: %s read as empty — skipping festpack lookup bench\n", fixture_path);
        return;
    }

    static jsmntok_t toks[FP_MAX_TOKENS];
    static fp_pack_t pack;
    fp_result_t r = fp_parse(json_buf, len, &pack, toks, FP_MAX_TOKENS);
    if (r != FP_OK) {
        fprintf(stderr, "ff_bench: fp_parse(%s) failed (%d) — skipping festpack lookup bench\n", fixture_path,
                (int)r);
        return;
    }

    char extra[64];
    snprintf(extra, sizeof(extra), "%u sets in pack", (unsigned)pack.n_sets);

    ff_now_row_t rows[8];
    long const iters = 20000;
    double t0 = bench_now_ms();
    for (long i = 0; i < iters; i++) {
        (void)ff_sched_now_playing(&pack, 1u, (int16_t)(360 + (i % 1440)), rows, 8);
    }
    double t1 = bench_now_ms();
    bench_report("ff_sched_now_playing (festpack lookup)", iters, t1 - t0, extra);
}

/* ---------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char const *fixture_path =
        (argc > 1) ? argv[1] : "festpack/tests/fixtures/lost-lands-2026.festpack.json";

    printf("ff_bench — S14 hardening pass sim micro-benchmarks\n");
    printf("(POSIX CLOCK_MONOTONIC; single-threaded; run on a quiet machine — these are\n"
           " relative-comparison numbers, not an SLA. See this PR's body for a before/\n"
           " after table.)\n\n");

    bench_render_key_and_face_build();
    bench_radar_compute();
    bench_map_draw_ops();
    bench_t9_prediction();
    bench_festpack_lookup(fixture_path);

    return 0;
}
