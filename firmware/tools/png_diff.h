/**
 * png_diff.h — pixel-diff core for the golden-screenshot tool
 * (S14 slice b: docs/specs/S14-testing-ci.md, "pixel diff, ≤0.5%
 * threshold, emits side-by-side diff PNGs").
 *
 * Deliberately split from compare_png.c's CLI/file-I/O wrapper so the
 * threshold math is directly unit-testable against synthetic in-memory
 * buffers (see tools/tests/test_png_diff.c) without needing to read or
 * write actual PNG files, including the exact 0.5% boundary itself.
 *
 * Pure C11 (memory buffers in, memory buffer/struct out); no PNG I/O
 * here — that's stb_image/stb_image_write, used only by compare_png.c.
 */
#ifndef FF_PNG_DIFF_H
#define FF_PNG_DIFF_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A pixel counts as "differing" if any of its three channels differs by
 * more than this from the corresponding pixel in the other image. 0
 * (byte-exact) is the only value used anywhere in this codebase today —
 * the 0.5% pixel-count budget exists to absorb anti-aliasing/rounding
 * noise across otherwise pixel-identical renders (S14 spec), not to
 * blur per-channel color tolerance — but the knob is kept generic
 * rather than hardcoding an equality compare, in case that changes. */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint64_t n_total_pixels;
    uint64_t n_diff_pixels;
    double   diff_pct;        /* n_diff_pixels / n_total_pixels * 100.0 */
    bool     dims_match;      /* false: width/height mismatch is a hard
                                  failure, never scored by percentage */
    bool     within_threshold; /* diff_pct <= threshold_pct (inclusive);
                                   always false when !dims_match */
} ff_png_diff_result_t;

/**
 * ff_png_diff_compare — compares two RGB24 (3 bytes/pixel, row-major, no
 * row padding) buffers.
 *
 * `threshold_pct` is the maximum allowed percentage of differing pixels,
 * inclusive: a result exactly at the threshold passes (`≤0.5%` per
 * spec, not `<0.5%`).
 *
 * If `a`/`b` have different `w`/`h`, `*out` reports `dims_match = false`
 * and `within_threshold = false` immediately (pixel counts are left 0 —
 * there is no meaningful per-pixel comparison across mismatched
 * dimensions).
 */
void ff_png_diff_compare(uint8_t const *a, uint32_t aw, uint32_t ah, uint8_t const *b, uint32_t bw, uint32_t bh,
                          double threshold_pct, ff_png_diff_result_t *out);

/**
 * ff_png_diff_sidebyside_size — byte size of the buffer
 * ff_png_diff_render_sidebyside needs for images of size w x h (RGB24,
 * three w-wide panels laid out horizontally: `a | b | highlighted-diff`,
 * so the output is `3*w` wide and `h` tall).
 */
size_t ff_png_diff_sidebyside_size(uint32_t w, uint32_t h);

/**
 * ff_png_diff_render_sidebyside — renders `a`, `b`, and a red/black
 * differing-pixel highlight map side by side into `out_buf`
 * (caller-allocated, at least `ff_png_diff_sidebyside_size(w, h)` bytes,
 * RGB24, row-major, `3*w` pixels wide by `h` tall). Requires `a` and `b`
 * to already be the same `w`/`h` (caller's job to check, e.g. via
 * `ff_png_diff_compare`'s `dims_match` first).
 */
void ff_png_diff_render_sidebyside(uint8_t const *a, uint8_t const *b, uint32_t w, uint32_t h, uint8_t *out_buf);

#ifdef __cplusplus
}
#endif

#endif /* FF_PNG_DIFF_H */
