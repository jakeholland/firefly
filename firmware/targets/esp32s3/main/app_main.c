/**
 * app_main.c — ESP32-S3 target entry point (S15 slice b — display + touch).
 *
 * Slice a booted ff_shell against three stub HALs with the screen dark.
 * Slice b keeps that boot EXACTLY (same clock stub, same no-op store, same
 * no-transport shell) and adds the real display + touch HAL on top, staged
 * so the maintainer can flash one gate at a time and watch the glass:
 *
 *   STAGE 1 (first light): expander up -> release resets -> backlight ->
 *            SPD2010 QSPI panel init -> a solid fill + two-colour split via
 *            esp_lcd_panel_draw_bitmap. NO LVGL. Proves the panel path.
 *   STAGE 2 (app on glass): + LVGL v9 via esp_lvgl_port, render one real
 *            ff_shell face (Radar, no selection) — the same projection the
 *            sim renders for that state.
 *   STAGE 3 (touch drives it): + SPD2010 touch -> an LVGL pointer indev
 *            wired to the SAME abstract input seam the sim ctl socket
 *            drives (ff_intent_emit -> ff_shell_intent_sink). A physical
 *            tap/swipe changes shell state.
 *
 * The stage is chosen by CONFIG_FF_BRINGUP_STAGE (menuconfig ->
 * "Firefly bring-up", default 1). core/ and app/ are UNCHANGED; every new
 * line is under targets/esp32s3/. Every init step logs so a dark screen
 * still says how far it got.
 *
 * The clock / store / no-transport rationale is unchanged from slice a —
 * see the git history of this file / the S15a PR body. NVS store and UART
 * transport remain later slices (c/d/e).
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ff_display.h"
#include "ff_face.h"
#include "ff_intent.h"
#include "ff_shell.h"

#if CONFIG_FF_DEMO_MODE
#include "ff_demo.h" /* S20 — demo-mode seeding */
#endif

static const char *TAG = "firefly";

#if !CONFIG_FF_DEMO_MODE
/* ff_clock_t — monotonic ms over esp_timer_get_time() (see slice a).
 * Compiled out under CONFIG_FF_DEMO_MODE, which drives a frozen demo clock
 * instead (below) and never reads real time. */
static uint32_t ff_esp_clock_now_ms(void *user)
{
    (void)user;
    return (uint32_t)(esp_timer_get_time() / 1000);
}
#endif

#if CONFIG_FF_DEMO_MODE
/* S20 demo mode pins the shell's clock at a fixed instant (Sat 21:30) so
 * the seeded world stays frozen and honest — freshness/countdowns read the
 * seeded moment rather than drifting with real boot time. ff_demo_seed sets
 * this counter; both the injected ff_clock_t AND every ff_shell_tick read
 * it (see ff_bringup_now_ms), so the projection ages against the same
 * clock the seed used. The demo festpack is EMBED_FILES'd (main/CMakeLists). */
static uint32_t s_demo_clock_ms;
static fp_pack_t s_demo_pack;
extern const uint8_t firefly_pack_start[] asm("_binary_firefly_fields_festpack_json_start");
extern const uint8_t firefly_pack_end[] asm("_binary_firefly_fields_festpack_json_end");

static uint32_t ff_demo_clock_now_ms(void *user)
{
    (void)user;
    return s_demo_clock_ms;
}
#endif

/* The monotonic "now" every ff_shell_tick in this file uses: the demo's
 * frozen clock under CONFIG_FF_DEMO_MODE, otherwise real esp_timer time.
 * Keeping tick and the injected clock on the SAME source is what makes the
 * demo's seeded freshness render correctly. */
static uint32_t ff_bringup_now_ms(void)
{
#if CONFIG_FF_DEMO_MODE
    return s_demo_clock_ms;
#else
    return ff_esp_clock_now_ms(NULL);
#endif
}

/* ff_store_t — no-op stub (real NVS store is a later slice; see slice a). */
static int ff_stub_store_get(void *io, char const *key, void *buf, size_t n)
{
    (void)io;
    (void)key;
    (void)buf;
    (void)n;
    return -1; /* "not found" */
}

static int ff_stub_store_set(void *io, char const *key, void const *buf, size_t n)
{
    (void)io;
    (void)key;
    (void)buf;
    return (int)n; /* discards; bytes go nowhere */
}

static ff_shell_t s_shell;
static ff_clock_t s_clock;
static ff_store_t s_store;

/* Bring the panel up through the QSPI init (shared by every stage). Returns
 * true on success; logs and returns false so app_main can park rather than
 * spin a half-initialised peripheral. */
static bool ff_bringup_panel(void)
{
    if (ff_display_expander_init() != ESP_OK) {
        ESP_LOGE(TAG, "expander init failed — LCD/TP resets not released; screen will stay dark");
        return false;
    }
    if (ff_display_panel_init() != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed");
        return false;
    }
    return true;
}

