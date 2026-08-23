/**
 * screenshot.h — S13c: shared LVGL-framebuffer -> PNG writer.
 *
 * Extracted out of main.c's original one-shot `--headless --screenshot`
 * path (S13 slice a/b) so the ctl socket's `{"cmd":"screenshot"}` handler
 * and that original path share exactly one XRGB8888->RGB24->PNG
 * implementation, rather than main.c growing a second copy.
 */
#ifndef FF_SCREENSHOT_H
#define FF_SCREENSHOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_screenshot_write — converts an LVGL XRGB8888 framebuffer (byte order
 * B,G,R,X per pixel — see lv_color32_t) to tightly-packed RGB24 and
 * writes it as a PNG at the exact path given. Returns 0 on success, 1 on
 * failure (out-of-memory converting, or the PNG write itself failing —
 * e.g. an unwritable directory in `path`). Prints a one-line diagnostic
 * to stderr on failure; callers needing a machine-readable reason (the
 * ctl socket) just report "screenshot write failed" for any nonzero
 * return, since stderr already carries the detail for a human running
 * ffsim directly.
 */
int ff_screenshot_write(char const *path, uint8_t const *xrgb_buf, int32_t w, int32_t h);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCREENSHOT_H */
