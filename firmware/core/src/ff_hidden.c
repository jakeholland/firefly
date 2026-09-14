/**
 * ff_hidden.c — the per-node hide set (docs/specs/S02-core-crew.md's
 * 2026-09-13 amendment §C). See ff_hidden.h for the whole rationale;
 * this file is mechanism only.
 */
#include "ff_hidden.h"

#include <string.h>

/* Blob header. The magic is a cheap, explicit "this is one of ours"
 * check so a key collision with some other record can never be read as a
 * hide list; the version exists so a future format change rejects old
 * blobs loudly instead of mis-parsing them. */
#define FF_HIDDEN_MAGIC   0x4648u /* 'F','H' little-endian */
#define FF_HIDDEN_VERSION 1u

void ff_hidden_init(ff_hidden_t *h)
{
    if (h == NULL) return;
    memset(h, 0, sizeof(*h));
}

static int ff_hidden_index_of(ff_hidden_t const *h, uint32_t node_id)
{
    for (uint8_t i = 0; i < h->count; i++) {
        if (h->ids[i] == node_id) return (int)i;
    }
    return -1;
}

bool ff_hidden_add(ff_hidden_t *h, uint32_t node_id)
{
    /* 0 is never a valid Meshtastic node id (the wire protocol's "unset"
     * convention — the same reason mc_client's rx-meta dispatch skips
     * `from == 0`). Hiding it would burn a slot on nobody. */
    if (h == NULL || node_id == 0u) return false;
    if (ff_hidden_index_of(h, node_id) >= 0) return true; /* already hidden: a no-op success */
    if (h->count >= FF_HIDDEN_MAX) return false;          /* full: fails honestly, never evicts */
    h->ids[h->count++] = node_id;
    return true;
}

bool ff_hidden_remove(ff_hidden_t *h, uint32_t node_id)
{
    if (h == NULL || node_id == 0u) return false;
    int const at = ff_hidden_index_of(h, node_id);
    if (at < 0) return false;
    /* Close the gap so insertion order survives an unhide from the
     * middle — the CREW page lists these in the order they were hidden
     * (ff_hidden_at), and a swap-with-last would silently reorder the
     * list under the wearer's finger. */
    for (uint8_t i = (uint8_t)at; i + 1u < h->count; i++) {
        h->ids[i] = h->ids[i + 1u];
    }
    h->count--;
    h->ids[h->count] = 0u;
    return true;
}

bool ff_hidden_contains(ff_hidden_t const *h, uint32_t node_id)
{
    if (h == NULL || node_id == 0u) return false;
    return ff_hidden_index_of(h, node_id) >= 0;
}

uint8_t ff_hidden_count(ff_hidden_t const *h)
{
    return (h == NULL) ? 0u : h->count;
}

uint32_t ff_hidden_at(ff_hidden_t const *h, uint8_t idx)
{
    if (h == NULL || idx >= h->count) return 0u;
    return h->ids[idx];
}

bool ff_hidden_key(char const *canonical, char *buf, size_t n)
{
    if (buf == NULL || n == 0u) return false;
    buf[0] = '\0';
    if (!ff_crewcode_valid(canonical) || n < FF_HIDDEN_KEY_MAX) return false;

    static char const prefix[] = "ff.hid.";
    size_t const prefix_len = sizeof(prefix) - 1u;
    memcpy(buf, prefix, prefix_len);
    /* The six symbols only — the constant `FIRE-` tag would cost 5 of
     * NVS's 15 key characters and distinguishes nothing. */
    memcpy(buf + prefix_len, canonical + (sizeof(FF_CREWCODE_TAG) - 1u), FF_CREWCODE_SYMBOLS);
    buf[prefix_len + FF_CREWCODE_SYMBOLS] = '\0';
    return true;
}

size_t ff_hidden_serialize(ff_hidden_t const *h, uint8_t *buf, size_t n)
{
    if (h == NULL || buf == NULL || n < FF_HIDDEN_BLOB_LEN) return 0u;
    memset(buf, 0, FF_HIDDEN_BLOB_LEN);
    buf[0] = (uint8_t)(FF_HIDDEN_MAGIC & 0xFFu);
    buf[1] = (uint8_t)((FF_HIDDEN_MAGIC >> 8) & 0xFFu);
    buf[2] = (uint8_t)FF_HIDDEN_VERSION;
    buf[3] = h->count;
    for (uint8_t i = 0; i < h->count; i++) {
        size_t const off = 4u + 4u * (size_t)i;
        buf[off]      = (uint8_t)(h->ids[i] & 0xFFu);
        buf[off + 1u] = (uint8_t)((h->ids[i] >> 8) & 0xFFu);
        buf[off + 2u] = (uint8_t)((h->ids[i] >> 16) & 0xFFu);
        buf[off + 3u] = (uint8_t)((h->ids[i] >> 24) & 0xFFu);
    }
    return FF_HIDDEN_BLOB_LEN;
}

bool ff_hidden_deserialize(ff_hidden_t *h, uint8_t const *buf, size_t n)
{
    if (h == NULL) return false;
    ff_hidden_init(h); /* empty on EVERY failure path below, never partial */
    if (buf == NULL || n != FF_HIDDEN_BLOB_LEN) return false;

    uint16_t const magic = (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
    if (magic != FF_HIDDEN_MAGIC) return false;
    if (buf[2] != (uint8_t)FF_HIDDEN_VERSION) return false;
    uint8_t const count = buf[3];
    if (count > FF_HIDDEN_MAX) return false;

    for (uint8_t i = 0; i < count; i++) {
        size_t const off = 4u + 4u * (size_t)i;
        uint32_t const id = (uint32_t)buf[off] | ((uint32_t)buf[off + 1u] << 8) |
                             ((uint32_t)buf[off + 2u] << 16) | ((uint32_t)buf[off + 3u] << 24);
        /* A stored 0, or a duplicate, is corruption rather than a
         * smaller list: reject the blob rather than load a hide set that
         * silently differs from the one that was written. */
        if (id == 0u) {
            ff_hidden_init(h);
            return false;
        }
        if (!ff_hidden_add(h, id) || h->count != (uint8_t)(i + 1u)) {
            ff_hidden_init(h);
            return false;
        }
    }
    return true;
}
