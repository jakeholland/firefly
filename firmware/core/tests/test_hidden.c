/**
 * test_hidden.c — the per-node hide set (docs/specs/S02-core-crew.md's
 * 2026-09-13 amendment §C, acceptance criterion **S02_AC13**).
 *
 * The half of AC13 that lives in core: the set itself, its honest-full
 * behaviour, its key derivation, and its round trip through the store
 * blob. The other half — "a hidden id is unpaired, freeing a slot; never
 * re-admitted while hidden; unhiding re-admits on the next qualifying
 * packet" — is a shell behaviour and is tested in
 * firmware/app/tests/test_shell.c.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "ff_hidden.h"

void setUp(void) {}
void tearDown(void) {}

static void S02_AC13_add_contains_remove(void)
{
    ff_hidden_t h;
    ff_hidden_init(&h);
    TEST_ASSERT_EQUAL_UINT8(0u, ff_hidden_count(&h));
    TEST_ASSERT_FALSE(ff_hidden_contains(&h, 0x1234u));

    TEST_ASSERT_TRUE(ff_hidden_add(&h, 0x1234u));
    TEST_ASSERT_TRUE(ff_hidden_contains(&h, 0x1234u));
    TEST_ASSERT_EQUAL_UINT8(1u, ff_hidden_count(&h));

    /* Hiding twice is a no-op SUCCESS, not an error and not a second
     * slot — a UI that reported a failure here would be inventing a
     * problem the wearer does not have. */
    TEST_ASSERT_TRUE(ff_hidden_add(&h, 0x1234u));
    TEST_ASSERT_EQUAL_UINT8(1u, ff_hidden_count(&h));

    TEST_ASSERT_TRUE(ff_hidden_remove(&h, 0x1234u));
    TEST_ASSERT_FALSE(ff_hidden_contains(&h, 0x1234u));
    TEST_ASSERT_EQUAL_UINT8(0u, ff_hidden_count(&h));
    TEST_ASSERT_FALSE(ff_hidden_remove(&h, 0x1234u)); /* nothing to remove */
}

static void S02_AC13_node_zero_is_never_hidden(void)
{
    /* 0 is the wire protocol's "unset", never a node. Hiding it would
     * burn a slot on nobody and would make `contains(0)` — which the
     * admission rule asks — answer for a packet with no sender. */
    ff_hidden_t h;
    ff_hidden_init(&h);
    TEST_ASSERT_FALSE(ff_hidden_add(&h, 0u));
    TEST_ASSERT_EQUAL_UINT8(0u, ff_hidden_count(&h));
    TEST_ASSERT_FALSE(ff_hidden_contains(&h, 0u));
}

static void S02_AC13_full_fails_honestly_and_never_evicts(void)
{
    ff_hidden_t h;
    ff_hidden_init(&h);
    for (uint32_t i = 0; i < FF_HIDDEN_MAX; i++) {
        TEST_ASSERT_TRUE(ff_hidden_add(&h, 1000u + i));
    }
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FF_HIDDEN_MAX, ff_hidden_count(&h));

    /* THE point of this module, versus ff_heard_t's LRU: a hide is a
     * user decision, so a full list refuses the new one rather than
     * silently forgetting an old one and putting somebody back on the
     * wearer's radar unasked. */
    TEST_ASSERT_FALSE(ff_hidden_add(&h, 9999u));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FF_HIDDEN_MAX, ff_hidden_count(&h));
    TEST_ASSERT_FALSE(ff_hidden_contains(&h, 9999u));
    for (uint32_t i = 0; i < FF_HIDDEN_MAX; i++) {
        TEST_ASSERT_TRUE(ff_hidden_contains(&h, 1000u + i)); /* every one still there */
    }

    /* An id already in a FULL list still succeeds — that path must not
     * be gated on the count. */
    TEST_ASSERT_TRUE(ff_hidden_add(&h, 1000u));

    /* Unhiding one makes room again. */
    TEST_ASSERT_TRUE(ff_hidden_remove(&h, 1005u));
    TEST_ASSERT_TRUE(ff_hidden_add(&h, 9999u));
}

