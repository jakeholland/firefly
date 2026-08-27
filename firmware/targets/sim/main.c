/**
 * ffsim — Firefly desktop sim target (S13 slice a, extended in slices b/c,
 * S16 slice d).
 *
 * Modes:
 *   ffsim                          window mode: opens an SDL window,
 *                                  runs LVGL's normal timer loop.
 *   ffsim --headless --screenshot DIR
 *                                  renders exactly one frame into an
 *                                  offscreen LVGL buffer (no SDL, no
 *                                  display server required) and writes
 *                                  it to DIR/boot.png, then exits 0.
 *   ffsim --headless --screenshot DIR --fixture FILE.json
 *                                  same, but loads FILE.json into an
 *                                  ff_app_state_t (fixture.h) and renders
 *                                  it instead of the boot screen, writing
 *                                  DIR/<stem>.png. A fixture whose
 *                                  active_face already has a real screen
 *                                  (radar/now/signals via the shared shell,
 *                                  scr_nav.h; compose as its own full
 *                                  screen) gets that; every other face
 *                                  still gets the S13 placeholder debug
 *                                  face (fixture_view.h) — see
 *                                  face_dispatch.h's ff_build_face_screen.
 *                                  This one-shot render never ticks a
 *                                  shell — see run_goldens.sh's byte-
 *                                  identical-goldens contract, which this
 *                                  path exists to preserve untouched.
 *   ffsim --fixture FILE.json      window mode with the fixture loaded
 *                                  (interactive preview; same
 *                                  face-selection and load path as
 *                                  headless). STATIC: no shell backs
 *                                  this, so every S10 button is a
 *                                  documented no-op — see ff_run_window's
 *                                  doc comment for the live alternative.
 *   ffsim --connect HOST:PORT [--pack FILE.json] [--dev-trust-all]
 *         [--fixture FILE.json]
 *                                  S16 slice d: LIVE window mode. Opens
 *                                  an SDL window driving a real
 *                                  `ff_shell_t` over the mc TCP
 *                                  transport, ticking it every frame and
 *                                  rebuilding the screen only when the
 *                                  tick's dirty bit says the rendered
 *                                  view changed. Every S10/S16 button
 *                                  (FLARE, GO/DISMISS/CANCEL, the T9
 *                                  keypad, ...) is live: the intent seam
 *                                  is bound to this shell. `--fixture`
 *                                  only seeds the screen shown before the
 *                                  first tick.
 *   ffsim --headless --ctl PORT [--fixture FILE.json] [--mock-clock]
 *         [--connect HOST:PORT] [--pack FILE.json] [--ctl-out DIR]
 *         [--dev-trust-all]
 *                                  S13 slice c: opens a persistent,
 *                                  headless control-socket-driven session
 *                                  instead of rendering once and exiting.
 *                                  See ctl_server.h and
 *                                  firmware/tools/dev/CTL.md for the wire
 *                                  protocol. Runs until a `{"cmd":"quit"}`
 *                                  is received. --ctl currently requires
 *                                  --headless — capturing a screenshot
 *                                  from window mode's SDL-backed display
 *                                  would need querying LVGL's internal
 *                                  draw buffer (a different code path
 *                                  than the FULL-mode offscreen buffer
 *                                  this file already owns), for zero
 *                                  benefit to this mode's actual
 *                                  consumers (the e2e harness always
 *                                  drives ffsim headless). --ctl-out DIR
 *                                  confines the ctl socket's "screenshot"
 *                                  command's writes under DIR (created if
 *                                  missing; defaults to --screenshot's
 *                                  DIR if that was also given, else a
 *                                  fresh temp directory) — see
 *                                  ctl_out_path.h and ctl_loop.c's
 *                                  ctl_loop_screenshot.
 *
 * --mock-clock freezes the LVGL tick source for the one-shot headless and
 * STATIC (fixture-only, no --connect) window paths (see ff_mock_tick_cb
 * below); ff_run_headless_once() UNCONDITIONALLY calls
 * lv_tick_set_cb(ff_mock_tick_cb) regardless of whether --mock-clock was
 * passed — headless rendering is deterministic either way (lv_refr_now()
 * does NOT skip the tick — it unconditionally calls lv_anim_refr_now()
 * internally, which reads the tick and runs one animation step; this
 * matters since S06's CLOSE-mode radar face starts a real lv_anim_t for
 * its pulsing rings, see app/screens/scr_radar.c — but the unconditional
 * freeze is what actually makes it deterministic, not the flag). The flag
 * is accepted and honored in headless mode purely so callers
 * (tests/run_goldens.sh) can pass it explicitly rather than depending on
 * undocumented default behavior. In --ctl mode --mock-clock instead gates
 * the ctl socket's `{"cmd":"clock"}` command — see ctl_loop.c. LIVE window
 * mode (--connect, no --ctl) always uses real time: there is no ctl
 * socket there to advance a frozen clock by hand, so a frozen tick would
 * just mean the window never re-renders past its first frame — --mock-
 * clock is silently ignored when combined with --connect in window mode.
 *
 * --connect/--pack (S13 slice a/b flags) drive an `ff_shell_t` — the S16
 * app shell — over the mc TCP transport (S16 slice b2; the interim
 * `live.{c,h}` wiring this file used before b2 is retired, see
 * docs/specs/S13-sim-target.md's Amendments). The setup sequence
 * (transport, --dev-trust-all, --pack, heading) is shared between live
 * window mode and the ctl loop via live_setup.h — see that header's top
 * comment for why. The ctl socket's `{"cmd":"state"}` dump reads
 * `ff_shell_view()`, plus a `"wall"` object from `ff_shell_wall()` (see
 * tools/dev/CTL.md).
 *
 * --dev-trust-all (S16 AC6, sim-only, COMPILED OUT of device builds —
 * see the #error guard in live_setup.c and ff_shell.h's dev-affordances
 * section): auto-pairs every NodeInfo sender, suspends the self filter,
 * and latches the wall clock from the host's own clock, so the
 * single-node dev meshtasticd can play a crew member. Logs a line naming
 * itself at startup. Without it, live mode routes every inbound event
 * through the exact same shell entry points and drops unpaired traffic —
 * a node that has only ever sent NodeInfo + Position produces zero feed
 * items and no roster slot.
 *
 * RENDER LIFECYCLE (S16 slice d, closes #17/#29): both live entry points
 * — this window mode and the ctl loop (ctl_loop.c) — tick the shell every
 * frame and rebuild the LVGL screen ONLY when `ff_shell_tick`'s return
 * says the rendered view actually changed, always `lv_obj_clean()`ing the
 * active screen first. `app/screens/scr_nav.c` also now builds content
 * into the ACTIVE tile only, not all three. See ctl_loop.h's top comment
 * for the full reasoning (shared by both entry points).
 *
 * The boot screen and the fixture debug face are both scaffolding: real
 * screens arrive with S06+.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>

#include "lvgl.h"

/* NOT re-defined here: STB_IMAGE_WRITE_IMPLEMENTATION lives in
 * screenshot.c (S13c extracted the XRGB8888->PNG writer out of this file
 * so the ctl socket's "screenshot" command and the one-shot
 * --headless --screenshot path share one implementation) — S10 slice b's
 * main.c (pre-extraction) had its own copy of this define/include; that
 * would now be a duplicate-symbol link error against ff-stb-image-write,
 * so it's intentionally dropped here rather than merged back in. */
