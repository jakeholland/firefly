/**
 * scr_widgets.c — see scr_widgets.h.
 */
#include "scr_widgets.h"

#include "ff_theme.h"
#include "scr_nav.h" /* ff_scr_button_create — the shared PRESS_LOCK-clearing button base (#145/#148) */

lv_obj_t *ff_scr_pill_create(lv_obj_t *parent, char const *text, ff_scr_pill_cfg_t const *cfg)
{
    lv_obj_t *btn = ff_scr_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, cfg->w, cfg->h);
    lv_obj_set_style_radius(btn, cfg->radius, 0);

    if (cfg->filled) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(cfg->bg_hex), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, cfg->border_width, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(cfg->bg_hex), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    }

    switch (cfg->press) {
    case FF_SCR_PILL_PRESS_DIM:
        /* A solid fill dims (ink wash) rather than tinting toward itself. */
        lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_INK), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
        break;
    case FF_SCR_PILL_PRESS_TINT:
        lv_obj_set_style_bg_color(btn, lv_color_hex(cfg->press_tint_hex), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
        break;
    case FF_SCR_PILL_PRESS_NONE:
    default:
        break; /* no press style — the resting styling above is all this pill ever shows */
    }

    if (cfg->use_pos) {
        lv_obj_set_pos(btn, cfg->x, cfg->y);
    } else {
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, cfg->dy);
    }

    if (cfg->cb != NULL) {
        lv_obj_add_event_cb(btn, cfg->cb, LV_EVENT_CLICKED, cfg->user_data);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, cfg->font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(cfg->fg_hex), 0);
    if (cfg->letter_space != 0) {
        lv_obj_set_style_text_letter_space(label, cfg->letter_space, 0);
    }
    lv_obj_center(label);

    return btn;
}

lv_obj_t *ff_scr_glass_rim_create(lv_obj_t *parent, uint32_t color_hex, lv_opa_t opa, int32_t border_px,
                                   bool screen_flip)
{
    /* Concentric with the VISIBLE glass, not the framebuffer: the bezel
     * window is offset from the pixel array (FF_THEME_GLASS_*), and a
     * ring hugging the edge is the one shape where a few px shows —
     * verbatim geometry from scr_radar.c's original radar_build_rim_tint
     * (S06/#154/#155), now the one place every edge-hugging ring shares
     * it (see this function's own doc comment in scr_widgets.h). */
    lv_obj_t *rim = lv_obj_create(parent);
    lv_obj_remove_style_all(rim);
    lv_obj_set_size(rim, 2 * FF_THEME_GLASS_R - 4, 2 * FF_THEME_GLASS_R - 4);
    lv_obj_set_style_radius(rim, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(rim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rim, border_px, 0);
    lv_obj_set_style_border_color(rim, lv_color_hex(color_hex), 0);
    lv_obj_set_style_border_opa(rim, opa, 0);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(rim, LV_ALIGN_CENTER, ff_theme_glass_cx(screen_flip) - FF_THEME_PUCK_RADIUS_PX,
                 ff_theme_glass_cy(screen_flip) - FF_THEME_PUCK_RADIUS_PX);
    return rim;
}
