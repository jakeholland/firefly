/**
 * ff_power.c — S25 slices a+b+c / S26 slice b. See ff_power.h for the
 * hardware contract.
 *
 * ## S25 slice c — battery-sense ADC (the citation)
 * Pin, attenuation, divider ratio and the measured correction below are
 * all read off Waveshare's own reference driver for this exact board
 * (ESP32-S3-Touch-LCD-1.46), `BAT_Driver.c`:
 * https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test/blob/main/main/BAT_Driver/BAT_Driver.c
 * — battery sense is ADC1 channel 7 (GPIO8), `ADC_ATTEN_DB_12`,
 * `ADC_BITWIDTH_DEFAULT`, through a 1:3 resistive divider, and their own
 * code applies a small measured correction on top of the naive ×3
 * (dividing by 0.990476) — this is a single-cell LiPo board, no fuel-
 * gauge IC, so the divider math is the entire "sensor".
 */
#include "ff_power.h"

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h" /* fix/quick-flare-detection — IRAM_ATTR for the BOOT edge ISR */
#include "esp_log.h"
#include "esp_timer.h" /* fix/quick-flare-detection — esp_timer_get_time(), the ISR edge timestamp source */

#include "ff_power_batt_conv.h"

/* SYS_EN / PWR_Control — the battery keep-alive latch. Drive HIGH to hold the
 * rail on; LOW is a soft power-off. Direct ESP32-S3 GPIO, NOT a TCA9554
 * expander pin, so this needs no I2C bring-up and can run first. Pin from
 * the board's reference driver (PWR_Key.h: PWR_Control_PIN 7). */
#define FF_PIN_PWR_HOLD GPIO_NUM_7

/* PWR key — read for press/long-press detection (core/ff_power_fsm.h owns
 * the decision; this pin sample is all this file does). Pin from the same
 * reference driver (PWR_Key.h: PWR_Key_Input_PIN 6). */
#define FF_PIN_PWR_KEY GPIO_NUM_6

/* BOOT key — GPIO0, the ESP32-family strapping pin (held LOW at reset ->
 * ROM bootloader; a normal input once running). S26's nav model makes this
 * the home button AND the reboot BOOT-release guard's input. */
#define FF_PIN_BOOT_KEY GPIO_NUM_0

/* THE ACTIVE-LEVEL INTERPRETATION CALL — see ff_power_pwr_pressed's doc
 * comment in ff_power.h. 1 = active-LOW (the reference driver's own
 * boot-time held read, `!gpio_get_level`), matching Waveshare's PWR_Key.c.
 * Flip THIS ONE DEFINE if bring-up on glass shows the button reads the
 * other way — nothing else in this file or its callers encodes the
 * polarity. */
#define FF_POWER_PWR_ACTIVE_LOW 1

/* S25 slice c — battery-sense ADC. See this file's top comment for the
 * citation; GPIO8 is ADC1's channel 7 on the ESP32-S3 (fixed by the SoC's
 * ADC-to-GPIO map, not a board choice — matches the reference driver's
 * own pin). 12 dB attenuation is the reference driver's own choice too:
 * the widest of the SoC's four attenuation steps, needed because the
 * divided pack voltage (a single-cell LiPo's ~3.0-4.35V pack, /3 at the
 * divider -> ~1.0-1.45V at the pin) still sits above the ~950mV a lower
 * attenuation setting could read without clipping. */
#define FF_PIN_BATT_ADC GPIO_NUM_8
#define FF_BATT_ADC_UNIT ADC_UNIT_1
#define FF_BATT_ADC_CHANNEL ADC_CHANNEL_7 /* GPIO8 on ADC1, this SoC's fixed map */
#define FF_BATT_ADC_ATTEN ADC_ATTEN_DB_12
#define FF_BATT_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT

/* Raw reads averaged per `ff_power_batt_mv()` call, cutting sample-to-
 * sample ADC noise before calibration/conversion even runs (see
 * ff_power.h's doc comment on that function for why this is a SEPARATE
 * concern from ff_batt_filter_t's own cross-tick smoothing, one layer
 * up). 8 is a small, cheap-to-read power of two — each `adc_oneshot_
 * read` is a few microseconds, so 8 of them add negligible cost to a
 * call this file's own caller (app_main.c) makes once every 2 seconds. */
#define FF_BATT_ADC_SAMPLES 8u

