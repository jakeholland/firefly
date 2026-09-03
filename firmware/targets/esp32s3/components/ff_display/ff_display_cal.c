/**
 * ff_display_cal.c — the S15 slice d crosshair-capture touch-calibration
 * flow, split out of ff_display.c (tech-debt sprint, device-target
 * slice: a pure mechanical move, zero behaviour change — see that PR for
 * the file map and rationale).
 *
 * Everything below is ff_display_run_calibration and the state/helpers
 * only it uses: five on-glass crosshair targets, one captured raw tap
 * per target, fed to ff_touchcal_solve (core/, unchanged). See
 * ff_display_run_calibration's own doc comment (ff_display.h) for the
 * full contract; the section comment just below (moved verbatim from
 * ff_display.c) has the capture-order/well-conditioned-fit rationale.
 *
 * ffd_screen_flip, ffd_active_cal, ffd_cal_capturing, ffd_lv_disp and
 * ffd_touch are shared with ff_display.c via ff_display_internal.h — see
 * that header's own doc comment for the extern-vs-accessor design choice
 * this split made, and each variable's own doc comment there for why
 * this file touches it.
 */
#include "ff_display.h"
#include "ff_display_internal.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ff_display";

/* =====================================================================
 * S15 slice d — crosshair capture calibration flow.
 *
 * Five targets (screen space): center first, then the four insets — all
 * comfortably inside the round glass, giving 3 distinct x and 3 distinct y
 * for a well-conditioned per-axis fit (docs/specs/S15d). One raw tap is
 * captured per target on a full press+release (debounce), then the pairs
 * feed ff_touchcal_solve. The capture runs with the active cal at identity
 * so the taps recorded are raw; the caller installs the solved transform.
 * ===================================================================== */
enum { FF_CAL_TARGET_COUNT = 5 };
static const int s_cal_tx[FF_CAL_TARGET_COUNT] = {206, 90, 322, 90, 322};
static const int s_cal_ty[FF_CAL_TARGET_COUNT] = {206, 90, 90, 322, 322};

static struct {
    volatile int captured; /* number of targets captured so far          */
    volatile bool done;    /* set true once all FF_CAL_TARGET_COUNT taken */
    ff_cal_point_t pts[FF_CAL_TARGET_COUNT];
    lv_obj_t *label;
    lv_obj_t *ring;
    lv_obj_t *bar_h;
    lv_obj_t *bar_v;
    lv_obj_t *dot;
} s_cal;

/* Move the crosshair to target `idx` and update the progress text. Runs
 * under the LVGL lock (from the setup path or the LVGL-task event cb). */
static void ff_cal_place_crosshair(int idx)
{
    int tx = s_cal_tx[idx];
    int ty = s_cal_ty[idx];
    lv_obj_set_pos(s_cal.ring, tx - 20, ty - 20);
    lv_obj_set_pos(s_cal.bar_h, tx - 20, ty - 1);
    lv_obj_set_pos(s_cal.bar_v, tx - 1, ty - 20);
    lv_obj_set_pos(s_cal.dot, tx - 3, ty - 3);
    lv_label_set_text_fmt(s_cal.label, "Tap the target\n%d / %d", idx + 1, FF_CAL_TARGET_COUNT);
}

/* One capture per target, on release (a deliberate, completed tap). */
static void ff_cal_release_cb(lv_event_t *e)
{
    if (s_cal.done) {
        return;
    }
    int idx = s_cal.captured;
    if (idx >= FF_CAL_TARGET_COUNT) {
        return;
    }
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    s_cal.pts[idx].raw_x = (int)p.x;
    s_cal.pts[idx].raw_y = (int)p.y;
    /* format v8 amendment: the crosshair is placed at the LOGICAL
     * framebuffer position (s_cal_tx/ty) — under a HARDWARE mirror
     * (screen_flip), it physically APPEARS at that position's 180-degree
     * rotation, which is where the raw ticks captured above (flip
     * deliberately skipped during capture — see ffd_cal_capturing's doc
     * comment, ff_display_internal.h) actually measure. Record the
     * PHYSICAL position as the fit target, not the logical one, so the
     * solved transform is a pure sensor characterization — see
     * ffd_cal_capturing's doc comment for the full reasoning. A no-op
     * rotation when screen_flip is off. */
    if (ffd_screen_flip) {
        ff_touchcal_flip180(s_cal_tx[idx], s_cal_ty[idx], FF_LCD_H_RES, FF_LCD_V_RES, &s_cal.pts[idx].screen_x,
                             &s_cal.pts[idx].screen_y);
    } else {
        s_cal.pts[idx].screen_x = s_cal_tx[idx];
        s_cal.pts[idx].screen_y = s_cal_ty[idx];
    }
    ESP_LOGI(TAG, "cal capture %d/%d: raw (%d,%d) -> target (%d,%d) [logical (%d,%d)%s]", idx + 1,
             FF_CAL_TARGET_COUNT, (int)p.x, (int)p.y, s_cal.pts[idx].screen_x, s_cal.pts[idx].screen_y,
             s_cal_tx[idx], s_cal_ty[idx], ffd_screen_flip ? ", screen_flip on" : "");

    s_cal.captured = idx + 1;
    if (s_cal.captured >= FF_CAL_TARGET_COUNT) {
        lv_label_set_text(s_cal.label, "Calibrating...");
        s_cal.done = true; /* published last, after pts[] is written */
    } else {
        ff_cal_place_crosshair(s_cal.captured);
    }
}

