/**
 * scr_music.c — see scr_music.h. Pure render + one per-frame ticker: no
 * domain logic (CLAUDE.md) — every branch below is "how to draw the
 * swarm/chrome for THIS state", never "should the swarm look like
 * this" (that call is `ff_beat_t`/`ff_swarm_t`'s, both firmware/core).
 *
 * ## Renderer choice: pre-created lv_obj circles, not a canvas, not a
 * complex gradient
 * Reasoned from the Map face's own measured numbers (`scr_map.c`'s top
 * comment): that face's draw-op pool exists because CREATING ~300
 * `lv_obj_t` synchronously during a full-screen rebuild measurably
 * stalled the touch-poll loop by ~520ms — a REBUILD cost, not a
 * per-frame one. This face's problem is different in kind: the swarm
 * moves at up to 30fps, but the shell's render-key rebuild
 * (`lv_obj_clean` + rebuild) happens RARELY (S16's own churn budget —
 * the particle state is deliberately kept OUT of the render key, see
 * `ff_shell.c`'s `shell_render_key` comment on `music.*`), so the
 * "hundreds of objects created at once" cost Map hit essentially never
 * recurs here. What DOES happen every frame is property MUTATIONS
 * (`lv_obj_set_size`/`lv_obj_align`/`_set_style_bg_opa`) on already-
 * existing objects — no `lv_obj_create`/`_delete`, no style-tree
 * rebuild.
 *
 * Each firefly is TWO such objects, not one: a small ink-white core dot
 * plus one bigger, dimmer halo ring (the accent color) behind it,
 * approximating the soft radial-gradient glow the coordinator's concept
 * mockup draws with a canvas `radialGradient` — `lv_conf.h` does not
 * set `LV_USE_DRAW_SW_COMPLEX_GRADIENTS` (checked; it is LVGL's own
 * default-off complex-gradient path), so a real per-object radial
 * gradient style is not available here without a new lv_conf.h flag
 * (and the matching `sdkconfig` key on the device side — out of scope
 * for a polish pass); one falling-opacity halo per firefly is the
 * documented fallback (docs/specs/S31-music-swarm.md's "Frame budget"
 * amendment) and reads as a soft glow at this size on glass.
 *
 * ## Object count is bounded by the LVGL heap, not just per-frame cost
 * A THREE-object-per-firefly design (core + two halo rings, 180 objects
 * total) was tried and measured to CRASH: this target's LVGL heap is a
 * fixed 64KB arena (`CONFIG_LV_MEM_SIZE_KILOBYTES=64` in the esp32s3
 * `sdkconfig.defaults`, mirrored by the sim's own LVGL default — see
 * lv_conf.h's own top comment, "every option not listed here falls back
 * to the documented default"), not a per-frame CPU budget question at
 * all. A standalone probe against this exact LVGL build (a plain
 * `lv_obj_create` + a few local style props + one `lv_obj_align`, the
 * shape every firefly object here takes) measured ~230-280 bytes PER
 * OBJECT once `spec_attr` (children/align bookkeeping, allocated lazily
 * on an object's first `lv_obj_align` call) is counted — 180 such
 * objects alone consumes essentially the entire 64KB arena before the
 * screen's own clock/chip labels (each needing their own allocation)
 * ever get a chance to run, and running `test_gesture_glue`'s
 * `S31_back_on_music_goes_home` against a real 180-object build
 * reproduced exactly that: `lv_realloc` returning NULL inside
 * `lv_obj_class_create_obj` while creating the CLOCK label, a plain
 * unchecked-NULL segfault two objects later. Dropping to TWO objects
 * per firefly (120 total, still inside the "2-3 concentric circles"
 * fallback range docs/specs/S31-music-swarm.md's own "Frame budget"
 * amendment allows) leaves comfortable headroom for the rest of the
 * screen's chrome — verified against the same probe methodology, and
 * pinned as a live regression by `test_gesture_glue.c`'s own
 * `S31_back_on_music_goes_home`, which now actually EXERCISES a full
 * Music build+teardown instead of merely asserting on `active_face`.
 * Per-frame CPU cost is unaffected by this — see "Per-frame cost
 * estimate" below, using the actual 120-object count.
 *
 * ## Per-frame cost estimate
 * 120 small filled-circle redraws (core radius ~2-9px, halo ~4-20px —
 * still a small fraction of the 412x412 panel's pixels in total) plus 2
 * cheap struct-field writes each (240 total) is the same order of
 * magnitude as the original 60-object design's "comfortably inside 8ms"
 * budget at Radar's own measured per-frame object count — 120 is 2x
 * that, not a new order of magnitude. A canvas would trade this for one
 * large buffer clear + 120 software-rasterized fills per frame
 * (strictly MORE per-frame work at this count, and loses LVGL's own
 * dirty-rect invalidation) — the same call the original design made,
 * unchanged by going from 1 to 2 objects per firefly. A canvas WOULD
 * sidestep the LVGL-heap ceiling above (a canvas needs one object plus
 * one pixel buffer, not N objects) — flagged here as the fallback this
 * file would reach for if a future change ever needs a third
 * object-per-firefly again; not needed at 120.
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

/* Core dot radius, px — the task's own formula (3 + 10*glow), on the
 * coordinator's 600px mockup canvas, scaled onto this puck's 412px
 * glass by the SAME `FF_SWARM_MOCK_TO_PUCK_SCALE` every other absolute
 * pixel constant in `ff_swarm.h` uses (see that header's own "Scale"
 * comment) — one shared scale factor, not a second copy of it. */
