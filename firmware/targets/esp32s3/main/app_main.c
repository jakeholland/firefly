/**
 * app_main.c — ESP32-S3 target entry point (S15 slice b — display + touch).
 *
 * Slice a booted ff_shell against three stub HALs with the screen dark.
 * Slice b keeps that boot EXACTLY (same clock stub, same no-op store, same
 * no-transport shell) and adds the real display + touch HAL on top, staged
 * so the maintainer can flash one gate at a time and watch the glass:
 *
 *   STAGE 1 (first light): expander up -> release resets -> backlight ->
 *            SPD2010 QSPI panel init -> a solid fill + two-colour split via
 *            esp_lcd_panel_draw_bitmap. NO LVGL. Proves the panel path.
 *   STAGE 2 (app on glass): + LVGL v9 via esp_lvgl_port, render one real
 *            ff_shell face (Radar, no selection) — the same projection the
 *            sim renders for that state.
 *   STAGE 3 (touch drives it): + SPD2010 touch -> an LVGL pointer indev
 *            wired to the SAME abstract input seam the sim ctl socket
 *            drives (ff_intent_emit -> ff_shell_intent_sink). A physical
 *            tap/swipe changes shell state. The stored touch calibration
 *            (if any) is loaded from ff_settings and applied.
 *   STAGE 4 (calibrate touch, S15d): + the crosshair capture flow — tap 5
 *            targets -> ff_touchcal_solve -> store to ff_settings -> apply
 *            LIVE and continue straight into the normal UI in the same
 *            boot (no reflash to feel the correction). Params + pairs are
 *            logged. See docs/specs/S15d-touch-calibration.md.
 *
 * The stage is chosen by CONFIG_FF_BRINGUP_STAGE (menuconfig ->
 * "Firefly bring-up", default 1). core/ and app/ are UNCHANGED; every new
 * line is under targets/esp32s3/. Every init step logs so a dark screen
 * still says how far it got.
 *
 * The clock / store / no-transport rationale is unchanged from slice a —
 * see the git history of this file / the S15a PR body. NVS store and UART
 * transport remain later slices (c/d/e).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h> /* S26 slice f — snprintf, ff_wakeup_cause_str's fallback branch */
#include <string.h>

#include "driver/gpio.h" /* S26 slice f — GPIO wake source config (touch-INT/PWR/BOOT) for light sleep */
#include "driver/usb_serial_jtag.h" /* S26 slice f amendment — usb_serial_jtag_is_connected(), sleep-inhibit sample */
#include "esp_err.h" /* esp_err_to_name() — S26 slice g's boot-splash failure log */
#include "esp_log.h"
#include "esp_sleep.h"  /* S26 slice f — esp_light_sleep_start() + wake-source config */
#include "esp_system.h" /* esp_restart() — S26 slice b's reboot action */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ff_button.h"     /* S26 slice e — core: the generic debounced push-button (BOOT-as-home) */
#include "ff_display.h"
#include "ff_face.h"
#include "ff_idle.h"       /* S26 slices c+f — core: inactivity -> dim -> screen off -> light sleep */
#include "ff_intent.h"
#include "ff_nvs_store.h" /* S21 §4 — the real NVS-backed store */
#include "ff_power.h"      /* S25 — battery keep-alive latch (must fire first) + S26b PWR/BOOT sampling */
#include "ff_power_fsm.h"  /* S26 slice b — core: the press-timing FSM + reboot BOOT-release guard */
#include "ff_settings.h"
#include "ff_shell.h"
#include "ff_touchcal.h"

#include "esp_heap_caps.h" /* PSRAM allocs: shell + demo festpack */
#if CONFIG_FF_DEMO_MODE /* S20 — the festpack is allocated in PSRAM (see s_demo_pack) */
#include "ff_demo.h"       /* S20 — demo-mode seeding */
#if CONFIG_FF_DEMO_LIVE
#include "ff_demoapply.h" /* S23c — apply an emitted event through the real inbound seam */
#include "ff_demofeed.h"  /* S23a — the deterministic synthetic event generator */
#endif
#endif

static const char *TAG = "firefly";

#if !CONFIG_FF_DEMO_MODE
/* ff_clock_t — monotonic ms over esp_timer_get_time() (see slice a).
 * Compiled out under CONFIG_FF_DEMO_MODE, which drives a frozen demo clock
 * instead (below) and never reads real time. */
static uint32_t ff_esp_clock_now_ms(void *user)
{
    (void)user;
    return (uint32_t)(esp_timer_get_time() / 1000);
}
#endif

#if CONFIG_FF_DEMO_MODE
/* S20 demo mode pins the shell's clock at a fixed instant (Sat 21:30) so
 * the seeded world stays frozen and honest — freshness/countdowns read the
 * seeded moment rather than drifting with real boot time. ff_demo_seed sets
 * this counter; both the injected ff_clock_t AND every ff_shell_tick read
 * it (see ff_bringup_now_ms), so the projection ages against the same
 * clock the seed used. The demo festpack is EMBED_FILES'd (main/CMakeLists). */
static uint32_t s_demo_clock_ms;
/* The parsed festpack is ~23 KB. It lives in PSRAM, not internal .bss: internal
 * RAM is scarce and must be left for LVGL's DMA strip buffers, and only the demo
 * build allocates it at all. Populated by ff_demo_seed via the shell's cfg.pack. */
static fp_pack_t *s_demo_pack;
/* S26 slice (a) — the jsmn token scratch fp_parse tokenizes into while
 * parsing the demo festpack above. FP_MAX_TOKENS * sizeof(jsmntok_t) is
 * 128KB: PSRAM, never internal DIRAM — reclaiming THAT is the whole point
 * of this slice (docs/specs/S26-device-lifecycle.md). Intentionally
 * NEVER FREED: ff_shell_cfg_t.toks is stored by the shell and reused by
 * every ff_shell_load_pack call for the shell's whole lifetime (same
 * contract as cfg.pack/s_demo_pack above), not just the one boot-time
 * demo parse — ff_shell_load_pack is the shell's real production
 * pack-load path (already used off-device by the sim's --pack/--connect
 * flow), so a future on-device reload would parse into freed memory if
 * this were freed after ff_demo_seed. 128KB out of 8MB PSRAM is a
 * trade worth making to keep that pointer valid for as long as the
 * shell might call ff_shell_load_pack again. */
static jsmntok_t *s_demo_toks;
extern const uint8_t firefly_pack_start[] asm("_binary_firefly_fields_festpack_json_start");
extern const uint8_t firefly_pack_end[] asm("_binary_firefly_fields_festpack_json_end");

/* S24d — the demo LOOPBACK sender. The demo build has no mesh transport, so
 * the real mc_send_* path refuses every send; this accepts them (returns 0)
 * so the shell pushes the OUT feed item and the user's own outbound shows in
 * the thread. Kept INSIDE this CONFIG_FF_DEMO_MODE block precisely so a field
 * build never compiles it (see the install site + ff_demo.h note). It echoes
 * the user's REAL outbound and reaches no radio; it fabricates nothing. */
static int demo_loopback_send_text(void *ctx, uint32_t dest, const char *utf8)
{
    (void)ctx;
    (void)dest;
    (void)utf8;
    return 0;
}
static int demo_loopback_send_private(void *ctx, uint32_t dest, const uint8_t *payload, size_t len)
{
    (void)ctx;
    (void)dest;
    (void)payload;
    (void)len;
    return 0;
}
static ff_wiring_sender_t ff_demo_loopback_sender(void)
{
    ff_wiring_sender_t s;
    s.send_text = demo_loopback_send_text;
    s.send_private = demo_loopback_send_private;
    s.ctx = NULL;
    return s;
}

