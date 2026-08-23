/**
 * ff_store.h — key/value persistence seam (spec S11).
 *
 * A tiny vtable so core code (ff_settings.c, and future persisted modules)
 * can read/write durable state without knowing whether it's landing in a
 * flat file (sim, see targets/sim/store_file.h) or NVS (esp32s3, S15).
 *
 * Pure interface: no implementation lives here. Zero I/O, zero allocation.
 */
#ifndef FF_STORE_H
#define FF_STORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_store_t — storage vtable injected into core modules that persist
 * state (ff_settings, and later crew/starred-sets/pack-id, per S11).
 *
 * get(io, key, buf, n) — read the value stored under `key` into `buf`
 *   (capacity `n` bytes). Returns the value's length on success, or a
 *   negative value if the key is absent, `buf` is too small to hold the
 *   full value, or the backing store failed. Never partially fills `buf`
 *   on a negative return.
 *
 * set(io, key, buf, n) — write `n` bytes from `buf` as the value for
 *   `key`, replacing any prior value under that key. Returns the number
 *   of bytes written (== n) on success, or a negative value on failure.
 *
 * `io` is an opaque handle owned by the backing implementation (e.g. a
 * `ff_store_file_t *` for the sim target) and passed back verbatim to
 * `get`/`set` as their first argument.
 */
typedef struct {
    int (*get)(void *io, char const *key, void *buf, size_t n);
    int (*set)(void *io, char const *key, void const *buf, size_t n);
    void *io;
} ff_store_t;

#ifdef __cplusplus
}
#endif

#endif /* FF_STORE_H */
