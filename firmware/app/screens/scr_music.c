/**
 * scr_music.c — see scr_music.h. Pure render + one per-frame ticker: no
 * domain logic (CLAUDE.md) — every branch below is "how to draw the
 * swarm/chrome for THIS state", never "should the swarm look like
 * this" (that call is `ff_beat_t`/`ff_swarm_t`'s, both firmware/core).
 *
 * ## Renderer: one `lv_canvas`, not 120 `lv_obj_t` circles (2026-09-09
 * S31 canvas renderer, superseding the S31-polish PR #247 design below)
 * The PR #247 design (a small ink-white core dot plus one halo ring per
 * firefly, 120 `lv_obj_t` mutated in place every frame — see
 * docs/specs/S31-music-swarm.md's now-superseded "Frame budget /
 * renderer choice" section) reasoned that 120 property MUTATIONS on
 * already-existing objects would cost the same order of magnitude as
 * Radar's own "comfortably inside 8ms" per-frame budget. Jake's own
 * field measurement (puck main 3ecaae7, `perf` console, Music face
 * open) proved that reasoning wrong: `lvgl_refresh min_us=128
 * avg_us=144705 max_us=265787 n=22` over a 5s window — 145ms/frame,
 * ~7fps, worst frames at 266ms. The 120-object mutation cost was NOT
 * "the same order of magnitude" as 60 objects; LVGL's own per-object
 * style/style-cache lookups, invalidation-area unions, and draw-call
 * dispatch overhead do not scale linearly with a flat "cheap struct
 * writes" model, and 120 small overlapping circles (each its own
 * invalidated area, each its own draw call) is exactly the shape LVGL's
 * object model costs the most for. This is WHY the firefly animation
 * "got less dynamic" after PR #247 (issue as reported): every motion
 * parameter (drift, twinkle, beat pull) still ran correctly underneath
 * — at 7fps, everything built on top of it reads as sluggish regardless
 * of how lively the underlying numbers are.
 *
 * The fix is exactly the fallback this file's own PR #247 comment
 * already flagged as the reach-for-next move ("A canvas WOULD sidestep
 * the LVGL-heap ceiling... not needed at 120" — it turned out to be
 * needed for the FRAME-TIME ceiling instead): ONE `lv_canvas` sized to
 * the glass (`FF_SCR_MUSIC_CANVAS_PX` == `FF_THEME_PUCK_PX`, 412x412),
 * backed by a single RGB565 pixel buffer (`s_canvas_buf`) allocated
 * ONCE, kept for the process lifetime (never re-allocated per Music
 * session — a fresh 339KB alloc/free pair on every launcher-to-Music
 * bounce is needless churn for a buffer this cheap to keep resident).
 * RGB565 (2 bytes/px), not this build's native display depth (32bpp
 * sim / 16bpp device via `LV_COLOR_DEPTH`) — `lv_canvas`'s color format
 * is independent of the display's; LVGL's own image blit converts
 * during composite. On the esp32s3 target the buffer is
 * `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` — PSRAM, NEVER this
 * target's fixed 64KB LVGL heap arena (the exact arena PR #247's own
 * 120-object design was sized against, `CONFIG_LV_MEM_SIZE_KILOBYTES`,
 * sdkconfig.defaults) — so this renderer is bounded by PSRAM (typically
 * several MB), not that 64KB ceiling, with enormous headroom. The sim
 * build (no PSRAM concept) uses plain `malloc`.
 *
 * Every frame: clear the buffer to the theme background color plus the
 * faint static ring baked in as directly-written pixels (`music_clear_
 * canvas`/`music_draw_ring` — the ring is no longer a separate `lv_obj`;
 * see "Chrome" below), then additively composite each of the 60
 * fireflies (`music_composite_particle`) from one of four PRE-RENDERED
 * "glow sprite" shapes (`music_build_sprites`, called once ever, guarded
 * by `s_sprites_ready` — a pure function of fixed constants, so every
 * run produces bit-identical sprites) — a core-white blob plus a
 * radially-falling-off accent-tinted halo ring baked into one small
 * (45x45) RGB565-plus-8-bit-alpha buffer per (glow-quartile, accent
 * color) pair, 8 sprites total, ~47.5KB (`music_sprite_t` is 6075
 * bytes: 2025px * (2-byte rgb565 + 1-byte alpha)). fix/s31-sprites-
 * psram (2026-09-09): that table used to be a plain `static` — ordinary
 * internal `.bss` — and on the esp32s3 target internal RAM is also
 * where `esp_lvgl_port`'s own (DMA-capable) display buffers must come
 * from; 47.5KB of sprite table pushed that allocation below what fit,
 * and the device parked on the boot splash forever
 * (`lvgl_port_add_disp_priv(389): Not enough memory for LVGL buffer`,
 * bench evidence on main a853bb5, PR #252). `s_sprites` is now a
 * pointer, lazily `heap_caps_calloc`'d from PSRAM on first use
 * (`music_ensure_sprites`, NULL-safe exactly like `s_canvas_buf`/
 * `music_ensure_canvas_buf` just below — same posture, same reasoning:
 * this data is read every frame but never DMA'd to a display
 * controller, so PSRAM's higher access latency costs nothing a human
 * can see). The sim build uses plain `calloc`. Only the SPRITE SIZE is
 * quantized to one of 4 glow buckets; the per-firefly
 * BRIGHTNESS still varies continuously (`scale = glow / bucket_center`,
 * applied to the sprite's own per-pixel alpha at blit time) so there is
 * no visible brightness banding — only a <=3px size step between
 * adjacent buckets, imperceptible given how slowly glow actually moves
 * frame to frame. Blitting is a direct, unlocked pixel loop into the
 * raw buffer (`music_canvas_add_px`, integer-only additive blend with
 * saturation, no floating point, no division) — "direct pixel writes in
 * a tight loop", not `lv_canvas_set_px` per pixel (that API's own doc
 * comment: "invalidates the canvas object every time" unless
 * invalidation is disabled around the whole loop, whereas this file
 * writes the raw buffer and invalidates ONCE at the end regardless).
 * One clear (169,744px) + one ring (~1,080px) + up to 60 sprite blits
 * (45x45 bounding box each, early-exiting on zero alpha) is the same
 * "a full 412x412 RGB565 clear + N sprite blits" shape the S31 canvas
 * renderer's own perf estimate is built from — see this PR's dated
 * amendment to docs/specs/S31-music-swarm.md for the measured sim
 * per-frame cost and the honest device estimate derived from it.
 *
 * ## Object count: ONE, not bounded by anything this file used to worry
 * about
 * The whole "180 objects crash the 64KB LVGL heap, so ship 120" story
 * (PR #247's own top-comment section, preserved in git history but
 * removed from this file since it no longer describes anything true
 * here) is moot: this renderer allocates exactly one `lv_obj_t` for the
 * canvas plus the two chrome labels (clock, chip) — three objects total,
 * regardless of firefly count. A future change wanting MORE visual
 * complexity per firefly (a second halo ring, a different falloff
 * shape) costs zero additional `lv_obj_t` — it is purely a sprite/blit
 * change, not an LVGL-heap negotiation.
 *
 * ## Chrome: the ring moved INTO the canvas
 * The faint static ring (`FF_SCR_MUSIC_RING_R_PX`, ~250px on the
 * mockup, scaled) used to be its own `lv_obj` (a transparent circle with
 * a 1px border), drawn UNDER the dot pool. A canvas is drawn as ONE
 * opaque `lv_image` over whatever the puck shows beneath it — an RGB565
 * canvas has no per-pixel destination alpha, so anything still living
 * as a separate `lv_obj` UNDER the canvas would simply be painted over
 * and vanish. The ring is therefore baked into the canvas buffer itself
 * every frame (`music_draw_ring`, a plain one-point-per-circumference-
 * pixel polar walk, no anti-aliasing — matching the previous border's
 * own crisp 1px look) rather than kept as a separate object. The clock
 * label and the source chip remain ordinary `lv_obj_t` labels, created
 * AFTER (so painted ON TOP of) the canvas — same "build order is z
 * order" convention this directory always uses; they never needed to
 * be canvas pixels since neither one animates every frame.
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
 * (core, deterministic — see ff_swarm.h), and redraws the canvas.
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
 * running. That settle-step draw does NOT feed the frame-period/canvas-
 * draw-time rolling stats below (`music_frame_stats_reset` runs right
 * after it) — those stats describe REAL per-frame timer ticks, and a
 * golden never produces any. Live operation (device + interactive sim)
 * continues stepping from exactly that same settled state once the
 * timer starts firing for real.
 *
 * ## Instrumentation: frame period + canvas draw time, for the NEXT
 * report to have numbers
 * `s_frame_stats` accumulates, over each real per-frame timer tick, the
 * elapsed wall time since the previous tick (`elapsed_ms`, already
 * computed below for the beat-detection dt) and the wall time
 * `music_redraw_canvas` itself took (`music_now_us`, a POSIX
 * `clock_gettime(CLOCK_MONOTONIC,...)` on the sim / `esp_timer_get_
 * time()` on the esp32s3 target — the same "shared clock SEAM, two
 * platform bodies" shape `ff_shell.c`'s own `FF_TARGET_SIM` branches
 * already use elsewhere in `firmware/app/`). Every
 * `FF_SCR_MUSIC_FRAME_STATS_WINDOW_MS` (1000ms) of REAL ticks, the
 * running sums are averaged into `s_frame_stats.last_*` and the window
 * resets — `ff_scr_music_debug_frame_stats()` (scr_music.h) exposes
 * exactly those last-CLOSED-window averages, `valid` false until the
 * first window has ever closed (an honest "n/a", never a fabricated
 * 0.00/0 — the same convention app_main.c's own `dbgconsole_perf_
 * window_line` already applies to its windowed metrics). The esp32s3
 * target's `music` console command (`app_main.c`'s `dbgconsole_music_
 * frame`, wired through `ff_dbgconsole_music_frame_fn`) is a thin
 * passthrough onto this getter — see that hook's own doc comment
 * (ff_debug_console.h) for why `ff_debug_console.c` cannot call this
 * getter directly.
 */