#if CONFIG_FF_DEMO_LIVE
/* S23 (slice b) — LIVE demo clock. When live, the demo clock ADVANCES from
 * the seeded epoch (s_demo_clock_ms, pinned by ff_demo_seed) by real
 * esp_timer elapsed, so ages tick and the ff_demofeed schedule fires.
 * s_demo_epoch_us is esp_timer's value latched the instant live mode
 * STARTS — deliberately AFTER ff_demo_seed returns, so seeding itself runs
 * against the frozen epoch (MAYA's 25-min-old fix etc. age honestly against
 * the seeded wall, exactly as S20). Until then s_demo_live_started is false
 * and the clock reads the frozen epoch. */
static int64_t s_demo_epoch_us;
static bool s_demo_live_started;

static void ff_demo_live_start(void)
{
    s_demo_epoch_us = esp_timer_get_time();
    s_demo_live_started = true;
}
#endif

/* The demo clock's current value in ms. S20 static: frozen at the seeded
 * epoch. S23 live: epoch + real elapsed since live start. Both the injected
 * ff_clock_t and every ff_shell_tick read THIS, so the projection ages
 * against one clock (the invariant S20 established). */
static uint32_t ff_demo_now_ms(void)
{
#if CONFIG_FF_DEMO_LIVE
    if (s_demo_live_started) {
        int64_t d_us = esp_timer_get_time() - s_demo_epoch_us;
        if (d_us < 0) d_us = 0; /* monotonic, but never age backwards on a fluke */
        return s_demo_clock_ms + (uint32_t)(d_us / 1000);
    }
#endif
    return s_demo_clock_ms; /* frozen epoch (S20 static, and pre-live-start under LIVE) */
}

static uint32_t ff_demo_clock_now_ms(void *user)
{
    (void)user;
    return ff_demo_now_ms();
}
#endif

/* The monotonic "now" every ff_shell_tick in this file uses: the demo's
 * frozen clock under CONFIG_FF_DEMO_MODE, otherwise real esp_timer time.
 * Keeping tick and the injected clock on the SAME source is what makes the
 * demo's seeded freshness render correctly. */
static uint32_t ff_bringup_now_ms(void)
{
#if CONFIG_FF_DEMO_MODE
    return ff_demo_now_ms(); /* frozen epoch (S20 static) or advancing (S23 live) */
#else
    return ff_esp_clock_now_ms(NULL);
#endif
}

static ff_shell_t *s_shell_p; /* PSRAM since S24(c/d); internal DRAM stays for LVGL DMA + stacks (#109) */
#define s_shell (*s_shell_p)
static ff_clock_t s_clock;
static ff_store_t s_store;
static ff_nvs_store_t s_nvs; /* S21 §4 — backing state for the NVS store */

#if CONFIG_FF_DEMO_MODE && CONFIG_FF_DEMO_LIVE
/* S23 (slice c) — the live demo's synthetic event source. Held across the
 * whole run; ff_demofeed_tick is drained each frame in the render loop and
 * every event is applied through the shell's real inbound seam (see the
 * apply loop). The idx->node_id map + string table live in ff_demoapply. */
static ff_demofeed_t s_demofeed;

/* Drain the generator up to `now_ms` and apply each due event through the
 * shell's mesh-inbound callbacks, so a synthetic signal/poke is
 * indistinguishable from a mesh one (S23 AC2/AC3). Called every frame,
 * before ff_shell_tick, so new feed items surface in the same projection. */
static void ff_demo_live_pump(uint32_t now_ms)
{
    mc_events_t ev = ff_shell_events(&s_shell);
    uint8_t member_count = 0;
    uint32_t const *node_ids = ff_demo_live_node_ids(&member_count);

    ff_demo_event_t evs[8];
    uint8_t k = ff_demofeed_tick(&s_demofeed, now_ms, evs, (uint8_t)(sizeof(evs) / sizeof(evs[0])));
    for (uint8_t i = 0; i < k; i++) {
        /* s_demo_pack is the parsed demo festpack (ff_demo_seed filled it via
         * cfg.pack) — a RALLY sources its place name + position from it. */
        ff_demo_apply_event(&ev, &evs[i], node_ids, member_count, s_demo_pack);
    }
}
#endif

/* S21 §3 — the shell's injected touch-calibration hook (ff_shell_cfg_t.
 * calibrate_touch), fired by FF_INTENT_CALIBRATE_TOUCH (the Settings
 * "CALIBRATE TOUCH" row). Runs the crosshair capture, applies the solved
 * transform to the LIVE touch path immediately (so the correction is felt
 * without a reflash), and hands it back so the shell persists it into
 * ff_settings (which now lands in NVS, §4). Returns true only on a valid
 * fit; a failed/aborted capture leaves the existing calibration untouched.
 *
 * DEVICE-RUNTIME (the deadlock the sim-only build can't exercise, fixed
 * here): ff_display_run_calibration BLOCKS the calling task in a vTaskDelay
 * loop waiting for the LVGL *task's* release-event cb to capture the five
 * crosshair taps. This hook fires SYNCHRONOUSLY from the Settings row's LVGL
 * click callback (ff_shell_intent_sink -> ff_shell_intent has no queue) —
 * i.e. from inside the esp_lvgl_port UI task — so running the flow here would
 * block that very task and starve the tap-capture -> permanent hang.
 * So it is DEFERRED, two-phase: the first call (from the UI task) only
 * REQUESTS the flow and returns false (the shell does nothing); the main
 * loop (a separate task) runs the crosshair flow off the UI task, then
 * re-emits FF_INTENT_CALIBRATE_TOUCH, and this second call returns the ready
 * result so the SHELL writes it into ff_settings and persists (-> NVS, §4) —
 * keeping the shell the single persistence owner so a later settings save
 * cannot clobber the cal. */
static volatile bool s_calib_requested = false;
static bool s_calib_ready = false;
static ff_touchcal_t s_calib_result;

static bool ff_calibrate_touch_cb(void *user, ff_touchcal_t *out_cal)
{
    (void)user;
    if (out_cal == NULL) {
        return false;
    }
    if (s_calib_ready) { /* second call: the main loop's re-emit — hand back the result */
        *out_cal = s_calib_result;
        s_calib_ready = false;
        return out_cal->valid;
    }
    s_calib_requested = true; /* first call, from the UI task — defer, don't block */
    return false;
}

/* ---------------------------------------------------------------------
 * S26 slice b — PWR button -> power menu -> soft power-off.
 *
 * `s_power_fsm` is core/ff_power_fsm.h's whole state (debounce, press
 * timing, the reboot BOOT-release guard) — this file only samples GPIO6/
 * GPIO0 (ff_power.h) and ticks the FSM every main-loop frame (below, next
 * to ff_shell_tick — "look at how ff_shell_tick is driven"); the FSM
 * makes every decision, so nothing here is an `if` about behavior
 * (CLAUDE.md's house rule).
 * ------------------------------------------------------------------- */
static ff_power_fsm_t s_power_fsm;

/* ---------------------------------------------------------------------
 * S26 slice e — BOOT-as-home-button (docs/specs/S26-device-lifecycle.md
 * "(e) Home button + launcher").
 *
 * `s_boot_button` is core/ff_button.h's whole state — the same debounce
 * shape `s_power_fsm` uses, reused rather than shared (see ff_button.h's
 * header comment for why this is a standalone module). This file only
 * samples GPIO0 (`ff_power_boot_pressed()`, already used above for the
 * reboot guard and for ff_idle's keep-awake feed) and ticks the
 * debouncer every main-loop frame, forwarding a debounced press as
 * FF_INTENT_HOME — `ff_route_home` (app/include/ff_route.h) makes the
 * entire nav decision, so nothing here is an `if` about behavior
 * (CLAUDE.md's house rule).
 * ------------------------------------------------------------------- */
static ff_button_t s_boot_button;

