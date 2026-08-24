/**
 * screenshot.c — see screenshot.h. Logic moved verbatim out of main.c's
 * original ff_convert_and_write_png (S13 slice a/b); behavior unchanged
 * except issue #3's fix (the output directory is created if missing —
 * see ff_screenshot_mkdir_parents below).
 */
#include "screenshot.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* Issue #3: `ffsim --headless --screenshot DIR` used to fail if DIR
 * didn't exist yet (the S13 AC's "works after mkdir" finding, from Wave
 * 0 verification). Create every missing directory component of `path`'s
 * containing directory, mkdir -p style, so callers can point the writer
 * at a not-yet-existing output tree. The leaf (the PNG filename itself)
 * is never created here — the loop stops at the last '/'.
 *
 * Deliberately tolerant of EEXIST at every level (the common case: the
 * directory is already there). Any other mkdir failure is reported by
 * the caller; a pre-existing FILE squatting on a directory name isn't
 * specially detected here — mkdir says EEXIST for it too, and the PNG
 * write that follows fails loudly on its own with the path named.
 *
 * Note the ctl socket's screenshot path never depends on this:
 * ff_ctl_out_resolve_path (ctl_out_path.h) rejects a request whose
 * containing directory doesn't already exist under the confined root,
 * BEFORE this writer is called — so this mkdir cannot be steered by a
 * ctl request into creating directories anywhere. It only ever acts on
 * paths ffsim built itself from --screenshot DIR. */
static int ff_screenshot_mkdir_parents(char const *path)
{
    char dir[4096];
    size_t n = strlen(path);
    if (n >= sizeof(dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dir, path, n + 1);

    for (char *p = dir + 1; *p != '\0'; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(dir, 0777) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return 0;
}

int ff_screenshot_write(char const *path, uint8_t const *xrgb_buf, int32_t w, int32_t h)
{
    if (ff_screenshot_mkdir_parents(path) != 0) {
        fprintf(stderr, "ffsim: failed to create output directory for %s\n", path);
        return 1;
    }

    uint8_t *rgb_buf = malloc((size_t)w * (size_t)h * 3);
    if (rgb_buf == NULL) {
        fprintf(stderr, "ffsim: out of memory allocating %ld byte PNG buffer\n", (long)w * h * 3);
        return 1;
    }
    for (int32_t i = 0; i < w * h; i++) {
        const uint8_t b = xrgb_buf[i * 4 + 0];
        const uint8_t g = xrgb_buf[i * 4 + 1];
        const uint8_t r = xrgb_buf[i * 4 + 2];
        rgb_buf[i * 3 + 0] = r;
        rgb_buf[i * 3 + 1] = g;
        rgb_buf[i * 3 + 2] = b;
    }

    int ok = stbi_write_png(path, w, h, 3, rgb_buf, w * 3);
    free(rgb_buf);

    if (!ok) {
        fprintf(stderr, "ffsim: failed to write %s\n", path);
        return 1;
    }
    printf("ffsim: wrote %s\n", path);
    return 0;
}