/* The pack-voltage conversion (the board's 1:3 divider composed with
 * Waveshare's own measured correction) is pure arithmetic with no ESP
 * dependency — hoisted into ff_power_batt_conv.h (review fix: also
 * where the ×1e6-scaled constant's derivation is documented and where
 * a HOST test, targets/sim/tests/test_batt_pack_mv.c, pins it by
 * literal) rather than defined here. */

static const char *TAG = "ff_power";

esp_err_t ff_power_latch_on(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << FF_PIN_PWR_HOLD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWR-hold gpio_config(GPIO%d) failed: %s", FF_PIN_PWR_HOLD, esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(FF_PIN_PWR_HOLD, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWR-hold set high failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "battery power latched on (SYS_EN GPIO%d high)", FF_PIN_PWR_HOLD);
    return ESP_OK;
}

/* Shared by ff_power_off (a fresh boot that never called ff_power_latch_on
 * on this path, e.g. a unit/bench harness) and ff_power_latch_on's own
 * config — idempotent (gpio_config on an already-output pin is a no-op
 * re-apply, not an error). */
static esp_err_t ff_power_hold_configure_output(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << FF_PIN_PWR_HOLD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

esp_err_t ff_power_off(void)
{
    esp_err_t err = ff_power_hold_configure_output();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWR-hold gpio_config(GPIO%d) failed on power-off: %s", FF_PIN_PWR_HOLD, esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(FF_PIN_PWR_HOLD, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWR-hold set low failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "soft power-off: SYS_EN GPIO%d low (battery: rail drops now; USB: board stays up)",
             FF_PIN_PWR_HOLD);
    return ESP_OK;
}

/* First-call input configuration, shared shape for both key inputs below:
 * configure once, lazily, on the first sample rather than requiring a
 * separate init call the way the two output pins above do — S26's spec
 * scope has app_main tick these every loop iteration with no other
 * bring-up step, and a missed explicit init would otherwise read a
 * floating pin forever. */
static bool s_pwr_key_configured;
static bool s_boot_key_configured;
static bool s_pwr_key_first_read_logged;

bool ff_power_pwr_pressed(void)
{
    if (!s_pwr_key_configured) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << FF_PIN_PWR_KEY,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            /* Non-fatal, same "log and continue" posture as
             * ff_power_latch_on's own failure path: a puck that cannot
             * read PWR is still more debuggable running than parked. A
             * failed config leaves this false forever (gpio_get_level on
             * an unconfigured pin is not something to trust either way),
             * which is the honest degrade — never claim a press that
             * cannot be sampled. */
            ESP_LOGE(TAG, "PWR-key gpio_config(GPIO%d) failed: %s", FF_PIN_PWR_KEY, esp_err_to_name(err));
            return false;
        }
        s_pwr_key_configured = true;
    }

    int const raw = gpio_get_level(FF_PIN_PWR_KEY);
    if (!s_pwr_key_first_read_logged) {
        s_pwr_key_first_read_logged = true;
        ESP_LOGI(TAG, "PWR key (GPIO%d) first raw read: %d (active_low=%d) — verify against a physical press on glass",
                 FF_PIN_PWR_KEY, raw, FF_POWER_PWR_ACTIVE_LOW);
    }

#if FF_POWER_PWR_ACTIVE_LOW
    return raw == 0;
#else
    return raw != 0;
#endif
}

bool ff_power_boot_pressed(void)
{
    if (!s_boot_key_configured) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << FF_PIN_BOOT_KEY,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, /* idle-high; pressed pulls to ground */
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BOOT-key gpio_config(GPIO%d) failed: %s", FF_PIN_BOOT_KEY, esp_err_to_name(err));
            return false; /* honest degrade — see ff_power_pwr_pressed's note */
        }
        s_boot_key_configured = true;
    }

    return gpio_get_level(FF_PIN_BOOT_KEY) == 0; /* active-LOW, standard ESP32 BOOT-button convention */
}

