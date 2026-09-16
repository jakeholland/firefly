/**
 * ff_power_latch_seq.h — S25 latch-hold amendment (2026-09-16 field
 * report, docs/specs/S25-power-latch.md's Amendments section): the pure,
 * ESP-free ORDER OF OPERATIONS for the SYS_EN pad-hold fix, hoisted out
 * of `ff_power.c` the same way `ff_power_batt_conv.h` hoists the battery
 * conversion math — so a HOST test (`targets/sim/tests/
 * test_power_latch_seq.c`) can pin the sequence by assertion, no `idf.py
 * build` required.
 *
 * ## Why the ORDER is the thing worth testing
 * `gpio_hold_en()`/`gpio_hold_dis()` (ESP-IDF's GPIO driver) latch a
 * pad's output level so it survives a digital-core reset — per
 * ESP-IDF's own doc comment on `gpio_hold_en()`: "This function can be
 * used to retain the state of GPIOs when the power domain of where
 * GPIO/IOMUX belongs to becomes off. For example, chip or system is
 * reset (e.g. watchdog time-out, Deep-sleep events are triggered), or
 * peripheral power-down in Light-sleep." That list names a watchdog
 * reset explicitly, and `esp_restart()`/a panic reboot go through the
 * same digital-core-reset path — none of them power off the RTC/IO_MUX
 * domain the hold state lives in, unlike an actual loss of chip power.
 *
 * Once a pad is held, ESP-IDF's own `gpio_hold_dis()` doc note says
 * plainly that further writes do not reach it: "the gpio will output
 * the default level if this function is called" — i.e. nothing else
 * changes the pin's level until hold is released. So:
 *   - latch-ON must set the pin HIGH, *then* enable the hold — enabling
 *     the hold before the level is set would latch the pin's PRIOR
 *     (floating/reset) level instead of the intended HIGH.
 *   - power-OFF must disable the hold, *then* drive the pin LOW —
 *     driving it low first would be silently swallowed by the still-
 *     active hold: `ff_power_off()` would return `ESP_OK` (no gpio call
 *     fails) while the rail never actually drops. Getting this one
 *     backwards is not a compile error, not a runtime error, and not
 *     observable on USB (where the rail is fed externally regardless) —
 *     only a battery bench test would ever catch it, which is exactly
 *     the class of defect that shipped the reported bug in the first
 *     place. Pinning the order in a host test closes that gap.
 *
 * No `esp_err_t` here (that is ESP-IDF's `esp_err.h`, deliberately not
 * included — same "no ADC, no gpio.h... nothing that only compiles
 * under idf.py build" posture `ff_power_batt_conv.h`'s own top comment
 * states for the battery math). Plain `int`, 0 == ok, mirrors
 * `esp_err_t`'s own `ESP_OK == 0` convention closely enough that
 * `ff_power.c` can pass real `esp_err_t` values through this seam
 * unchanged (a narrowing/widening `int`<->`esp_err_t` cast at the call
 * site, not a value translation).
 */
#ifndef FF_POWER_LATCH_SEQ_H
#define FF_POWER_LATCH_SEQ_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_power_latch_hal_t — the three primitive pin operations this
 * sequence composes, injected so a host test can substitute a recording
 * mock for the real `driver/gpio.h` calls. `io` is an opaque handle
 * passed back to each function verbatim (same shape as `ff_store_t`,
 * core/include/ff_store.h) — NULL is a valid value; the real ESP32-S3
 * binding (`ff_power.c`) has no state to pass and leaves it NULL,
 * closing over the pin number via `#define FF_PIN_PWR_HOLD` instead.
 *
 * Each function returns 0 on success, matching `ESP_OK`; any nonzero
 * value is treated as a failure and short-circuits the sequence
 * (mirrors every other HAL call in this component's "return the
 * underlying error, already logged by the caller" posture).
 */
typedef struct {
    void *io;
    int (*set_level)(void *io, int level); /* wraps gpio_set_level(FF_PIN_PWR_HOLD, level) */
    int (*hold_en)(void *io);              /* wraps gpio_hold_en(FF_PIN_PWR_HOLD) */
    int (*hold_dis)(void *io);             /* wraps gpio_hold_dis(FF_PIN_PWR_HOLD) */
} ff_power_latch_hal_t;

/**
 * ff_power_latch_seq_on — the latch-ON sequence: set the pin HIGH, then
 * enable the pad hold. Stops and returns the first nonzero result (the
 * hold is never enabled if the level write itself failed — there would
 * be nothing correct to hold).
 */
static inline int ff_power_latch_seq_on(ff_power_latch_hal_t const *hal)
{
    int err = hal->set_level(hal->io, 1);
    if (err != 0) {
        return err;
    }
    return hal->hold_en(hal->io);
}

/**
 * ff_power_latch_seq_off — the power-OFF sequence: disable the pad
 * hold, then drive the pin LOW. Stops and returns the first nonzero
 * result. Safe to call even when the hold was never enabled (a caller
 * that reaches power-off without ever having called the ON sequence,
 * e.g. a bench/unit harness) — `hal->hold_dis` is expected to be a
 * no-op on an unheld pin (this project has not yet verified that claim
 * against real ESP-IDF behavior on this board; see ff_power.c's own
 * call site comment).
 */
static inline int ff_power_latch_seq_off(ff_power_latch_hal_t const *hal)
{
    int err = hal->hold_dis(hal->io);
    if (err != 0) {
        return err;
    }
    return hal->set_level(hal->io, 0);
}

#ifdef __cplusplus
}
#endif

#endif /* FF_POWER_LATCH_SEQ_H */
