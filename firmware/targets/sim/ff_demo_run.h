/**
 * ff_demo_run.h — targets/sim: the `--demo` render driver (S20).
 *
 * Boots a real (no-transport) ff_shell_t into the fictional Firefly Fields
 * world via ff_demo_seed (app/include/ff_demo.h) and renders every
 * populated face. This is the sim's primary S20 verification path — the
 * PNGs it writes are how the maintainer sees demo mode without a device.
 */
#ifndef FF_DEMO_RUN_H
#define FF_DEMO_RUN_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_run_demo_headless — render the demo world's faces to
 * `screenshot_dir`/<name>.png and exit. Writes:
 *   radar.png        (DANA selected — live arrow)
 *   radar_riley.png  (RILEY selected — close-range rings)
 *   radar_maya.png   (MAYA selected — "LAST SEEN 25 MIN")
 *   radar_sam.png    (SAM selected  — "NO FIX YET")
 *   now.png          (Saturday lineup, FIREFLY mid-set, starred countdown)
 *   map.png          (stages + landmarks + crew dots)
 *   signals.png      (the seeded feed)
 *
 * Each face is rendered from a freshly-seeded shell (the four Radar shots
 * differ only in which crew member is the default selection). Returns 0 on
 * success, 1 on any setup/seed/render/write failure.
 */
int ff_run_demo_headless(const char *screenshot_dir);

/**
 * ff_run_demo_window — window mode: seed the demo world once and show it
 * live in an SDL window (Radar first; swipe/tap navigate as usual, since
 * the intent seam is bound to the demo shell). Never returns in practice
 * (exits on window close / Ctrl+C). Returns nonzero only on a setup
 * failure before the loop.
 */
int ff_run_demo_window(void);

#ifdef __cplusplus
}
#endif

#endif /* FF_DEMO_RUN_H */
