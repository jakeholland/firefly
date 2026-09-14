/**
 * ff_crewstart.h — core/crewstart: the puck-initiated crew, as a pure
 * state machine.
 *
 * Spec: `docs/specs/S02-core-crew.md`'s **2026-09-14 amendment, slice D2
 * — puck-initiated crew**, which owns the puck half of
 * `docs/specs/A02-crew-join.md` §2 (Start a crew) and §3.3 (Leave).
 *
 * ## Why this exists at all
 *
 * A02 §2 assumes the organiser holds a phone: it mints the code there,
 * writes the channel over BLE, and the puck is a passive display. The
 * festival topology is asymmetric — the wearer carries a puck with no
 * camera and a T9 keyboard, and the phone is somebody else's. So the
 * puck has to be able to create the crew itself: mint a code, write the
 * derived channel to its own comms brain over the SAME admin path the
 * owner-name push already uses, verify the write actually landed, and
 * then show the code on slice D's SHOW CODE face so phones can scan it.
 *
 * ## What is pure, and what is not
 *
 * Everything about SEQUENCING that write is here: which state we are in,
 * what the caller should do next, what counts as proof, when to give up.
 * Nothing here touches a radio, a store, a clock or an RNG —
 * `ff_shell.c` performs each requested action and reports the result
 * back in. That split is what makes the whole sequence testable against
 * a scripted event stub (`core/tests/test_crewstart.c`) rather than only
 * against a bench.
 *
 * The entropy source is likewise injected (`ff_crewstart_rand_fn`): on
 * the puck it is `esp_random`, in the sim `/dev/urandom`, in a test a
 * scripted counter. A core module that reached for `rand()` here would
 * mint a guessable crew code, and A02 §1.6's threat model — 30 bits is a
 * privacy fence — does not survive a predictable 30 bits.
 *
 * ## The honest-failure rule this module exists to enforce
 *
 * A channel write is not "done" when the radio accepts the frame. It is
 * done when the radio, re-read, reports the channel we asked for. Every
 * intermediate state is therefore named and reportable, and every
 * failure carries WHY (`ff_crewstart_fail_t`) — the face says
 * WRITING -> VERIFYING -> READY/FAILED with the reason, never a
 * cheerful "done" over a radio that quietly refused. This is the same
 * rule the NAME push already follows (`ff_shell.h`'s
 * `ff_shell_mesh_name_status_t`: "the shell never assumes a push
 * succeeded merely because it was accepted for send").
 *
 * Pure C11, zero dependencies, no I/O, no allocation.
 */
#ifndef FF_CREWSTART_H
#define FF_CREWSTART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff_crewcode.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Channel-name buffer: Meshtastic's own "Less than 12 bytes" budget
 *  (channel.proto), NUL-terminated. Matches `mc_channel_t.name` exactly
 *  so the shell's translation between the two is a plain copy. */
#define FF_CREWSTART_NAME_MAX 12u

/** Pre-shared-key buffer — AES256 is the largest Meshtastic carries. */
#define FF_CREWSTART_PSK_MAX 32u

/** The crew channel always lands on the primary slot (A02 §4.2). */
#define FF_CREWSTART_CREW_INDEX 0u

/** A02 §1.5: the crew exists so people can find each other, so the
 *  crew channel's `position_precision` is 32, always explicitly
 *  present — never left absent and never the import path's cautious 0. */
#define FF_CREWSTART_CREW_PRECISION 32u

/**
 * Not a toggle — the shipped policy `ff_crewstart_on_channel` enforces
 * unconditionally, named so review can see it is deliberate rather than
 * a magic `true` buried in a comparison: a read-back row that STATES a
 * `position_precision` for the crew channel must state exactly
 * `FF_CREWSTART_CREW_PRECISION`. #47's hazard is a radio that accepts
 * the channel write but silently keeps positions coarse — the crew sees
 * km-scale positions while the puck's own face says READY — and a
 * verify step that checked name and key but not this is a real gap in
 * the claim READY makes (found in PR #312's review, not turned on there
 * for lack of a bench that could tell "never echoed" from "echoed and
 * wrong"; the 2026-09-14 bench round settled it — see
 * `ff_crewstart_begin_start`'s `precision_strict` parameter for the one
 * part of this rule that a real radio's silence still leaves open). */
#define FF_CREWSTART_PRECISION_REQUIRED true