/* ---------------------------------------------------------------------
 * fix/quick-flare-detection (2026-09-03) — BOOT edge ISR + edge ring.
 *
 * WHY: `ff_power_boot_pressed()` above is a level SAMPLE — believed only
 * once `ff_button_tick`'s 30ms debounce elapses, at whatever cadence the
 * render loop happens to poll it (app_main.c's tick period; see this
 * PR's own body for the worst-case arithmetic). A physical press shorter
 * than roughly (tick period + debounce) can be missed entirely, and a
 * press whose edge lands during a slow frame (a face rebuild under
 * `ff_display_lock`) is timestamped LATE — both eat into the multitap
 * gap budget and are exactly the "finicky" symptom the maintainer
 * reported. This block captures the RAW falling edge in a GPIO ISR,
 * timestamped by `esp_timer_get_time()` (microsecond hardware timer,
 * independent of the render loop's own cadence), so a press is recorded
 * the instant it happens regardless of how busy the main task is.
 *
 * The debounced `ff_power_boot_pressed()`/`ff_button_tick` path above is
 * UNCHANGED and still owns ordinary HOME delivery — this is a second,
 * additional signal feeding ONLY the multitap counter (via
 * `ff_shell_multitap_edge`, app_main.c's BOOT sampling block); it never
 * replaces the debounced path.
 * ------------------------------------------------------------------- */

/* FF_POWER_BOOT_EDGE_RING_LEN is public (ff_power.h) — app_main.c sizes
 * its per-tick drain buffer with it. */

/* fix/quick-flare-detection (2026-09-04, review round 2) — the shortest
 * gap between two ring pushes that is believed to be the SAME physical
 * press double-recorded (the edge ISR AND the wake-edge synthesis both
 * capturing it — see `ff_power_boot_isr_synthesize_wake_edge`'s own doc
 * comment for exactly when that can happen) rather than two genuine
 * presses. Independent constant, mirrors `ff_multitap.h`'s
 * `FF_MULTITAP_BOUNCE_MS` / `ff_button.h`'s `FF_BUTTON_DEBOUNCE_MS` BY
 * VALUE only, same "zero cross-layer dependency" reasoning those two
 * headers already give for not sharing their own constants with each
 * other — this is a THIRD, independent layer's own copy of the same
 * "30 ms is bounce" judgment, not a shared one. (The shell's own
 * `ff_multitap_press` would also reject a true duplicate via its own
 * `FF_MULTITAP_BOUNCE_MS` rule even if this layer's dedup somehow
 * missed one — defense in depth, not the only line of defense.) */
#define FF_POWER_BOOT_EDGE_DEDUP_MS ((uint32_t)30u)

/* Single-producer (the ISR, pinned to whichever core services GPIO0, OR
 * the main task itself when synthesizing a wake edge post-resume —
 * never BOTH at once, since the ISR is disabled across the suspend/
 * resume boundary, see `ff_power_boot_isr_suspend_for_sleep`'s doc
 * comment), single-consumer (the main task's own drain,
 * `ff_power_boot_take_edges`, called once per render-loop tick) ring —
 * no lock needed under that discipline: the producer only ever advances
 * `s_boot_edge_head` after writing the slot, and the consumer only ever
 * advances `s_boot_edge_tail` after reading it, so neither side can
 * observe a torn write. `volatile` on the indices is what stops the
 * compiler from caching either across the loop in
 * `ff_power_boot_take_edges` — the ISR can run between any two consumer
 * instructions. */
static volatile uint32_t s_boot_edge_ring[FF_POWER_BOOT_EDGE_RING_LEN];
static volatile uint8_t  s_boot_edge_head; /* next slot the producer will write */
static volatile uint8_t  s_boot_edge_tail; /* next slot the consumer will read */
static volatile uint32_t s_boot_edge_last_push_ms;   /* timestamp of the most recent successful push */
static volatile bool     s_boot_edge_have_pushed;    /* false until the first successful push ever */
static bool               s_boot_isr_installed;
static bool               s_boot_isr_service_installed;

/* IRAM_ATTR explicitly (review round 2) rather than relying on the
 * compiler inlining this into the also-IRAM_ATTR `ff_power_boot_isr`
 * below — it has a SECOND caller now
 * (`ff_power_boot_isr_synthesize_wake_edge`) that is not itself
 * IRAM_ATTR, and inlining is never a guarantee; a call from flash-resident
 * code into a flash-resident function is fine for that second caller
 * (it runs well after resume, flash is mapped again), but the FIRST
 * caller (the ISR) can fire while flash is not necessarily safely
 * accessible, so the function itself must live in IRAM regardless of
 * which caller reaches it. */
