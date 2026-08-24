/**
 * ff_heard.h — core/heard: bounded, LRU-evictable "heard but unpaired"
 * node list.
 *
 * Spec: docs/specs/S08-signals-t9.md's Amendments (S08 PR #25 code review,
 * MEDIUM finding — roster exhaustion from untrusted RF), which itself
 * points back to S04's stated v1 pairing model ("add from heard nodes").
 *
 * ## Why this exists, separate from `core/include/ff_crew.h`
 * `ff_crew_t`'s paired roster and "which nodes has this puck recently
 * heard on the mesh" are two different things that `app/ff_wiring.c`
 * used to conflate: it called `ff_crew_upsert` (find-**or-create**) for
 * every inbound packet, including ones from senders that turned out to
 * be unpaired and got dropped. `ff_crew_t` documents "no eviction in
 * v1" for its fixed `FF_CREW_MAX` (8) slots — so eight packets from
 * eight distinct, never-before-heard node ids permanently occupied
 * every roster slot before any of them were ever paired, silently
 * blocking any REAL crew member from being pairable for the rest of the
 * session. At a festival with thousands of Meshtastic nodes in range,
 * this needs no malice at all, just normal RF noise.
 *
 * The fix: the paired roster is user-chosen, protected, and never
 * writable by inbound radio traffic (see `ff_crew_find`'s doc comment in
 * ff_crew.h — a pure read-only lookup, no create-on-miss). Heard-but-
 * unpaired nodes go here instead: a SEPARATE, bounded, LRU-evictable
 * list whose only job is populating the "add from heard nodes" pairing
 * UI. Nothing here ever promotes a node into the crew roster — only an
 * explicit user pairing action (calling `ff_crew_upsert`/
 * `ff_crew_set_paired` from the Settings/pairing screen, not yet built)
 * does that.
 *
 * Placed in core/, alongside ff_crew, rather than app/: which nodes have
 * been heard is domain state (the same category of fact as "who is my
 * crew"), not a UI concern — `app/ff_wiring.c` is a consumer of this
 * list (it's what populates it from decoded radio events), not its
 * owner.
 *
 * Pure C11, no I/O, zero heap allocation — `ff_heard_t` is a plain
 * struct (fixed-size array only) safe to put on the stack or in a
 * static. Zero-initialize or call `ff_heard_init` before use.
 */
#ifndef FF_HEARD_H
#define FF_HEARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max distinct heard-but-unpaired node ids tracked at once. Generous
 * headroom over FF_CREW_MAX (8) since this list's whole purpose is
 * surfacing MORE candidates than the paired roster could ever hold, but
 * still fixed/bounded — unlike the paired roster, this list is allowed
 * (expected) to evict under real festival RF volume; that's the point. */
#define FF_HEARD_MAX 16

typedef struct {
    uint32_t node_id;
    uint32_t last_heard_ms; /* absolute clock timestamp of the most recent sighting */
} ff_heard_entry_t;

/**
 * ff_heard_t — the whole heard-node list. Fully-defined (not opaque),
 * same "callers/tests can put it on the stack, inspect fields directly"
 * convention as `ff_crew_t`/`ff_feed_t`.
 */
typedef struct {
    ff_heard_entry_t entries[FF_HEARD_MAX];
    uint8_t          count;
} ff_heard_t;

/** ff_heard_init — clear a heard-list to empty. */
void ff_heard_init(ff_heard_t *h);

/**
 * ff_heard_note — record (or refresh) a sighting of `node_id` at
 * `now_ms`.
 *
 * An already-tracked id just gets its `last_heard_ms` updated in place
 * (it does not need to "move" anywhere — eviction below reads
 * timestamps directly, not physical position, so an updated timestamp
 * alone is sufficient to protect a recently-reheard id from eviction).
 *
 * A brand-new id, with the list not yet full, appends normally.
 *
 * A brand-new id, with the list already at `FF_HEARD_MAX`, evicts the
 * entry with the SMALLEST `last_heard_ms` (least-recently-heard) and
 * overwrites it — true LRU eviction, per the S08 PR #25 review ruling
 * ("bounded, LRU-evictable... populating the add-from-heard-nodes
 * pairing UI"). No-op if `h` is NULL.
 */
void ff_heard_note(ff_heard_t *h, uint32_t node_id, uint32_t now_ms);

/**
 * ff_heard_remove — forget `node_id` entirely. Returns true iff it was
 * tracked. No-op (false) if `h` is NULL or the id is not tracked.
 *
 * Added for S16 slice b2 (PR #46 review caveat): a puck can note its OWN
 * id here if its NodeInfo arrives before `on_my_info` names it —
 * whichever order the radio picks — and without removal that entry
 * lingers until LRU eviction, so S12's "add from heard nodes" pairing UI
 * would offer the user their own puck. The shell purges its own id the
 * moment `on_my_info` lands. This is the only removal path: heard
 * entries otherwise only ever age out via LRU eviction, by design.
 */
bool ff_heard_remove(ff_heard_t *h, uint32_t node_id);

/** ff_heard_count — number of distinct node ids currently tracked, 0..FF_HEARD_MAX. */
uint8_t ff_heard_count(ff_heard_t const *h);

/**
 * ff_heard_at — the `idx`-th tracked entry (insertion-slot order, NOT
 * sorted by recency — a caller building a "most recently heard first"
 * UI list should sort by `last_heard_ms` itself; this accessor makes no
 * ordering promise beyond "every currently-tracked id appears exactly
 * once across `0..ff_heard_count(h)`"). NULL if `h` is NULL or
 * `idx >= ff_heard_count(h)`.
 */
ff_heard_entry_t const *ff_heard_at(ff_heard_t const *h, uint8_t idx);

/** ff_heard_contains — true iff `node_id` is currently tracked. */
bool ff_heard_contains(ff_heard_t const *h, uint32_t node_id);

#ifdef __cplusplus
}
#endif

#endif /* FF_HEARD_H */