#define FF_SCR_MUSIC_CORE_R_BASE_PX 3.0f
#define FF_SCR_MUSIC_CORE_R_GAIN_PX 10.0f

/* Halo ring radius, as a multiple of the core radius this step — "a
 * soft halo... about 2.2x the core radius" (the task's own number). One
 * ring, not two (see this file's top comment, "Object count is bounded
 * by the LVGL heap") — a two-ring falloff read marginally smoother but
 * doubled the halo's own object cost for a difference this small on a
 * 412px glass. */
#define FF_SCR_MUSIC_HALO_MULT 2.2f

/* Opacity curves (lv_opa_t, 0-255), all glow-driven, each an
 * interpretation call flagged per AGENTS.md (no mockup pins exact
 * alpha values — canvas `globalAlpha`/gradient stops don't translate
 * 1:1 to LVGL object opacity). Core: "ink white at 60-100% opacity"
 * (the task's own range) — 153/255 == 60%, 255/255 == 100%. The halo
 * is fainter, so it reads as a soft glow rather than a second
 * hard-edged dot. */
#define FF_SCR_MUSIC_CORE_OPA_BASE 153.0f
#define FF_SCR_MUSIC_CORE_OPA_GAIN 102.0f
#define FF_SCR_MUSIC_HALO_OPA_GAIN 120.0f

/* Faint static ring, r~250 on the coordinator's mockup (scaled the
 * same way) — drawn once at build time in the surface color, never
 * animated. */
#define FF_SCR_MUSIC_RING_R_PX (250.0f * FF_SWARM_MOCK_TO_PUCK_SCALE) /* ~171.7px */

/* Source chip position — "a small muted chip at the bottom" (S31
 * polish, owner feedback on PR #245 — see scr_music.h's own top
 * comment). Comfortably inside FF_THEME_GLASS_R (200) so it's never
 * clipped under the bezel. Interpretation call, flagged per AGENTS.md —
 * no mockup pins an exact margin. */
#define FF_SCR_MUSIC_CHIP_BOTTOM_MARGIN_PX 34

typedef struct {
    lv_obj_t *core;
    lv_obj_t *halo;
} music_dot_t;

static ff_swarm_t s_swarm;
static music_dot_t s_dots[FF_SWARM_PARTICLE_COUNT];
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
 * the same state. Colors are set once at BUILD time (fixed per
 * particle — accent color never changes), so only size/position/
 * opacity mutate here, per this file's own top-comment cost reasoning. */
static void music_redraw_dots(void)
{
    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        music_dot_t const *dot = &s_dots[i];
        if (dot->core == NULL) continue;
        ff_swarm_particle_t const *p = &s_swarm.particles[i];

        float dx, dy;
        music_deg_to_offset(p->theta_deg, p->r_px, &dx, &dy);
        int32_t const dx_i = (int32_t)dx;
        int32_t const dy_i = (int32_t)dy;

        float const glow = (p->glow < 0.0f) ? 0.0f : ((p->glow > 1.0f) ? 1.0f : p->glow);
        float const core_r = (FF_SCR_MUSIC_CORE_R_BASE_PX + FF_SCR_MUSIC_CORE_R_GAIN_PX * glow)
                              * FF_SWARM_MOCK_TO_PUCK_SCALE;
        int32_t const core_d = (int32_t)(2.0f * core_r);
        int32_t const halo_d = (int32_t)(2.0f * core_r * FF_SCR_MUSIC_HALO_MULT);

        lv_opa_t const core_opa = (lv_opa_t)(FF_SCR_MUSIC_CORE_OPA_BASE + glow * FF_SCR_MUSIC_CORE_OPA_GAIN);
        lv_opa_t const halo_opa = (lv_opa_t)(glow * FF_SCR_MUSIC_HALO_OPA_GAIN);

        lv_obj_set_size(dot->core, core_d, core_d);
        lv_obj_align(dot->core, LV_ALIGN_CENTER, dx_i, dy_i);
        lv_obj_set_style_bg_opa(dot->core, core_opa, 0);

        lv_obj_set_size(dot->halo, halo_d, halo_d);
        lv_obj_align(dot->halo, LV_ALIGN_CENTER, dx_i, dy_i);
        lv_obj_set_style_bg_opa(dot->halo, halo_opa, 0);
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
        s_dots[i].core = NULL;
        s_dots[i].halo = NULL;
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
    case FF_APP_MUSIC_SRC_MIC: return FF_THEME_COLOR_LIVE_GREEN; /* a working audio source — the same "it's real" green LIVE chips use elsewhere; unreachable in practice since the chip is not built at all while source == MIC (see ff_scr_music_build) — kept for switch-completeness/-Wswitch */
    case FF_APP_MUSIC_SRC_IMU: return FF_THEME_COLOR_AMBER;
    case FF_APP_MUSIC_SRC_NONE:
    default: return FF_THEME_COLOR_MUTED; /* honest absence "stays calm" — never an alarm color, per the concept sheet */
    }
}