static void IRAM_ATTR ff_power_boot_edge_push(uint32_t ms)
{
    uint8_t const head = s_boot_edge_head;
    uint8_t const next = (uint8_t)((head + 1u) % FF_POWER_BOOT_EDGE_RING_LEN);
    if (next == s_boot_edge_tail) {
        /* Ring full — a genuinely pathological burst (>8 undrained
         * edges) or a consumer that has stopped draining. Drop this
         * edge rather than overwrite an undrained one (silently
         * corrupting an earlier edge's timestamp would be worse: a
         * dropped edge just makes one run undercount, exactly as a
         * missed sample always could; a corrupted one could fabricate
         * a false 5th press). Never blocks — this runs in ISR context
         * on real hardware. */
        return;
    }
    s_boot_edge_ring[head] = ms;
    s_boot_edge_head = next;
    s_boot_edge_last_push_ms = ms;
    s_boot_edge_have_pushed = true;
}

static void IRAM_ATTR ff_power_boot_isr(void *arg)
{
    (void)arg;
    ff_power_boot_edge_push((uint32_t)(esp_timer_get_time() / 1000));
}

/**
 * ff_power_boot_isr_init — arm the GPIO0 falling-edge ISR that feeds
 * `ff_power_boot_take_edges`. Call once, at startup, before the render
 * loop's first tick (app_main.c's "S27 sounds device wiring"-style
 * contiguous init block is the right neighborhood — see that file's own
 * BOOT sampling comment for exactly where this landed). Idempotent-safe
 * to call more than once (gpio_config/isr_handler_add re-apply cleanly,
 * same posture as every other HAL bring-up call in this file), but this
 * codebase only calls it the one time.
 *
 * Reuses `s_boot_key_configured` (the flag `ff_power_boot_pressed`'s own
 * lazy config sets) so that function's own first call does NOT re-run
 * `gpio_config` with `GPIO_INTR_DISABLE` and stomp the NEGEDGE type this
 * function sets below — both functions read/write the SAME GPIO0
 * hardware intr-type register, so whichever configures the pin first
 * must be the one whose type sticks, and the ISR's NEGEDGE type is the
 * one that must win (ff_power_boot_pressed only reads gpio_get_level,
 * which is unaffected by intr_type either way).
 */
esp_err_t ff_power_boot_isr_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << FF_PIN_BOOT_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BOOT-key edge gpio_config(GPIO%d) failed: %s — quick-flare will fall back to the "
                       "debounced-tick timing this PR set out to improve on",
                 FF_PIN_BOOT_KEY, esp_err_to_name(err));
        return err;
    }
    s_boot_key_configured = true; /* see this function's own doc comment above */

    if (!s_boot_isr_service_installed) {
        err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            /* INVALID_STATE = some other component already installed
             * the shared ISR service — not an error for us, we still
             * just add our own handler below. */
            ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
            return err;
        }
        s_boot_isr_service_installed = true;
    }

    err = gpio_isr_handler_add(FF_PIN_BOOT_KEY, ff_power_boot_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add(GPIO%d) failed: %s", FF_PIN_BOOT_KEY, esp_err_to_name(err));
        return err;
    }

    s_boot_isr_installed = true;
    ESP_LOGI(TAG, "BOOT (GPIO%d) edge ISR armed (NEGEDGE) — quick-flare multitap now timed off real press edges",
             FF_PIN_BOOT_KEY);
    return ESP_OK;
}

/**
 * ff_power_boot_take_edges — drain up to `max` recorded edges (oldest
 * first) into `out_ms`, returning how many were actually drained. Call
 * once per render-loop tick (app_main.c's BOOT sampling block); each
 * drained timestamp is an `esp_timer_get_time()/1000` reading from the
 * instant a real falling edge fired, meant to be fed straight to
 * `ff_shell_multitap_edge`. Safe to call with `out_ms == NULL` (drains
 * and discards — not used anywhere in this codebase, but harmless) or
 * `max == 0` (returns 0, drains nothing). Returns 0 if the ISR was never
 * armed (`ff_power_boot_isr_init` not called or failed) — the ring
 * simply stays empty forever in that case, same honest-degrade posture
 * as `ff_power_batt_mv`'s "never initialized" path.
 */
size_t ff_power_boot_take_edges(uint32_t *out_ms, size_t max)
{
    size_t n = 0;
    while (n < max) {
        uint8_t const tail = s_boot_edge_tail;
        if (tail == s_boot_edge_head) break; /* empty */
        if (out_ms != NULL) {
            out_ms[n] = s_boot_edge_ring[tail];
        }
        s_boot_edge_tail = (uint8_t)((tail + 1u) % FF_POWER_BOOT_EDGE_RING_LEN);
        n++;
    }
    return n;
}