/**
 * How long a write may sit unacknowledged before it is retried
 * (`FF_CREWSTART_WRITING`).
 *
 * An admin write to our OWN comms brain is a one-hop, wired hand-off:
 * the packet never leaves the UART, and `AdminModule` replies from the
 * same tick. 12 s is therefore generous by an order of magnitude, and
 * deliberately so — the cost of waiting slightly too long is a slower
 * failure message, while the cost of waiting too little is a SECOND
 * channel write racing the first one's reboot.
 */
#define FF_CREWSTART_ACK_TIMEOUT_MS 12000u

/**
 * How long the read-back may take before the write is declared unproven
 * (`FF_CREWSTART_VERIFYING`).
 *
 * Meshtastic's `AdminModule` schedules a REBOOT a few seconds after a
 * channel write lands (`saveChanges()` -> `rebootAtMsec()`), so the
 * verifying read-back is gated on the comms brain coming back and
 * replaying its `want_config` — seconds of radio downtime, then a fresh
 * handshake. 45 s covers a slow boot plus a re-handshake with headroom;
 * past that, "I cannot prove this worked" is the honest answer and the
 * face says so rather than spinning forever.
 */
#define FF_CREWSTART_VERIFY_TIMEOUT_MS 45000u

/**
 * Bounded retry. Three attempts total (the first plus two retries) for
 * the WRITE leg only.
 *
 * VERIFICATION is deliberately NOT retried in this machine: a read-back
 * that timed out has already burned a reboot cycle, and re-writing a
 * channel the radio may or may not already hold is how a puck ends up
 * oscillating between two configurations unattended. A wearer who wants
 * another go presses the button again, which is a fresh, consented
 * attempt rather than an unattended loop.
 */
#define FF_CREWSTART_MAX_ATTEMPTS 3u

/** Store key the pre-crew snapshot lives under (see
 *  `ff_crewstart_snapshot_serialize`). One key, not per-crew: there is
 *  exactly one "what this radio looked like before Firefly touched it". */
#define FF_CREWSTART_SNAPSHOT_KEY "ff.crewpre"

/** Serialized snapshot length, fixed — see
 *  `ff_crewstart_snapshot_serialize` for the layout. */
#define FF_CREWSTART_SNAPSHOT_BLOB_LEN 52u

/**
 * The channel this machine wants written, and expects to read back.
 *
 * Deliberately NOT `mc_channel_t`: core cannot include meshclient
 * headers (docs/ARCHITECTURE.md's layering), and the shell — one of the
 * two files allowed to see both — translates. The fields are the same
 * facts in the same units, so that translation is a copy, not a
 * reinterpretation.
 */
typedef struct {
    uint8_t  index;                     /* channel-table slot, 0 = primary */
    char     name[FF_CREWSTART_NAME_MAX]; /* NUL-terminated; "" is Meshtastic's own default channel */
    uint8_t  psk[FF_CREWSTART_PSK_MAX];
    uint8_t  psk_len;                   /* 0, 1, 16 or 32 — 1 is Meshtastic's well-known-key shorthand */
    bool     is_primary;
    uint32_t position_precision;        /* always written explicitly, A02 §1.5 */
    /* PRESENCE-FLAGGED, mirroring `mc_channel_t.has_position_precision`
     * byte for byte (the shell's translation between the two is a plain
     * copy — see `shell_channel_to_core`). Meaningful only on a READ:
     * `ff_crewstart_take_action`'s WRITE always carries an explicit
     * `position_precision` (A02 §1.5's "saying it out loud is free"), so
     * `desired`'s own flag is never consulted for that leg. On the
     * VERIFYING read-back it is the whole reason absent and "stated as
     * 0" are not the same failure — see `ff_crewstart_on_channel`. */
    bool     has_position_precision;
} ff_crewstart_channel_t;

/** Which operation is in flight. */
typedef enum {
    FF_CREWSTART_OP_NONE = 0,
    FF_CREWSTART_OP_START, /* mint a code and put this radio on that crew */
    FF_CREWSTART_OP_LEAVE, /* put this radio back the way it was found */
} ff_crewstart_op_t;

/**
 * The face's own vocabulary, in order. GENERATING is a real state and
 * not an implementation detail: it is where a missing entropy source
 * fails, and a wearer who sees it stuck there learns something true.
 */
