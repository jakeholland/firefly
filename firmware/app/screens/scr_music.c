/**
 * scr_music.c — see scr_music.h. Pure render + one per-frame ticker: no
 * domain logic (CLAUDE.md) — every branch below is "how to draw the
 * swarm/chrome for THIS state", never "should the swarm look like
 * this" (that call is `ff_beat_t`/`ff_swarm_t`'s, both firmware/core).
 *
 * ## Renderer choice: pre-created lv_obj circles, not a canvas
 * Reasoned from the Map face's own measured numbers (`scr_map.c`'s top
 * comment): that face's draw-op pool exists because CREATING ~300
 * `lv_obj_t` synchronously during a full-screen rebuild measurably
 * stalled the touch-poll loop by ~520ms — a REBUILD cost, not a
 * per-frame one. This face's problem is different in kind: 60 particles
 * moving at up to 30fps, but the shell's render-key rebuild
 * (`lv_obj_clean` + rebuild) happens RARELY (S16's own churn budget —
 * the particle state is deliberately kept OUT of the render key, see
 * `ff_shell.c`'s `shell_render_key` comment on `music.*`), so the
 * "hundreds of objects created at once" cost Map hit essentially never
 * recurs here. What DOES happen every frame is 60 property MUTATIONS
 * (`lv_obj_set_pos`/`_set_size`/`_set_style_bg_opa`) on already-existing
 * objects — no `lv_obj_create`/`_delete`, no style-tree rebuild, just an
 * invalidate-and-redraw of a handful of small filled circles. LVGL's own
 * cost for that shape of update is dominated by the actual pixel fill
 * (60 circles of a few px radius each — a few hundred px^2 total, a
 * small fraction of the 412x412 panel) plus 60 cheap struct-field
 * writes, comfortably inside an 8ms/frame budget on the S3's 240MHz
 * core (measured precedent: Radar's own live arrow+ring redraw, a
 * similar-order-of-magnitude per-frame object count, already runs at
 * interactive rates on this hardware). An `lv_canvas` would trade this
 * for one large buffer clear + 60 software-rasterized fills per frame —
 * strictly MORE per-frame work for a particle count this small, and
 * loses LVGL's own dirty-rect invalidation (only the circles that
 * actually moved get redrawn) for zero benefit. Plain persistent
 * `lv_obj_t`s is the right call here; a canvas would be the right call
 * at a particle count high enough that per-object overhead dominates
 * (not 60).
 *
 * ## Per-frame mechanism: this file's own lv_timer, not
 * ff_face_dispatch_tick
 * Per docs/specs/S31-music-swarm.md's own instruction ("Follow the
 * Map's approach: the face rebuild happens on view changes only;
 * per-frame particle drawing runs from an LVGL timer inside the
 * screen"): `ff_scr_music_build` creates ONE `lv_timer_t` (deleted
 * automatically when this screen's content is torn down — the same
 * self-nulling `LV_EVENT_DELETE` contract `scr_flare.c`'s sender
 * countdown label already establishes for a per-build LVGL resource).
 * The timer reads the LIVE `ff_app_state_t const *state` pointer it was
 * built with (stable across frames — it is `&sh->view`, the same
 * pointer `shell_project` rewrites in place every tick, never
 * reallocated — the identical "read straight off the live view,
 * outside the dirty path" trick `ff_face_dispatch_tick` uses for the
 * flare sender's countdown chip) for `music.loudness`/`music.
 * beat_count`/`radar.batt_pct`, advances this file's own `ff_swarm_t`
 * (core, deterministic — see ff_swarm.h), and redraws the dot pool.
 *
 * ## Golden determinism without ever calling lv_timer_handler
 * The sim's one-shot headless renderer (`targets/sim/main.c`'s
 * `ff_run_headless_once`) builds a fixture and calls `lv_refr_now()`
 * ONCE — it never drains LVGL's timer queue at all (see that function's
 * own top comment: "a single lv_refr_now() call with no timers run...
 * never reads the tick"). So this file's lv_timer NEVER fires for a
 * golden. `ff_scr_music_build` therefore does its own ONE deterministic
 * "settle" step — `ff_swarm_init` then exactly one `ff_swarm_step` at a
 * fixed frame duration, fed `state->music.loudness` and "was there
 * already a beat" (`state->music.beat_count != 0`) — so the very FIRST
 * painted frame already shows the fixture's specified loudness/beat
 * state (quiet vs. loud vs. mid-flare), fully reproducibly from
 * `state->music.seed` alone, with no dependency on the timer ever
 * running. Live operation (device + interactive sim) continues
 * stepping from exactly that same settled state once the timer starts
 * firing for real.
 */
#include "scr_music.h"

#include <math.h>
#include <stdio.h>

#include "ff_beat.h"
#include "ff_swarm.h"
#include "ff_theme.h"
#include "lvgl.h"

/* One simulated frame's worth of time for the build-time "settle" step
 * — see this file's top comment, "Golden determinism". Matches the
 * normal (non-battery-throttled) frame period so a golden's one settled
 * frame looks like an ordinary live frame, not a fast-forwarded one. */