/**
 * ff_power_boot_isr_suspend_for_sleep — call immediately before
 * (re-)configuring the level wake and entering `esp_light_sleep_start()`.
 *
 * ## The race this fixes (review round 2, 2026-09-04)
 * GPIO0's hardware intr-type register is the SAME register the edge
 * ISR's `GPIO_INTR_NEGEDGE` and the light-sleep wake's
 * `GPIO_INTR_LOW_LEVEL` both write to (ESP light-sleep GPIO wake only
 * supports LEVEL types, not EDGE — ESP-IDF's own `gpio_wakeup_enable`
 * documentation). The FIRST version of this function assumed that
 * merely LEAVING the type at LOW_LEVEL (armed once at startup,
 * `ff_configure_light_sleep_wake`, app_main.c) was enough, and did
 * nothing else — but `gpio_isr_handler_add` (in `ff_power_boot_isr_init`)
 * leaves our own edge-ISR HANDLER attached and its interrupt ENABLED
 * regardless of intr_type; a still-held BOOT press, once the type flips
 * to LOW_LEVEL, would re-trigger THAT handler on the level condition —
 * a level interrupt does not clear itself the way an edge one does, so
 * a long-held BOOT could fire it repeatedly (double count, or an ISR
 * storm for as long as the level holds) BEFORE this file ever got a
 * chance to synthesize the wake edge cleanly. Confirmed as a real race,
 * not a theoretical one, by independent review of PR #194.
 *
 * ## The fix — disable BEFORE reconfiguring, in this exact order
 *  1. `gpio_intr_disable(GPIO0)` FIRST — stop our own handler from
 *     being invoked at all, before anything about the pin's type
 *     changes. This is the step that actually prevents the storm: once
 *     the interrupt is masked, changing intr_type underneath it cannot
 *     call our handler no matter what level the pin sits at.
 *  2. `gpio_wakeup_enable(GPIO0, GPIO_INTR_LOW_LEVEL)` — (re-)arm the
 *     level wake trigger for THIS sleep cycle (idempotent re-apply,
 *     same posture as every other HAL bring-up call in this file;
 *     `ff_configure_light_sleep_wake`'s own one-time startup call is
 *     what arms it the FIRST time, before this function has ever run —
 *     every sleep AFTER the first is armed here instead, since
 *     `ff_power_boot_isr_rearm_after_sleep`, below, flips the type back
 *     to NEGEDGE after each wake).
 *
 * The caller (app_main.c) proceeds to `esp_light_sleep_start()`
 * immediately after this returns — no other GPIO0 configuration happens
 * in between. No-op if the edge ISR was never installed.
 */
void ff_power_boot_isr_suspend_for_sleep(void)
{
    if (!s_boot_isr_installed) return;

    esp_err_t err = gpio_intr_disable(FF_PIN_BOOT_KEY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT-key gpio_intr_disable before sleep failed: %s — the held-BOOT double-count race this "
                       "function exists to close may not be fully closed this cycle",
                 esp_err_to_name(err));
    }

    err = gpio_wakeup_enable(FF_PIN_BOOT_KEY, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT-key gpio_wakeup_enable before sleep failed: %s — a BOOT press may not wake this sleep "
                       "cycle (the earlier startup-time arm may still cover it)",
                 esp_err_to_name(err));
    }
}

/**
 * ff_power_boot_isr_rearm_after_sleep — call IMMEDIATELY after
 * `esp_light_sleep_start()` returns, BEFORE anything else — no
 * `ESP_LOGI`, no `esp_sleep_get_wakeup_cause()` read, nothing (review
 * round 2's explicit ordering requirement). Re-arms `GPIO_INTR_NEGEDGE`
 * + re-enables the interrupt so live presses are captured again during
 * this new ACTIVE period, closing the race `ff_power_boot_isr_suspend_
 * for_sleep`'s own doc comment describes as fast as possible after
 * wake — every instruction between `esp_light_sleep_start()` returning
 * and this call running is time GPIO0 sits at LOW_LEVEL with the
 * interrupt still disabled (safe — disabled, not storming — but also
 * not yet capturing a genuine SECOND press that might land in that
 * window), so nothing is inserted ahead of it. No-op if the edge ISR
 * was never installed.
 */
