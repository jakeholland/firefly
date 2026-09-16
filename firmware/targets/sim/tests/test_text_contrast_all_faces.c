/**
 * test_text_contrast_all_faces.c — puck-ux-usability-review slice 4
 * (docs/reviews/puck-ux-usability-2026-09-15.md, finding 8 + slice 4's
 * own acceptance criterion 1: "no FF_THEME_COLOR_DIM in any
 * lv_obj_set_style_text_color call — grep-asserted in a test").
 *
 * This file goes further than a grep: rather than asserting the absence
 * of one named constant (which would miss any OTHER colour, opacity, or
 * future regression that also lands under WCAG AA), it MEASURES every
 * text object's real, resolved on-screen colour against its real,
 * resolved effective background, for every committed golden fixture, and
 * fails on anything below 4.5:1 — the same "build the real screen, force
 * the real state, read the real result" discipline
 * `test_press_feedback_all_faces.c` already established for press
 * feedback, applied here to legibility (AGENTS.md's proxy-check lesson:
 * "measure, don't reason harder"). A grep for `FF_THEME_COLOR_DIM` alone
 * would not have caught `FF_THEME_COLOR_MUTED` used at a REDUCED text
 * opacity (`lv_obj_set_style_text_opa`) — a real second way for text to
 * quietly end up under 4.5:1 that this codebase already has three call
 * sites of (settings section headers at `LV_OPA_60`, the Signals popup's
 * scope hint at `LV_OPA_40`) — so this test resolves opacity too, not
 * just the named colour constant.
 *
 * ## What "effective background" means here
 * LVGL has no single "the background behind this label" query — a label
 * sits on whatever its nearest OPAQUE ancestor painted. This walker
 * composes every ancestor's own `bg_color`/`bg_opa` from the fixture's
 * OUTERMOST object (seeded with `FF_THEME_COLOR_BG`, the puck's own
 * canvas colour — nothing in this codebase paints the screen object
 * itself) down to the label's immediate parent, using the same
 * Porter-Duff-style `lv_color_mix` LVGL's own renderer uses to composite
 * a partially-opaque fill over what's beneath it. This is an
 * APPROXIMATION for a genuinely non-rectangular or gradient background —
 * neither of which this codebase's flat, solid-fill design language
 * uses anywhere — and is exact for every real case here.
 *
 * ## Scope / known exclusions
 * - Empty-text labels (`""`, e.g. an unused/placeholder object): nothing
 *   is drawn, so there is no contrast question to ask.
 * - Hidden labels (`LV_OBJ_FLAG_HIDDEN`): not on screen, nothing to read.
 * - A label whose OWN `text_opa` is below full: the label's DISPLAYED
 *   colour is `lv_color_mix(text_color, effective_bg, text_opa)`, which
 *   this walker computes and checks — a dim colour hiding behind a
 *   reduced opacity is exactly the finding-8-adjacent gap a grep alone
 *   would miss (see top comment).
 */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "lvgl.h"

#include "face_dispatch.h"
#include "ff_theme.h"
#include "fixture.h"

#ifndef FF_FIXTURE_DIR
#define FF_FIXTURE_DIR "tests/fixtures/"
#endif

/* WCAG AA for normal text. This codebase's smallest body text
 * (FF_THEME_FONT_CHIP/LABEL, montserrat_14) is well under the 18pt/14pt-
 * bold "large text" carve-out at any plausible physical size on this
 * glass (docs/reviews/puck-ux-usability-2026-09-15.md's own §5 table:
 * 0.87mm cap height), so every text object on this device is held to the
 * stricter, "normal text" 4.5:1 bar — never the 3.0:1 large-text one. */
#define FF_TEXT_CONTRAST_MIN 4.5

void setUp(void) {}

void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* ---------------------------------------------------------------------
 * WCAG 2.1 relative luminance / contrast ratio, computed directly from
 * 8-bit sRGB channel values — the same formula
 * docs/reviews/puck-ux-usability-2026-09-15.md's own §5 contrast table
 * was computed with (that table's numbers were independently
 * re-verified against this exact formula before this PR: INK/BG
 * 17.08:1, MUTED/BG 5.78:1, DIM/BG 2.64:1 — all match).
 * ------------------------------------------------------------------- */

