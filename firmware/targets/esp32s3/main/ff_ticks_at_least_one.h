/**
 * ff_ticks_at_least_one.h — pure integer arithmetic behind app_main.c's
 * `ff_ticks_at_least_one()` helper (fix/mic-dump-device-path: the `mic
 * dump` poll-loop bug, "5ms poll rounds to 0 ticks at CONFIG_FREERTOS_
 * HZ=100 and never actually sleeps" — the same bug class PR #216 fixed
 * for the i2c bus-scan's 5ms retry timeout).
 *
 * Factored out into a header with ZERO FreeRTOS/ESP-IDF includes so it
 * has a real, host-run test rather than shipping this arithmetic
 * unverified — the same "extraction-grade for a host test" reasoning
 * targets/esp32s3/components/ff_meshclient/include/
 * mc_transport_uart_accept.h already documents for the identical shape
 * (a pure calculation behind an otherwise ESP-IDF-only caller). See
 * that header's own test, targets/esp32s3/components/ff_meshclient/
 * tests/test_transport_uart_accept.c, for the precedent this mirrors —
 * and this file's own tests/test_ticks_at_least_one.c for the actual
 * coverage, wired into the sim/host ctest run from
 * targets/sim/CMakeLists.txt (device main.c is never itself part of
 * that host build).
 *
 * `pdMS_TO_TICKS(ms)` (FreeRTOS's own freertos/projdefs.h) reduces to
 * `((TickType_t)(((TickType_t)(ms) * (TickType_t)configTICK_RATE_HZ) /
 * (TickType_t)1000))` on every FreeRTOS port this project targets —
 * truncating INTEGER division toward zero, so any `ms` under one tick
 * period (1000/configTICK_RATE_HZ ms — 10ms at this project's own
 * CONFIG_FREERTOS_HZ=100, firmware/targets/esp32s3/sdkconfig) computes
 * to exactly 0 ticks, and `vTaskDelay(0)`/a 0-tick blocking-call timeout
 * is a bare yield, never a real sleep or wait. `ff_ticks_at_least_one_
 * calc` below reproduces that SAME formula — parameterized by
 * `tick_rate_hz` so a host test can exercise this project's real
 * CONFIG_FREERTOS_HZ=100 without linking FreeRTOS at all, and so it is
 * provably the thing app_main.c's real `ff_ticks_at_least_one` wrapper
 * calls, never a parallel reimplementation that could quietly drift —
 * then clamps a 0 result up to 1, the one behavior change this whole
 * fix is about.
 */
#ifndef FF_TICKS_AT_LEAST_ONE_H
#define FF_TICKS_AT_LEAST_ONE_H

#include <stdint.h>

static inline uint32_t ff_ticks_at_least_one_calc(uint32_t ms, uint32_t tick_rate_hz)
{
    uint32_t const ticks = (uint32_t)(((uint64_t)ms * (uint64_t)tick_rate_hz) / 1000u);
    return (ticks == 0u) ? 1u : ticks;
}

#endif /* FF_TICKS_AT_LEAST_ONE_H */