#include "ff_version.h"

#include "ctl_loop.h"      /* S16 slice d — the --ctl session, extracted out of this file */
#include "ctl_server.h"
#include "face_dispatch.h" /* PR #25 UX review follow-up — ff_build_face_screen extracted
                             * here (shared with targets/sim/tests/test_face_hit_targets.c)
                             * instead of defined locally in this file. */
#include "ff_intent.h"     /* S16 slice d — binding the seam for live window mode */
#include "ff_shell.h"      /* S16b2 — live mode is the app shell now (live.{c,h} retired) */
#include "ff_wall.h"       /* S18c — the no-pack-window decay backstop (build-date proximity guard) */
#include "fixture.h"
#include "live_setup.h"    /* S16 slice d — the setup sequence shared with ctl_loop.c */
#include "screenshot.h"    /* ff_screenshot_write — the one-shot headless render path */

/* The sim window/framebuffer IS the device panel: 412x412 (S15 slice c,
 * ff_theme.h's FF_THEME_WINDOW_PX). The puck fills it with no margin, so a
 * sim golden PNG is the same 412x412 frame the device draws — sim and glass
 * match pixel-for-pixel. */
#define FF_SIM_WINDOW_W 412
#define FF_SIM_WINDOW_H 412

#define FF_COLOR_BG_DARK 0x0b0b10
#define FF_COLOR_AMBER   0xffc66b