typedef enum {
    FF_CREWSTART_IDLE = 0,
    FF_CREWSTART_GENERATING,
    FF_CREWSTART_WRITING,
    FF_CREWSTART_VERIFYING,
    FF_CREWSTART_READY,
    FF_CREWSTART_FAILED,
} ff_crewstart_state_t;

/**
 * Why it failed. Every value here is something the face can say in
 * plain words, because "it didn't work" is not a report.
 */
typedef enum {
    FF_CREWSTART_FAIL_NONE = 0,
    FF_CREWSTART_FAIL_NO_LINK,        /* the comms brain is not connected */
    FF_CREWSTART_FAIL_REGION_UNSET,   /* A02 §1.7 — never guess a radio region */
    FF_CREWSTART_FAIL_NO_ENTROPY,     /* no CSPRNG wired up; a guessable code is not offered */
    FF_CREWSTART_FAIL_NO_SNAPSHOT,    /* LEAVE with nothing recorded to restore */
    FF_CREWSTART_FAIL_SEND,           /* the radio would not even accept the frame */
    FF_CREWSTART_FAIL_NAK,            /* the mesh reported this specific write failed */
    FF_CREWSTART_FAIL_TIMEOUT_ACK,    /* no ACK/NAK at all, retries exhausted */
    FF_CREWSTART_FAIL_TIMEOUT_VERIFY, /* the radio never came back to be read */
    FF_CREWSTART_FAIL_MISMATCH,       /* it came back holding something else */
} ff_crewstart_fail_t;

/**
 * What the caller should DO next. Read once per tick with
 * `ff_crewstart_take_action`, which clears it — an action is a
 * one-shot request, never a level that a slow caller performs twice.
 */
typedef enum {
    FF_CREWSTART_ACT_NONE = 0,
    FF_CREWSTART_ACT_WRITE,  /* send the admin channel write in `out` */
    FF_CREWSTART_ACT_REREAD, /* re-run want_config so the channel table comes back */
} ff_crewstart_action_t;

/**
 * ff_crewstart_rand_fn — 32 bits from the caller's CSPRNG. `ctx` is
 * passed back untouched. See this header's top comment for why the
 * source is injected rather than chosen here.
 */
typedef uint32_t (*ff_crewstart_rand_fn)(void *ctx);

/**
 * The machine. A plain struct the caller owns; `ff_crewstart_init`
 * zeroes it. Every field is readable, but only through the accessors
 * below in code that is not this module's own test.
 */
typedef struct {
    ff_crewstart_state_t   state;
    ff_crewstart_op_t      op;
    ff_crewstart_fail_t    fail;
    ff_crewstart_action_t  pending;

    /* The channel being written, and the one the read-back must match. */
    ff_crewstart_channel_t desired;

    /* The minted code, "" for LEAVE (there is no code to show after
     * leaving, and inventing one would be the exact fabrication this
     * face exists to avoid). */
    char                   code[FF_CREWCODE_LEN + 1u];

    bool                   has_packet_id;
    uint32_t               packet_id;

    uint32_t               state_since_ms;
    uint8_t                attempts; /* write attempts started, incl. the first */

    /* Whether an ABSENT `position_precision` on the read-back is trusted
     * (READY, reported "unreported") or treated as the same failure as a
     * stated-but-wrong one (MISMATCH). Set once, at `begin_start`, from
     * the caller's `precision_strict` argument — see that function's own
     * doc comment. Meaningless for LEAVE, which is under no obligation
     * to restore precision 32. */
    bool                   precision_strict;

    /* The read-back's OWN `position_precision`, presence-flagged exactly
     * like `mc_channel_t`'s — captured the moment `ff_crewstart_on_channel`
     * reaches READY, for whichever op. `has_precision == false` after a
     * READY means the radio proved it holds the right name and key but
     * never stated a precision (only reachable for LEAVE, or for START
     * with `precision_strict == false`) — read through
     * `ff_crewstart_has_precision`/`ff_crewstart_precision`, never these
     * fields directly, matching this struct's own top comment. */
    bool                   has_precision;
    uint32_t               precision;
} ff_crewstart_t;

/** Zero the machine into `FF_CREWSTART_IDLE`. NULL-safe (no-op). */
void ff_crewstart_init(ff_crewstart_t *f);

