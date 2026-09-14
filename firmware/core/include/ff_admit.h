/**
 * ff_admit.h — core/admit: the crew-admission rule, as a pure function.
 *
 * Spec: docs/specs/S02-core-crew.md's 2026-09-13 amendment §B (the puck
 * half of docs/specs/A02-crew-join.md §4.1/§4.2).
 *
 * ## The rule, in one sentence
 *
 * > A node becomes crew when this radio delivers us a packet it
 * > decrypted with our crew channel's key, from an id that is not us and
 * > not hidden, not via MQTT, on one of four portnums.
 *
 * ## Why this is a policy change, not a flag flip
 *
 * `ff_shell.h`'s ROSTER TRUST POLICY says the paired roster never grows
 * from anything the radio says. That policy was written when membership
 * had no other definition. A02 gives it one: **possession of the crew
 * channel's key is membership.** A node the radio decrypted on our crew
 * channel has proved it holds a 32-byte key that only came from someone
 * who had the code — a strictly stronger claim than the old policy's
 * "the radio said so", and the claim the roster may now grow on. The
 * policy sentence is amended, not deleted: *the roster grows from an
 * explicit user action, or from proof of the crew key — and from nothing
 * else.*
 *
 * ## Why it lives in core
 *
 * Because it is a domain decision with six independent clauses, each of
 * which is a way to silently admit a stranger if it regresses. As a pure
 * function over an explicit input struct it is exhaustively testable
 * clause by clause (`S02_AC11_*`), and — because it returns a REASON
 * rather than a bool — a test can prove *which* clause rejected a
 * packet, not merely that something did. That distinction is the whole
 * anti-proxy point: a rule that rejects the MQTT case for the wrong
 * reason passes a bool-only test and ships the bug.
 *
 * Pure C11, zero dependencies, no I/O, no state.
 */
#ifndef FF_ADMIT_H
#define FF_ADMIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The four portnums that may admit a sender (amendment §B clause 6).
 *
 * 269 is Firefly's own (`ff_proto.h`'s FF_PORTNUM) and is NOT
 * `PRIVATE_APP`: in portnums.proto `PRIVATE_APP = 256` and
 * `ATAK_FORWARDER = 257`; 269 is simply a value Firefly picked inside
 * the documented private range 256-511 and **must be matched by raw
 * value**. Matching the `PRIVATE_APP` case instead would silently admit
 * nobody. */
#define FF_ADMIT_PORTNUM_TEXT     1u  /* TEXT_MESSAGE_APP */
#define FF_ADMIT_PORTNUM_POSITION 3u  /* POSITION_APP */
#define FF_ADMIT_PORTNUM_NODEINFO 4u  /* NODEINFO_APP */
#define FF_ADMIT_PORTNUM_FIREFLY  269u

/**
 * Why a packet did or did not admit its sender.
 *
 * Ordered so that the FIRST failing clause is what gets reported, in the
 * amendment's own clause order, which makes a test's expectation a
 * statement about the rule rather than about this function's internal
 * short-circuiting.
 */
typedef enum {
    FF_ADMIT_YES = 0,          /* every clause passed — admit the sender */
    FF_ADMIT_NO_DISABLED,      /* FF_CREW_AUTO_ON_CHANNEL=n / auto-crew off */
    FF_ADMIT_NO_ENCRYPTED,     /* clause 1 — not delivered decrypted */
    FF_ADMIT_NO_NO_CREW_CHANNEL, /* clause 2 — we have not resolved a crew index */
    FF_ADMIT_NO_OTHER_CHANNEL, /* clause 2 — a different (or absent) channel index */
    FF_ADMIT_NO_SELF,          /* clause 3 — from 0, or from our own id */
    FF_ADMIT_NO_HIDDEN,        /* clause 4 — the wearer hid this id */
    FF_ADMIT_NO_VIA_MQTT,      /* clause 5 — arrived over the internet */
    FF_ADMIT_NO_PORTNUM,       /* clause 6 — telemetry, routing, anything else */
} ff_admit_result_t;

/**
 * Everything the rule needs, stated explicitly.
 *
 * Every optional fact is presence-flagged rather than sentinel-coded,
 * the same discipline `mc_rx_meta_t` uses: absent must never read as 0,
 * because on this rule 0 is a *meaningful* channel index (Firefly writes
 * the crew channel at the primary slot) and reading "we don't know"
 * as "index 0" is exactly how a stranger on the public channel gets
 * admitted.
 */
typedef struct {
    /** The shipped default is on (`CONFIG_FF_CREW_AUTO_ON_CHANNEL`,
     * default y). False must admit NOBODY, by any route. */
    bool auto_crew_enabled;

    /** Clause 1. True iff the radio handed us a DECRYPTED payload. A
     * packet the radio could not decrypt never reaches a client with a
     * payload at all, so in practice this is a property of the delivery
     * rather than a check we perform — but it is the load-bearing one
     * (possession of the PSK is what membership means), so it is stated
     * rather than assumed. */
    bool decrypted;

    /** Clause 2, our half: have we actually resolved which index the
     * crew channel occupies on THIS radio (by name-and-PSK match against
     * the channel table)? With no match there is no crew channel, and
     * nothing is ever admitted — never a fallback to 0. */
    bool     crew_index_known;
    uint32_t crew_index;

    /** Clause 2, the packet's half. `has_channel_index` is false for a
     * packet whose `channel` field is not a channel-table index (an
     * encrypted-variant packet, where the field carries the channel
     * HASH; or a PKI-encrypted DM, which proves possession of a
     * key-pair, not of the crew key). */
    bool     has_channel_index;
    uint32_t channel_index;

    /** Clause 3. `has_my_node_id` false means the handshake has not told
     * us who we are yet — in which case we cannot prove a packet is not
     * our own echo, and admitting on it would create a roster slot for
     * ourselves. */
    uint32_t from;
    bool     has_my_node_id;
    uint32_t my_node_id;

    /** Clause 4 — `ff_hidden_contains(&hidden, from)`, resolved by the
     * caller so this module needs no include of the hide set. */
    bool hidden;

    /** Clause 5 — `MeshPacket.via_mqtt`. A crew is people who are here;
     * an MQTT path can replay. */
    bool via_mqtt;

    /** Clause 6. `has_portnum` is false for a packet we never decoded a
     * payload for. */
    bool     has_portnum;
    uint32_t portnum;
} ff_admit_in_t;

/**
 * ff_admit_portnum_ok — clause 6 alone, exported because the shell's
 * debug surface and the tests both want to ask it directly and a second
 * copy of the four constants is exactly the drift this avoids.
 */
bool ff_admit_portnum_ok(uint32_t portnum);

/**
 * ff_admit — evaluate the rule. Returns `FF_ADMIT_YES` only when every
 * clause passes; otherwise the first clause that failed, in the
 * amendment's order.
 *
 * A NULL input returns `FF_ADMIT_NO_DISABLED` — there is no packet, so
 * there is nothing to admit, and the least-claiming answer is the right
 * one.
 *
 * This function decides membership and NOTHING else. It never implies a
 * position, a name, a time or a freshness: the caller records membership
 * only (amendment §F), and a member admitted before their NodeInfo
 * arrives renders as "NEW CREW MEMBER" with a `NAME?` chip until a real
 * name turns up.
 */
ff_admit_result_t ff_admit(ff_admit_in_t const *in);

/** A short, stable, lower-case tag for a result — for debug/ctl output
 * and test failure messages. Never rendered on a face. */
char const *ff_admit_reason(ff_admit_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* FF_ADMIT_H */