/* Builds the boot placeholder UI (dark puck + centered "FIREFLY" label)
 * on whatever the current default display's active screen is. */
static void ff_build_boot_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_SIM_WINDOW_W - 16, FF_SIM_WINDOW_H - 16);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(puck);
    lv_label_set_text(label, "FIREFLY");
    lv_obj_set_style_text_color(label, lv_color_hex(FF_COLOR_AMBER), 0);
    lv_obj_center(label);
}

/* Full-frame render mode: the whole buffer is the flushed frame, so the
 * flush callback only needs to signal completion. */
static void ff_headless_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

/* --mock-clock (one-shot headless/STATIC window paths only — see
 * ctl_loop.c's ff_ctl_loop_tick_cb for the --ctl loop's clock): a frozen
 * tick source (always reports the
 * same instant). Headless rendering is already deterministic without it
 * — a single lv_refr_now() call with no timers run and no animations
 * started never reads the tick at all — but it's accepted (and honored)
 * in headless mode too so callers (tests/run_goldens.sh) can pass it
 * explicitly rather than relying on that being true forever as a
 * coincidence. */
static uint32_t ff_mock_tick_cb(void)
{
    return 0;
}

/* ff_clock_t's now_ms callback shape for LIVE window mode — a thin
 * wrapper rather than a raw SDL_GetTicks cast: SDL_GetTicks takes no
 * arguments and ff_clock_t's callback takes a `void *user`, so casting
 * the function pointer itself would call through an incompatible type,
 * undefined behavior under C11 6.3.2.3 even where it happens to work on
 * a given ABI (the same class of bug ff_intent.h's top comment warns
 * about for its own callback shape). */
static uint32_t ff_win_clock_now_ms(void *user)
{
    (void)user;
    return SDL_GetTicks();
}

/* Renders exactly one frame — either the fixture debug face (if
 * fixture_path is non-NULL) or the boot placeholder — to
 * DIR/<name>.png. Returns 0 on success, 1 on any failure (fixture load,
 * OOM, or PNG write). Deliberately unaffected by S16 slice d: this path
 * never ticks a shell, so the golden suite it feeds stays byte-identical
 * — a lifecycle refactor that changed pixels here would be wrong (see
 * this file's top comment). */
static int ff_run_headless_once(const char *screenshot_dir, const char *fixture_path)
{
    lv_init();
    lv_tick_set_cb(ff_mock_tick_cb);

    const int32_t w = FF_SIM_WINDOW_W;
    const int32_t h = FF_SIM_WINDOW_H;
    const uint32_t buf_size = (uint32_t)(w * h * 4);

    uint8_t *xrgb_buf = malloc(buf_size);
    if (xrgb_buf == NULL) {
        fprintf(stderr, "ffsim: out of memory allocating %u byte framebuffer\n", buf_size);
        lv_deinit();
        return 1;
    }

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, xrgb_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, ff_headless_flush_cb);
    lv_display_set_default(disp);

    char path[4096];
    if (fixture_path != NULL) {
        ff_app_state_t state;
        ff_fixture_result_t fr = ff_fixture_load_file(fixture_path, &state);
        if (fr != FF_FIXTURE_OK) {
            fprintf(stderr, "ffsim: failed to load fixture %s (error %d)\n", fixture_path, (int)fr);
            free(xrgb_buf);
            lv_deinit();
            return 1;
        }
        /* Every S10 button (GO/DISMISS/CANCEL/FLARE) emits a semantic
         * intent through the seam (S16) — a one-shot headless render
         * never binds anything to it (see ff_intent.h's top comment:
         * unbound is a safe no-op), so buttons still render, just
         * inertly, with no special-casing needed here at all. */
        ff_build_face_screen(&state);

        char stem[256];
        ff_fixture_stem(fixture_path, stem, sizeof(stem));
        snprintf(path, sizeof(path), "%s/%s.png", screenshot_dir, stem);
    } else {
        ff_build_boot_screen();
        snprintf(path, sizeof(path), "%s/boot.png", screenshot_dir);
    }

    lv_refr_now(disp);
    int rc = ff_screenshot_write(path, xrgb_buf, w, h);

    free(xrgb_buf);
    lv_deinit();
    return rc;
}