/**
 * ff_crewstart_begin_start — mint a code and begin putting this radio on
 * that crew.
 *
 * Draws `FF_CREWCODE_BITS` from `rng` (masked down from the 32 it
 * returns — the mask is applied HERE, where the draw is known to be a
 * raw 32-bit word, rather than in `ff_crewcode_from_bits`, which
 * deliberately rejects out-of-range input), renders the canonical code,
 * derives the PSK with `ff_crewcode_psk`, and builds the channel A02
 * §1.5 specifies: name = the code, index 0, PRIMARY, precision 32. LoRa
 * config — region, modem preset — is not part of this struct and is
 * never written (A02 §1.7).
 *
 * The state on return is `FF_CREWSTART_GENERATING`; the next
 * `ff_crewstart_tick` moves it to `FF_CREWSTART_WRITING` and the
 * following `ff_crewstart_take_action` hands back the channel to
 * write.
 *
 * `precision_strict` (`FF_CREW_PRECISION_STRICT`, Kconfig default y,
 * injected — core has no Kconfig of its own, see `ff_shell.c`'s
 * `sh->crew_precision_strict`) decides ONE thing: what the verifying
 * read-back means when it proves the right name and key but states NO
 * `position_precision` at all. `FF_CREWSTART_PRECISION_REQUIRED` (not
 * configurable) already fails a STATED-but-wrong precision every time;
 * an ABSENT one is the case a bench had to settle, because "never
 * echoed" and "echoed and wrong" look identical from here. The
 * 2026-09-14 bench round (a Heltec V3 on Meshtastic 2.7.x) proved a real
 * radio DOES echo `module_settings.position_precision` back after an
 * import that set it, so `true` reads an absence as the same #47 hazard
 * a wrong value is; `false` reports it honestly as "unreported" and
 * still lands READY. Meaningless for `ff_crewstart_begin_leave` — LEAVE
 * restores whatever the radio held before Firefly ever wrote a channel,
 * which is under no obligation to be precision 32.
 *
 * Returns false (and lands in `FF_CREWSTART_FAILED` with
 * `FF_CREWSTART_FAIL_NO_ENTROPY`) if `rng` is NULL. Returns false
 * without touching the machine if `f` is NULL or an operation is
 * already in flight — a second press while a write is in the air is
 * ignored, not queued.
 */
bool ff_crewstart_begin_start(ff_crewstart_t *f, ff_crewstart_rand_fn rng, void *rng_ctx, bool precision_strict,
                               uint32_t now_ms);

/**
 * ff_crewstart_begin_leave — begin putting this radio back the way it
 * was found.
 *
 * `restore` is the pre-crew snapshot taken before the crew was ever
 * written (`ff_crewstart_snapshot_deserialize`). Pass NULL when there is
 * none: the machine then fails honestly with
 * `FF_CREWSTART_FAIL_NO_SNAPSHOT` rather than inventing a configuration
 * for somebody's radio. (`ff_crewstart_default_primary` exists for the
 * caller that genuinely wants the stock default and says so.)
 *
 * Same "already in flight is ignored" rule as `begin_start`.
 */
bool ff_crewstart_begin_leave(ff_crewstart_t *f, ff_crewstart_channel_t const *restore, uint32_t now_ms);

/**
 * ff_crewstart_default_primary — Meshtastic's stock primary channel:
 * empty name, the 1-byte `\x01` well-known-key shorthand, precision 0,
 * PRIMARY, index 0.
 *
 * This is what a radio that has never been provisioned holds, and it is
 * what LEAVE restores when the caller explicitly chooses to restore a
 * default rather than a snapshot. It is a DEFAULT, not a guess about the
 * radio's history — which is why it is a separate, named call the caller
 * has to make on purpose, instead of a silent fallback inside
 * `begin_leave`.
 */
void ff_crewstart_default_primary(ff_crewstart_channel_t *out);

/**
 * ff_crewstart_consider — record which operation the caller is ASKING
 * about, without starting it, and clear any finished run's result.
 *
 * This is what a confirm face needs: the wearer has pressed START CREW
 * or LEAVE CREW, the face has to know which question to ask, and
 * nothing must have reached the radio yet. It is a separate call rather
 * than a field a UI layer pokes, so "which operation is being
 * considered" and "an operation is running" cannot drift apart.
 *
 * A no-op while a run is in flight — a face cannot re-label a write
 * that is already out.
 */
void ff_crewstart_consider(ff_crewstart_t *f, ff_crewstart_op_t op);

