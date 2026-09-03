/**
 * ff_clock.h — shared monotonic clock seam.
 *
 * Extraction-grade libraries (meshclient, and eventually core) need a
 * source of "now" without depending on a specific platform (sim uses
 * SDL_GetTicks()/POSIX clock_gettime, esp32s3 uses esp_timer). This is a
 * tiny vtable + user pointer, the same shape as ff_transport_t: no
 * globals, injected at init.
 *
 * Lives under firmware/platform/ rather than firmware/core/ or
 * firmware/meshclient/ so that both can depend on it without either
 * depending on the other (meshclient is extraction-grade: zero includes
 * from core/ or app/, per docs/specs/S03-meshclient.md AC7).
 */
#ifndef FF_CLOCK_H
#define FF_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Monotonically nondecreasing milliseconds since some arbitrary
     * epoch (boot, process start, whatever the platform finds cheapest).
     * Never negative, wraps per uint32_t rules like any embedded tick
     * counter — callers compare with subtraction, not '<', so wraparound
     * is safe. */
    uint32_t (*now_ms)(void *user);
    void *user;
} ff_clock_t;

/**
 * ff_time_reached — wraparound-safe "has now_ms reached (or passed)
 * deadline_ms yet", the comparison-by-subtraction convention documented
 * above. INCLUSIVE at the boundary: now_ms == deadline_ms already
 * returns true. Correct across uint32_t ms rollover as long as the true
 * gap between now_ms and deadline_ms stays within 2^31 ms (~24.8 days),
 * same as any twos-complement deadline check.
 *
 * The single copy of a one-liner six core modules used to each define
 * for themselves (ff_button.c, ff_power_fsm.c, ff_idle.c, ff_notify.c,
 * ff_flare.c, ff_demofeed.c) — behaviour is unchanged, just de-duplicated.
 */
static inline bool ff_time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

#ifdef __cplusplus
}
#endif

#endif /* FF_CLOCK_H */