#define FF_SCR_MUSIC_SETTLE_DT_S (1.0f / 30.0f)

/* Frame cap — docs/specs/S31-music-swarm.md's "Frame budget": 30fps
 * normally, 15fps once battery is a KNOWN reading at or below 20%. An
 * unknown battery (`batt_pct < 0`, no ADC on this target) is NOT
 * treated as low — same "unknown is not low" convention `ff_radar_batt_
 * is_low` (ff_radar.h) already applies everywhere else in this app. */
#define FF_SCR_MUSIC_FPS_NORMAL 30u
#define FF_SCR_MUSIC_FPS_LOW_BATT 15u
#define FF_SCR_MUSIC_LOW_BATT_PCT 20

/* Particle dot size/opacity range — small firefly points, never so
 * large they read as a different kind of UI element. Interpretation
 * call, flagged per AGENTS.md — no mockup artboard is in-tree for this
 * face (CLAUDE.md: "screen mockups... live as Claude artifacts (ask
 * Jake)"). */
#define FF_SCR_MUSIC_DOT_MIN_PX 3
#define FF_SCR_MUSIC_DOT_MAX_PX 8
/* Keep every particle's rendered center comfortably inside the glass
 * radius even at r == 1.0, so a dot's own radius never pokes past the
 * bezel. */
#define FF_SCR_MUSIC_MAX_RADIUS_PX ((float)(FF_THEME_GLASS_R - 12))

static ff_swarm_t s_swarm;
static lv_obj_t *s_dots[FF_SWARM_PARTICLE_COUNT];
static uint32_t s_last_beat_count;
static uint32_t s_last_tick_ms;
static uint32_t s_period_ms;

/* Same 0deg-is-top, clockwise convention `scr_launcher.c`'s own
 * `launcher_deg_to_offset` uses (this file's independent copy — see
 * that function's own comment for why a shared helper isn't worth it
 * for one three-line rotation). Particles are ordinary content, not an
 * edge-hugging element, so they use the plain puck centre — not the
 * flip-aware glass centre — per `ff_theme.h`'s own "content away from
 * the edge keeps using the puck centre" guidance. */
static void music_deg_to_offset(float deg, float radius_px, float *dx, float *dy)
{
    float const rad = deg * ((float)M_PI / 180.0f);
    *dx = radius_px * sinf(rad);
    *dy = -radius_px * cosf(rad);
}

/* Applies the current `s_swarm` state to the already-created dot pool.
 * Shared verbatim by the build-time initial placement and every later
 * timer tick — one code path, so the two can never draw differently for
 * the same state. */
static void music_redraw_dots(void)
{
    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        lv_obj_t *dot = s_dots[i];
        if (dot == NULL) continue;
        ff_swarm_particle_t const *p = &s_swarm.particles[i];

        float dx, dy;
        music_deg_to_offset(p->theta_deg, p->r * FF_SCR_MUSIC_MAX_RADIUS_PX, &dx, &dy);

        float const glow = (p->glow < 0.0f) ? 0.0f : ((p->glow > 1.0f) ? 1.0f : p->glow);
        int32_t const diam = FF_SCR_MUSIC_DOT_MIN_PX +
                              (int32_t)(glow * (float)(FF_SCR_MUSIC_DOT_MAX_PX - FF_SCR_MUSIC_DOT_MIN_PX));
        lv_opa_t const opa = (lv_opa_t)(60.0f + glow * 195.0f);

        lv_obj_set_size(dot, diam, diam);
        lv_obj_align(dot, LV_ALIGN_CENTER, (int32_t)dx, (int32_t)dy);
        lv_obj_set_style_bg_opa(dot, opa, 0);
    }
}

static uint32_t music_period_ms(int8_t batt_pct)
{
    bool const low = (batt_pct >= 0) && (batt_pct <= FF_SCR_MUSIC_LOW_BATT_PCT);
    uint32_t const fps = low ? FF_SCR_MUSIC_FPS_LOW_BATT : FF_SCR_MUSIC_FPS_NORMAL;
    return 1000u / fps;
}

static void music_timer_cb(lv_timer_t *timer)
{
    ff_app_state_t const *state = (ff_app_state_t const *)lv_timer_get_user_data(timer);
    if (state == NULL) return;

    uint32_t const now = lv_tick_get();
    uint32_t const elapsed_ms = now - s_last_tick_ms; /* wraparound-safe unsigned subtraction over a short interval */
    s_last_tick_ms = now;
    if (elapsed_ms == 0u) return;

    /* A new beat is "beat_count moved since the last frame we drew",
     * not a transient per-tick edge flag — see ff_beat.h's own doc
     * comment for why: this timer polls at 15-30fps, slower than the
     * shell's own tick rate, so a bare edge flag sampled here could
     * miss a beat entirely between two polls. */
    bool const beat_now = (state->music.beat_count != s_last_beat_count);
    s_last_beat_count = state->music.beat_count;

    ff_swarm_step(&s_swarm, state->music.loudness, beat_now, (float)elapsed_ms / 1000.0f);
    music_redraw_dots();

    uint32_t const want_period = music_period_ms(state->radar.batt_pct);
    if (want_period != s_period_ms) {
        s_period_ms = want_period;
        lv_timer_set_period(timer, want_period);
    }
}

