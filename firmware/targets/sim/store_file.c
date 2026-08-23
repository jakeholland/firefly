/**
 * store_file.c — trivial file-backed ff_store_t for the sim target.
 * See store_file.h for the format/scope notes.
 *
 * On-disk format: a flat sequence of records, no header/footer.
 *   [key_len: uint8_t][key: key_len bytes][val_len: uint16_t][val: val_len bytes]
 * repeated until EOF. Keys are unique — ff_store_file_set rewrites the
 * file, dropping any existing record for the key before appending the
 * new one.
 *
 * Multi-byte integers are written in the host's native byte order. This
 * target only ever reads back a file it wrote on the same machine/build
 * (desktop sim, not a cross-arch wire format), so that's not a portability
 * concern here.
 */
#include "store_file.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FF_STORE_FILE_MAX_KEY_LEN 255

/* Streams `n` bytes from `in` to `out` via a small fixed chunk buffer, so
 * neither the record's key nor a giant value ever needs a matching stack
 * buffer. Pass out=NULL to drain-without-copying (used to skip a record
 * being replaced). Returns false on a short read (truncated/corrupt file)
 * or a short write (disk full etc). */
static bool ff_copy_bytes(FILE *in, FILE *out, size_t n)
{
    uint8_t chunk[256];
    while (n > 0) {
        size_t take = n < sizeof(chunk) ? n : sizeof(chunk);
        if (fread(chunk, 1, take, in) != take) {
            return false;
        }
        if (out != NULL && fwrite(chunk, 1, take, out) != take) {
            return false;
        }
        n -= take;
    }
    return true;
}

int ff_store_file_get(void *io, char const *key, void *buf, size_t n)
{
    ff_store_file_t *fsio = (ff_store_file_t *)io;
    if (fsio == NULL || key == NULL) {
        return -1;
    }

    size_t key_len = strlen(key);
    if (key_len == 0 || key_len > FF_STORE_FILE_MAX_KEY_LEN) {
        return -1;
    }

    FILE *f = fopen(fsio->path, "rb");
    if (f == NULL) {
        return -1; /* no store file yet -> key not found */
    }

    int result = -1;
    uint8_t rec_key_len;
    while (fread(&rec_key_len, sizeof(rec_key_len), 1, f) == 1) {
        char rec_key[FF_STORE_FILE_MAX_KEY_LEN + 1];
        if (fread(rec_key, 1, rec_key_len, f) != (size_t)rec_key_len) {
            break; /* truncated/corrupt file */
        }

        uint16_t rec_val_len;
        if (fread(&rec_val_len, sizeof(rec_val_len), 1, f) != 1) {
            break;
        }

        bool is_match = (rec_key_len == key_len) && (memcmp(rec_key, key, key_len) == 0);
        if (is_match) {
            if (n < (size_t)rec_val_len) {
                result = -1; /* buffer too small; never partially fill buf */
            } else if (fread(buf, 1, rec_val_len, f) == (size_t)rec_val_len) {
                result = (int)rec_val_len;
            } else {
                result = -1; /* truncated file */
            }
            break; /* keys are unique on disk (see file header comment) */
        }

        if (fseek(f, (long)rec_val_len, SEEK_CUR) != 0) {
            break;
        }
    }

    fclose(f);
    return result;
}

int ff_store_file_set(void *io, char const *key, void const *buf, size_t n)
{
    ff_store_file_t *fsio = (ff_store_file_t *)io;
    if (fsio == NULL || key == NULL || (buf == NULL && n > 0)) {
        return -1;
    }

    size_t key_len = strlen(key);
    if (key_len == 0 || key_len > FF_STORE_FILE_MAX_KEY_LEN || n > UINT16_MAX) {
        return -1;
    }

    char tmp_path[FF_STORE_FILE_PATH_MAX + 8];
    int written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", fsio->path);
    if (written < 0 || (size_t)written >= sizeof(tmp_path)) {
        return -1;
    }

    FILE *out = fopen(tmp_path, "wb");
    if (out == NULL) {
        return -1;
    }

    bool ok = true;

    /* Copy every existing record except the one matching `key`. */
    FILE *in = fopen(fsio->path, "rb");
    if (in != NULL) {
        uint8_t rec_key_len;
        while (ok && fread(&rec_key_len, sizeof(rec_key_len), 1, in) == 1) {
            char rec_key[FF_STORE_FILE_MAX_KEY_LEN + 1];
            if (fread(rec_key, 1, rec_key_len, in) != (size_t)rec_key_len) {
                ok = false;
                break;
            }

            uint16_t rec_val_len;
            if (fread(&rec_val_len, sizeof(rec_val_len), 1, in) != 1) {
                ok = false;
                break;
            }

            bool is_match = (rec_key_len == key_len) && (memcmp(rec_key, key, key_len) == 0);
            if (is_match) {
                ok = ff_copy_bytes(in, NULL, rec_val_len); /* drop this record */
            } else {
                ok = fwrite(&rec_key_len, sizeof(rec_key_len), 1, out) == 1 &&
                     fwrite(rec_key, 1, rec_key_len, out) == (size_t)rec_key_len &&
                     fwrite(&rec_val_len, sizeof(rec_val_len), 1, out) == 1 &&
                     ff_copy_bytes(in, out, rec_val_len);
            }
        }
        fclose(in);
    }

    if (ok) {
        uint8_t new_key_len = (uint8_t)key_len;
        uint16_t new_val_len = (uint16_t)n;
        ok = fwrite(&new_key_len, sizeof(new_key_len), 1, out) == 1 &&
             fwrite(key, 1, key_len, out) == key_len &&
             fwrite(&new_val_len, sizeof(new_val_len), 1, out) == 1 &&
             (n == 0 || fwrite(buf, 1, n, out) == n);
    }

    if (fclose(out) != 0) {
        ok = false;
    }

    if (!ok) {
        remove(tmp_path);
        return -1;
    }

    if (rename(tmp_path, fsio->path) != 0) {
        remove(tmp_path);
        return -1;
    }

    return (int)n;
}

ff_store_t ff_store_file_init(ff_store_file_t *fsio, char const *path)
{
    ff_store_t st;
    st.get = ff_store_file_get;
    st.set = ff_store_file_set;
    st.io = fsio;

    if (fsio != NULL) {
        if (path == NULL) {
            fsio->path[0] = '\0';
        } else {
            strncpy(fsio->path, path, sizeof(fsio->path) - 1);
            fsio->path[sizeof(fsio->path) - 1] = '\0';
        }
    }

    return st;
}
