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

#ifdef __cplusplus
}
#endif

#endif /* FF_CLOCK_H */