/* Creates one firefly's 2-object pool (core + one halo ring) as a
 * child of `puck`, with its FIXED (never-mutated-again) accent color —
 * live-green for every `FF_SWARM_GREEN_EVERY_N`th firefly, amber
 * otherwise (`ff_swarm_particle_t.is_accent_green`, a core/domain fact
 * — see ff_swarm.h). The halo is created BEFORE the core so the core
 * paints on top (build-order-is-z-order, this directory's usual
 * convention). */
static void music_create_dot(lv_obj_t *puck, ff_swarm_particle_t const *p, music_dot_t *out)
{
    uint32_t const accent_color = p->is_accent_green ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_AMBER;

    lv_obj_t *halo = lv_obj_create(puck);
    lv_obj_remove_style_all(halo);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(halo, lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *core = lv_obj_create(puck);
    lv_obj_remove_style_all(core);
    lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(core, lv_color_hex(FF_THEME_COLOR_INK), 0); /* "core dot in ink white" */
    lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
    lv_obj_clear_flag(core, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(core, LV_OBJ_FLAG_CLICKABLE);

    out->halo = halo;
    out->core = core;
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

    /* Faint static ring (r~250 on the mockup, scaled) — drawn first so
     * everything else paints on top of it. */
    lv_obj_t *ring = lv_obj_create(puck);
    lv_obj_remove_style_all(ring);
    int32_t const ring_d = (int32_t)(2.0f * FF_SCR_MUSIC_RING_R_PX);
    lv_obj_set_size(ring, ring_d, ring_d);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    /* Deterministic swarm (re)seed + one build-time settle step — see
     * this file's top comment, "Golden determinism". */
    ff_swarm_init(&s_swarm, state->music.seed);
    s_last_beat_count = state->music.beat_count;
    bool const beat_for_settle = (state->music.beat_count != 0u);
    ff_swarm_step(&s_swarm, state->music.loudness, beat_for_settle, FF_SCR_MUSIC_SETTLE_DT_S);

    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        music_create_dot(puck, &s_swarm.particles[i], &s_dots[i]);
    }
    music_redraw_dots();

    /* Chrome, drawn AFTER the dots so it paints on top — same
     * build-order-is-z-order convention every other face in this
     * directory uses. The clock stays centred in ink; no QUIET/LOUD
     * word (S31 polish — see scr_music.h's own top comment). */
    lv_obj_t *clock_lbl = lv_label_create(puck);
    lv_label_set_text(clock_lbl, state->radar.clock_str[0] != '\0' ? state->radar.clock_str : "--:--");
    lv_obj_set_style_text_font(clock_lbl, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(clock_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(clock_lbl, LV_ALIGN_CENTER, 0, 0);

    /* Source chip — ONLY when the source is not the mic (the S31
     * polish narrowing of the original "always shown" rule — see
     * scr_music.h's own "S31 polish" comment for the owner's call and
     * the honesty reasoning). With the mic running, nothing else draws
     * here at all: just the clock and the swarm. */
    if (state->music.source != FF_APP_MUSIC_SRC_MIC) {
        lv_obj_t *chip_lbl = lv_label_create(puck);
        lv_label_set_text(chip_lbl, music_source_text(state->music.source));
        lv_obj_set_style_text_font(chip_lbl, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(chip_lbl, lv_color_hex(music_source_color(state->music.source)), 0);
        lv_obj_set_style_text_letter_space(chip_lbl, 1, 0);
        lv_obj_align(chip_lbl, LV_ALIGN_BOTTOM_MID, 0, -FF_SCR_MUSIC_CHIP_BOTTOM_MARGIN_PX);
    }

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