#include "scr_music.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(FF_TARGET_SIM)
#include <time.h>
#else
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" /* fix/mic-dump-device-path — portMUX_TYPE for s_frame_stats_lock, below */
#endif

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
 * is_low` (ff_radar.h) already applies everywhere else. */
#define FF_SCR_MUSIC_FPS_NORMAL 30u
#define FF_SCR_MUSIC_FPS_LOW_BATT 15u
#define FF_SCR_MUSIC_LOW_BATT_PCT 20

/* Canvas size: the whole glass, same 412px puck coordinate space every
 * particle position/the ring already lived in — see this file's top
 * comment, "Renderer". */
#define FF_SCR_MUSIC_CANVAS_PX ((uint32_t)FF_THEME_PUCK_PX)

/* Core dot radius, px — the task's own formula (3 + 10*glow), on the
 * coordinator's 600px mockup canvas, scaled onto this puck's 412px
 * glass by the SAME `FF_SWARM_MOCK_TO_PUCK_SCALE` every other absolute
 * pixel constant in `ff_swarm.h` uses (see that header's own "Scale"
 * comment) — one shared scale factor, not a second copy of it. */
#define FF_SCR_MUSIC_CORE_R_BASE_PX 3.0f
#define FF_SCR_MUSIC_CORE_R_GAIN_PX 10.0f

/* Halo ring radius, as a multiple of the core radius this step — "a
 * soft halo... about 2.2x the core radius" (the task's own number). */
#define FF_SCR_MUSIC_HALO_MULT 2.2f

/* Opacity curves (0-255), glow-driven, each an interpretation call
 * flagged per AGENTS.md (no mockup pins exact alpha values). Core: "ink
 * white at 60-100% opacity" (the task's own range) — 153/255 == 60%,
 * 255/255 == 100%. The halo is fainter, so it reads as a soft glow
 * rather than a second hard-edged dot. */
#define FF_SCR_MUSIC_CORE_OPA_BASE 153.0f
#define FF_SCR_MUSIC_CORE_OPA_GAIN 102.0f
#define FF_SCR_MUSIC_HALO_OPA_GAIN 120.0f

/* Faint static ring, r~250 on the coordinator's mockup (scaled the
 * same way) — baked into the canvas every frame (this file's top
 * comment, "Chrome"), never animated. */
#define FF_SCR_MUSIC_RING_R_PX (250.0f * FF_SWARM_MOCK_TO_PUCK_SCALE) /* ~171.7px */

/* Source chip position — "a small muted chip at the bottom" (S31
 * polish, owner feedback on PR #245 — see scr_music.h's own top
 * comment). Comfortably inside FF_THEME_GLASS_R (200) so it's never
 * clipped under the bezel. Interpretation call, flagged per AGENTS.md —
 * no mockup pins an exact margin. */
#define FF_SCR_MUSIC_CHIP_BOTTOM_MARGIN_PX 34

/* Glow sprites: 4 pre-rendered (core+halo) shapes, bucketed by glow
 * quartile, each rendered once for each of the two accent tints —
 * this file's top comment, "Renderer", has the full reasoning. Sprite
 * radius (22px) comfortably covers the largest halo this puck ever
 * draws (glow==1: core_r ~8.9px, halo_r ~19.6px — see the constants
 * above), with a couple of px of margin for the falloff to reach 0
 * before the sprite edge. */
#define FF_SCR_MUSIC_GLOW_STEPS 4u
#define FF_SCR_MUSIC_SPRITE_R_PX 22
#define FF_SCR_MUSIC_SPRITE_DIM (2 * FF_SCR_MUSIC_SPRITE_R_PX + 1) /* 45 */
#define FF_SCR_MUSIC_SPRITE_PX (FF_SCR_MUSIC_SPRITE_DIM * FF_SCR_MUSIC_SPRITE_DIM)

/* Frame-period/canvas-draw-time rolling window — this file's top
 * comment, "Instrumentation". */
#define FF_SCR_MUSIC_FRAME_STATS_WINDOW_MS 1000u

typedef struct {
    uint16_t rgb565[FF_SCR_MUSIC_SPRITE_PX];
    uint8_t alpha[FF_SCR_MUSIC_SPRITE_PX];
} music_sprite_t;

/* [glow bucket][accent tint: 0 == amber, 1 == live-green]. Computed
 * once ever by `music_build_sprites` (a pure function of fixed
 * constants — bit-identical every run, so goldens stay reproducible),
 * guarded by `s_sprites_ready` so a second/third Music session in the
 * same process (or the same sim binary running several fixtures) never
 * redoes the work.
 *
 * fix/s31-sprites-psram (2026-09-09): a pointer, not the table itself —
 * lazily `heap_caps_calloc`'d from PSRAM by `music_ensure_sprites` on
 * first use (this file's top comment, "Renderer", has the full boot-
 * failure writeup). NULL-safe throughout the draw path exactly like
 * `s_canvas_buf`: `music_build_sprites` bails out (never sets
 * `s_sprites_ready`) if the allocation fails, and `music_composite_
 * particle` no-ops if `s_sprites` is still NULL — the swarm simply
 * never draws rather than crash, the same posture a failed canvas
 * allocation already gets. */
static music_sprite_t (*s_sprites)[2];
static float s_sprite_glow_center[FF_SCR_MUSIC_GLOW_STEPS];
static bool s_sprites_ready;

#if defined(FF_TARGET_SIM)
/* [test-only] fix/s31-sprites-psram (2026-09-09): lets a sim unit test
 * exercise music_build_sprites' allocation-failure path without a real
 * OOM (the sim's `calloc` essentially never fails, and there is no
 * internal-RAM/PSRAM distinction to actually exhaust in the sim) — see
 * `ff_scr_music_debug_force_sprite_alloc_fail`'s own doc comment,
 * scr_music.h. Defaults false; a test that sets it true MUST restore it
 * in its own teardown (mirrors this suite's "tests own their state"
 * convention — test_ctl_music_idle_drain.c's tearDown comment). Not
 * compiled into the esp32s3 target build at all, so it costs the device
 * nothing and cannot be reached by any device code path. */
static bool s_test_force_sprite_alloc_fail;
#endif

/* The canvas pixel buffer — allocated ONCE (lazily, on the first ever
 * `ff_scr_music_build`) and kept for the process lifetime; see this
 * file's top comment, "Renderer", for why this is never freed/
 * re-allocated per Music session. `s_canvas_stride_px` is the
 * RGB565-pixel stride LVGL itself computed for this buffer
 * (`lv_draw_buf_width_to_stride`) — read back rather than assumed to be
 * `FF_SCR_MUSIC_CANVAS_PX`, so a future `LV_DRAW_BUF_STRIDE_ALIGN`
 * change (today: 1, i.e. no padding) can never silently desync this
 * file's own row indexing from LVGL's. */
static uint16_t *s_canvas_buf;
static uint32_t s_canvas_stride_px;
static lv_obj_t *s_canvas_obj;

static ff_swarm_t s_swarm;
static uint32_t s_last_beat_count;
static uint32_t s_last_tick_ms;
static uint32_t s_period_ms;

/* fix/s31-music-idle-drain (2026-09-09) — see this file's "Pause on
 * DIM/OFF" section below (music_timer_cb) and scr_music.h's own doc
 * comment on ff_scr_music_debug_render_ticks for what this counts and
 * why. `s_screen_was_awake` starts true (matches the shell's own
 * screen_awake default, ff_shell.c) so the very first frame after a
 * build is never mistaken for a resume-from-DIM edge. */
static uint32_t s_render_tick_count;
static bool s_screen_was_awake = true;

/* Frame-period (ms) / canvas-draw-time (us) rolling window — see this
 * file's top comment, "Instrumentation". */
typedef struct {
    uint32_t window_start_ms;
    bool have_window_start;
    uint32_t sum_period_ms;
    uint32_t sum_draw_us;
    uint32_t n_samples;

    bool valid; /* true once at least one window has ever closed */
    uint32_t last_valid_ms; /* lv_tick_get() at the moment `valid` most recently became/stayed true — 2026-09-09
                                amendment, the "keep the last values for 30s after leaving the face" fix below */
    float last_frame_period_avg_ms;
    uint32_t last_canvas_draw_avg_us;
} music_frame_stats_t;

/** 2026-09-09 amendment (fix/s31-beat-real-audio, docs/specs/
 *  S31-music-swarm.md's dated amendment) — `ff_scr_music_debug_frame_
 *  stats` reports `frame_ms=n/a canvas_us=n/a` once the last real
 *  window closed more than this long ago, rather than showing an
 *  ARBITRARILY stale number forever (this file's own `s_frame_stats` is
 *  only ever reset on the NEXT face build — nothing previously bounded
 *  how old "the last real numbers" could be once the face was left).
 *  30s: long enough that a bench operator who just left Music and reads
 *  the console a few seconds later still sees the real numbers from the
 *  session that just ended (the deliverable's own stated acceptance),
 *  short enough that a MUCH later read (a different face, a different
 *  session entirely) gets the honest n/a it should. */
#define FF_SCR_MUSIC_FRAME_STATS_KEEP_MS 30000u

static music_frame_stats_t s_frame_stats;

/* fix/mic-dump-device-path — root cause of the device-only "`music`
 * console line's frame_ms/canvas_us always read n/a" bug (never
 * reproduced in the sim: see below for why). `s_frame_stats` is a
 * cross-task producer/consumer on the esp32s3 target: `music_timer_cb`
 * (below) writes it from ONE task — esp_lvgl_port's own "taskLVGL",
 * inside an `lv_timer_handler()` pass — while `ff_scr_music_debug_
 * frame_stats()` is read from a COMPLETELY DIFFERENT task (app_main.c's
 * render loop, via the `music` console command's `dbgconsole_music_
 * frame` hook) with NO synchronization at all. That is exactly the
 * shape `ff_display.c`'s own "2026-09-08 QA hardening item 2" comment
 * already documents and guards for its sibling `lvgl_refresh`/`flush`
 * perf windows (`s_refresh_perf`/`s_flush_perf`, guarded by a
 * `portMUX_TYPE` spinlock) — this file's `s_frame_stats` was simply
 * never given the same treatment when it was added (2026-09-09 S31
 * canvas renderer), so an unlucky read (most concretely: the very FIRST
 * window ever closing, `valid` flipping false->true while the several
 * fields it's paired with are written in sequence, not atomically) can
 * observe a torn combination. A short critical section (not
 * `ff_display_lock()`/`lvgl_port_lock()`: the producer runs INSIDE an
 * `lv_timer` callback that esp_lvgl_port already runs under ITS OWN
 * lock, so taking a second, different lock around a few-word struct
 * update would be redundant nesting for no benefit — a spinlock is the
 * right tool for "protect a few words for a few instructions", same
 * reasoning `esp_lcd_touch_spd2010.c`'s own `portENTER_CRITICAL(&tp->
 * data.lock)` documents for its identical shape) protects every touch
 * point: this reset, `music_frame_stats_add`, and the getter's read.
 * Compiled out on `FF_TARGET_SIM`: the sim is single-threaded for this
 * state (no LVGL port task — see "Golden determinism" above), so a lock
 * there would be pure overhead with nothing to protect against; this is
 * also exactly why a sim test could never have caught this class of bug
 * in the first place. */
#if defined(FF_TARGET_SIM)
#define FF_MUSIC_STATS_LOCK() ((void)0)
#define FF_MUSIC_STATS_UNLOCK() ((void)0)
#else
static portMUX_TYPE s_frame_stats_lock = portMUX_INITIALIZER_UNLOCKED;
#define FF_MUSIC_STATS_LOCK() portENTER_CRITICAL(&s_frame_stats_lock)
#define FF_MUSIC_STATS_UNLOCK() portEXIT_CRITICAL(&s_frame_stats_lock)
#endif

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

static float music_clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* `esp_timer_get_time()`/`clock_gettime(CLOCK_MONOTONIC,...)` — the
 * same "shared clock seam, two platform bodies" shape `ff_shell.c`'s
 * own `FF_TARGET_SIM` branches already use (app/ff_shell.c). Only used
 * for the diagnostic canvas-draw-time measurement (this file's top
 * comment, "Instrumentation") — never anything the swarm's own motion
 * or a golden's pixels depend on. */
#if defined(FF_TARGET_SIM)
static uint64_t music_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}
#else
static uint64_t music_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}
#endif

/* FF_THEME_COLOR_* are packed 0xRRGGBB — the theme's own convention
 * (ff_theme.h). Converted to RGB565 at sprite/ring-build time (never
 * hand-computed hex, which is exactly the kind of arithmetic a stray
 * transcription error hides in) via the ordinary truncating >>3/>>2
 * channel-width reduction LVGL's own RGB565 pack uses. */
static uint16_t music_rgb565_from_hex(uint32_t hex)
{
    uint32_t const r = (hex >> 16) & 0xFFu;
    uint32_t const g = (hex >> 8) & 0xFFu;
    uint32_t const b = hex & 0xFFu;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Renders one glow-bucket's (core + halo) shape into `out`, tinted with
 * `halo_rgb565` for the halo band (the core is always ink-white,
 * `core_rgb565` — a fixed fact of every firefly, never tinted). Pure
 * function of `glow_b` (the bucket's own representative glow value) and
 * the two colors — see this file's top comment, "Renderer", for the
 * bucket/continuous-brightness split. */
static void music_build_sprite_shape(float glow_b, uint16_t core_rgb565, uint16_t halo_rgb565, music_sprite_t *out)
{
    float const core_r = (FF_SCR_MUSIC_CORE_R_BASE_PX + FF_SCR_MUSIC_CORE_R_GAIN_PX * glow_b) *
                          FF_SWARM_MOCK_TO_PUCK_SCALE;
    float const halo_r = core_r * FF_SCR_MUSIC_HALO_MULT;
    float const core_opa = music_clamp01((FF_SCR_MUSIC_CORE_OPA_BASE + glow_b * FF_SCR_MUSIC_CORE_OPA_GAIN) / 255.0f) *
                            255.0f;
    float const halo_opa = music_clamp01((glow_b * FF_SCR_MUSIC_HALO_OPA_GAIN) / 255.0f) * 255.0f;

    for (int32_t dy = -FF_SCR_MUSIC_SPRITE_R_PX; dy <= FF_SCR_MUSIC_SPRITE_R_PX; dy++) {
        for (int32_t dx = -FF_SCR_MUSIC_SPRITE_R_PX; dx <= FF_SCR_MUSIC_SPRITE_R_PX; dx++) {
            uint32_t const idx = (uint32_t)(dy + FF_SCR_MUSIC_SPRITE_R_PX) * (uint32_t)FF_SCR_MUSIC_SPRITE_DIM +
                                  (uint32_t)(dx + FF_SCR_MUSIC_SPRITE_R_PX);
            float const r = sqrtf((float)(dx * dx + dy * dy));
            if (r <= core_r) {
                out->rgb565[idx] = core_rgb565;
                out->alpha[idx] = (uint8_t)core_opa;
            } else if (r <= halo_r) {
                float const t = (halo_r > core_r) ? (r - core_r) / (halo_r - core_r) : 1.0f;
                float const falloff = 1.0f - t; /* linear: core edge (1.0) -> halo_r (0.0) */
                out->rgb565[idx] = halo_rgb565;
                out->alpha[idx] = (uint8_t)(halo_opa * falloff);
            } else {
                out->rgb565[idx] = 0u;
                out->alpha[idx] = 0u;
            }
        }
    }
}

/* Lazily allocates the sprite table from PSRAM (device) / the ordinary
 * heap (sim) — see the `s_sprites` declaration's own doc comment above
 * for the boot-failure history this replaces. NULL-safe like `music_
 * ensure_canvas_buf` just below: a failed allocation leaves `s_sprites
 * == NULL` and every caller already tolerates that. `heap_caps_calloc`/
 * `calloc` (zero-initialized) rather than `_malloc` purely so a
 * still-zeroed sprite would fail safe (fully transparent, alpha==0)
 * rather than show garbage pixels in the window between allocation and
 * `music_build_sprite_shape` filling it in — belt-and-braces, since
 * that window is one synchronous function call with no way for a draw
 * to land inside it. */
static void music_ensure_sprites(void)
{
    if (s_sprites != NULL) return;

#if defined(FF_TARGET_SIM)
    if (s_test_force_sprite_alloc_fail) return; /* [test-only] see s_test_force_sprite_alloc_fail's own comment */
    music_sprite_t (*const sprites)[2] =
        (music_sprite_t (*)[2])calloc((size_t)FF_SCR_MUSIC_GLOW_STEPS, sizeof(music_sprite_t) * 2u);
#else
    music_sprite_t (*const sprites)[2] = (music_sprite_t (*)[2])heap_caps_calloc(
        (size_t)FF_SCR_MUSIC_GLOW_STEPS, sizeof(music_sprite_t) * 2u, MALLOC_CAP_SPIRAM);
#endif
    if (sprites == NULL) return;

    s_sprites = sprites;
}

static void music_build_sprites(void)
{
    if (s_sprites_ready) return;

    music_ensure_sprites();
    if (s_sprites == NULL) return; /* allocation failed — music_composite_particle stays NULL-safe */

    uint16_t const ink = music_rgb565_from_hex(FF_THEME_COLOR_INK);
    uint16_t const amber = music_rgb565_from_hex(FF_THEME_COLOR_AMBER);
    uint16_t const green = music_rgb565_from_hex(FF_THEME_COLOR_LIVE_GREEN);

    for (uint32_t b = 0; b < FF_SCR_MUSIC_GLOW_STEPS; b++) {
        /* Quartile centers: 0.125, 0.375, 0.625, 0.875. */
        float const glow_b = ((float)b + 0.5f) / (float)FF_SCR_MUSIC_GLOW_STEPS;
        s_sprite_glow_center[b] = glow_b;
        music_build_sprite_shape(glow_b, ink, amber, &s_sprites[b][0]);
        music_build_sprite_shape(glow_b, ink, green, &s_sprites[b][1]);
    }
    s_sprites_ready = true;
}

/* Allocates the canvas pixel buffer if it doesn't exist yet — see this
 * file's top comment, "Renderer", for why PSRAM (device) / plain malloc
 * (sim), never the LVGL heap, and why this is never freed. A failed
 * allocation leaves `s_canvas_buf == NULL`: `ff_scr_music_build` then
 * simply does not create the canvas object at all (the clock/chip chrome
 * still builds normally) rather than crash — the same NULL-safe posture
 * every getter/setter in this codebase already carries. */
static void music_ensure_canvas_buf(void)
{
    if (s_canvas_buf != NULL) return;

    uint32_t const stride_bytes = lv_draw_buf_width_to_stride(FF_SCR_MUSIC_CANVAS_PX, LV_COLOR_FORMAT_RGB565);
    if (stride_bytes == 0u) return; /* defensive: an LVGL build with no RGB565 stride rule at all */
    size_t const bytes = (size_t)stride_bytes * (size_t)FF_SCR_MUSIC_CANVAS_PX;

#if defined(FF_TARGET_SIM)
    uint16_t *const buf = (uint16_t *)malloc(bytes);
#else
    uint16_t *const buf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#endif
    if (buf == NULL) return;

    s_canvas_buf = buf;
    s_canvas_stride_px = stride_bytes / 2u; /* RGB565 == 2 bytes/px */
}

/* Additive blend of `src_rgb565` (weighted by `alpha`, 0-255) into the
 * canvas pixel at (x,y) — integer-only, saturating, no division (a
 * `>>8` in place of `/255` — the standard fast alpha-blend
 * approximation; deterministic, so cross-compiler goldens stay
 * bit-identical). Silently clips (alpha==0 or out-of-bounds is a no-op)
 * — every caller already keeps fireflies well inside the canvas
 * (`FF_SWARM_R_MAX_PX` + the largest halo radius is comfortably under
 * this canvas's own half-width), so this bound is defensive, not load-
 * bearing. */
static inline void music_canvas_add_px(uint16_t *canvas, uint32_t stride_px, int32_t x, int32_t y,
                                        uint16_t src_rgb565, uint8_t alpha)
{
    if (alpha == 0u) return;
    if (x < 0 || y < 0 || x >= (int32_t)FF_SCR_MUSIC_CANVAS_PX || y >= (int32_t)FF_SCR_MUSIC_CANVAS_PX) return;

    uint16_t *const dst = &canvas[(uint32_t)y * stride_px + (uint32_t)x];
    uint16_t const d = *dst;

    uint32_t const dr = ((uint32_t)(d >> 11) & 0x1Fu) << 3;
    uint32_t const dg = ((uint32_t)(d >> 5) & 0x3Fu) << 2;
    uint32_t const db = ((uint32_t)d & 0x1Fu) << 3;

    uint32_t const sr = ((uint32_t)(src_rgb565 >> 11) & 0x1Fu) << 3;
    uint32_t const sg = ((uint32_t)(src_rgb565 >> 5) & 0x3Fu) << 2;
    uint32_t const sb = ((uint32_t)src_rgb565 & 0x1Fu) << 3;

    uint32_t nr = dr + ((sr * alpha) >> 8);
    uint32_t ng = dg + ((sg * alpha) >> 8);
    uint32_t nb = db + ((sb * alpha) >> 8);
    if (nr > 255u) nr = 255u;
    if (ng > 255u) ng = 255u;
    if (nb > 255u) nb = 255u;

    *dst = (uint16_t)(((nr >> 3) << 11) | ((ng >> 2) << 5) | (nb >> 3));
}

/* Composites one firefly's glow sprite onto the canvas — see this
 * file's top comment, "Renderer", for the bucket-by-size / continuous-
 * by-brightness split. */
static void music_composite_particle(uint16_t *canvas, uint32_t stride_px, ff_swarm_particle_t const *p)
{
    /* fix/s31-sprites-psram — NULL-safe: a failed sprite-table
     * allocation (see `s_sprites`'s own doc comment) means `music_
     * build_sprites` never set `s_sprites_ready`/populated `s_sprite_
     * glow_center` either, so bail before touching any of it — the
     * swarm simply draws no fireflies onto an otherwise-normal cleared-
     * plus-ring canvas, rather than dereference a NULL pointer. */
    if (s_sprites == NULL) return;

    float dx, dy;
    music_deg_to_offset(p->theta_deg, p->r_px, &dx, &dy);
    int32_t const cx = (int32_t)(FF_SCR_MUSIC_CANVAS_PX / 2u) + (int32_t)dx;
    int32_t const cy = (int32_t)(FF_SCR_MUSIC_CANVAS_PX / 2u) + (int32_t)dy;

    float const glow = music_clamp01(p->glow);
    uint32_t bucket = (uint32_t)(glow * (float)FF_SCR_MUSIC_GLOW_STEPS);
    if (bucket >= FF_SCR_MUSIC_GLOW_STEPS) bucket = FF_SCR_MUSIC_GLOW_STEPS - 1u;

    float const glow_b = s_sprite_glow_center[bucket];
    float scale = (glow_b > 0.0f) ? (glow / glow_b) : 1.0f;
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 2.0f) scale = 2.0f; /* defensive cap — see this file's top comment for the expected ~0.5x-1.5x range */

    music_sprite_t const *const sprite = &s_sprites[bucket][p->is_accent_green ? 1 : 0];

    for (int32_t sy = -FF_SCR_MUSIC_SPRITE_R_PX; sy <= FF_SCR_MUSIC_SPRITE_R_PX; sy++) {
        for (int32_t sx = -FF_SCR_MUSIC_SPRITE_R_PX; sx <= FF_SCR_MUSIC_SPRITE_R_PX; sx++) {
            uint32_t const idx = (uint32_t)(sy + FF_SCR_MUSIC_SPRITE_R_PX) * (uint32_t)FF_SCR_MUSIC_SPRITE_DIM +
                                  (uint32_t)(sx + FF_SCR_MUSIC_SPRITE_R_PX);
            uint8_t const a0 = sprite->alpha[idx];
            if (a0 == 0u) continue; /* the common case for most of the 45x45 bounding box at low glow */

            float af = (float)a0 * scale;
            if (af > 255.0f) af = 255.0f;
            music_canvas_add_px(canvas, stride_px, cx + sx, cy + sy, sprite->rgb565[idx], (uint8_t)af);
        }
    }
}

/* Fills the whole canvas with the theme background color — see this
 * file's top comment, "Renderer", for the per-frame cost accounting. */
static void music_clear_canvas(uint16_t *canvas, uint32_t stride_px)
{
    uint16_t const bg = music_rgb565_from_hex(FF_THEME_COLOR_BG);
    for (uint32_t y = 0; y < FF_SCR_MUSIC_CANVAS_PX; y++) {
        uint16_t *const row = &canvas[y * stride_px];
        for (uint32_t x = 0; x < FF_SCR_MUSIC_CANVAS_PX; x++) {
            row[x] = bg;
        }
    }
}

/* Baked-in replacement for the old separate ring `lv_obj` — see this
 * file's top comment, "Chrome". One point per pixel of circumference
 * (2*pi*r, rounded up) so the walk never leaves a gap; a solid
 * overwrite (no falloff), matching the previous 1px border's own crisp
 * look. */
static void music_draw_ring(uint16_t *canvas, uint32_t stride_px)
{
    uint16_t const ring_rgb = music_rgb565_from_hex(FF_THEME_COLOR_SURFACE);
    int32_t const cx = (int32_t)(FF_SCR_MUSIC_CANVAS_PX / 2u);
    int32_t const cy = (int32_t)(FF_SCR_MUSIC_CANVAS_PX / 2u);
    uint32_t const steps = (uint32_t)(2.0f * (float)M_PI * FF_SCR_MUSIC_RING_R_PX) + 1u;

    for (uint32_t i = 0; i < steps; i++) {
        float const theta = (float)i * (2.0f * (float)M_PI / (float)steps);
        int32_t const x = cx + (int32_t)(FF_SCR_MUSIC_RING_R_PX * sinf(theta));
        int32_t const y = cy - (int32_t)(FF_SCR_MUSIC_RING_R_PX * cosf(theta));
        if (x < 0 || y < 0 || x >= (int32_t)FF_SCR_MUSIC_CANVAS_PX || y >= (int32_t)FF_SCR_MUSIC_CANVAS_PX) continue;
        canvas[(uint32_t)y * stride_px + (uint32_t)x] = ring_rgb;
    }
}

/* Redraws the whole canvas from the current `s_swarm` state: clear,
 * ring, then every firefly, then ONE `lv_obj_invalidate`. Returns the
 * wall-clock cost of everything except the final invalidate (an O(1)
 * call) — this file's top comment, "Instrumentation". A NULL buffer
 * (allocation failed — see `music_ensure_canvas_buf`) is a safe no-op
 * that reports 0us (never drawn, never claimed to have been). */
static uint32_t music_redraw_canvas(void)
{
    if (s_canvas_buf == NULL) return 0u;

    uint64_t const t0 = music_now_us();

    music_clear_canvas(s_canvas_buf, s_canvas_stride_px);
    music_draw_ring(s_canvas_buf, s_canvas_stride_px);
    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        music_composite_particle(s_canvas_buf, s_canvas_stride_px, &s_swarm.particles[i]);
    }

    uint64_t const draw_us = music_now_us() - t0;

    if (s_canvas_obj != NULL) {
        lv_obj_invalidate(s_canvas_obj);
    }

    return (draw_us > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)draw_us;
}

static void music_frame_stats_reset(void)
{
    FF_MUSIC_STATS_LOCK();
    memset(&s_frame_stats, 0, sizeof(s_frame_stats));
    FF_MUSIC_STATS_UNLOCK();
}

/* Folds one REAL per-frame timer tick's (period, draw-time) pair into
 * the rolling last-second window — this file's top comment,
 * "Instrumentation". Never called for the build-time settle step (see
 * `ff_scr_music_build`) or while the screen is paused at DIM/OFF (see
 * `music_timer_cb`'s own early-return, which never reaches this). */
static void music_frame_stats_add(uint32_t period_ms, uint32_t draw_us, uint32_t now_ms)
{
    FF_MUSIC_STATS_LOCK();
    if (!s_frame_stats.have_window_start) {
        s_frame_stats.window_start_ms = now_ms;
        s_frame_stats.have_window_start = true;
    }
    s_frame_stats.sum_period_ms += period_ms;
    s_frame_stats.sum_draw_us += draw_us;
    s_frame_stats.n_samples++;

    uint32_t const elapsed = now_ms - s_frame_stats.window_start_ms; /* wraparound-safe over a short window */
    if (elapsed >= FF_SCR_MUSIC_FRAME_STATS_WINDOW_MS && s_frame_stats.n_samples > 0u) {
        s_frame_stats.last_frame_period_avg_ms = (float)s_frame_stats.sum_period_ms / (float)s_frame_stats.n_samples;
        s_frame_stats.last_canvas_draw_avg_us = s_frame_stats.sum_draw_us / s_frame_stats.n_samples;
        s_frame_stats.valid = true;
        s_frame_stats.last_valid_ms = now_ms; /* 2026-09-09 amendment — see FF_SCR_MUSIC_FRAME_STATS_KEEP_MS's own doc comment */

        s_frame_stats.window_start_ms = now_ms;
        s_frame_stats.sum_period_ms = 0u;
        s_frame_stats.sum_draw_us = 0u;
        s_frame_stats.n_samples = 0u;
    }
    FF_MUSIC_STATS_UNLOCK();
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

    /* fix/s31-music-idle-drain (2026-09-09) — "the swarm timer must
     * pause on DIM/OFF (no rendering, no beat processing) and resume on
     * wake". `state->music.screen_awake` is the S26 idle FSM's own
     * ACTIVE/not-ACTIVE fact, pushed by `ff_shell_set_screen_awake` (see
     * that function's own doc comment, ff_shell.h) and read straight off
     * the LIVE view here, same "outside the render key" placement
     * loudness/beat_count already use (this file's top comment, "Per-
     * frame mechanism"). While not awake: skip stepping/redrawing
     * entirely — this is the actual CPU-cost half of the fix (the mic
     * itself already stops being FED new samples the instant idle
     * leaves ACTIVE, `ff_shell_music_wants_mic`'s own doc comment, but
     * this timer would otherwise keep re-stepping the swarm 15-30x/sec
     * on stale loudness/beat state for no one to see, screen dark or
     * not) — and keep re-pinning `s_last_tick_ms` every tick so a LONG
     * DIM/OFF stretch never shows up as one giant `elapsed_ms` jump the
     * instant the screen wakes back up. */
    if (!state->music.screen_awake) {
        s_last_tick_ms = now;
        s_screen_was_awake = false;
        return;
    }
    if (!s_screen_was_awake) {
        /* Resuming from DIM/OFF THIS frame: treat it exactly like the
         * first frame after a build (this file's own build-time "settle"
         * convention, top comment) — pin the baseline now and step for
         * real next frame, rather than integrating whatever elapsed
         * while dark as one lurching jump. */
        s_last_tick_ms = now;
        s_screen_was_awake = true;
        return;
    }

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
    uint32_t const draw_us = music_redraw_canvas();
    s_render_tick_count++; /* fix/s31-music-idle-drain — see ff_scr_music_debug_render_ticks's own doc comment (scr_music.h) */
    music_frame_stats_add(elapsed_ms, draw_us, now);

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
 * `lv_timer_t*`/canvas object pointer across a rebuild. The canvas
 * PIXEL BUFFER itself is deliberately NOT freed here — see this file's
 * top comment, "Renderer", for why it outlives the screen. */
static void music_content_delete_cb(lv_event_t *e)
{
    lv_timer_t *timer = (lv_timer_t *)lv_event_get_user_data(e);
    if (timer != NULL) {
        lv_timer_delete(timer);
    }
    s_canvas_obj = NULL;
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

void ff_scr_music_build(ff_app_state_t const *state)
{
    if (state == NULL) return;

    music_build_sprites();
    music_ensure_canvas_buf();

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
    /* fix/s31-music-idle-drain — a fresh Music session starts a fresh
     * count (mirrors s_last_beat_count just above) and assumes awake
     * (matches the shell's own screen_awake default, ff_shell.c) so the
     * timer's very first real tick is never mistaken for a resume-from-
     * DIM edge. */
    s_render_tick_count = 0u;
    s_screen_was_awake = true;

    /* The canvas: one lv_obj, sized/positioned to fill the puck exactly
     * like the old dot-pool's implicit full-puck coordinate space did.
     * A NULL buffer (allocation failed) means no canvas object at all —
     * the clock/chip chrome below still builds; see music_ensure_
     * canvas_buf's own doc comment. */
    lv_obj_t *canvas = NULL;
    if (s_canvas_buf != NULL) {
        canvas = lv_canvas_create(puck);
        lv_canvas_set_buffer(canvas, s_canvas_buf, (int32_t)FF_SCR_MUSIC_CANVAS_PX, (int32_t)FF_SCR_MUSIC_CANVAS_PX,
                              LV_COLOR_FORMAT_RGB565);
        lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    }
    s_canvas_obj = canvas;

    music_redraw_canvas(); /* the settle-step frame — see "Golden determinism" above */
    music_frame_stats_reset(); /* the settle draw is not a real per-frame tick — see this file's top comment */

    /* Chrome, drawn AFTER the canvas so it paints on top — same
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
    /* fix/mic-dump-device-path — defensive: lv_timer_create can return
     * NULL on allocation failure (the timer struct itself, from LVGL's
     * own heap) same as any other LVGL allocation this file already
     * checks (s_canvas_buf/s_sprites just above/below) — this feature
     * has a real history of tight internal-RAM headroom on device (PR
     * #253, "device out of internal RAM" from this same S31 canvas
     * renderer's sprite table). Without this timer the swarm simply
     * never steps/redraws past the one build-time settle frame and the
     * frame-stats window never closes (an honest n/a from
     * ff_scr_music_debug_frame_stats — never a hang), same silent-and-
     * safe posture music_ensure_canvas_buf's own NULL handling takes;
     * `music_content_delete_cb` already null-checks its `timer` user
     * pointer so passing NULL through here is safe either way. */
    lv_timer_t *timer = lv_timer_create(music_timer_cb, s_period_ms, (void *)state);
    s_last_tick_ms = lv_tick_get();
    lv_obj_add_event_cb(puck, music_content_delete_cb, LV_EVENT_DELETE, (void *)timer);
}

uint32_t ff_scr_music_debug_render_ticks(void)
{
    return s_render_tick_count;
}

ff_scr_music_frame_stats_t ff_scr_music_debug_frame_stats(void)
{
    ff_scr_music_frame_stats_t out = {0};

    /* Snapshot the three fields this getter needs UNDER the lock (fix/
     * mic-dump-device-path — see s_frame_stats_lock's own doc comment
     * above for why this cross-task read needs one at all), then do the
     * age-vs-KEEP_MS honesty check against the snapshot, not the live
     * struct, outside it — keeps the critical section to a handful of
     * word-copies, never a snprintf-adjacent computation. */
    bool valid;
    uint32_t last_valid_ms;
    float frame_period_avg_ms;
    uint32_t canvas_draw_avg_us;
    FF_MUSIC_STATS_LOCK();
    valid = s_frame_stats.valid;
    last_valid_ms = s_frame_stats.last_valid_ms;
    frame_period_avg_ms = s_frame_stats.last_frame_period_avg_ms;
    canvas_draw_avg_us = s_frame_stats.last_canvas_draw_avg_us;
    FF_MUSIC_STATS_UNLOCK();

    if (!valid) return out; /* honest: no window has ever closed this session */

    /* 2026-09-09 amendment — see FF_SCR_MUSIC_FRAME_STATS_KEEP_MS's own
     * doc comment: stale beyond the keep window reports the same honest
     * n/a as "never populated", never an arbitrarily old number. */
    uint32_t const age_ms = lv_tick_get() - last_valid_ms; /* wraparound-safe over any real session length */
    if (age_ms > FF_SCR_MUSIC_FRAME_STATS_KEEP_MS) return out;

    out.valid = true;
    out.frame_period_avg_ms = frame_period_avg_ms;
    out.canvas_draw_avg_us = canvas_draw_avg_us;
    return out;
}

#if defined(FF_TARGET_SIM)
void ff_scr_music_debug_force_sprite_alloc_fail(bool fail)
{
    s_test_force_sprite_alloc_fail = fail;
}
#endif

bool ff_scr_music_debug_sprites_ready(void)
{
    return s_sprites_ready;
}
