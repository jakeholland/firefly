/**
 * png_diff.c — pixel-diff core implementation. See png_diff.h.
 */
#include "png_diff.h"

#include <stdlib.h>
#include <string.h>

static bool ff_channel_diff_exceeds(uint8_t av, uint8_t bv, int tolerance)
{
    int d = (int)av - (int)bv;
    if (d < 0) d = -d;
    return d > tolerance;
}

void ff_png_diff_compare(uint8_t const *a, uint32_t aw, uint32_t ah, uint8_t const *b, uint32_t bw, uint32_t bh,
                          double threshold_pct, ff_png_diff_result_t *out)
{
    memset(out, 0, sizeof(*out));

    if (aw != bw || ah != bh) {
        out->width = aw;
        out->height = ah;
        out->dims_match = false;
        out->within_threshold = false;
        return;
    }

    out->width = aw;
    out->height = ah;
    out->dims_match = true;
    out->n_total_pixels = (uint64_t)aw * (uint64_t)ah;

    for (uint64_t i = 0; i < out->n_total_pixels; i++) {
        uint8_t const *pa = a + i * 3;
        uint8_t const *pb = b + i * 3;
        if (ff_channel_diff_exceeds(pa[0], pb[0], 0) || ff_channel_diff_exceeds(pa[1], pb[1], 0) ||
            ff_channel_diff_exceeds(pa[2], pb[2], 0)) {
            out->n_diff_pixels++;
        }
    }

    out->diff_pct = out->n_total_pixels == 0 ? 0.0 : (double)out->n_diff_pixels / (double)out->n_total_pixels * 100.0;
    out->within_threshold = out->diff_pct <= threshold_pct;
}

size_t ff_png_diff_sidebyside_size(uint32_t w, uint32_t h)
{
    return (size_t)(3u * w) * (size_t)h * 3u;
}

void ff_png_diff_render_sidebyside(uint8_t const *a, uint8_t const *b, uint32_t w, uint32_t h, uint8_t *out_buf)
{
    uint32_t out_w = 3u * w;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint64_t src_i = (uint64_t)y * w + x;
            uint8_t const *pa = a + src_i * 3;
            uint8_t const *pb = b + src_i * 3;

            uint8_t *dst_a = out_buf + ((uint64_t)y * out_w + x) * 3;
            uint8_t *dst_b = out_buf + ((uint64_t)y * out_w + (w + x)) * 3;
            uint8_t *dst_d = out_buf + ((uint64_t)y * out_w + (2u * w + x)) * 3;

            dst_a[0] = pa[0];
            dst_a[1] = pa[1];
            dst_a[2] = pa[2];

            dst_b[0] = pb[0];
            dst_b[1] = pb[1];
            dst_b[2] = pb[2];

            bool differs = ff_channel_diff_exceeds(pa[0], pb[0], 0) || ff_channel_diff_exceeds(pa[1], pb[1], 0) ||
                            ff_channel_diff_exceeds(pa[2], pb[2], 0);
            if (differs) {
                dst_d[0] = 255;
                dst_d[1] = 0;
                dst_d[2] = 0;
            } else {
                dst_d[0] = 0;
                dst_d[1] = 0;
                dst_d[2] = 0;
            }
        }
    }
}