/**
 * ff_run_window — window mode. Two shapes, chosen by whether
 * `connect_hostport` was given:
 *
 *  - STATIC (no --connect): a `--fixture`-loaded (or boot) screen is
 *    built exactly once and LVGL's own timer loop just keeps repainting
 *    that same static tree. No shell exists, so the intent seam is never
 *    bound and every S10/S16 button is a documented no-op — printed as a
 *    NOTE for a takeover fixture specifically, since that screen has no
 *    other on-screen way to leave.
 *  - LIVE (--connect given, S16 slice d): a real `ff_shell_t` is brought
 *    up (`ff_live_setup` — the same sequence the ctl loop uses),
 *    `--fixture` seeds only the very first frame, and every frame after
 *    that ticks the shell and rebuilds the screen ONLY on a dirty tick
 *    (build-once/update-in-place, closing #17/#29 the same way
 *    ctl_loop.c does). The intent seam IS bound here, so every button is
 *    live: this is the first mode where a window-mode FLARE tap, T9
 *    keystroke, GO/DISMISS, ... actually does something.
 *
 * Neither shape ever returns in practice — the process exits via window
 * close or Ctrl+C, same as every window-mode build before this slice.
 */
static int ff_run_window(const char *fixture_path, bool mock_clock, const char *connect_hostport,
                          const char *pack_path, bool dev_trust_all)
{
    lv_init();

    if (connect_hostport != NULL) {
        /* LIVE window mode. --mock-clock is silently ignored here (see
         * this file's top comment): SDL_GetTicks is the only clock that
         * can actually advance a live window without a ctl socket to
         * drive it by hand. */
        lv_tick_set_cb((lv_tick_get_cb_t)SDL_GetTicks);
    } else {
        lv_tick_set_cb(mock_clock ? ff_mock_tick_cb : (lv_tick_get_cb_t)SDL_GetTicks);
    }

    lv_display_t *disp = lv_sdl_window_create(FF_SIM_WINDOW_W, FF_SIM_WINDOW_H);
    lv_sdl_window_set_title(disp, "Firefly (ffsim)");
    lv_sdl_mouse_create();

    if (connect_hostport != NULL) {
        static ff_shell_t s_win_shell;
        static fp_pack_t s_win_pack;
        static ff_clock_t s_win_clock;

        s_win_clock.now_ms = ff_win_clock_now_ms;
        s_win_clock.user = NULL;

        ff_shell_cfg_t shell_cfg;
        memset(&shell_cfg, 0, sizeof(shell_cfg));
        shell_cfg.clock = &s_win_clock;
        shell_cfg.store = NULL; /* settings persistence is S16 slice e */
        shell_cfg.pack = &s_win_pack;

        ff_live_setup_cfg_t live_cfg = {
            .connect_hostport = connect_hostport,
            .pack_path = pack_path,
            .dev_trust_all = dev_trust_all,
        };
        ff_live_setup_t live;
        if (ff_live_setup(&s_win_shell, &shell_cfg, &live_cfg, &live) != 0) {
            return 1;
        }

        /* Every S10/S16 button now reaches this shell — see this
         * function's doc comment. */
        ff_intent_emit_bind(ff_shell_intent_sink, &s_win_shell);

        if (fixture_path != NULL) {
            ff_app_state_t seed;
            ff_fixture_result_t fr = ff_fixture_load_file(fixture_path, &seed);
            if (fr != FF_FIXTURE_OK) {
                fprintf(stderr, "ffsim: failed to load fixture %s (error %d) — seeding the boot screen instead\n",
                        fixture_path, (int)fr);
                ff_build_boot_screen();
            } else {
                ff_build_face_screen(&seed);
            }
        } else {
            ff_build_boot_screen();
        }

        while (true) {
            bool const dirty = ff_shell_tick(&s_win_shell, SDL_GetTicks());
            if (dirty) {
                lv_obj_clean(lv_screen_active());
                ff_build_face_screen(ff_shell_view(&s_win_shell));
            }
            uint32_t next_ms = lv_timer_handler();
            SDL_Delay(next_ms > 0 ? next_ms : 1);
        }
    }

    if (fixture_path != NULL) {
        ff_app_state_t state;
        ff_fixture_result_t fr = ff_fixture_load_file(fixture_path, &state);
        if (fr != FF_FIXTURE_OK) {
            fprintf(stderr, "ffsim: failed to load fixture %s (error %d)\n", fixture_path, (int)fr);
            return 1;
        }

        /* STATIC preview: no --connect means no shell, so no target
         * binds the intent seam (`ff_intent_emit_bind` — see ff_intent.h's
         * top comment for why unbound is a documented, safe no-op). Every
         * S10 button (GO/DISMISS/CANCEL/FLARE) still emits its intent
         * through the seam; it just reaches nothing. Printed for takeover
         * fixtures specifically because that screen has no other way to
         * leave. */
        if (state.flare.takeover_active) {
            fprintf(stderr,
                    "ffsim: NOTE — this is a STATIC single-frame preview (no --connect). GO/DISMISS/CANCEL/"
                    "FLARE emit intents through the seam (S16), but this window binds nothing "
                    "to it, so every button here is inert. This takeover screen has NO "
                    "on-screen way to leave — close the window or Ctrl+C to exit. Pass --connect "
                    "HOST:PORT for a live window where these buttons actually work.\n");
        }

        ff_build_face_screen(&state);
    } else {
        ff_build_boot_screen();
    }

    while (true) {
        uint32_t next_ms = lv_timer_handler();
        SDL_Delay(next_ms > 0 ? next_ms : 1);
    }
}