static double srgb_channel_linear(uint8_t c8)
{
    double c = (double)c8 / 255.0;
    return (c <= 0.03928) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

static double relative_luminance(lv_color_t c)
{
    return 0.2126 * srgb_channel_linear(c.red) + 0.7152 * srgb_channel_linear(c.green) +
           0.0722 * srgb_channel_linear(c.blue);
}

static double contrast_ratio(lv_color_t a, lv_color_t b)
{
    double la = relative_luminance(a);
    double lb = relative_luminance(b);
    double lighter = la > lb ? la : lb;
    double darker = la > lb ? lb : la;
    return (lighter + 0.05) / (darker + 0.05);
}

/* ---------------------------------------------------------------------
 * Effective background — see this file's top comment.
 * ------------------------------------------------------------------- */

static lv_color_t effective_bg_color(lv_obj_t *label)
{
    enum { MAX_DEPTH = 48 };
    lv_obj_t *chain[MAX_DEPTH];
    int n = 0;
    lv_obj_t *p = lv_obj_get_parent(label);
    while (p != NULL && n < MAX_DEPTH) {
        chain[n++] = p;
        p = lv_obj_get_parent(p);
    }

    /* Compose from the OUTERMOST ancestor (closest to the fixture's own
     * root, drawn first) down to the label's immediate parent (drawn
     * last, i.e. on top) — the same order the renderer itself paints in,
     * so each layer's partial opacity blends over what a real frame
     * would actually show beneath it. */
    lv_color_t bg = lv_color_hex(FF_THEME_COLOR_BG);
    for (int i = n - 1; i >= 0; i--) {
        lv_opa_t opa = lv_obj_get_style_bg_opa(chain[i], LV_PART_MAIN);
        if (opa > LV_OPA_TRANSP) {
            lv_color_t c = lv_obj_get_style_bg_color(chain[i], LV_PART_MAIN);
            bg = lv_color_mix(c, bg, opa);
        }
    }
    return bg;
}

typedef struct {
    int checked;
    int violations;
} contrast_result_t;

static void contrast_walk(lv_obj_t *obj, char const *fixture_name, contrast_result_t *out)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        char const *text = lv_label_get_text(obj);
        if (text != NULL && text[0] != '\0') {
            lv_color_t const text_color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
            lv_opa_t const text_opa = lv_obj_get_style_text_opa(obj, LV_PART_MAIN);
            lv_color_t const bg = effective_bg_color(obj);
            /* The label's REAL displayed colour, opacity blended in — a
             * dim colour hiding behind a reduced text_opa is exactly the
             * gap a bare grep for FF_THEME_COLOR_DIM would miss (top
             * comment). Full opacity (255) is a no-op mix. */
            lv_color_t const displayed = (text_opa >= LV_OPA_COVER) ? text_color : lv_color_mix(text_color, bg, text_opa);

            double const ratio = contrast_ratio(displayed, bg);
            out->checked++;
            if (ratio < FF_TEXT_CONTRAST_MIN) {
                out->violations++;
                printf("  LOW-CONTRAST [%s] text=\"%s\" color=#%02X%02X%02X opa=%u bg=#%02X%02X%02X ratio=%.2f:1 "
                       "(floor %.1f:1)\n",
                       fixture_name, text, displayed.red, displayed.green, displayed.blue, (unsigned)text_opa,
                       bg.red, bg.green, bg.blue, ratio, FF_TEXT_CONTRAST_MIN);
            }
        }
    }

    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        contrast_walk(lv_obj_get_child(obj, i), fixture_name, out);
    }
}

static void contrast_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static uint32_t contrast_tick_cb(void)
{
    return 0;
}

static contrast_result_t contrast_fixture(char const *path, char const *name)
{
    lv_init();
    lv_tick_set_cb(contrast_tick_cb);

    const int32_t w = FF_THEME_WINDOW_PX;
    const int32_t h = FF_THEME_WINDOW_PX;
    const uint32_t buf_size = (uint32_t)(w * h * 4);
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf, path);

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, contrast_flush_cb);
    lv_display_set_default(disp);

    ff_app_state_t state;
    ff_fixture_result_t fr = ff_fixture_load_file(path, &state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FF_FIXTURE_OK, fr, path);

    ff_build_face_screen(&state);
    lv_refr_now(disp);

    contrast_result_t result = {0, 0};
    contrast_walk(lv_screen_active(), name, &result);

    free(buf);
    lv_deinit();

    return result;
}

/* The test — every *.json under tests/fixtures/, no allowlist. Legibility
 * is universal, same reasoning test_press_feedback_all_faces.c gives for
 * press feedback: a control missing it on ANY face is a real defect, not
 * just on the faces this review happened to screenshot. */