/* Park forever (still logging a heartbeat) after a fatal bring-up error, so
 * the serial log stays readable instead of scrolling a reset loop. */
static void ff_park(const char *why)
{
    ESP_LOGE(TAG, "parked: %s", why);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    const int stage = CONFIG_FF_BRINGUP_STAGE;
    ESP_LOGI(TAG, "firefly esp32s3 target booting (S15 slice b: display+touch, STAGE %d)", stage);

#if CONFIG_FF_DEMO_MODE
    s_clock.now_ms = ff_demo_clock_now_ms; /* frozen demo clock (Sat 21:30) */
#else
    s_clock.now_ms = ff_esp_clock_now_ms;
#endif
    s_clock.user = NULL;
    s_store.get = ff_stub_store_get;
    s_store.set = ff_stub_store_set;
    s_store.io = NULL;

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &s_clock;
    cfg.store = &s_store;
#if CONFIG_FF_DEMO_MODE
    cfg.pack = &s_demo_pack; /* ff_demo_seed parses the embedded festpack into this */
#endif
    /* cfg.transport / cfg.haptic left zeroed — see slice a. */

    int rc = ff_shell_init(&s_shell, &cfg);
    if (rc != 0) {
        ff_park("ff_shell_init failed");
        return;
    }

#if CONFIG_FF_DEMO_MODE
    {
        size_t const pack_len = (size_t)(firefly_pack_end - firefly_pack_start);
        int const drc = ff_demo_seed(&s_shell, (char const *)firefly_pack_start, pack_len, &s_demo_clock_ms, 0);
        if (drc != 0) {
            ESP_LOGE(TAG, "S20 demo seed failed (%d) — festpack parse or wall latch", drc);
        } else {
            ESP_LOGI(TAG, "S20 DEMO MODE: seeded Firefly Fields (Sat 21:30) — %u byte festpack, no mesh",
                     (unsigned)pack_len);
        }
    }
#endif

    (void)ff_shell_tick(&s_shell, ff_bringup_now_ms());
    ff_app_state_t const *view = ff_shell_view(&s_shell);
    ESP_LOGI(TAG, "shell up: active_face=%d radar_mode=%d link=%d", (int)view->active_face,
             (int)view->radar.mode, (int)ff_shell_link(&s_shell));

    /* ---- Display bring-up (all stages) ---- */
    if (!ff_bringup_panel()) {
        ff_park("panel bring-up failed");
        return;
    }

    /* ---- STAGE 1: first light, no LVGL ---- */
    if (stage <= 1) {
        if (ff_display_draw_test_pattern() != ESP_OK) {
            ff_park("test pattern draw failed");
            return;
        }
        ESP_LOGI(TAG, "STAGE 1 complete: first-light pattern on glass. Keeping shell alive.");
        /* Keep ticking the shell (proves it survives under FreeRTOS) while
         * the static test pattern stays on the panel. No LVGL, no redraw. */
        while (true) {
            (void)ff_shell_tick(&s_shell, ff_bringup_now_ms());
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* ---- STAGE 2+: LVGL v9 up, render a real face ---- */
    lv_display_t *disp = ff_display_lvgl_start();
    if (disp == NULL) {
        ff_park("LVGL display bring-up failed");
        return;
    }

    /* ---- STAGE 3: touch + bind the input seam ---- */
    if (stage >= 3) {
        if (ff_display_touch_start(disp) != ESP_OK) {
            ff_park("touch bring-up failed");
            return;
        }
        /* Bind the SAME seam the sim binds (targets/sim/ctl_loop.c): every
         * screen emit now reaches this shell. Unbind is unnecessary — the
         * shell lives for the process lifetime. */
        ff_intent_emit_bind(ff_shell_intent_sink, &s_shell);
        ESP_LOGI(TAG, "input seam bound: touch -> ff_shell_intent");
    }

    /* First face build (under the LVGL lock — the port owns the LVGL task). */
    if (ff_display_lock(0)) {
        ff_face_build(ff_shell_view(&s_shell));
        ff_display_unlock();
    }
    ESP_LOGI(TAG, "STAGE %d complete: first face flushed to glass", stage);

    /* Render lifecycle mirrors the sim (targets/sim/ctl_loop.c): tick the
     * shell every frame, rebuild the LVGL tree ONLY on a dirty tick. The
     * esp_lvgl_port task does the actual flushing; we just own the model. */
    while (true) {
        bool const dirty = ff_shell_tick(&s_shell, ff_bringup_now_ms());
        if (dirty) {
            if (ff_display_lock(100 /* ms */)) {
                lv_obj_clean(lv_screen_active());
                ff_face_build(ff_shell_view(&s_shell));
                ff_display_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