void ff_power_boot_isr_rearm_after_sleep(void)
{
    if (!s_boot_isr_installed) return;

    esp_err_t const err = gpio_set_intr_type(FF_PIN_BOOT_KEY, GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT-key NEGEDGE re-arm after light sleep failed: %s — quick-flare edge timing degrades to "
                       "the debounced-tick path until the next successful re-arm",
                 esp_err_to_name(err));
        return; /* don't enable an interrupt whose type we just failed to set */
    }
    esp_err_t const enable_err = gpio_intr_enable(FF_PIN_BOOT_KEY);
    if (enable_err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT-key gpio_intr_enable after light sleep failed: %s", esp_err_to_name(enable_err));
    }
}

/**
 * ff_power_boot_isr_synthesize_wake_edge — call AFTER
 * `ff_power_boot_isr_rearm_after_sleep` (above) has already run, once
 * the caller has determined the wake cause and logged it (order no
 * longer matters for THIS call — the re-arm was the time-critical part).
 *
 * `wake_was_boot_gpio`: the caller's own best determination of whether
 * THIS wake was caused by BOOT specifically (app_main.c's own
 * light-sleep block: `esp_sleep_get_wakeup_cause() ==
 * ESP_SLEEP_WAKEUP_GPIO && ff_power_boot_pressed()` — a level-read
 * fallback, not a per-pin status register, because
 * `esp_sleep_get_gpio_wakeup_status()` is compiled out for this chip's
 * light-sleep GPIO wake path; see that call site's own comment for the
 * full reasoning on why the level read is sound here). When true, this
 * function pushes a synthesized edge at the current
 * `esp_timer_get_time()` (i.e., "now") UNLESS a real edge was already
 * pushed within `FF_POWER_BOOT_EDGE_DEDUP_MS` (30 ms) — the just-
 * re-armed NEGEDGE ISR races this call and MAY have already captured
 * the exact same physical press (e.g. if BOOT was released and the
 * falling edge of a near-simultaneous SECOND press landed in the brief
 * window between the wake firing and the re-arm completing); pushing a
 * synthetic edge on top of that would double-count one physical press.
 * Without this synthesis at all, the press that woke the puck would
 * silently not count toward the multitap gesture, even though it is
 * exactly the kind of "first tap from a dark screen" press S10's own
 * spec requires to count. The synthesized timestamp is necessarily a
 * few instructions LATE relative to the true edge (there is no way to
 * recover the exact wake instant from a level trigger), but that error
 * is bounded by the wake-resume path's own latency — not a whole tick
 * period, the way the OLD debounced-tick timing this PR fixes could be
 * — and is a strictly better estimate than dropping the edge entirely.
 */
void ff_power_boot_isr_synthesize_wake_edge(bool wake_was_boot_gpio)
{
    if (!s_boot_isr_installed || !wake_was_boot_gpio) return;

    uint32_t const ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_boot_edge_have_pushed && (ms - s_boot_edge_last_push_ms) < FF_POWER_BOOT_EDGE_DEDUP_MS) {
        /* The re-armed ISR already captured this same physical press —
         * see this function's own doc comment. Skip the synthesis. */
        return;
    }
    ff_power_boot_edge_push(ms);
}

/* ---------------------------------------------------------------------
 * S25 slice c — battery-sense ADC. See this file's top comment for the
 * hardware citation and the constants block above for the pin/atten/
 * correction rationale.
 * ------------------------------------------------------------------- */

static adc_oneshot_unit_handle_t s_batt_adc;      /* NULL until ff_power_batt_init succeeds */
static adc_cali_handle_t         s_batt_cali;     /* NULL until a calibration scheme is established */
static char const               *s_batt_cali_kind = "NONE"; /* "curve" / "line" / "NONE" — honesty label, see doc comment */
static bool                      s_batt_first_reading_logged;

/* Tiered calibration bring-up, same order (and the same reason to try
 * each) as ESP-IDF's own esp_adc oneshot_read example: curve fitting
 * first, then line fitting on any chip where that scheme compiles in,
 * then none. Both `#if` guards mirror `adc_cali_schemes.h`'s own
 * per-chip feature macros rather than assuming either is available —
 * on THIS chip (ESP32-S3) only `ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED`
 * is ever defined (ESP32-S3 has no line-fitting scheme at all), so the
 * line-fitting branch below is inert here, kept for the same
 * "don't assume this file never runs on a different chip" portability
 * reason the SDK's own example keeps it. Sets s_batt_cali/s_batt_cali_kind;
 * returns true iff a scheme was established. */