/* ---------------------------------------------------------------------
 * S26 slice c — inactivity -> dim -> screen off.
 *
 * `s_idle` is core/ff_idle.h's whole state; this file only feeds it
 * input (touch, PWR SHORT_PRESS, BOOT — the render loop, next to
 * `s_power_fsm`'s tick above) and enacts its ACTIVE/DIM/OFF decision
 * (backlight percent + skipping the rebuild while OFF, both below in
 * the render loop). The keep-awake predicate combining flare
 * takeover/power-menu/calibration is `ff_shell_keep_awake`
 * (app/include/ff_shell.h) — this file never itself branches on any of
 * those three facts (CLAUDE.md's house rule).
 *
 * touch_cal_running (the third keep-awake source): this file always
 * passes `false` for it. `ff_display_run_calibration` BLOCKS this very
 * task (see `ff_calibrate_touch_cb`'s doc comment above) for the whole
 * crosshair capture, so NO `ff_idle_tick` call from the render loop
 * below can ever run WHILE a calibration is in progress — the parameter
 * would be observably true on zero ticks either way. The practical
 * equivalent instead: `ff_idle_input` is called once the blocking flow
 * returns (both the boot-time STAGE 4 path and the runtime deferred
 * path below), so a long capture session never leaves a stale idle
 * reference that slams straight to DIM/OFF the instant it exits.
 * Flagged here as this slice's interpretation call (AGENTS.md). */
static ff_idle_t s_idle;

/* ---------------------------------------------------------------------
 * S26 slice f — timer-based light sleep after screen-off (BEGIN).
 *
 * Enact-only, same split as slice c above: `ff_idle_tick` (core) is the
 * ONLY thing that decides FF_IDLE_STATE_SLEEP; this file only calls
 * `esp_light_sleep_start()` when it reports that state (in the render
 * loop below, right next to the backlight enact) and configures WHICH
 * hardware events are allowed to end that sleep.
 *
 * Wake sources, and why:
 *   - TIMER (`esp_sleep_enable_timer_wakeup`), FF_LIGHT_SLEEP_TIMER_WAKE_US
 *     below — keeps the mesh link serviced (a normal tick still runs on
 *     every wake, per the spec: "each timer wake services the link and
 *     returns to sleep unless input arrived") even with the puck sitting
 *     untouched in a pocket.
 *   - GPIO: PWR (GPIO6), BOOT (GPIO0), and the SPD2010 touch INT line
 *     (GPIO4) — `esp_sleep_enable_gpio_wakeup()` is light sleep's own
 *     wake mechanism (distinct from deep sleep's ext0/ext1), driven by
 *     `gpio_wakeup_enable()` per pin.
 *   - NOT UART: this spec slice is explicit that a UART wake is wrong for
 *     this board — the XIAO comms brain runs STOCK Meshtastic with no
 *     wake preamble, so RX bytes that arrive mid-sleep and trigger a wake
 *     are simply lost off the front of whatever frame was in flight
 *     (corrupts the next packet, not just delays it). ESP-IDF light sleep
 *     has no UART wake source configured here at all — nothing to
 *     disable, just nothing enabled.
 *
 * ## Touch-INT (GPIO4) wake is BEST-EFFORT, not the guaranteed path
 *
 * Honest-data note (do not oversell this line): ff_display.c's own S15b
 * comment (`ff_display_touch_start`, the `int_gpio_num` config) records
 * that on THIS board, wiring the SPD2010's INT line as an interrupt
 * source starved the reader — no press ever reached LVGL — which is WHY
 * touch is polled instead of interrupt-driven at runtime. That is direct
 * prior evidence this specific board's INT line may never assert on a
 * touch at all, not merely "unverified" — arming it as a light-sleep
 * wake source here is a free, best-effort addition (`gpio_wakeup_enable`
 * costs nothing to configure) that may simply never fire in practice.
 * `FF_PIN_TOUCH_INT`'s active-level (open-drain, active-LOW, the common
 * touch-controller convention, matching the runtime polarity assumption
 * elsewhere on this board) is unverified either way, same posture as
 * ff_power.c's `FF_POWER_PWR_ACTIVE_LOW` flag; an internal pull-up is
 * enabled here so the line reads a clean idle HIGH regardless of the
 * board's own pull-up. None of this matters for correctness: the 1500 ms
 * TIMER wake above is the guaranteed path regardless of whether INT ever
 * asserts, so a touch arriving while asleep costs AT MOST one timer
 * period (1.5 s) of latency before the next scheduled wake notices it —
 * never a missed wake. S26f AC1 (on-glass) will show which: if the log's
 * wake-cause line ever reads GPIO on a touch, this line works on this
 * unit after all; if every wake during a touch test reads TIMER, it
 * doesn't, and the timer alone is still carrying the AC. */
#define FF_PIN_TOUCH_INT GPIO_NUM_4 /* matches ff_display.c's FF_PIN_TP_INT (4) — SPD2010 touch INT, normally unused/polled */

/* Timer wake period. Spec range is 1-2 s; 1500 ms is the midpoint —
 * frequent enough that a held mesh link still looks "serviced" on a
 * human timescale (a message arriving while asleep is noticed within
 * 1.5 s of the next wake, well under the 6 s banner-relevant timescales
 * elsewhere in this spec) without waking so often that light sleep stops
 * being worth entering in the first place (30-40 wakes/min at the 1 s end
 * of the range vs 40 at 1.5 s vs ~30 at 2 s — the difference across the
 * whole range is small relative to DIM/OFF's already-15s/30s cadence, so
 * this is a soft pick, not a load-bearing one). */
#define FF_LIGHT_SLEEP_TIMER_WAKE_US (1500 * 1000)

/* Human-readable wake cause, for the on-glass log line the spec's AC1
 * asks for ("log sleep entry + wake cause ... so the maintainer can read
 * it on glass"). Deliberately NOT a full switch over every
 * esp_sleep_wakeup_cause_t value — only the causes this file's own wake
 * config above can actually produce (TIMER, GPIO) get a name; anything
 * else (a cause this file never enabled) is reported literally as
 * "OTHER(<n>)" rather than guessed at. */
static const char *ff_wakeup_cause_str(esp_sleep_wakeup_cause_t cause, char *scratch, size_t scratch_len)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
        return "TIMER";
    case ESP_SLEEP_WAKEUP_GPIO:
        return "GPIO";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        return "UNDEFINED (no prior sleep — cold boot or reset)";
    default:
        snprintf(scratch, scratch_len, "OTHER(%d)", (int)cause);
        return scratch;
    }
}

/* Arm every S26f wake source once, before the render loop's first
 * possible sleep. Idempotent to call more than once (gpio_config /
 * gpio_wakeup_enable / esp_sleep_enable_* re-apply cleanly), but this
 * file only calls it the one time, right before `while (true)` starts. */