/**
 * ff_crewstart_fail_now — stop before any write, for a precondition the
 * caller checked and this module cannot see: no link, region UNSET.
 *
 * Only meaningful from `FF_CREWSTART_GENERATING` (the window between
 * `begin_*` and the first action) or `FF_CREWSTART_IDLE`; from a state
 * where a write is already in the air it is a no-op, because a frame
 * that is out cannot be un-sent and pretending otherwise would leave the
 * face reporting a state the radio does not share.
 */
void ff_crewstart_fail_now(ff_crewstart_t *f, ff_crewstart_op_t op, ff_crewstart_fail_t why, uint32_t now_ms);

/**
 * ff_crewstart_take_action — what to do next, exactly once.
 *
 * Returns the pending action and clears it. `out` receives the channel
 * to write when the action is `FF_CREWSTART_ACT_WRITE` (and is left
 * untouched otherwise, so a caller that reads it after a NONE cannot
 * mistake a stale buffer for a request). NULL-safe: returns
 * `FF_CREWSTART_ACT_NONE`.
 */
ff_crewstart_action_t ff_crewstart_take_action(ff_crewstart_t *f, ff_crewstart_channel_t *out);

/**
 * ff_crewstart_on_write_result — the radio's answer to the WRITE action:
 * did it accept the frame for sending, and under which packet id.
 *
 * `accepted == false` retries while attempts remain, and otherwise fails
 * with `FF_CREWSTART_FAIL_SEND`. `accepted == true` with no packet id
 * (`has_packet_id == false`) is accepted and moves straight to
 * VERIFYING: without an id there is no routing reply to correlate, so
 * the read-back becomes the ONLY proof — which it already was anyway.
 */
void ff_crewstart_on_write_result(ff_crewstart_t *f, bool accepted, bool has_packet_id, uint32_t packet_id,
                                   uint32_t now_ms);

/**
 * ff_crewstart_on_routing_ack — a ROUTING_APP reply. Ignored unless
 * `request_id` matches THIS write's packet id (a caller that cannot
 * match should pass the event through anyway; matching is this module's
 * job, not the caller's).
 *
 * ACK moves to VERIFYING and asks for the read-back. NAK retries while
 * attempts remain, then fails with `FF_CREWSTART_FAIL_NAK`.
 */
void ff_crewstart_on_routing_ack(ff_crewstart_t *f, uint32_t request_id, bool ok, uint32_t now_ms);

/**
 * ff_crewstart_on_channel — one row of the radio's channel table, from
 * the verifying read-back.
 *
 * Only rows at the target index are considered. A matching row (name AND
 * psk, byte for byte) is the proof, and moves to `FF_CREWSTART_READY`. A
 * row at the target index that does NOT match is a decisive failure
 * (`FF_CREWSTART_FAIL_MISMATCH`), not a reason to keep waiting: the
 * radio has answered the question and the answer was no.
 *
 * For a START, "match" also covers `position_precision` (#47's hazard —
 * see `FF_CREWSTART_PRECISION_REQUIRED`/`precision_strict`): a row that
 * states a precision other than `FF_CREWSTART_CREW_PRECISION` is always
 * a MISMATCH, and a row that states none at all is a MISMATCH too
 * unless this run's `precision_strict` says otherwise. LEAVE never
 * applies this — it restores whatever the radio held before, which is
 * under no obligation to be precision 32.
 *
 * Rows arriving in any other state are ignored, so the ordinary
 * every-handshake channel traffic costs nothing here.
 */
void ff_crewstart_on_channel(ff_crewstart_t *f, ff_crewstart_channel_t const *ch, uint32_t now_ms);

/**
 * ff_crewstart_tick — advance the machine's deadlines. Safe to call
 * every tick in every state.
 *
 * Also arms the FIRST write: `begin_*` leaves the machine in
 * `FF_CREWSTART_GENERATING`, and the next tick moves it to
 * `FF_CREWSTART_WRITING` with a `FF_CREWSTART_ACT_WRITE` pending. That
 * one-tick window is deliberate — it is where a caller-side
 * precondition (`ff_crewstart_fail_now`) can still stop the run before
 * anything reaches the radio — and the arming lives here, not in
 * `ff_crewstart_take_action`, so the write deadline is stamped with a
 * real `now_ms` instead of with whenever the button was pressed.
 */
void ff_crewstart_tick(ff_crewstart_t *f, uint32_t now_ms);

