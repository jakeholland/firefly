/**
 * compare_png.c — golden-screenshot pixel diff CLI (S14 slice b).
 *
 * Usage: compare_png A.png B.png [--threshold PCT] [--diff-out PATH]
 *
 * Compares two PNGs pixel-for-pixel (RGB24, alpha ignored — screenshots
 * are opaque). Exit code 0 if the percentage of differing pixels is
 * <= PCT (default 0.5, matching docs/specs/S14-testing-ci.md's "pixel
 * diff, ≤0.5% threshold"), else 1. Exit code 2 on a hard failure (file
 * unreadable, dimension mismatch — mismatched dimensions are never a
 * percentage-scored difference).
 *
 * On failure (exit 1), if --diff-out PATH was given, writes a
 * side-by-side [A | B | differing-pixels-highlighted-red] PNG to PATH
 * for CI artifact upload (see tests/run_goldens.sh).
 *
 * All the actual comparison math lives in png_diff.c/png_diff.h (unit
 * tested directly, including the exact 0.4%/0.5%/0.6% threshold
 * boundary, in tools/tests/test_png_diff.c) — this file is I/O plumbing
 * only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png_diff.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* Issue #26: the vendored stb_image_write.h uses sprintf, which newer
 * toolchains flag with -Wdeprecated-declarations — a build failure under
 * our -Werror in sanitizer builds. Suppressed for the vendored header's
 * own compilation only (same targeted push/pop as
 * targets/sim/screenshot.c, the other include site); this file's own
 * code keeps full -Wall -Wextra -Werror. stb_image.h above doesn't need
 * it — that header contains no sprintf. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

#define FF_COMPARE_PNG_DEFAULT_THRESHOLD_PCT 0.5

static void usage(char const *argv0)
{
    fprintf(stderr, "usage: %s A.png B.png [--threshold PCT] [--diff-out PATH]\n", argv0);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    char const *path_a = argv[1];
    char const *path_b = argv[2];
    double threshold_pct = FF_COMPARE_PNG_DEFAULT_THRESHOLD_PCT;
    char const *diff_out_path = NULL;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold_pct = atof(argv[++i]);
        } else if (strcmp(argv[i], "--diff-out") == 0 && i + 1 < argc) {
            diff_out_path = argv[++i];
        } else {
            fprintf(stderr, "compare_png: unrecognized argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    int aw, ah, an;
    uint8_t *a = stbi_load(path_a, &aw, &ah, &an, 3);
    if (a == NULL) {
        fprintf(stderr, "compare_png: failed to read %s: %s\n", path_a, stbi_failure_reason());
        return 2;
    }

    int bw, bh, bn;
    uint8_t *b = stbi_load(path_b, &bw, &bh, &bn, 3);
    if (b == NULL) {
        fprintf(stderr, "compare_png: failed to read %s: %s\n", path_b, stbi_failure_reason());
        stbi_image_free(a);
        return 2;
    }

    ff_png_diff_result_t result;
    ff_png_diff_compare(a, (uint32_t)aw, (uint32_t)ah, b, (uint32_t)bw, (uint32_t)bh, threshold_pct, &result);

    if (!result.dims_match) {
        fprintf(stderr, "compare_png: dimension mismatch: %s is %dx%d, %s is %dx%d\n", path_a, aw, ah, path_b, bw,
                bh);
        stbi_image_free(a);
        stbi_image_free(b);
        return 2;
    }

    printf("compare_png: %s vs %s: %llu/%llu pixels differ (%.4f%%, threshold %.4f%%) -> %s\n", path_a, path_b,
           (unsigned long long)result.n_diff_pixels, (unsigned long long)result.n_total_pixels, result.diff_pct,
           threshold_pct, result.within_threshold ? "PASS" : "FAIL");

    int rc = result.within_threshold ? 0 : 1;

    if (!result.within_threshold && diff_out_path != NULL) {
        size_t sz = ff_png_diff_sidebyside_size((uint32_t)aw, (uint32_t)ah);
        uint8_t *diff_buf = malloc(sz);
        if (diff_buf == NULL) {
            fprintf(stderr, "compare_png: out of memory allocating %zu byte diff buffer\n", sz);
        } else {
            ff_png_diff_render_sidebyside(a, b, (uint32_t)aw, (uint32_t)ah, diff_buf);
            int ok = stbi_write_png(diff_out_path, 3 * aw, ah, 3, diff_buf, 3 * aw * 3);
            if (ok) {
                printf("compare_png: wrote diff artifact %s\n", diff_out_path);
            } else {
                fprintf(stderr, "compare_png: failed to write diff artifact %s\n", diff_out_path);
            }
            free(diff_buf);
        }
    }

    stbi_image_free(a);
    stbi_image_free(b);
    return rc;
}