static lv_obj_t *ff_cal_make_bar(lv_obj_t *parent, int w, int h, lv_color_t col)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, col, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE); /* clicks pass to root */
    return o;
}

esp_err_t ff_display_run_calibration(ff_touchcal_t *out_cal)
{
    if (out_cal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ff_touchcal_identity(out_cal);

    if (ffd_lv_disp == NULL || ffd_touch == NULL) {
        ESP_LOGE(TAG, "run_calibration needs lvgl_start + touch_start first");
        return ESP_ERR_INVALID_STATE;
    }

    /* Capture RAW: the active transform must be identity while we record,
     * AND (format v8 amendment) the screen-flip rotation step must be
     * skipped too — see ffd_cal_capturing's own doc comment
     * (ff_display_internal.h) for why. */
    ff_touchcal_identity(&ffd_active_cal);
    ffd_cal_capturing = true;
    memset(&s_cal, 0, sizeof(s_cal));

    const lv_color_t accent = lv_color_hex(0xFFC66B); /* Firefly amber */

    if (!ff_display_lock(1000)) {
        ESP_LOGE(TAG, "run_calibration: LVGL lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    /* Full-screen black backdrop that catches every tap. */
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_cal.label = lv_label_create(root);
    lv_obj_set_style_text_color(s_cal.label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_cal.label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cal.label, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_remove_flag(s_cal.label, LV_OBJ_FLAG_CLICKABLE);

    /* Crosshair: an open ring + a thin cross + a centre dot, all in accent
     * and all click-through so the tap always reaches `root`. */
    s_cal.ring = lv_obj_create(root);
    lv_obj_remove_style_all(s_cal.ring);
    lv_obj_set_size(s_cal.ring, 40, 40);
    lv_obj_set_style_radius(s_cal.ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_cal.ring, 3, 0);
    lv_obj_set_style_border_color(s_cal.ring, accent, 0);
    lv_obj_set_style_bg_opa(s_cal.ring, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_cal.ring, LV_OBJ_FLAG_CLICKABLE);

    s_cal.bar_h = ff_cal_make_bar(root, 40, 2, accent);
    s_cal.bar_v = ff_cal_make_bar(root, 2, 40, accent);
    s_cal.dot = ff_cal_make_bar(root, 6, 6, accent);
    lv_obj_set_style_radius(s_cal.dot, LV_RADIUS_CIRCLE, 0);

    lv_obj_add_event_cb(root, ff_cal_release_cb, LV_EVENT_RELEASED, NULL);
    ff_cal_place_crosshair(0);
    ff_display_unlock();

    ESP_LOGI(TAG, "S15d calibration started: tap each of %d crosshairs on the glass",
             FF_CAL_TARGET_COUNT);

    /* Block this task until the LVGL-task event cb has captured all five. */
    while (!s_cal.done) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ffd_cal_capturing = false; /* every point is captured; live touch handling resumes normally */

    bool ok = ff_touchcal_solve(s_cal.pts, FF_CAL_TARGET_COUNT, out_cal);

    ESP_LOGI(TAG, "=== S15d touch calibration result ===");
    for (int i = 0; i < FF_CAL_TARGET_COUNT; i++) {
        ESP_LOGI(TAG, "  pair %d/%d: raw (%d,%d) -> screen (%d,%d)", i + 1, FF_CAL_TARGET_COUNT,
                 s_cal.pts[i].raw_x, s_cal.pts[i].raw_y, s_cal.pts[i].screen_x, s_cal.pts[i].screen_y);
    }
    if (ok) {
        ESP_LOGI(TAG, "  params: ax=%.6f bx=%.4f ay=%.6f by=%.4f", (double)out_cal->ax,
                 (double)out_cal->bx, (double)out_cal->ay, (double)out_cal->by);
        ESP_LOGI(TAG, "  transform: sx = %.6f*rawx %+.4f   sy = %.6f*rawy %+.4f", (double)out_cal->ax,
                 (double)out_cal->bx, (double)out_cal->ay, (double)out_cal->by);
    } else {
        ESP_LOGE(TAG, "  DEGENERATE capture (no x- or y-spread) — identity kept, no correction. "
                      "Re-run calibration and tap the distinct targets.");
    }
    ESP_LOGI(TAG, "=====================================");

    /* Tear the calibration screen down; the caller rebuilds the real face. */
    if (ff_display_lock(1000)) {
        lv_obj_clean(lv_screen_active());
        ff_display_unlock();
    }
    /* Drop the now-dangling object pointers (screen was cleaned). */
    memset(&s_cal, 0, sizeof(s_cal));

    return ESP_OK;
}