static void S_TEXT_CONTRAST_every_label_on_every_fixture_meets_aa(void)
{
    DIR *d = opendir(FF_FIXTURE_DIR);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, FF_FIXTURE_DIR);

    int total_checked = 0;
    int total_violations = 0;
    int fixtures_swept = 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t nlen = strlen(entry->d_name);
        bool is_json = (nlen > 5) && (strcmp(entry->d_name + nlen - 5, ".json") == 0);
        if (!is_json) {
            continue;
        }

        char name[sizeof(entry->d_name)];
        snprintf(name, sizeof(name), "%s", entry->d_name);

        char path[sizeof(FF_FIXTURE_DIR) + sizeof(entry->d_name)];
        snprintf(path, sizeof(path), "%s%s", FF_FIXTURE_DIR, entry->d_name);

        contrast_result_t r = contrast_fixture(path, name);
        total_checked += r.checked;
        total_violations += r.violations;
        fixtures_swept++;
    }
    closedir(d);

    printf("test_text_contrast_all_faces: swept %d fixture(s), checked %d text label(s), %d violation(s)\n",
           fixtures_swept, total_checked, total_violations);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, fixtures_swept, "no fixtures found under " FF_FIXTURE_DIR);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, total_checked, "swept fixtures but found zero text labels");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, total_violations,
                                  "one or more text labels fall below WCAG AA (4.5:1) against their real, resolved "
                                  "background — see the LOW-CONTRAST lines above");
}

/* ---------------------------------------------------------------------
 * A second, narrower guard: the literal grep-style acceptance criterion
 * slice 4 names explicitly ("no FF_THEME_COLOR_DIM in any
 * lv_obj_set_style_text_color call — grep-asserted in a test"). Kept
 * alongside the measurement-based sweep above, not instead of it: a
 * grep is cheap, exact, and catches the specific regression finding 8
 * names by construction (a future contributor pattern-matching an
 * existing `FF_THEME_COLOR_DIM` call site elsewhere in the file and
 * reusing it for text), while the sweep above catches the broader
 * "anything at all under 4.5:1" property a grep cannot express.
 * ------------------------------------------------------------------- */

#ifndef FF_APP_SCREENS_DIR
#define FF_APP_SCREENS_DIR "../../app/screens/"
#endif

static bool file_contains_dim_text_color_call(char const *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return false; /* file not found is a NOT_NULL failure elsewhere, not a silent pass here */
    }
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        /* Narrow, textual match for the exact shape every real call site
         * in this codebase uses: `text_color(..., lv_color_hex(FF_THEME_
         * COLOR_DIM)`. Deliberately NOT matched: `radar_make_chip(...,
         * FF_THEME_COLOR_DIM, ...)` (a bg_hex parameter, not a text
         * colour — scr_radar.c's two LAST SEEN/SIGNAL chips) and the
         * plain border-colour use in scr_radar.c's ghost-dot outline —
         * neither line contains the literal substring "text_color". */
        if (strstr(line, "text_color") != NULL && strstr(line, "FF_THEME_COLOR_DIM") != NULL) {
            found = true;
            printf("  DIM-AS-TEXT [%s] %s", path, line);
        }
    }
    fclose(f);
    return found;
}

static void S_NO_DIM_in_any_text_color_call_across_app_screens(void)
{
    static char const *const files[] = {
        FF_APP_SCREENS_DIR "scr_banner.c",     FF_APP_SCREENS_DIR "scr_compose.c",
        FF_APP_SCREENS_DIR "scr_flare.c",      FF_APP_SCREENS_DIR "scr_inbox.c",
        FF_APP_SCREENS_DIR "scr_launcher.c",   FF_APP_SCREENS_DIR "scr_lineup.c",
        FF_APP_SCREENS_DIR "scr_map.c",        FF_APP_SCREENS_DIR "scr_nav.c",
        FF_APP_SCREENS_DIR "scr_power_menu.c", FF_APP_SCREENS_DIR "scr_radar.c",
        FF_APP_SCREENS_DIR "scr_settings.c",   FF_APP_SCREENS_DIR "scr_widgets.c",
        FF_APP_SCREENS_DIR "scr_music.c",
    };
    bool any_found = false;
    int files_checked = 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        FILE *probe = fopen(files[i], "r");
        if (!probe) {
            continue; /* a screen file that doesn't exist in this checkout — skip, don't fail the build over it */
        }
        fclose(probe);
        files_checked++;
        if (file_contains_dim_text_color_call(files[i])) {
            any_found = true;
        }
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, files_checked, "none of the expected app/screens/*.c files were found — "
                                                            "check FF_REPO_ROOT / the test's working directory");
    TEST_ASSERT_FALSE_MESSAGE(any_found,
                              "FF_THEME_COLOR_DIM must never be passed to a *_text_color call — see the "
                              "DIM-AS-TEXT lines above");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S_TEXT_CONTRAST_every_label_on_every_fixture_meets_aa);
    RUN_TEST(S_NO_DIM_in_any_text_color_call_across_app_screens);

    return UNITY_END();
}
