/**
 * ff_nvs_store.h — a real ESP-IDF NVS-backed ff_store_t for the esp32s3
 * target (S21 §4 — "persistence that sticks").
 *
 * This is the device half of the S11/S15 `ff_store` = NVS deliverable: it
 * persists the `ff_settings` versioned blob (bytes in, bytes out — the store
 * knows nothing about the blob's shape; ff_settings.c stays the pure
 * (de)serializer) to a flash key, so touch calibration, brightness, name,
 * quiet hours — everything in ff_settings — survive a reboot. It replaces
 * the no-op stub app_main used through S15.
 *
 * Core stays pure: NVS lives here in the target HAL, behind the same
 * `ff_store_t` vtable seam the sim's file-backed store (targets/sim/
 * store_file.h) implements. The shell loads settings from it at init and
 * saves on every change, exactly as it already did against the stub — only
 * now the bytes land in flash.
 *
 * Degraded mode is honest, not fatal: if NVS init or open fails (a fresh or
 * corrupt partition that even erase-and-retry can't recover), the returned
 * store's `get` reports "not found" and `set` is a no-op, so the shell falls
 * back to the exact defaults and simply does not persist — the puck still
 * boots and runs, it just won't remember settings across a reboot, and it
 * says so in the log rather than crashing.
 */
#ifndef FF_NVS_STORE_H
#define FF_NVS_STORE_H

#include <stdbool.h>

#include "nvs.h"

#include "ff_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_nvs_store_t — backing state for the NVS store. Caller owns the storage
 * (app_main holds one static instance); pass its address to
 * ff_nvs_store_init and thereafter only through the returned ff_store_t.
 */
typedef struct {
    nvs_handle_t handle;
    bool open; /* false when NVS init/open failed — get/set then degrade to no-op */
} ff_nvs_store_t;

/**
 * ff_nvs_store_init — initialize NVS flash (erasing and retrying once on a
 * version-mismatch / no-free-pages partition, the standard ESP-IDF idiom),
 * open the "firefly" namespace read/write, and return an ff_store_t bound to
 * `io`. Never fails hard: on any error `io->open` is left false and the
 * returned vtable degrades to "no persistence" (get -> not found, set ->
 * no-op) so the shell runs on defaults. Logs what happened either way.
 */
ff_store_t ff_nvs_store_init(ff_nvs_store_t *io);

#ifdef __cplusplus
}
#endif

#endif /* FF_NVS_STORE_H */