static void ff_configure_light_sleep_wake(void)
{
    /* Same "log and continue, never abort a boot over a single non-fatal
     * HAL call" posture this file already uses throughout (ff_power_latch_on,
     * ff_bringup_panel's callers, ...) — each wake source below is
     * independent, so a failure on one (say, the touch-INT line) should
     * not cost the other two (PWR/BOOT still work) or the timer wake
     * (which alone already guarantees the link keeps getting serviced). */
    esp_err_t err = esp_sleep_enable_timer_wakeup(FF_LIGHT_SLEEP_TIMER_WAKE_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_enable_timer_wakeup failed: %s — light sleep would never wake on its own",
                 esp_err_to_name(err));
    }

    /* PWR (GPIO6) and BOOT (GPIO0): configured here directly (input,
     * matching ff_power.c's own pin config for each — no pull on PWR, an
     * internal pull-up on BOOT) rather than relying on
     * ff_power_pwr_pressed()/ff_power_boot_pressed() having already run
     * at least once — gpio_config re-apply is idempotent (same
     * "configuring an already-configured pin is a no-op re-apply, not an
     * error" precedent ff_power.c's own comments already establish), so
     * this function can run at any point in bring-up without an ordering
     * dependency on those two. Both active-LOW — FF_POWER_PWR_ACTIVE_LOW
     * / the standard BOOT-button convention, see ff_power.h. */
    const gpio_config_t pwr_io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_6,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if ((err = gpio_config(&pwr_io)) != ESP_OK || (err = gpio_wakeup_enable(GPIO_NUM_6, GPIO_INTR_LOW_LEVEL)) != ESP_OK) {
        ESP_LOGE(TAG, "PWR (GPIO%d) light-sleep wake config failed: %s", GPIO_NUM_6, esp_err_to_name(err));
    }

    const gpio_config_t boot_io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if ((err = gpio_config(&boot_io)) != ESP_OK || (err = gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL)) != ESP_OK) {
        ESP_LOGE(TAG, "BOOT (GPIO%d) light-sleep wake config failed: %s", GPIO_NUM_0, esp_err_to_name(err));
    }

    /* Touch INT (GPIO4) — see this block's top comment for the
     * active-LOW interpretation call. Not otherwise configured anywhere
     * in this codebase (ff_display.c polls the controller instead). */
    const gpio_config_t touch_int_io = {
        .pin_bit_mask = 1ULL << FF_PIN_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if ((err = gpio_config(&touch_int_io)) != ESP_OK ||
        (err = gpio_wakeup_enable(FF_PIN_TOUCH_INT, GPIO_INTR_LOW_LEVEL)) != ESP_OK) {
        ESP_LOGE(TAG, "touch-INT (GPIO%d) light-sleep wake config failed: %s — timer wake still covers it",
                 (int)FF_PIN_TOUCH_INT, esp_err_to_name(err));
    }

    err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_enable_gpio_wakeup failed: %s — GPIO wakes may not fire; timer wake still covers it",
                 esp_err_to_name(err));
    }

    /* Interpretation call #1 (PR body): force the VDD_SDIO power domain
     * (flash + this board's octal PSRAM, CONFIG_SPIRAM_MODE_OCT — the
     * shell + festpack scratch live there, see ff_shell_init/s_shell_p
     * above) to stay powered across every light sleep, rather than
     * trusting esp-idf's own AUTO heuristic. Reading esp-idf's own
     * source (esp_hw_support/sleep_modes.c, can_power_down_vddsdio):
     * AUTO would only power VDD_SDIO down if the timer wake were the
     * SOLE wake trigger (`wakeup_triggers == RTC_TIMER_TRIG_EN`) — ours
     * also has GPIO wake armed above, so AUTO would already keep it on
     * in this config. This ON is a deliberate, explicit pin of that
     * fact rather than a silent dependency on triggers-config staying
     * exactly this shape — a future edit that (say) dropped the GPIO
     * wakes would otherwise flip PSRAM's sleep behaviour with no
     * warning. */
    err = esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_ON);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_pd_config(VDDSDIO, ON) failed: %s — PSRAM/flash power state across sleep is un-pinned",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "S26f light-sleep wake sources armed: timer=%dms, GPIO wake on PWR(6)/BOOT(0)/touch-INT(%d), VDD_SDIO forced ON",
             (int)(FF_LIGHT_SLEEP_TIMER_WAKE_US / 1000), (int)FF_PIN_TOUCH_INT);
}

/* ---------------------------------------------------------------------
 * S26 slice f AMENDMENT (2026-09-02, maintainer decision) — don't enter
 * light sleep while USB is connected.
 *
 * The ESP32-S3's native USB-Serial/JTAG peripheral powers down during
 * light sleep, so the puck vanishes from the host the instant the screen
 * sleeps — breaking every dev/flash session tethered over USB. A
 * USB-powered puck is also not battery-limited, so there is no cost to
 * simply never sleeping while connected. Per this repo's house rule
 * (CLAUDE.md: "all logic goes in firmware/core/"), the DECISION —
 * whether that means SLEEP is actually withheld — is core's
 * (`ff_idle_tick`'s new `sleep_inhibit` parameter, ff_idle.h). This file
 * only SAMPLES the fact and passes the bool through: no `if` about idle
 * behavior here.
 *
 * `usb_serial_jtag_is_connected()` (driver/usb_serial_jtag.h) is backed
 * by a SOF-packet connection monitor that starts itself automatically at
 * system init (`ESP_SYSTEM_INIT_FN`, esp_driver_usb_serial_jtag's
 * usb_serial_jtag_connection_monitor.c) — it needs no
 * `usb_serial_jtag_driver_install()` call and no new Kconfig: this
 * project's sdkconfig already sets `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED=y`
 * (USB-Serial/JTAG is the secondary console), which is exactly the
 * condition esp_driver_usb_serial_jtag/CMakeLists.txt uses to force-link
 * the connection monitor object (`-u usb_serial_jtag_connection_monitor_include`)
 * — confirmed by reading that CMakeLists.txt and the sdkconfig checked
 * into this repo, not assumed. main/CMakeLists.txt adds the
 * `esp_driver_usb_serial_jtag` component to PRIV_REQUIRES so this file
 * can include its header; no sdkconfig edit was needed or made.
 * "Connected" per that function's own doc comment means receiving SOF
 * packets from a host (a live serial/JTAG link), not merely VBUS power —
 * a USB power bank with no host on the other end reads as NOT connected,
 * which is the right behaviour here (nothing to keep a port alive for).
 *
 * `s_usb_connected_logged` tracks the last value LOGGED (not the value
 * fed to ff_idle_tick, which is resampled fresh every frame) purely so
 * the render loop below logs the transition ONCE per change rather than
 * every 20 ms frame while connected. */
static bool s_usb_connected_logged = false;
/* S26 slice f amendment (END) --------------------------------------- */

/* ff_shell_cfg_t.power_off: called by the shell's FF_INTENT_POWER_OFF
 * handler. Composes the two device-IO calls the spec's slice (b) section
 * names — GPIO7 low (ff_power.h; a pure two-pin HAL with no display
 * dependency) and the backlight to 0 (ff_display.h) — deliberately HERE,
 * in app_main, not inside ff_power. */
static void ff_power_off_cb(void *user)
{
    (void)user;
    (void)ff_power_off();
    (void)ff_display_set_brightness(0);
}

/* ff_shell_cfg_t.power_reboot: called by the shell's FF_INTENT_POWER_REBOOT
 * handler. Only ARMS the FSM's reboot-BOOT-release guard and returns —
 * esp_restart() is NOT called here. The main loop's tick (below) is the
 * only place that calls esp_restart(), once ff_power_fsm_reboot_ready()
 * reports the guard satisfied (BOOT released), so a reboot requested
 * while a thumb happens to be resting on BOOT (S26's home button) never
 * risks the ROM bootloader (S26 AC4). */
static void ff_power_reboot_cb(void *user)
{
    (void)user;
    ff_power_fsm_request_reboot(&s_power_fsm);
}

/* Bring the panel up through the QSPI init (shared by every stage). Returns
 * true on success; logs and returns false so app_main can park rather than
 * spin a half-initialised peripheral. */
static bool ff_bringup_panel(void)
{
    if (ff_display_expander_init() != ESP_OK) {
        ESP_LOGE(TAG, "expander init failed — LCD/TP resets not released; screen will stay dark");
        return false;
    }
    if (ff_display_panel_init() != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed");
        return false;
    }
    return true;
}

/* Park forever (still logging a heartbeat) after a fatal bring-up error, so
 * the serial log stays readable instead of scrolling a reset loop. */
