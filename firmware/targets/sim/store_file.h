/**
 * store_file.h — trivial file-backed ff_store_t for the sim target
 * (spec S11, slice a: "trivial file-backed store for the sim ... a
 * single key-value file").
 *
 * All keys live in one flat file at a caller-chosen path. Each ff_store_file_set
 * call rewrites the whole file (read existing records, drop any with the
 * same key, append the new one, write atomically via a temp file + rename)
 * — simple and correct, not optimized for large key counts. Fine for a
 * handful of settings/crew/pack-id keys on a desktop sim target.
 *
 * Not built for the esp32s3 target — that gets an NVS-backed store with S15.
 */
#ifndef FF_SIM_STORE_FILE_H
#define FF_SIM_STORE_FILE_H

#include <stddef.h>

#include "ff_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FF_STORE_FILE_PATH_MAX 512

typedef struct {
    char path[FF_STORE_FILE_PATH_MAX];
} ff_store_file_t;

/**
 * ff_store_file_init — configure `fsio` to back onto the file at `path`
 * (created lazily on the first ff_store_file_set call; reading before
 * that returns "not found", not an error). `path` is copied in, truncated
 * to FF_STORE_FILE_PATH_MAX - 1 bytes if too long.
 *
 * Returns an ff_store_t vtable wired to `fsio` — pass `fsio` unchanged
 * for the vtable's lifetime.
 */
ff_store_t ff_store_file_init(ff_store_file_t *fsio, char const *path);

/* Vtable-compatible get/set, exposed directly in case a caller wants to
 * build its own ff_store_t rather than use ff_store_file_init. */
int ff_store_file_get(void *io, char const *key, void *buf, size_t n);
int ff_store_file_set(void *io, char const *key, void const *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* FF_SIM_STORE_FILE_H */
