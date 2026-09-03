/**
 * mock_store.h — shared in-memory ff_store_t mock for core unit tests
 * (debt/test-harness PR).
 *
 * Extracted out of core/tests/test_settings.c and core/tests/test_wall.c,
 * which had each hand-rolled their own `mock_get`/`mock_set` and had
 * already diverged: test_settings.c's counted `set_calls`/`get_calls` and
 * `strncpy`-with-explicit-NUL `mock_set` (returning `(int)n`, the bytes
 * written) vs. test_wall.c's uncounted, `snprintf`-keyed `mock_set`
 * (returning a bare `0`). This header is the superset of both — counters
 * plus the bounds check plus the `(int)n` return — so both call sites
 * behave exactly as test_settings.c's did (the stricter, more complete
 * shape) and neither test file loses anything it was already relying on.
 *
 * Header-only (`static inline`, not a `.c` translation unit + CMake
 * target) — the two current call sites are single translation units
 * each including this once; if a third grows a real reason to link it as
 * a library instead, do that then, not preemptively here.
 *
 * A single fixed-size slot (MOCK_SLOT_CAP bytes), not a general
 * key/value store: exactly what both existing call sites need (each
 * only ever stores one settings blob under one key at a time).
 */
#ifndef FF_TEST_SUPPORT_MOCK_STORE_H
#define FF_TEST_SUPPORT_MOCK_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ff_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_STORE_SLOT_CAP 256

typedef struct {
    bool has_value;
    size_t len;
    uint8_t data[MOCK_STORE_SLOT_CAP];
    char key[64];
    int set_calls;
    int get_calls;
} mock_store_io_t;

static inline void mock_store_reset(mock_store_io_t *m)
{
    memset(m, 0, sizeof(*m));
}

static inline int mock_store_get(void *io, char const *key, void *buf, size_t n)
{
    mock_store_io_t *m = (mock_store_io_t *)io;
    m->get_calls++;
    if (!m->has_value || strcmp(m->key, key) != 0) {
        return -1;
    }
    if (n < m->len) {
        return -1; /* buffer too small */
    }
    memcpy(buf, m->data, m->len);
    return (int)m->len;
}

static inline int mock_store_set(void *io, char const *key, void const *buf, size_t n)
{
    mock_store_io_t *m = (mock_store_io_t *)io;
    m->set_calls++;
    if (n > MOCK_STORE_SLOT_CAP) {
        return -1;
    }
    strncpy(m->key, key, sizeof(m->key) - 1);
    m->key[sizeof(m->key) - 1] = '\0';
    memcpy(m->data, buf, n);
    m->len = n;
    m->has_value = true;
    return (int)n;
}

static inline ff_store_t mock_store_vtable(mock_store_io_t *m)
{
    ff_store_t st;
    st.get = mock_store_get;
    st.set = mock_store_set;
    st.io = m;
    return st;
}

#ifdef __cplusplus
}
#endif

#endif /* FF_TEST_SUPPORT_MOCK_STORE_H */
