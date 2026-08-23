/**
 * screenshot.c — see screenshot.h. Logic moved verbatim out of main.c's
 * original ff_convert_and_write_png (S13 slice a/b); behavior unchanged.
 */
#include "screenshot.h"

#include <stdio.h>
#include <stdlib.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int ff_screenshot_write(char const *path, uint8_t const *xrgb_buf, int32_t w, int32_t h)
{
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