static void S02_AC13_order_is_insertion_order_across_a_middle_removal(void)
{
    ff_hidden_t h;
    ff_hidden_init(&h);
    for (uint32_t i = 0; i < 5u; i++) TEST_ASSERT_TRUE(ff_hidden_add(&h, 100u + i));
    TEST_ASSERT_TRUE(ff_hidden_remove(&h, 102u));

    /* The CREW page lists hides in the order they were made; a
     * swap-with-last would silently reorder the list under the wearer's
     * finger between one render and the next. */
    TEST_ASSERT_EQUAL_UINT8(4u, ff_hidden_count(&h));
    TEST_ASSERT_EQUAL_UINT32(100u, ff_hidden_at(&h, 0));
    TEST_ASSERT_EQUAL_UINT32(101u, ff_hidden_at(&h, 1));
    TEST_ASSERT_EQUAL_UINT32(103u, ff_hidden_at(&h, 2));
    TEST_ASSERT_EQUAL_UINT32(104u, ff_hidden_at(&h, 3));
    TEST_ASSERT_EQUAL_UINT32(0u, ff_hidden_at(&h, 4)); /* past the count: 0, not stale */
}

static void S02_AC13_key_is_per_crew_code(void)
{
    char k1[FF_HIDDEN_KEY_MAX], k2[FF_HIDDEN_KEY_MAX];
    TEST_ASSERT_TRUE(ff_hidden_key("FIRE-4K9M7X", k1, sizeof(k1)));
    TEST_ASSERT_EQUAL_STRING("ff.hid.4K9M7X", k1);
    TEST_ASSERT_TRUE(ff_hidden_key("FIRE-000000", k2, sizeof(k2)));
    TEST_ASSERT_EQUAL_STRING("ff.hid.000000", k2);
    TEST_ASSERT_TRUE(strcmp(k1, k2) != 0); /* two crews never share a key */

    /* NVS caps keys at 15 characters (targets/esp32s3/main/ff_nvs_store.c). */
    TEST_ASSERT_TRUE(strlen(k1) <= 15u);

    /* Not on a crew channel = no hide list to load, which is a different
     * thing from an empty one and must not fall back to some default
     * key that another crew could also land on. */
    char bad[FF_HIDDEN_KEY_MAX];
    TEST_ASSERT_FALSE(ff_hidden_key("LongFast", bad, sizeof(bad)));
    TEST_ASSERT_EQUAL_STRING("", bad);
    TEST_ASSERT_FALSE(ff_hidden_key(NULL, bad, sizeof(bad)));
    char tiny[4];
    TEST_ASSERT_FALSE(ff_hidden_key("FIRE-4K9M7X", tiny, sizeof(tiny)));
}

