/**
 * ff_hidden.h — core/hidden: the bounded, per-node "don't show me this
 * person" set.
 *
 * Spec: docs/specs/S02-core-crew.md's 2026-09-13 amendment §C (the puck
 * half of docs/specs/A02-crew-join.md §4.5).
 *
 * ## What hiding is, and what it is not
 *
 * Hide is **per node id, local to this puck, and never transmitted.**
 * Nobody is told they were hidden. On a mesh where possession of the
 * crew key IS membership there is no "kick" to perform, and a UI that
 * implied one would be lying about what the radio is doing. Hidden is
 * "off my radar", not "blocked": their messages still arrive and their
 * thread stays reachable from the CREW page's HIDDEN section; they just
 * raise no notification and appear on no face.
 *
 * ## Why a separate list, rather than a flag on `ff_crew_member_t`
 *
 * The exact precedent `ff_heard.h` set, for the same reason: `ff_crew_t`
 * is under a DRAM budget (`firmware/tools/check_dram_budget.py`) and
 * should not grow for state that is not per-member. It is also not
 * per-member state in the first place — a hidden id is deliberately NOT
 * in the roster (hide is implemented as *unpair* + *remember*, which is
 * how hiding frees a roster slot for the ninth person, amendment §C/§E),
 * so there would be no member to hang the flag on.
 *
 * ## No LRU here — unlike `ff_heard_t`
 *
 * `ff_heard_t` evicts its least-recently-heard entry because its entries
 * are observations and the newest ones matter most. A hide is a **user
 * decision**. Silently forgetting one would put somebody back on the
 * wearer's radar without being asked, which is precisely the kind of
 * quiet, confident wrongness this project refuses to ship. A full list
 * therefore fails honestly (`ff_hidden_add` returns false) and the CREW
 * page says so in words: *"You've hidden as many people as your puck can
 * remember (16). Unhide someone first."*
 *
 * Pure C11, zero dependencies, no I/O, no allocation. Persistence is the
 * caller's job through the `ff_store_t` seam — see
 * `ff_hidden_serialize`/`ff_hidden_deserialize` and `ff_hidden_key`.
 */
#ifndef FF_HIDDEN_H
#define FF_HIDDEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff_crewcode.h" /* FF_CREWCODE_SYMBOLS — see ff_hidden_key */

#ifdef __cplusplus
extern "C" {
#endif

/** How many people this puck can remember hiding. Sized to twice
 * `FF_CREW_MAX` and matching `FF_HEARD_MAX`'s 16: enough that a crew of
 * eight can hide a whole second crew's worth of strangers, small enough
 * that the NVS record stays a single 68-byte blob. */
#define FF_HIDDEN_MAX 16u

typedef struct {
    uint32_t ids[FF_HIDDEN_MAX];
    uint8_t  count;
} ff_hidden_t;

/** Serialized size of an `ff_hidden_t`: a 4-byte header (2-byte magic,
 * 1-byte version, 1-byte count) followed by a FIXED `FF_HIDDEN_MAX`
 * little-endian ids, zero-filled past the count. See
 * `ff_hidden_serialize` for why the length is fixed and the byte order
 * explicit. */
#define FF_HIDDEN_BLOB_LEN (4u + 4u * FF_HIDDEN_MAX)

/** Longest store key `ff_hidden_key` produces, including the NUL.
 * `"ff.hid." + 6 symbols` = 13 chars — inside NVS's 15-character key
 * limit (see `targets/esp32s3/main/ff_nvs_store.c`), with room to spare. */
#define FF_HIDDEN_KEY_MAX 16u

/** Reset to empty. NULL-safe. */
void ff_hidden_init(ff_hidden_t *h);

/**
 * Add `node_id` to the set.
 *
 * Returns true if the id is in the set afterwards — including the
 * already-present case, which is a no-op success rather than an error
 * (hiding someone twice is not a failure, and a UI that reported one
 * would be inventing a problem). Returns **false** only when the set is
 * genuinely full and `node_id` is not already in it, and when `h` is
 * NULL or `node_id` is 0 (0 is never a valid Meshtastic node id — the
 * wire protocol reserves it as "unset").
 *
 * Never evicts. See the header comment on why.
 */
bool ff_hidden_add(ff_hidden_t *h, uint32_t node_id);

/** Remove `node_id`. Returns true iff it was actually present. */
bool ff_hidden_remove(ff_hidden_t *h, uint32_t node_id);

/** True iff `node_id` is hidden. NULL-safe (false). */
bool ff_hidden_contains(ff_hidden_t const *h, uint32_t node_id);

/** How many ids are hidden. NULL-safe (0). */
uint8_t ff_hidden_count(ff_hidden_t const *h);

/** The `idx`-th hidden id in insertion order, or 0 when out of range.
 * Insertion order, not sorted: the CREW page's HIDDEN section lists
 * people in the order the wearer hid them, which is the order they will
 * be looking for them in. */
uint32_t ff_hidden_at(ff_hidden_t const *h, uint8_t idx);

/**
 * ff_hidden_key — the `ff_store_t` key this crew's hide list lives under.
 *
 * **Keyed by crew code**, so leaving a crew and rejoining it restores the
 * hides you had (amendment §C) rather than silently un-hiding everyone.
 * The key is `"ff.hid."` + the code's six symbols — the `FIRE-` tag is
 * dropped because it is constant across every Firefly crew and NVS keys
 * are capped at 15 characters, and the six symbols alone are the whole
 * of the code's entropy, so two distinct crews can never collide on a
 * key.
 *
 * Returns false (and writes "" when it can) for a NULL/short buffer or a
 * `canonical` that is not a valid crew code — a puck that is not on a
 * crew channel has no hide list to load, which is a different thing from
 * an empty one and must not be papered over with a fallback key.
 */
bool ff_hidden_key(char const *canonical, char *buf, size_t n);

/**
 * ff_hidden_serialize — write `h` into `buf` as exactly
 * `FF_HIDDEN_BLOB_LEN` bytes.
 *
 * Fixed length regardless of `count` (bytes past the count are zero), so
 * the record never has to be resized in NVS and a short read is
 * unambiguously corruption rather than an older, smaller, still-valid
 * write. Ids are written little-endian explicitly rather than
 * `memcpy`ing the struct: the sim writes this file on a desktop and the
 * puck writes it on an ESP32, and a fixture blob has to mean the same
 * thing in both places.
 *
 * Returns the number of bytes written, or 0 on a NULL/short buffer.
 */
size_t ff_hidden_serialize(ff_hidden_t const *h, uint8_t *buf, size_t n);

/**
 * ff_hidden_deserialize — read a blob written by `ff_hidden_serialize`.
 *
 * Returns false, leaving `h` **initialised empty**, for anything that is
 * not exactly a well-formed current-version blob: wrong length, wrong
 * magic, unknown version, or a `count` above `FF_HIDDEN_MAX`. Reject and
 * start empty, never partially trust — a half-read hide list would
 * un-hide an arbitrary subset of people the wearer deliberately hid, and
 * they would have no way to tell that had happened.
 */
bool ff_hidden_deserialize(ff_hidden_t *h, uint8_t const *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* FF_HIDDEN_H */