/* S18 slice c (#40): the no-pack bootstrap window's decay backstop. The
 * fixed [FLOOR, CEILING) plausibility window decays silently as real time
 * creeps toward the ceiling, and the no-pack handshake path still rides
 * it. FF_WALL_BUILD_UNIX_S is this build's date, injected by CMake
 * (string(TIMESTAMP)); when it lands within 12 months of the ceiling we
 * print a LOUD, DATED warning to stderr on every startup — so a CI run
 * (the headless-screenshot step invokes ffsim and shows its output)
 * surfaces the deadline with a full year of runway to bump the epoch.
 * Deliberately a warning, never an exit code: a build's pass/fail must not
 * depend on the day it ran (#40's calendar-flakiness rule). The predicate
 * and the threshold live in core (ff_wall_ceiling_deadline_near /
 * FF_WALL_CEILING_WARN_LEAD_S), so this cannot drift from the gate. */
static void ffsim_warn_if_epoch_ceiling_near(void)
{
#ifdef FF_WALL_BUILD_UNIX_S
    int64_t const build_unix = (int64_t)FF_WALL_BUILD_UNIX_S;
    if (ff_wall_ceiling_deadline_near(build_unix)) {
        fprintf(stderr,
                "\n"
                "================================================================\n"
                "  WARNING (S18c/#40): the wall-clock plausibility ceiling is\n"
                "  near. This build's date (unix %lld) is within 12 months of\n"
                "  FF_WALL_EPOCH_CEILING (unix %lld). The no-pack bootstrap\n"
                "  window is DECAYING — bump FF_WALL_EPOCH_FLOOR per the\n"
                "  Release checklist in firmware/README.md.\n"
                "================================================================\n\n",
                (long long)build_unix, (long long)FF_WALL_EPOCH_CEILING);
    }
#endif
}