static void S02_AC13_blob_round_trips(void)
{
    ff_hidden_t a;
    ff_hidden_init(&a);
    TEST_ASSERT_TRUE(ff_hidden_add(&a, 0xDEADBEEFu));
    TEST_ASSERT_TRUE(ff_hidden_add(&a, 0x00000001u));
    TEST_ASSERT_TRUE(ff_hidden_add(&a, 0xFFFFFFFFu));

    uint8_t blob[FF_HIDDEN_BLOB_LEN];
    TEST_ASSERT_EQUAL_size_t(FF_HIDDEN_BLOB_LEN, ff_hidden_serialize(&a, blob, sizeof(blob)));

    ff_hidden_t b;
    TEST_ASSERT_TRUE(ff_hidden_deserialize(&b, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(a.count, b.count);
    for (uint8_t i = 0; i < a.count; i++) {
        TEST_ASSERT_EQUAL_UINT32(ff_hidden_at(&a, i), ff_hidden_at(&b, i));
    }

    /* Explicit little-endian on the wire, so a blob means the same thing
     * whether the sim wrote it on a desktop or the puck wrote it in NVS. */
    TEST_ASSERT_EQUAL_HEX8(0xEFu, blob[4]);
    TEST_ASSERT_EQUAL_HEX8(0xBEu, blob[5]);
    TEST_ASSERT_EQUAL_HEX8(0xADu, blob[6]);
    TEST_ASSERT_EQUAL_HEX8(0xDEu, blob[7]);

    /* Fixed length regardless of count: bytes past the count are zero. */
    for (size_t i = 4u + 4u * 3u; i < FF_HIDDEN_BLOB_LEN; i++) {
        TEST_ASSERT_EQUAL_HEX8(0u, blob[i]);
    }
}

static void S02_AC13_empty_round_trips_as_empty(void)
{
    ff_hidden_t a;
    ff_hidden_init(&a);
    uint8_t blob[FF_HIDDEN_BLOB_LEN];
    TEST_ASSERT_EQUAL_size_t(FF_HIDDEN_BLOB_LEN, ff_hidden_serialize(&a, blob, sizeof(blob)));
    ff_hidden_t b;
    TEST_ASSERT_TRUE(ff_hidden_deserialize(&b, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(0u, ff_hidden_count(&b));
}

static void S02_AC13_corrupt_blobs_reject_into_empty(void)
{
    ff_hidden_t a;
    ff_hidden_init(&a);
    TEST_ASSERT_TRUE(ff_hidden_add(&a, 7u));
    TEST_ASSERT_TRUE(ff_hidden_add(&a, 8u));
    uint8_t good[FF_HIDDEN_BLOB_LEN];
    ff_hidden_serialize(&a, good, sizeof(good));

    struct { char const *what; size_t len; int off; uint8_t val; } const mutations[] = {
        {"short read",      FF_HIDDEN_BLOB_LEN - 1u, -1, 0u},
        {"bad magic lo",    FF_HIDDEN_BLOB_LEN,       0, 0x00u},
        {"bad magic hi",    FF_HIDDEN_BLOB_LEN,       1, 0x00u},
        {"unknown version", FF_HIDDEN_BLOB_LEN,       2, 99u},
        {"count too big",   FF_HIDDEN_BLOB_LEN,       3, (uint8_t)(FF_HIDDEN_MAX + 1u)},
        {"id zero",         FF_HIDDEN_BLOB_LEN,       4, 0x00u}, /* 7 -> 0 in the low byte... */
    };

    for (size_t m = 0; m < sizeof(mutations) / sizeof(mutations[0]); m++) {
        uint8_t buf[FF_HIDDEN_BLOB_LEN];
        memcpy(buf, good, sizeof(buf));
        if (mutations[m].off >= 0) buf[mutations[m].off] = mutations[m].val;

        ff_hidden_t b;
        ff_hidden_init(&b);
        TEST_ASSERT_TRUE(ff_hidden_add(&b, 4242u)); /* pre-dirtied, to prove it is reset */

        bool const ok = ff_hidden_deserialize(&b, buf, mutations[m].len);
        TEST_ASSERT_FALSE_MESSAGE(ok, mutations[m].what);
        /* Reject means EMPTY, never partially trusted: a half-read hide
         * list silently un-hides an arbitrary subset of people the
         * wearer deliberately hid, and nothing would tell them. */
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, ff_hidden_count(&b), mutations[m].what);
        TEST_ASSERT_FALSE(ff_hidden_contains(&b, 4242u));
    }

    /* A LONGER-than-expected read is rejected too: the record is fixed
     * length, so extra bytes are somebody else's data under our key, not
     * a newer, bigger hide list. */
    uint8_t longer[FF_HIDDEN_BLOB_LEN + 8u];
    memset(longer, 0, sizeof(longer));
    memcpy(longer, good, FF_HIDDEN_BLOB_LEN);
    ff_hidden_t d;
    TEST_ASSERT_FALSE(ff_hidden_deserialize(&d, longer, sizeof(longer)));
    TEST_ASSERT_EQUAL_UINT8(0u, ff_hidden_count(&d));

    /* NULL buffer. */
    ff_hidden_t c;
    TEST_ASSERT_FALSE(ff_hidden_deserialize(&c, NULL, FF_HIDDEN_BLOB_LEN));
    TEST_ASSERT_EQUAL_UINT8(0u, ff_hidden_count(&c));
}

static void S02_AC13_null_safety(void)
{
    ff_hidden_init(NULL);
    TEST_ASSERT_FALSE(ff_hidden_add(NULL, 1u));
    TEST_ASSERT_FALSE(ff_hidden_remove(NULL, 1u));
    TEST_ASSERT_FALSE(ff_hidden_contains(NULL, 1u));
    TEST_ASSERT_EQUAL_UINT8(0u, ff_hidden_count(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, ff_hidden_at(NULL, 0));
    uint8_t buf[FF_HIDDEN_BLOB_LEN];
    TEST_ASSERT_EQUAL_size_t(0u, ff_hidden_serialize(NULL, buf, sizeof(buf)));
    TEST_ASSERT_FALSE(ff_hidden_deserialize(NULL, buf, sizeof(buf)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S02_AC13_add_contains_remove);
    RUN_TEST(S02_AC13_node_zero_is_never_hidden);
    RUN_TEST(S02_AC13_full_fails_honestly_and_never_evicts);
    RUN_TEST(S02_AC13_order_is_insertion_order_across_a_middle_removal);
    RUN_TEST(S02_AC13_key_is_per_crew_code);
    RUN_TEST(S02_AC13_blob_round_trips);
    RUN_TEST(S02_AC13_empty_round_trips_as_empty);
    RUN_TEST(S02_AC13_corrupt_blobs_reject_into_empty);
    RUN_TEST(S02_AC13_null_safety);
    return UNITY_END();
}
