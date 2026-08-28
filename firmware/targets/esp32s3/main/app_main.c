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
 *            tap/swipe changes shell state. The stored touch calibration
 *            (if any) is loaded from ff_settings and applied.
 *   STAGE 4 (calibrate touch, S15d): + the crosshair capture flow — tap 5
 *            targets -> ff_touchcal_solve -> store to ff_settings -> apply
 *            LIVE and continue straight into the normal UI in the same
 *            boot (no reflash to feel the correction). Params + pairs are
 *            logged. See docs/specs/S15d-touch-calibration.md.
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
#include "ff_nvs_store.h" /* S21 §4 — the real NVS-backed store */
#include "ff_settings.h"
#include "ff_shell.h"
#include "ff_touchcal.h"

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

static ff_shell_t s_shell;
static ff_clock_t s_clock;
static ff_store_t s_store;
static ff_nvs_store_t s_nvs; /* S21 §4 — backing state for the NVS store */

/* S21 §3 — the shell's injected touch-calibration hook (ff_shell_cfg_t.
 * calibrate_touch), fired by FF_INTENT_CALIBRATE_TOUCH (the Settings
 * "CALIBRATE TOUCH" row). Runs the crosshair capture, applies the solved
 * transform to the LIVE touch path immediately (so the correction is felt
 * without a reflash), and hands it back so the shell persists it into
 * ff_settings (which now lands in NVS, §4). Returns true only on a valid
 * fit; a failed/aborted capture leaves the existing calibration untouched.
 *
 * DEVICE-RUNTIME (the deadlock the sim-only build can't exercise, fixed
 * here): ff_display_run_calibration BLOCKS the calling task in a vTaskDelay
 * loop waiting for the LVGL *task's* release-event cb to capture the five
 * crosshair taps. This hook fires SYNCHRONOUSLY from the Settings row's LVGL
 * click callback (ff_shell_intent_sink -> ff_shell_intent has no queue) —
 * i.e. from inside the esp_lvgl_port UI task — so running the flow here would
 * block that very task and starve the tap-capture -> permanent hang.
 * So it is DEFERRED, two-phase: the first call (from the UI task) only
 * REQUESTS the flow and returns false (the shell does nothing); the main
 * loop (a separate task) runs the crosshair flow off the UI task, then
 * re-emits FF_INTENT_CALIBRATE_TOUCH, and this second call returns the ready
 * result so the SHELL writes it into ff_settings and persists (-> NVS, §4) —
 * keeping the shell the single persistence owner so a later settings save
 * cannot clobber the cal. */
static volatile bool s_calib_requested = false;
static bool s_calib_ready = false;
static ff_touchcal_t s_calib_result;

static bool ff_calibrate_touch_cb(void *user, ff_touchcal_t *out_cal)
{
    (void)user;
    if (out_cal == NULL) {
        return false;
    }
    if (s_calib_ready) { /* second call: the main loop's re-emit — hand back the result */
        *out_cal = s_calib_result;
        s_calib_ready = false;
        return out_cal->valid;
    }
    s_calib_requested = true; /* first call, from the UI task — defer, don't block */
    return false;
}

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

    /* S21 §4 — real NVS store, replacing the no-op stub. ff_shell_init loads
     * settings from it below (persisted values on a returning puck, exact
     * defaults on a fresh/empty partition), and saves on every change; on an
     * NVS failure the store degrades to no-op and the puck runs on defaults
     * (ff_nvs_store_init logs which). */
    s_store = ff_nvs_store_init(&s_nvs);

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &s_clock;
    cfg.store = &s_store;
    /* S21 §3 — the touch-calibration hook the Settings CALIBRATE TOUCH row
     * drives through FF_INTENT_CALIBRATE_TOUCH. */
    cfg.calibrate_touch = ff_calibrate_touch_cb;
    cfg.calibrate_touch_user = NULL;
#if CONFIG_FF_DEMO_MODE
    cfg.pack = &s_demo_pack; /* ff_demo_seed parses the embedded festpack into this */