int main(int argc, char **argv)
{
    ffsim_warn_if_epoch_ceiling_near();

    bool headless = false;
    bool mock_clock = false;
    const char *screenshot_dir = NULL;
    const char *fixture_path = NULL;
    const char *ctl_port_str = NULL;
    const char *connect_hostport = NULL;
    const char *pack_path = NULL;
    const char *ctl_out_arg = NULL;
    bool dev_trust_all = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_dir = argv[++i];
        } else if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
            fixture_path = argv[++i];
        } else if (strcmp(argv[i], "--mock-clock") == 0) {
            mock_clock = true;
        } else if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
            ctl_port_str = argv[++i];
        } else if (strcmp(argv[i], "--ctl-out") == 0 && i + 1 < argc) {
            ctl_out_arg = argv[++i];
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            connect_hostport = argv[++i];
        } else if (strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
            pack_path = argv[++i];
        } else if (strcmp(argv[i], "--dev-trust-all") == 0) {
            dev_trust_all = true; /* sim-only by construction — see live_setup.c */
        }
    }

    printf("ffsim: %s\n", ff_version_string());

    if (ctl_port_str != NULL) {
        if (!headless) {
            fprintf(stderr, "ffsim: --ctl currently requires --headless (see main.c's top comment)\n");
            return 1;
        }
        char *end = NULL;
        long port = strtol(ctl_port_str, &end, 10);
        if (end == ctl_port_str || *end != '\0' || port <= 0 || port > 65535) {
            fprintf(stderr, "ffsim: --ctl expects a port number, got \"%s\"\n", ctl_port_str);
            return 1;
        }

        static ff_shell_t s_shell;
        static fp_pack_t s_pack;
        static ff_ctl_loop_ctx_t s_ctx;

        ff_shell_cfg_t shell_cfg;
        memset(&shell_cfg, 0, sizeof(shell_cfg));
        shell_cfg.store = NULL; /* settings persistence is S16 slice e */

        ff_ctl_loop_cfg_t loop_cfg = {
            .fixture_path = fixture_path,
            .mock_clock = mock_clock,
            .connect_hostport = connect_hostport,
            .pack_path = pack_path,
            .dev_trust_all = dev_trust_all,
            .ctl_out_arg = ctl_out_arg,
            .screenshot_dir = screenshot_dir,
        };
        if (ff_ctl_loop_open(&s_ctx, &s_shell, &s_pack, &shell_cfg, &loop_cfg) != 0) {
            /* ff_ctl_loop_open already tore down what IT opened on every
             * failure path, including lv_deinit() — nothing left to
             * clean up here. */
            return 1;
        }

        ff_ctl_server_t ctl_srv;
        if (ff_ctl_open(&ctl_srv, (uint16_t)port, FF_CTL_DEFAULT_IDLE_TIMEOUT_MS) != 0) {
            fprintf(stderr, "ffsim: failed to open ctl socket on 127.0.0.1:%u\n", (unsigned)port);
            ff_ctl_loop_close(&s_ctx);
            ff_shell_close(&s_shell);
            lv_deinit();
            return 1;
        }
        printf("ffsim: ctl socket listening on 127.0.0.1:%u\n", (unsigned)port);

        bool quit_requested = false;
        ff_ctl_handlers_t handlers = ff_ctl_loop_handlers(&s_ctx, &quit_requested);

        while (!quit_requested) {
            ff_ctl_loop_pump(&s_ctx);
            lv_timer_handler();
            if (ff_ctl_poll(&ctl_srv, &handlers)) break;
            usleep(5000); /* ~200 Hz: responsive without busy-spinning a CPU core */
        }

        ff_ctl_close(&ctl_srv);
        ff_ctl_loop_close(&s_ctx);
        ff_shell_close(&s_shell);
        lv_deinit();
        return 0;
    }

    if (dev_trust_all && connect_hostport == NULL) {
        /* Meaningful only where a live shell with a transport exists.
         * Fail loud rather than silently accept a flag that would do
         * nothing (CLAUDE.md: honest over pretty). */
        fprintf(stderr, "ffsim: --dev-trust-all requires --connect (a live session)\n");
        return 1;
    }

    if (headless) {
        if (screenshot_dir == NULL) {
            fprintf(stderr, "ffsim: --headless requires --screenshot DIR (or --ctl PORT)\n");
            return 1;
        }
        if (connect_hostport != NULL) {
            fprintf(stderr,
                    "ffsim: --connect needs a live session (--ctl PORT, or window mode) — a one-shot "
                    "--headless --screenshot render never ticks a shell\n");
            return 1;
        }
        /* mock_clock is unconditionally honored in headless mode (see
         * ff_run_headless_once's tick setup) — accepted here without a
         * "not meaningful" warning since passing it explicitly is the
         * documented, supported way callers (tests/run_goldens.sh) opt
         * into that guarantee rather than depending on an undocumented
         * default. */
        (void)mock_clock;
        return ff_run_headless_once(screenshot_dir, fixture_path);
    }

    return ff_run_window(fixture_path, mock_clock, connect_hostport, pack_path, dev_trust_all);
}