static void ff_park(const char *why)
{
    ESP_LOGE(TAG, "parked: %s", why);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    /* S25 — latch battery power ON before anything else. On battery the board
     * only stays alive while PWR is physically held; this drives the SYS_EN
     * hold line high so it survives the button release. Must beat the display
     * bring-up's reset delays, hence the very first line. Non-fatal on failure
     * (USB boots run regardless; battery was going to drop either way, and a
     * live-but-unlatched puck is more debuggable than a park loop).
     *
     * S26g AC1: this call stays app_main's first statement — nothing above
     * it, nothing inserted before it by this slice. The timestamp logged
     * right after is read together with ff_display's own "S26g AC1: first
     * splash pixel" log (targets/esp32s3/components/ff_display/ff_display.c)
     * so the maintainer can confirm the order (latch, then splash pixel,
     * never the reverse) straight off the serial log. */
    (void)ff_power_latch_on();
    ESP_LOGI(TAG, "S26g AC1: latch requested at t=%lld us (app_main's first statement)",
             (long long)esp_timer_get_time());

    /* S26 slice b — zero the PWR-button FSM before anything can tick it
     * (the main loop below is the only tick site). */
    ff_power_fsm_init(&s_power_fsm);

    /* S26 slice e — zero the BOOT-button debouncer, same "before anything
     * can tick it" placement. */
    ff_button_init(&s_boot_button);

    /* S26 slice c — zero the idle FSM; re-pinned to "now" right before
     * the render loop starts (below), after every one-shot bring-up step
     * (including a possibly long, human-paced STAGE 4 calibration) has
     * had its say, so the very first DIM/OFF countdown starts at the
     * actual beginning of interactive use, not at cold boot. */
    ff_idle_init(&s_idle);

    s_shell_p = heap_caps_calloc(1, sizeof(ff_shell_t), MALLOC_CAP_SPIRAM);
    if (s_shell_p == NULL) { ESP_LOGE(TAG, "shell PSRAM alloc failed"); return; }
    const int stage = CONFIG_FF_BRINGUP_STAGE;
    ESP_LOGI(TAG, "firefly esp32s3 target booting (S15 slice b: display+touch, STAGE %d)", stage);

#if CONFIG_FF_DEMO_MODE
    s_clock.now_ms = ff_demo_clock_now_ms; /* frozen demo clock (Sat 21:30) */
#else
    s_clock.now_ms = ff_esp_clock_now_ms;
#endif
    s_clock.user = NULL;

    /* S21 §4 — real NVS store, replacing the no-op stub. ff_shell_init loads
     * settings from it below (persisted values on a returning puck, exact
     * defaults on a fresh/empty partition), and saves on every change; on an
     * NVS failure the store degrades to no-op and the puck runs on defaults
     * (ff_nvs_store_init logs which). */
    s_store = ff_nvs_store_init(&s_nvs);

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &s_clock;
    cfg.store = &s_store;
    /* S21 §3 — the touch-calibration hook the Settings CALIBRATE TOUCH row
     * drives through FF_INTENT_CALIBRATE_TOUCH. */
    cfg.calibrate_touch = ff_calibrate_touch_cb;
    cfg.calibrate_touch_user = NULL;
    /* S26 slice b — the power-menu action hooks (FF_INTENT_POWER_OFF /
     * FF_INTENT_POWER_REBOOT). See ff_power_off_cb/ff_power_reboot_cb
     * above. */
    cfg.power_off = ff_power_off_cb;
    cfg.power_off_user = NULL;
    cfg.power_reboot = ff_power_reboot_cb;
    cfg.power_reboot_user = NULL;
#if CONFIG_FF_DEMO_MODE
    s_demo_pack = heap_caps_malloc(sizeof(*s_demo_pack), MALLOC_CAP_SPIRAM);
    if (s_demo_pack == NULL) {
        ff_park("demo festpack PSRAM alloc failed");
        return;
    }
    cfg.pack = s_demo_pack; /* ff_demo_seed parses the embedded festpack into this */

    /* S26 slice (a) — jsmn scratch for ff_shell_load_pack, allocated once
     * and kept alive for the process lifetime (never freed) — see
     * s_demo_toks's declaration comment for why. PSRAM, never internal
     * DIRAM. */
    s_demo_toks = heap_caps_malloc((size_t)FP_MAX_TOKENS * sizeof(jsmntok_t), MALLOC_CAP_SPIRAM);
    if (s_demo_toks == NULL) {
        ff_park("demo festpack token-scratch PSRAM alloc failed");
        return;
    }
    cfg.toks = s_demo_toks;
    cfg.ntoks = FP_MAX_TOKENS;
#endif
    /* cfg.transport / cfg.haptic left zeroed — see slice a. (cfg.pack set above
     * only under CONFIG_FF_DEMO_MODE.) */

    int rc = ff_shell_init(&s_shell, &cfg);
    if (rc != 0) {
        ff_park("ff_shell_init failed");
        return;
    }

#if CONFIG_FF_DEMO_MODE
    {
        size_t const pack_len = (size_t)(firefly_pack_end - firefly_pack_start);
        int const drc = ff_demo_seed(&s_shell, (char const *)firefly_pack_start, pack_len, &s_demo_clock_ms, 0);
        /* S26 slice (a) — s_demo_toks is deliberately NOT freed here: the
         * shell stores cfg.toks (ff_shell_cfg_t.toks) and ff_shell_load_pack
         * is the shell's real production pack-load path, not a one-shot
         * boot helper — a future on-device reload must find the pointer
         * still valid. See s_demo_toks's declaration comment above. */
        if (drc != 0) {
            ESP_LOGE(TAG, "S20 demo seed failed (%d) — festpack parse or wall latch", drc);
        } else {
            ESP_LOGI(TAG, "S20 DEMO MODE: seeded Firefly Fields (Sat 21:30) — %u byte festpack, no mesh",
                     (unsigned)pack_len);
#if CONFIG_FF_DEMO_LIVE
            /* S23: seeding done against the frozen epoch — now let the demo
             * clock advance, and arm the generator (fixed seed + demo epoch +
             * demo crew size => a reproducible synthetic event stream). */
            uint8_t member_count = 0;
            (void)ff_demo_live_node_ids(&member_count);
            ff_demofeed_init(&s_demofeed, FF_DEMO_LIVE_SEED, s_demo_clock_ms, member_count);
            ff_demo_live_start();
            ESP_LOGI(TAG, "S23 LIVE DEMO: clock advancing from epoch, ff_demofeed armed (seed=0x%08x, crew=%u)",
                     (unsigned)FF_DEMO_LIVE_SEED, (unsigned)member_count);
#endif
        }
        /* S24d — install the demo LOOPBACK sender: the demo build has no
         * mesh transport, so the real mc_send_* path refuses every send and
         * the thread's whole send half is mute. The loopback accepts each
         * send (returns 0) so the shell pushes the OUT feed item and the
         * user's own flare/rally/message shows in the thread. This echoes
         * the user's REAL outbound as an OUT item in a clearly-demo build;
         * it fabricates no incoming content and reaches no radio.
         *
         * Defined right here, inside the CONFIG_FF_DEMO_MODE block, NOT in
         * ff_demo.c: ff_demo.c is linked into the device image regardless of
         * the config and ESP-IDF does not dead-strip it (verified with nm),
         * so a loopback symbol in ff_demo.c would survive into a FIELD
         * build. #if-gated here it is simply not compiled when demo mode is
         * off — a field image carries no loopback symbol and keeps the real
         * mc_send path. */
        ff_shell_set_sender(&s_shell, ff_demo_loopback_sender());
    }
#endif

    (void)ff_shell_tick(&s_shell, ff_bringup_now_ms());
    ff_app_state_t const *view = ff_shell_view(&s_shell);
    ESP_LOGI(TAG, "shell up: active_face=%d radar_mode=%d link=%d", (int)view->active_face,
             (int)view->radar.mode, (int)ff_shell_link(&s_shell));

    /* ---- Display bring-up (all stages) ---- */
    if (!ff_bringup_panel()) {
        ff_park("panel bring-up failed");
        return;
    }

    /* format v8 amendment (maintainer ask, 2026-09-02) — apply the SCREEN
     * NORMAL|FLIPPED setting's hardware panel mirror right after the panel
     * is up and BEFORE the boot splash below, so the splash itself (the
     * FIRST content on glass) already renders upright in whichever
     * orientation the case is mounted — a flip applied only after the
     * splash would show it upside-down for one frame on every boot of a
     * flipped puck. `view` (fetched above from the shell, which already
     * loaded settings from NVS via ff_shell_init — see this file's own
     * boot-order note near ff_shell_init) carries the persisted value;
     * a failure here is logged and non-fatal (same "cosmetic loss, not a
     * park reason" policy the boot splash below already uses) — a puck
     * that boots un-mirrored when it should have flipped is a wrong-side-
     * up UI, not a dark/broken one, and CONFIG_FF_GLASS_RULER below
     * assumes the panel's NORMAL orientation for its own diagnostic
     * reading, so this intentionally runs before that block too. */
    esp_err_t const flip_err = ff_display_set_flip(view->settings.screen_flip);
    if (flip_err != ESP_OK) {
        ESP_LOGE(TAG, "boot screen-flip apply failed (%s) — continuing bring-up regardless",
                 esp_err_to_name(flip_err));
    }

#if CONFIG_FF_GLASS_RULER
    /* Device-only diagnostic (default OFF, docs/hardware/glass-offset.md):
     * drawn right after the panel comes up — same spot the boot splash
     * claims below, "after the latch, like the splash" — then HOLDS
     * forever instead of continuing bring-up, so the maintainer can read
     * it against the physical bezel with no time pressure. This entire
     * block (and ff_display_draw_glass_ruler itself) compiles out of a
     * normal build. */
    esp_err_t const ruler_err = ff_display_draw_glass_ruler();
    if (ruler_err != ESP_OK) {
        ff_park("glass ruler draw failed");
        return;
    }
    ESP_LOGI(TAG, "CONFIG_FF_GLASS_RULER: glass ruler on glass, holding forever "
                  "(never continuing to the splash/LVGL/normal boot) — see "
                  "docs/hardware/glass-offset.md to read dx/dy off it");
    while (true) {
        ESP_LOGI(TAG, "glass ruler: count the ticks hidden under the bezel on each side");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    /* S26 slice (g) — the boot splash: drawn HERE, immediately after the
     * panel + backlight are up and BEFORE ff_display_lvgl_start(), so it
     * is the FIRST content on glass — earlier than LVGL init could ever
     * put pixels up (docs/specs/S26-device-lifecycle.md "(g) Boot
     * animation"). Same raw esp_lcd_panel_draw_bitmap path STAGE 1's test
     * pattern uses just below, chosen over an LVGL-screen splash
     * specifically to beat LVGL's own init latency rather than merely
     * occupy it. Runs for every stage/build (field and demo identical,
     * no CONFIG_FF_DEMO_MODE gate) — STAGE 1 below is a manual bring-up
     * debug aid, not the normal boot path, and simply overdraws the
     * splash with its own pattern when selected via menuconfig. A
     * failure here is logged and non-fatal (a missing splash is a
     * cosmetic loss, not a reason to park a boot the field test depends
     * on) — the panel is already known-good at this point (ff_bringup_panel
     * just succeeded), so bring-up continues either way. */
    esp_err_t const splash_err = ff_display_draw_boot_splash();
    if (splash_err != ESP_OK) {
        ESP_LOGE(TAG, "boot splash failed (%s) — continuing bring-up regardless", esp_err_to_name(splash_err));
    }

    /* ---- STAGE 1: first light, no LVGL ---- */
    if (stage <= 1) {
        if (ff_display_draw_test_pattern() != ESP_OK) {
            ff_park("test pattern draw failed");
            return;
        }
        ESP_LOGI(TAG, "STAGE 1 complete: first-light pattern on glass. Keeping shell alive.");
        /* Keep ticking the shell (proves it survives under FreeRTOS) while
         * the static test pattern stays on the panel. No LVGL, no redraw. */
        while (true) {
            (void)ff_shell_tick(&s_shell, ff_bringup_now_ms());
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* ---- STAGE 2+: LVGL v9 up, render a real face ---- */
    lv_display_t *disp = ff_display_lvgl_start();
    if (disp == NULL) {
        ff_park("LVGL display bring-up failed");
        return;
    }

    /* ---- STAGE 3+: touch + bind the input seam ---- */
    if (stage >= 3) {
        if (ff_display_touch_start(disp) != ESP_OK) {
            ff_park("touch bring-up failed");
            return;
        }
        /* Bind the SAME seam the sim binds (targets/sim/ctl_loop.c): every
         * screen emit now reaches this shell. Unbind is unnecessary — the
         * shell lives for the process lifetime. */
        ff_intent_emit_bind(ff_shell_intent_sink, &s_shell);
        ESP_LOGI(TAG, "input seam bound: touch -> ff_shell_intent");

        /* ---- Touch calibration (S15 slice d) ----
         * STAGE 4 (CAL): run the crosshair capture -> solve -> store ->
         * apply LIVE, then fall through into the normal UI in the SAME boot
         * (no reflash to feel it). STAGE 3: load whatever cal ff_settings
         * holds and apply it. Either way the transform is installed in the
         * one touch seam (ff_display process_coordinates) so gestures,
         * long-press and buttons are all corrected. */
        ff_settings_t settings;
        ff_settings_load(&settings, &s_store); /* NVS (S21 §4): persisted cal, else the identity (uncalibrated) default */

        ff_touchcal_t cal;
        if (stage >= 4) {
            if (ff_display_run_calibration(&cal) != ESP_OK) {
                ff_park("touch calibration flow failed");
                return;
            }
            /* Store into settings so persistence rides the existing seam.
             * S21 §4: s_store is now the real NVS store, so this save PERSISTS
             * across reboot (no longer the no-op stub S15d shipped with). The
             * params are still logged by ff_display_run_calibration. Note the
             * in-app CALIBRATE TOUCH row (S21 §3) is now the primary way to
             * (re)calibrate without a reflash; this boot-time STAGE 4 path
             * remains for bring-up. */
            settings.touch_calibrated = cal.valid;
            settings.touch_ax = cal.ax;
            settings.touch_bx = cal.bx;
            settings.touch_ay = cal.ay;
            settings.touch_by = cal.by;
            ff_settings_save(&settings, &s_store);
            ESP_LOGI(TAG, "touch cal saved to ff_settings via NVS — persists across reboot (S21 §4)");
        } else {
            /* STAGE 3: reconstruct the transform from stored settings. */
            cal.ax = settings.touch_ax;
            cal.bx = settings.touch_bx;
            cal.ay = settings.touch_ay;
            cal.by = settings.touch_by;
            cal.valid = settings.touch_calibrated;
            ESP_LOGI(TAG, "touch cal loaded from ff_settings: calibrated=%d",
                     (int)settings.touch_calibrated);
        }
        ff_display_touch_set_cal(&cal);
    }

    /* First face build (under the LVGL lock — the port owns the LVGL task). */
    if (ff_display_lock(0)) {
        ff_face_build(ff_shell_view(&s_shell));
        ff_display_unlock();
    }
    ESP_LOGI(TAG, "STAGE %d complete: first face flushed to glass", stage);

    /* Apply the persisted display brightness on boot (#100). The setting
     * lives in ff_settings (core, projected into the view); the app forwards
     * it to the LEDC backlight HAL — core never touches IO. Track the last
     * applied value so the render loop below only re-programs the PWM when it
     * actually changes. Note (#bug1): brightness is deliberately EXCLUDED from
     * the shell's render key, so a change does NOT mark the view dirty — the
     * LEDC apply below runs every tick (not only on a dirty one) precisely so a
     * live brightness drag tracks the finger without rebuilding the face. */
    uint8_t last_brightness = ff_shell_view(&s_shell)->settings.brightness_pct;
    (void)ff_display_set_brightness(last_brightness);

    /* format v8 amendment — track the last-applied SCREEN flip the same
     * way `last_brightness` tracks brightness above, so the render loop
     * below re-applies the hardware mirror the moment the setting
     * changes (Settings SCREEN row, or CALIBRATE TOUCH's own re-apply
     * are the only writers), with no reboot. Already applied once at
     * boot (right after ff_bringup_panel, before the splash — see
     * above); this is the LIVE-change path. Unlike brightness, screen_
     * flip is NOT excluded from the shell's render key (a flip is a rare,
     * discrete toggle, never a live drag), so it also drives a normal
     * dirty-tick face rebuild (Settings row, Radar's glass-centred rim
     * tint) through the existing `rebuild_pending` mechanism below — this
     * variable ONLY gates the separate display-HAL mirror call, which
     * must not wait for `rebuild_pending`'s finger-down/screen-blank
     * gating (a stale hardware orientation while the device screen is
     * briefly OFF/asleep would be a bug users could see as a
     * disorienting flash on wake, not a cosmetic no-op). */
    bool last_screen_flip = ff_shell_view(&s_shell)->settings.screen_flip;

    /* Defer-rebuild-mid-tap latch (the on-glass "just highlights, won't open"
     * report). A dirty tick that lands while a finger is down is NOT rebuilt
     * this frame — a rebuild (lv_obj_clean + ff_face_build) between a tap's
     * press and release destroys the button under the finger, so its
     * LV_EVENT_CLICKED never fires. Instead the dirty state is REMEMBERED here
     * and the rebuild runs on the first frame after the finger lifts. This
     * cannot deadlock: a finger held down over churning content just delays
     * the visual refresh until release (correct — nothing on glass should
     * change under a stationary finger anyway), and a real device touch is
     * transient, so the pending rebuild always drains. */
    bool rebuild_pending = false;

    /* S26 slice c — pin the idle reference to "now", the actual start of
     * interactive use: everything above (panel bring-up, LVGL start,
     * touch start, and — worst case — a human-paced STAGE 4 crosshair
     * capture) can take an unbounded amount of real time, and `s_idle`
     * was zero-initialised (ref_ms == 0) long before any of it ran.
     * Pinning here means the first DIM/OFF countdown starts now, not at
     * cold boot. `last_idle_state` tracks the previous frame's state so
     * the render loop below only re-programs the backlight on an actual
     * ACTIVE/DIM/OFF transition (or a live brightness change while
     * ACTIVE), not every single frame. */
    ff_idle_input(&s_idle, ff_bringup_now_ms());
    ff_idle_state_t last_idle_state = FF_IDLE_STATE_ACTIVE;

    /* S26 slice f — arm the light-sleep wake sources once, right before
     * the render loop can first reach SLEEP. See
     * ff_configure_light_sleep_wake's own doc comment above for the wake
     * sources and the PSRAM/VDD_SDIO interpretation call. */
    ff_configure_light_sleep_wake();

    /* Render lifecycle mirrors the sim (targets/sim/ctl_loop.c): tick the
     * shell every frame, rebuild the LVGL tree ONLY on a dirty tick. The
     * esp_lvgl_port task does the actual flushing; we just own the model. */
    while (true) {
        /* S21 §3 (device-runtime): drain a deferred CALIBRATE-TOUCH request in
         * THIS (main) task. ff_display_run_calibration blocks waiting for the
         * LVGL task to capture the taps, so it must not run from the click
         * callback (see ff_calibrate_touch_cb). Re-emitting the intent from
         * here lets the shell persist the result via NVS. */
        if (s_calib_requested) {
            s_calib_requested = false;
            ff_touchcal_t cal;
            if (ff_display_run_calibration(&cal) == ESP_OK) {
                ff_display_touch_set_cal(&cal); /* apply live */
                s_calib_result = cal;
                s_calib_ready = true;
                ff_intent_t calin = {.kind = FF_INTENT_CALIBRATE_TOUCH, .u = {0}};
                ff_shell_intent(&s_shell, &calin); /* shell writes cal -> ff_settings -> NVS */
            } else {
                ESP_LOGE(TAG, "in-app calibration failed — keeping existing cal");
            }
            /* run_calibration cleaned the screen — rebuild the real face. */
            if (ff_display_lock(100)) {
                lv_obj_clean(lv_screen_active());
                ff_face_build(ff_shell_view(&s_shell));
                ff_display_unlock();
            }
            /* S26 slice c — the blocking capture above (five taps' worth
             * of real, human-paced time) ran with no ff_idle_tick call
             * anywhere near it (see s_idle's doc comment for why
             * touch_cal_running is passed false below); treat its
             * completion as a wake so a long session does not slam
             * straight to DIM/OFF the instant this returns. */
            ff_idle_input(&s_idle, ff_bringup_now_ms());
        }

        /* ff_bringup_now_ms() is the demo clock under CONFIG_FF_DEMO_MODE —
         * frozen at the seeded epoch (S20 static) or advancing from it (S23
         * live) — so the seeded projection ages against demo time; real
         * esp_timer time otherwise. */
        uint32_t const now_ms = ff_bringup_now_ms();

#if CONFIG_FF_DEMO_MODE && CONFIG_FF_DEMO_LIVE
        /* S23 (slice c): drain the synthetic event source and apply due
         * events through the real inbound seam BEFORE the tick, so a new
         * signal is already in the feed when this frame's view is rebuilt. */
        ff_demo_live_pump(now_ms);
#endif

        /* S26 slice b — tick the PWR-button FSM every frame, same spot as
         * the demo pump above: BEFORE ff_shell_tick, so a LONG_PRESS this
         * frame already opens the power menu in the view ff_shell_tick is
         * about to (re)project, rather than lagging one frame behind. The
         * FSM makes the decision (short/long/release, debounced); this file
         * only samples GPIO6 and forwards the decision as an intent — no
         * `if` about behavior here (CLAUDE.md's house rule). */
        ff_power_fsm_event_t const pwr_ev = ff_power_fsm_tick(&s_power_fsm, now_ms, ff_power_pwr_pressed());
        if (pwr_ev == FF_POWER_FSM_EVENT_LONG_PRESS) {
            ff_intent_t const open_power_menu = {.kind = FF_INTENT_POWER_MENU_OPEN, .u = {0}};
            ff_shell_intent(&s_shell, &open_power_menu);
        }
        /* The reboot BOOT-release guard (S26 AC4): polled every frame
         * regardless of pwr_ev — a reboot armed by ff_power_reboot_cb (via
         * FF_INTENT_POWER_REBOOT, dispatched through the same ff_shell_intent
         * seam as above) becomes ready on the first frame BOOT reads
         * released, and esp_restart() is called from exactly ONE place: here. */
        if (ff_power_fsm_reboot_ready(&s_power_fsm, ff_power_boot_pressed())) {
            ESP_LOGI(TAG, "S26b reboot guard ready (BOOT released) — esp_restart()");
            esp_restart();
        }

        /* S26 slice e — the BOOT-as-home-button debounce, same "tick every
         * frame, forward the decision as an intent" placement as the PWR
         * FSM above: ff_button_tick debounces GPIO0 (already sampled above
         * for the reboot guard and the idle-input feed below — reading it
         * again here is a second, independent sample, exactly like
         * ff_power_pwr_pressed's own reuse pattern) and fires true exactly
         * once per physical press; this file only forwards that as
         * FF_INTENT_HOME. `ff_route_home` (app/include/ff_route.h) owns
         * the whole nav decision — no `if` about behavior here. */
        if (ff_button_tick(&s_boot_button, now_ms, ff_power_boot_pressed())) {
            ff_intent_t const home = {.kind = FF_INTENT_HOME, .u = {0}};
            ff_shell_intent(&s_shell, &home);
        }

        /* S26 slice c — feed touch + BOOT into ff_idle_input every frame,
         * same "always feed, let the FSM decide" placement as the PWR FSM
         * above. A held BOOT (or a held finger) reads true every frame
         * it stays down — harmless: ff_idle_input just keeps re-pinning
         * ACTIVE, exactly like a genuinely continuous touch should. */
        if (ff_display_touch_is_down() || ff_power_boot_pressed()) {
            ff_idle_input(&s_idle, now_ms);
        }

        /* PWR SHORT_PRESS — the WHOLE decision (keep_awake no-op /
         * ACTIVE->force-off / DIM,OFF->wake) is core's
         * ff_idle_short_press (S26 AC: "while OFF = wake; while ACTIVE =
         * go OFF immediately" — this is where slice (b)'s reserved
         * SHORT_PRESS lands; DIM extended to "wake" too and the
         * keep_awake no-op are both documented interpretation calls in
         * ff_idle.h's doc comment on that function, PR body). This file
         * only samples the FSM event and the keep-awake predicate and
         * hands both to core — no `if` about idle behavior here
         * (CLAUDE.md's house rule). Reads the view from the PREVIOUS
         * frame (ff_shell_tick for THIS frame has not run yet), same
         * one-frame lag the LONG_PRESS/reboot-guard handling above
         * already accepts. */
        if (pwr_ev == FF_POWER_FSM_EVENT_SHORT_PRESS) {
            ff_idle_short_press(&s_idle, now_ms, ff_shell_keep_awake(ff_shell_view(&s_shell), false));
        }

        bool const dirty = ff_shell_tick(&s_shell, now_ms);
        /* S26(c)+(d) — a pushed banner wakes a dim/off screen. Mechanical
         * forward of the shell's decision (ff_shell_take_wake); the idle
         * FSM then re-pins ACTIVE exactly as a touch would. */
        if (ff_shell_take_wake(&s_shell)) {
            ff_idle_input(&s_idle, now_ms);
        }
        ff_app_state_t const *v = ff_shell_view(&s_shell);

        /* format v8 amendment — apply the hardware panel mirror the
         * instant the SCREEN setting changes, independent of the dirty-
         * tick face rebuild below (which still happens too, via the
         * normal render key — see `last_screen_flip`'s own doc comment
         * above for why this needs its own unconditional check rather
         * than piggybacking on `rebuild_pending`). */
        if (v->settings.screen_flip != last_screen_flip) {
            last_screen_flip = v->settings.screen_flip;
            esp_err_t const flip_err = ff_display_set_flip(last_screen_flip);
            if (flip_err != ESP_OK) {
                ESP_LOGE(TAG, "live screen-flip apply failed (%s)", esp_err_to_name(flip_err));
            }
        }

        /* S26 slice f amendment — sample USB-Serial/JTAG connection state
         * fresh every frame (same "always sample, let core decide"
         * placement as every other ff_idle input on this loop) and log
         * ONLY the transition, not every frame — see
         * s_usb_connected_logged's doc comment above. `"%s"` (not the
         * ternary directly as the format argument): ESP_LOGx's LOG_FORMAT
         * macro adjacent-string-literal-pastes the format argument at
         * PREPROCESS time, which requires it to be an actual string
         * literal token, not a runtime expression — a ternary there
         * fails to compile under -Werror=format (confirmed: this is
         * exactly what idf.py build first caught). */
        bool const usb_connected = usb_serial_jtag_is_connected();
        if (usb_connected != s_usb_connected_logged) {
            ESP_LOGI(TAG, "%s", usb_connected ? "S26f: USB connected — light sleep inhibited"
                                               : "S26f: USB disconnected — light sleep armed");
            s_usb_connected_logged = usb_connected;
        }

        /* S26 slice c — the idle decision itself: ticked every frame
         * (same "always tick" contract as the PWR FSM), against THIS
         * frame's freshly-projected view. `usb_connected` is S26f's
         * amendment: sleep_inhibit, NOT keep_awake — see ff_idle.h's
         * "Sleep inhibit" section (DIM/OFF still happen; only the
         * OFF->SLEEP transition is withheld, and ref_ms is never
         * re-pinned by it). */
        bool const keep_awake = ff_shell_keep_awake(v, false);
        ff_idle_state_t const idle_state = ff_idle_tick(&s_idle, now_ms, keep_awake, usb_connected);

        /* Backlight enact — the VALUE is core's decision
         * (ff_idle_brightness_pct: stored_pct unchanged for ACTIVE —
         * AC2's "wake restores exactly the pre-dim brightness_pct" —
         * FF_BRIGHTNESS_MIN_PCT for DIM, a true 0 for OFF); this file
         * only decides WHEN to re-program the LEDC duty (an actual
         * idle-state transition, or — while ACTIVE — the projected
         * brightness percent itself moving, #100/#bug1's original
         * reasoning: brightness is deliberately EXCLUDED from the
         * shell's render key so a live Settings drag does not force a
         * face rebuild mid-drag, so that comparison must keep running
         * every ACTIVE frame, not only a dirty one) and which HAL call a
         * `0` result means (ff_display_backlight_off — a true zero,
         * bypassing ff_display_set_brightness's non-zero floor) versus
         * a nonzero one (ff_display_backlight_on, the alias for
         * ff_display_set_brightness) — never which idle STATE maps to
         * which percent; that mapping lives in core, unit-tested there
         * (S26c_AC2_brightness_* in core/tests/test_idle.c). */
        uint8_t const pct = ff_idle_brightness_pct(idle_state, v->settings.brightness_pct);
        bool const live_brightness_drag =
            (idle_state == FF_IDLE_STATE_ACTIVE) && (v->settings.brightness_pct != last_brightness);
        if (idle_state != last_idle_state || live_brightness_drag) {
            last_brightness = v->settings.brightness_pct;
            if (pct == 0u) {
                (void)ff_display_backlight_off();
            } else {
                (void)ff_display_backlight_on(pct);
            }
        }
        last_idle_state = idle_state;

        /* Accumulate the dirty bit into the pending latch — NEVER cleared by
         * skipping, only by an actual rebuild below — so a change that arrives
         * mid-tap is not lost when we defer it (fix: rebuild-mid-tap). S26
         * slice c: a dirty tick that lands while OFF is deferred exactly the
         * same way — it drains on the first non-OFF frame after a wake ("a
         * dirty view is rebuilt on wake"), not lost either. */
        if (dirty) {
            rebuild_pending = true;
        }

        /* Rebuild only when something is pending, no finger is down, AND the
         * screen is not OFF or SLEEP (S26 slice c: "skip rendering" — the
         * LVGL tick still runs via esp_lvgl_port's own task; only the face
         * teardown+rebuild is skipped here; S26 slice f folds SLEEP into
         * this SAME check, same as the sim's ctl_loop.c screen_blank gate —
         * the screen is just as blank while asleep as it is OFF). While a
         * finger is down we hold off so the button under it survives to
         * emit its CLICKED on release; the pending flag stays set and
         * drains on the first frame after the finger lifts (or after the
         * screen wakes, whichever is later). */
        bool const screen_blank = (idle_state == FF_IDLE_STATE_OFF) || (idle_state == FF_IDLE_STATE_SLEEP);
        if (rebuild_pending && !ff_display_touch_is_down() && !screen_blank) {
            if (ff_display_lock(100 /* ms */)) {
                lv_obj_clean(lv_screen_active());
                ff_face_build(v);
                ff_display_unlock();
                rebuild_pending = false;
            }
        }

        /* S26 slice f — the actual light-sleep enact. `idle_state` above
         * is core's decision (ff_idle_tick) — this file only reads it:
         * SLEEP means "sleep now"; anything else means the normal 20 ms
         * frame delay. Waking (whether the timer or a GPIO source fired)
         * re-enters the loop at the top, where the very next
         * `ff_power_fsm_tick` / touch-feed / `ff_idle_tick` calls are the
         * spec's "one normal tick" — if real input came with the wake
         * (PWR/BOOT/touch), that tick lands ACTIVE via the existing
         * ff_idle_input/ff_idle_short_press paths (no new code needed —
         * see ff_idle.h); if only the timer fired, `ff_idle_tick` reports
         * SLEEP again (S26f: "OFF and SLEEP are sticky") and this same
         * branch sends the device straight back to sleep — the DECISION
         * that a bare timer wake stays SLEEP already happened in core, not
         * here. */
        if (idle_state == FF_IDLE_STATE_SLEEP) {
            ESP_LOGI(TAG, "S26f: entering light sleep");
            esp_light_sleep_start();
            char cause_scratch[16];
            ESP_LOGI(TAG, "S26f: light sleep wake, cause=%s",
                     ff_wakeup_cause_str(esp_sleep_get_wakeup_cause(), cause_scratch, sizeof(cause_scratch)));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