/**
 * ff_crewstart_dismiss — acknowledge a finished run, returning the
 * machine to IDLE.
 *
 * Only from `FF_CREWSTART_READY`/`FF_CREWSTART_FAILED` — a run in
 * flight is never dismissed out from under the radio, because the write
 * would still land and the machine would have stopped watching for it.
 */
void ff_crewstart_dismiss(ff_crewstart_t *f);

/** Accessors. All NULL-safe. */
ff_crewstart_state_t ff_crewstart_state(ff_crewstart_t const *f);
ff_crewstart_op_t    ff_crewstart_op(ff_crewstart_t const *f);
ff_crewstart_fail_t  ff_crewstart_failure(ff_crewstart_t const *f);
/** The minted code, or "" — never NULL, so callers may `strcmp` it. */
char const          *ff_crewstart_code(ff_crewstart_t const *f);
/** True while an operation is in flight (GENERATING/WRITING/VERIFYING). */
bool                 ff_crewstart_busy(ff_crewstart_t const *f);
/** Write attempts started so far, including the first. */
uint8_t              ff_crewstart_attempts(ff_crewstart_t const *f);

/**
 * ff_crewstart_has_precision / ff_crewstart_precision — what the
 * VERIFYING read-back proved about `position_precision`, captured the
 * moment the machine reached `FF_CREWSTART_READY`. False/0 before then,
 * and after a run that never reached READY — never a stale value from a
 * previous op (`ff_crewstart_init`, reached by every `begin_*` and
 * `consider`, zeroes both). `ff_crewstart_precision` is meaningless when
 * `ff_crewstart_has_precision` is false — the radio proved the channel
 * but never stated a precision, which is a real, reportable fact
 * ("unreported"), not the same as 0. NULL-safe: false/0 for NULL. */
bool                 ff_crewstart_has_precision(ff_crewstart_t const *f);
uint32_t             ff_crewstart_precision(ff_crewstart_t const *f);

/** Short, stable, all-caps names for logs and test messages — this
 *  codebase's existing `ff_*_name` convention. Never NULL; "?" for an
 *  out-of-range value. */
char const *ff_crewstart_state_name(ff_crewstart_state_t s);
char const *ff_crewstart_op_name(ff_crewstart_op_t op);
char const *ff_crewstart_fail_name(ff_crewstart_fail_t f);

/* ---------------------------------------------------------------------
 * The pre-crew snapshot
 * ---------------------------------------------------------------------
 * Taken ONCE, before the very first crew write, and never overwritten by
 * a Firefly crew channel (A02 §2.1 step 4). It is what LEAVE restores,
 * and the reason LEAVE can be honest about the difference between "your
 * radio goes back to its old settings" and "your radio goes back to the
 * factory default".
 *
 * Layout, 52 bytes, little-endian, versioned exactly like
 * `ff_hidden_serialize`'s:
 *
 *   [0..1]  magic 0xF5C0
 *   [2]     version (1)
 *   [3]     index
 *   [4..15] name, NUL-padded, 12 bytes
 *   [16..47] psk, zero-padded, 32 bytes
 *   [48]    psk_len
 *   [49]    is_primary (0/1)
 *   [50..51] position_precision, u16 LE (Meshtastic's own values are
 *            0..32; a u16 is already three orders of magnitude of
 *            headroom and keeps the record a round 52 bytes)
 * ------------------------------------------------------------------- */

/**
 * ff_crewstart_snapshot_serialize — write `ch` as exactly
 * `FF_CREWSTART_SNAPSHOT_BLOB_LEN` bytes. Returns the length written, or
 * 0 for a NULL argument, a buffer that is too small, or a channel this
 * record cannot represent faithfully (an over-long name, an over-long
 * psk, a precision above 65535) — a snapshot that silently clipped any
 * of those would restore a channel that is not the one that was there.
 */
size_t ff_crewstart_snapshot_serialize(ff_crewstart_channel_t const *ch, uint8_t *buf, size_t n);

/**
 * ff_crewstart_snapshot_deserialize — read a blob written by
 * `ff_crewstart_snapshot_serialize`. `*out` is zeroed on EVERY failure
 * path, never left partially filled: a half-read snapshot restored onto
 * a radio is worse than no snapshot at all, which is a state LEAVE
 * already reports honestly.
 */
bool ff_crewstart_snapshot_deserialize(ff_crewstart_channel_t *out, uint8_t const *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* FF_CREWSTART_H */