static bool ff_power_batt_cali_init(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t const cfg = {
            .unit_id = FF_BATT_ADC_UNIT,
            .chan = FF_BATT_ADC_CHANNEL,
            .atten = FF_BATT_ADC_ATTEN,
            .bitwidth = FF_BATT_ADC_BITWIDTH,
        };
        esp_err_t const err = adc_cali_create_scheme_curve_fitting(&cfg, &s_batt_cali);
        if (err == ESP_OK) {
            s_batt_cali_kind = "curve";
            return true;
        }
        ESP_LOGW(TAG, "battery ADC curve-fit calibration unavailable: %s", esp_err_to_name(err));
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t const cfg = {
            .unit_id = FF_BATT_ADC_UNIT,
            .atten = FF_BATT_ADC_ATTEN,
            .bitwidth = FF_BATT_ADC_BITWIDTH,
        };
        esp_err_t const err = adc_cali_create_scheme_line_fitting(&cfg, &s_batt_cali);
        if (err == ESP_OK) {
            s_batt_cali_kind = "line";
            return true;
        }
        ESP_LOGW(TAG, "battery ADC line-fit calibration unavailable: %s", esp_err_to_name(err));
    }
#endif
    s_batt_cali_kind = "NONE";
    s_batt_cali = NULL;
    return false;
}

esp_err_t ff_power_batt_init(void)
{
    adc_oneshot_unit_init_cfg_t const unit_cfg = {
        .unit_id = FF_BATT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_batt_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC unit init failed: %s — battery reading will stay unknown", esp_err_to_name(err));
        s_batt_adc = NULL;
        return err;
    }

    adc_oneshot_chan_cfg_t const chan_cfg = {
        .atten = FF_BATT_ADC_ATTEN,
        .bitwidth = FF_BATT_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_batt_adc, FF_BATT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC channel config (GPIO%d) failed: %s — battery reading will stay unknown",
                 FF_PIN_BATT_ADC, esp_err_to_name(err));
        return err;
    }

    bool const calibrated = ff_power_batt_cali_init();
    ESP_LOGI(TAG, "battery ADC up: unit=1 chan=%d (GPIO%d) atten=12dB calibration=%s%s", FF_BATT_ADC_CHANNEL,
             FF_PIN_BATT_ADC, s_batt_cali_kind, calibrated ? "" : " (uncalibrated — readings will report unknown)");
    return ESP_OK; /* an uncalibrated ADC is a logged degrade, not an init failure — see ff_power.h's doc comment */
}

uint16_t ff_power_batt_mv(void)
{
    if (s_batt_adc == NULL || s_batt_cali == NULL) {
        return 0; /* not initialized, or no calibration scheme — honest "unknown", never a fabricated figure */
    }

    uint32_t raw_sum = 0;
    for (uint32_t i = 0; i < FF_BATT_ADC_SAMPLES; i++) {
        int raw = 0;
        esp_err_t const err = adc_oneshot_read(s_batt_adc, FF_BATT_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "battery ADC read failed: %s", esp_err_to_name(err));
            return 0;
        }
        raw_sum += (uint32_t)raw;
    }
    uint32_t const raw_avg = raw_sum / FF_BATT_ADC_SAMPLES;

    int cal_mv = 0;
    esp_err_t const cal_err = adc_cali_raw_to_voltage(s_batt_cali, (int)raw_avg, &cal_mv);
    if (cal_err != ESP_OK || cal_mv < 0) {
        ESP_LOGW(TAG, "battery ADC calibration convert failed: %s", esp_err_to_name(cal_err));
        return 0;
    }

    /* Pin mV -> pack mV: the pure conversion, host-testable —
     * ff_power_batt_conv.h's own doc comment has the arithmetic, the
     * uint64_t-overflow reasoning, and the clamp rationale. */
    uint16_t const pack_mv = ff_power_batt_pack_mv_from_cal_mv((uint32_t)cal_mv);

    if (!s_batt_first_reading_logged) {
        s_batt_first_reading_logged = true;
        ESP_LOGI(TAG,
                 "S25c first battery reading — raw_avg=%u calibration=%s cal_mv=%d pack_mv=%u "
                 "(sanity-check pack_mv against a multimeter on the pack)",
                 (unsigned)raw_avg, s_batt_cali_kind, cal_mv, (unsigned)pack_mv);
    }

    return pack_mv;
}