#endif
    /* cfg.transport / cfg.haptic left zeroed — see slice a. (cfg.pack set above
     * only under CONFIG_FF_DEMO_MODE.) */

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

    /* ---- STAGE 3+: touch + bind the input seam ---- */
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

        /* ---- Touch calibration (S15 slice d) ----
         * STAGE 4 (CAL): run the crosshair capture -> solve -> store ->
         * apply LIVE, then fall through into the normal UI in the SAME boot
         * (no reflash to feel it). STAGE 3: load whatever cal ff_settings
         * holds and apply it. Either way the transform is installed in the
         * one touch seam (ff_display process_coordinates) so gestures,
         * long-press and buttons are all corrected. */
        ff_settings_t settings;
        ff_settings_load(&settings, &s_store); /* NVS (S21 §4): persisted cal, else the identity (uncalibrated) default */

        ff_touchcal_t cal;
        if (stage >= 4) {
            if (ff_display_run_calibration(&cal) != ESP_OK) {
                ff_park("touch calibration flow failed");
                return;
            }
            /* Store into settings so persistence rides the existing seam.
             * S21 §4: s_store is now the real NVS store, so this save PERSISTS
             * across reboot (no longer the no-op stub S15d shipped with). The
             * params are still logged by ff_display_run_calibration. Note the
             * in-app CALIBRATE TOUCH row (S21 §3) is now the primary way to
             * (re)calibrate without a reflash; this boot-time STAGE 4 path
             * remains for bring-up. */
            settings.touch_calibrated = cal.valid;
            settings.touch_ax = cal.ax;
            settings.touch_bx = cal.bx;
            settings.touch_ay = cal.ay;
            settings.touch_by = cal.by;
            ff_settings_save(&settings, &s_store);
            ESP_LOGI(TAG, "touch cal saved to ff_settings via NVS — persists across reboot (S21 §4)");
        } else {
            /* STAGE 3: reconstruct the transform from stored settings. */
            cal.ax = settings.touch_ax;
            cal.bx = settings.touch_bx;
            cal.ay = settings.touch_ay;
            cal.by = settings.touch_by;
            cal.valid = settings.touch_calibrated;
            ESP_LOGI(TAG, "touch cal loaded from ff_settings: calibrated=%d",
                     (int)settings.touch_calibrated);
        }
        ff_display_touch_set_cal(&cal);
    }

    /* First face build (under the LVGL lock — the port owns the LVGL task). */
    if (ff_display_lock(0)) {
        ff_face_build(ff_shell_view(&s_shell));
        ff_display_unlock();
    }
    ESP_LOGI(TAG, "STAGE %d complete: first face flushed to glass", stage);

    /* Apply the persisted display brightness on boot (#100). The setting
     * lives in ff_settings (core, projected into the view); the app forwards
     * it to the LEDC backlight HAL — core never touches IO. Track the last
     * applied value so the render loop below only re-programs the PWM when it
     * actually changes. Note (#bug1): brightness is deliberately EXCLUDED from
     * the shell's render key, so a change does NOT mark the view dirty — the
     * LEDC apply below runs every tick (not only on a dirty one) precisely so a
     * live brightness drag tracks the finger without rebuilding the face. */
    uint8_t last_brightness = ff_shell_view(&s_shell)->settings.brightness_pct;
    (void)ff_display_set_brightness(last_brightness);

    /* Render lifecycle mirrors the sim (targets/sim/ctl_loop.c): tick the
     * shell every frame, rebuild the LVGL tree ONLY on a dirty tick. The
     * esp_lvgl_port task does the actual flushing; we just own the model. */
    while (true) {
        /* S21 §3 (device-runtime): drain a deferred CALIBRATE-TOUCH request in
         * THIS (main) task. ff_display_run_calibration blocks waiting for the
         * LVGL task to capture the taps, so it must not run from the click
         * callback (see ff_calibrate_touch_cb). Re-emitting the intent from
         * here lets the shell persist the result via NVS. */
        if (s_calib_requested) {
            s_calib_requested = false;
            ff_touchcal_t cal;
            if (ff_display_run_calibration(&cal) == ESP_OK) {
                ff_display_touch_set_cal(&cal); /* apply live */
                s_calib_result = cal;
                s_calib_ready = true;
                ff_intent_t calin = {.kind = FF_INTENT_CALIBRATE_TOUCH, .u = {0}};
                ff_shell_intent(&s_shell, &calin); /* shell writes cal -> ff_settings -> NVS */
            } else {
                ESP_LOGE(TAG, "in-app calibration failed — keeping existing cal");
            }
            /* run_calibration cleaned the screen — rebuild the real face. */
            if (ff_display_lock(100)) {
                lv_obj_clean(lv_screen_active());
                ff_face_build(ff_shell_view(&s_shell));
                ff_display_unlock();
            }
        }

        /* ff_bringup_now_ms() is the frozen demo clock under CONFIG_FF_DEMO_MODE
         * (so the seeded projection ages against demo time), real esp_timer time
         * otherwise (S20). */
        bool const dirty = ff_shell_tick(&s_shell, ff_bringup_now_ms());
        ff_app_state_t const *v = ff_shell_view(&s_shell);

        /* Re-program the backlight whenever the projected percent moved
         * (#100/#bug1) — EVERY tick, NOT only on a dirty one. A live
         * brightness drag is deliberately kept out of the shell's render key
         * (ff_shell.c shell_render_key) so it does not force a face rebuild
         * that would destroy the slider mid-drag; that means a brightness-only
         * change no longer sets the dirty bit, so the backlight apply must run
         * outside the dirty gate to follow the finger live. Cheap: a percent
         * compare and, only on an actual change, one LEDC re-program. */
        if (v->settings.brightness_pct != last_brightness) {
            last_brightness = v->settings.brightness_pct;
            (void)ff_display_set_brightness(last_brightness);
        }

        if (dirty) {
            if (ff_display_lock(100 /* ms */)) {
                lv_obj_clean(lv_screen_active());
                ff_face_build(v);
                ff_display_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