/* Self-nulling delete hook, same contract as scr_flare.c's own
 * `flare_sender_countdown_lbl_delete_cb`: fires when the shell tears
 * down this face's content (the next `lv_obj_clean(lv_screen_active())`
 * before a different face builds), so this file never holds a dangling
 * `lv_timer_t*`/dot pointers across a rebuild. */
static void music_content_delete_cb(lv_event_t *e)
{
    lv_timer_t *timer = (lv_timer_t *)lv_event_get_user_data(e);
    if (timer != NULL) {
        lv_timer_delete(timer);
    }
    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        s_dots[i] = NULL;
    }
}

static char const *music_source_text(ff_app_music_src_t source)
{
    switch (source) {
    case FF_APP_MUSIC_SRC_MIC: return "MIC";
    case FF_APP_MUSIC_SRC_IMU: return "IMU";
    case FF_APP_MUSIC_SRC_NONE:
    default: return "NO SOURCE";
    }
}

static uint32_t music_source_color(ff_app_music_src_t source)
{
    switch (source) {
    case FF_APP_MUSIC_SRC_MIC: return FF_THEME_COLOR_LIVE_GREEN; /* a working audio source — the same "it's real" green LIVE chips use elsewhere */
    case FF_APP_MUSIC_SRC_IMU: return FF_THEME_COLOR_AMBER;
    case FF_APP_MUSIC_SRC_NONE:
    default: return FF_THEME_COLOR_MUTED; /* honest absence "stays calm" — never an alarm color, per the concept sheet */
    }
}

void ff_scr_music_build(ff_app_state_t const *state)
{
    if (state == NULL) return;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE);

    /* Deterministic swarm (re)seed + one build-time settle step — see
     * this file's top comment, "Golden determinism". */
    ff_swarm_init(&s_swarm, state->music.seed);
    s_last_beat_count = state->music.beat_count;
    bool const beat_for_settle = (state->music.beat_count != 0u);
    ff_swarm_step(&s_swarm, state->music.loudness, beat_for_settle, FF_SCR_MUSIC_SETTLE_DT_S);

    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(puck);
        lv_obj_remove_style_all(dot);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        s_dots[i] = dot;
    }
    music_redraw_dots();

    /* Chrome, drawn AFTER the dots so it paints on top — same
     * build-order-is-z-order convention every other face in this
     * directory uses. */
    lv_obj_t *clock_lbl = lv_label_create(puck);
    lv_label_set_text(clock_lbl, state->radar.clock_str[0] != '\0' ? state->radar.clock_str : "--:--");
    lv_obj_set_style_text_font(clock_lbl, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(clock_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(clock_lbl, LV_ALIGN_CENTER, 0, -18);

    /* QUIET/LOUD word — the SAME threshold (FF_BEAT_LOUD_THRESHOLD,
     * ff_beat.h) ff_shell.c's render key buckets `music.loudness`
     * against, so a rebuild only ever happens when this word would
     * actually change (see shell_render_key's own comment on
     * `music.loudness`) — this word is never stale by construction. */
    bool const loud = state->music.loudness >= FF_BEAT_LOUD_THRESHOLD;
    lv_obj_t *word_lbl = lv_label_create(puck);
    lv_label_set_text(word_lbl, loud ? "LOUD" : "QUIET");
    lv_obj_set_style_text_font(word_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(word_lbl, lv_color_hex(loud ? FF_THEME_COLOR_AMBER : FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(word_lbl, 2, 0);
    lv_obj_align(word_lbl, LV_ALIGN_CENTER, 0, 8);

    /* Source chip — S31's honesty rule: always shown, never invented,
     * calm (never an alarm color) when there is genuinely no source. */
    lv_obj_t *chip_lbl = lv_label_create(puck);
    lv_label_set_text(chip_lbl, music_source_text(state->music.source));
    lv_obj_set_style_text_font(chip_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(chip_lbl, lv_color_hex(music_source_color(state->music.source)), 0);
    lv_obj_set_style_text_letter_space(chip_lbl, 1, 0);
    lv_obj_align(chip_lbl, LV_ALIGN_CENTER, 0, 28);

    /* Per-frame ticker — see this file's top comment, "Per-frame
     * mechanism". `state` is `&sh->view` (the live shell's own
     * projection, rewritten in place every tick, never reallocated) on
     * both real targets; the sim's headless-once golden path also
     * passes a stable pointer for the single settle step above, and
     * never drains the timer queue at all (see "Golden determinism"). */
    s_period_ms = music_period_ms(state->radar.batt_pct);
    lv_timer_t *timer = lv_timer_create(music_timer_cb, s_period_ms, (void *)state);
    s_last_tick_ms = lv_tick_get();
    lv_obj_add_event_cb(puck, music_content_delete_cb, LV_EVENT_DELETE, (void *)timer);
}
