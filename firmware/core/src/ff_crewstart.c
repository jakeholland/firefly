/**
 * ff_crewstart.c — see ff_crewstart.h for the full contract.
 *
 * Pure C11: no I/O, no allocation, no clock of its own (every entry
 * point that can change state takes `now_ms` from the caller).
 */
#include "ff_crewstart.h"

#include <string.h>

#define FF_CREWSTART_SNAPSHOT_MAGIC   0xF5C0u
#define FF_CREWSTART_SNAPSHOT_VERSION 1u

/* ------------------------------------------------------------------ */

static void crewstart_enter(ff_crewstart_t *f, ff_crewstart_state_t s, uint32_t now_ms)
{
    f->state = s;
    f->state_since_ms = now_ms;
}

static void crewstart_fail(ff_crewstart_t *f, ff_crewstart_fail_t why, uint32_t now_ms)
{
    f->fail = why;
    f->pending = FF_CREWSTART_ACT_NONE;
    f->has_packet_id = false;
    f->packet_id = 0u;
    crewstart_enter(f, FF_CREWSTART_FAILED, now_ms);
}

/* Start (or re-start) a write attempt, or fail when the bounded retry
 * budget is spent. `why` is the failure to report on exhaustion — the
 * LAST thing that actually went wrong, not a generic "gave up", so the
 * face can still say whether the radio refused, NAKed, or said nothing
 * at all. */
static void crewstart_arm_write(ff_crewstart_t *f, ff_crewstart_fail_t why, uint32_t now_ms)
{
    if (f->attempts >= FF_CREWSTART_MAX_ATTEMPTS) {
        crewstart_fail(f, why, now_ms);
        return;
    }
    f->attempts++;
    f->has_packet_id = false;
    f->packet_id = 0u;
    f->pending = FF_CREWSTART_ACT_WRITE;
    crewstart_enter(f, FF_CREWSTART_WRITING, now_ms);
}

/* ------------------------------------------------------------------ */

void ff_crewstart_init(ff_crewstart_t *f)
{
    if (f == NULL) return;
    memset(f, 0, sizeof(*f));
    f->state = FF_CREWSTART_IDLE;
    f->op = FF_CREWSTART_OP_NONE;
    f->fail = FF_CREWSTART_FAIL_NONE;
    f->pending = FF_CREWSTART_ACT_NONE;
}

bool ff_crewstart_busy(ff_crewstart_t const *f)
{
    if (f == NULL) return false;
    return f->state == FF_CREWSTART_GENERATING || f->state == FF_CREWSTART_WRITING ||
           f->state == FF_CREWSTART_VERIFYING;
}

bool ff_crewstart_begin_start(ff_crewstart_t *f, ff_crewstart_rand_fn rng, void *rng_ctx, bool precision_strict,
                               uint32_t now_ms)
{
    if (f == NULL || ff_crewstart_busy(f)) return false;

    ff_crewstart_init(f);
    f->op = FF_CREWSTART_OP_START;
    f->precision_strict = precision_strict;
    crewstart_enter(f, FF_CREWSTART_GENERATING, now_ms);

    if (rng == NULL) {
        crewstart_fail(f, FF_CREWSTART_FAIL_NO_ENTROPY, now_ms);
        return false;
    }

    /* The mask lives here, where the value is known to be a raw 32-bit
     * draw. `ff_crewcode_from_bits` deliberately REJECTS out-of-range
     * input instead of masking, so that a caller elsewhere handing it 32
     * meaningful bits finds out rather than silently getting 30. */
    uint32_t const bits = rng(rng_ctx) & ((1u << FF_CREWCODE_BITS) - 1u);
    if (!ff_crewcode_from_bits(bits, f->code)) {
        /* Unreachable with a masked draw; treated as an entropy failure
         * rather than asserted, because the alternative to a code is
         * never a made-up code. */
        f->code[0] = '\0';
        crewstart_fail(f, FF_CREWSTART_FAIL_NO_ENTROPY, now_ms);
        return false;
    }

    uint8_t psk[FF_CREWCODE_PSK_LEN];
    if (!ff_crewcode_psk(f->code, psk)) {
        f->code[0] = '\0';
        crewstart_fail(f, FF_CREWSTART_FAIL_NO_ENTROPY, now_ms);
        return false;
    }

    memset(&f->desired, 0, sizeof(f->desired));
    f->desired.index = FF_CREWSTART_CREW_INDEX;
    /* A02 §1.3: the channel NAME *is* the canonical code, verbatim. 11
     * characters plus the NUL is exactly FF_CREWSTART_NAME_MAX. */
    memcpy(f->desired.name, f->code, FF_CREWCODE_LEN + 1u);
    memcpy(f->desired.psk, psk, sizeof(psk));
    f->desired.psk_len = (uint8_t)sizeof(psk);
    f->desired.is_primary = true;
    f->desired.position_precision = FF_CREWSTART_CREW_PRECISION;
    f->desired.has_position_precision = true; /* always explicit on a write, A02 §1.5 */
    return true;
}

void ff_crewstart_default_primary(ff_crewstart_channel_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->index = FF_CREWSTART_CREW_INDEX;
    out->name[0] = '\0'; /* channel.proto: an empty name IS the default channel */
    out->psk[0] = 0x01u; /* the 1-byte shorthand for Meshtastic's well-known default key */
    out->psk_len = 1u;
    out->is_primary = true;
    out->position_precision = 0u;
    out->has_position_precision = true; /* a known, concrete fact: the stock default states 0 */
}

bool ff_crewstart_begin_leave(ff_crewstart_t *f, ff_crewstart_channel_t const *restore, uint32_t now_ms)
{
    if (f == NULL || ff_crewstart_busy(f)) return false;

    ff_crewstart_init(f);
    f->op = FF_CREWSTART_OP_LEAVE;
    crewstart_enter(f, FF_CREWSTART_GENERATING, now_ms);

    if (restore == NULL) {
        crewstart_fail(f, FF_CREWSTART_FAIL_NO_SNAPSHOT, now_ms);
        return false;
    }
    f->desired = *restore;
    /* There is no code after leaving, and there must not appear to be
     * one — the SHOW CODE face reads this. */
    f->code[0] = '\0';
    return true;
}

void ff_crewstart_consider(ff_crewstart_t *f, ff_crewstart_op_t op)
{
    if (f == NULL || ff_crewstart_busy(f)) return;
    ff_crewstart_init(f); /* a previous run's result is not this one's */
    f->op = op;
}

void ff_crewstart_fail_now(ff_crewstart_t *f, ff_crewstart_op_t op, ff_crewstart_fail_t why, uint32_t now_ms)
{
    if (f == NULL) return;
    /* A frame already in the air cannot be un-sent; reporting a failure
     * over it would leave the face describing a state the radio does not
     * share. */
    if (f->state == FF_CREWSTART_WRITING || f->state == FF_CREWSTART_VERIFYING) return;
    f->op = op;
    f->code[0] = '\0';
    crewstart_fail(f, why, now_ms);
}

ff_crewstart_action_t ff_crewstart_take_action(ff_crewstart_t *f, ff_crewstart_channel_t *out)
{
    if (f == NULL) return FF_CREWSTART_ACT_NONE;

    ff_crewstart_action_t const act = f->pending;
    f->pending = FF_CREWSTART_ACT_NONE;
    if (act == FF_CREWSTART_ACT_WRITE && out != NULL) {
        *out = f->desired;
    }
    return act;
}

void ff_crewstart_on_write_result(ff_crewstart_t *f, bool accepted, bool has_packet_id, uint32_t packet_id,
                                   uint32_t now_ms)
{
    if (f == NULL || f->state != FF_CREWSTART_WRITING) return;

    if (!accepted) {
        crewstart_arm_write(f, FF_CREWSTART_FAIL_SEND, now_ms);
        return;
    }

    if (!has_packet_id) {
        /* No id means no routing reply to correlate. The read-back was
         * always the real proof, so go straight to it rather than
         * waiting out an ACK that can never be matched. */
        f->has_packet_id = false;
        f->pending = FF_CREWSTART_ACT_REREAD;
        crewstart_enter(f, FF_CREWSTART_VERIFYING, now_ms);
        return;
    }

    f->has_packet_id = true;
    f->packet_id = packet_id;
}

void ff_crewstart_on_routing_ack(ff_crewstart_t *f, uint32_t request_id, bool ok, uint32_t now_ms)
{
    if (f == NULL || f->state != FF_CREWSTART_WRITING) return;
    if (!f->has_packet_id || request_id != f->packet_id) return;

    if (ok) {
        f->pending = FF_CREWSTART_ACT_REREAD;
        crewstart_enter(f, FF_CREWSTART_VERIFYING, now_ms);
        return;
    }
    crewstart_arm_write(f, FF_CREWSTART_FAIL_NAK, now_ms);
}

void ff_crewstart_on_channel(ff_crewstart_t *f, ff_crewstart_channel_t const *ch, uint32_t now_ms)
{
    if (f == NULL || ch == NULL || f->state != FF_CREWSTART_VERIFYING) return;
    if (ch->index != f->desired.index) return;

    bool const name_ok = (strncmp(ch->name, f->desired.name, FF_CREWSTART_NAME_MAX) == 0);
    bool const psk_ok = (ch->psk_len == f->desired.psk_len) &&
                        (f->desired.psk_len == 0u || memcmp(ch->psk, f->desired.psk, f->desired.psk_len) == 0);

    /* #47's hazard: a radio that accepts the channel write but silently
     * keeps positions coarse reaches READY while the crew sees km-scale
     * positions. Only a START's own target carries this requirement —
     * LEAVE restores whatever the radio held before Firefly ever wrote a
     * channel, which is under no obligation to be precision 32. */
    bool precision_ok = true;
    if (f->op == FF_CREWSTART_OP_START) {
        if (ch->has_position_precision) {
            /* FF_CREWSTART_PRECISION_REQUIRED: not a toggle — a channel
             * that STATES a precision must state exactly the crew's
             * own. _Static_assert below pins that this is unconditional
             * policy, not a runtime flag masquerading as one. */
            precision_ok = (ch->position_precision == FF_CREWSTART_CREW_PRECISION);
        } else {
            /* The bench fact (2026-09-14, Heltec V3 / Meshtastic 2.7.x)
             * is that a real radio DOES echo module_settings.position_
             * precision back after an import that set it, so an absent
             * field here is a real signal and not decoding noise.
             * Whether that signal is trusted as READY-with-"unreported"
             * or folded into the same MISMATCH a wrong value gets is
             * this run's own `precision_strict`
             * (FF_CREW_PRECISION_STRICT, Kconfig default y). */
            precision_ok = !f->precision_strict;
        }
    }

    if (name_ok && psk_ok && precision_ok) {
        f->has_precision = ch->has_position_precision;
        f->precision = ch->has_position_precision ? ch->position_precision : 0u;
        f->pending = FF_CREWSTART_ACT_NONE;
        crewstart_enter(f, FF_CREWSTART_READY, now_ms);
        return;
    }

    /* The radio answered the question and the answer was no. Waiting
     * for a better row would be waiting for a row that is not coming —
     * the table carries one entry per index. */
    crewstart_fail(f, FF_CREWSTART_FAIL_MISMATCH, now_ms);
}

_Static_assert(FF_CREWSTART_PRECISION_REQUIRED,
               "ff_crewstart_on_channel's STATED-but-wrong-precision leg assumes this is unconditionally true; "
               "if it is ever meant to be configurable, that leg needs a matching update");

void ff_crewstart_tick(ff_crewstart_t *f, uint32_t now_ms)
{
    if (f == NULL) return;

    /* GENERATING is the window between `begin_*` and the first write:
     * it exists so the caller can still stop the run (`fail_now`, for a
     * precondition this module cannot see) before anything reaches the
     * radio. The first tick after that window arms the first write —
     * here, and not in `ff_crewstart_take_action`, because the WRITING
     * deadline has to be stamped with a real `now_ms` rather than with
     * whenever the button happened to be pressed. */
    if (f->state == FF_CREWSTART_GENERATING) {
        crewstart_arm_write(f, FF_CREWSTART_FAIL_SEND, now_ms);
        return;
    }

    /* Unsigned wrap-safe elapsed, the same arithmetic every other
     * millisecond deadline in this tree uses. */
    uint32_t const elapsed = now_ms - f->state_since_ms;

    if (f->state == FF_CREWSTART_WRITING && f->pending == FF_CREWSTART_ACT_NONE &&
        elapsed >= FF_CREWSTART_ACK_TIMEOUT_MS) {
        crewstart_arm_write(f, FF_CREWSTART_FAIL_TIMEOUT_ACK, now_ms);
        return;
    }
    if (f->state == FF_CREWSTART_VERIFYING && elapsed >= FF_CREWSTART_VERIFY_TIMEOUT_MS) {
        /* Deliberately NOT retried — see FF_CREWSTART_MAX_ATTEMPTS. */
        crewstart_fail(f, FF_CREWSTART_FAIL_TIMEOUT_VERIFY, now_ms);
    }
}

void ff_crewstart_dismiss(ff_crewstart_t *f)
{
    if (f == NULL) return;
    if (f->state != FF_CREWSTART_READY && f->state != FF_CREWSTART_FAILED) return;
    ff_crewstart_init(f);
}

ff_crewstart_state_t ff_crewstart_state(ff_crewstart_t const *f)
{
    return (f == NULL) ? FF_CREWSTART_IDLE : f->state;
}

ff_crewstart_op_t ff_crewstart_op(ff_crewstart_t const *f)
{
    return (f == NULL) ? FF_CREWSTART_OP_NONE : f->op;
}

ff_crewstart_fail_t ff_crewstart_failure(ff_crewstart_t const *f)
{
    return (f == NULL) ? FF_CREWSTART_FAIL_NONE : f->fail;
}

char const *ff_crewstart_code(ff_crewstart_t const *f)
{
    return (f == NULL) ? "" : f->code;
}

uint8_t ff_crewstart_attempts(ff_crewstart_t const *f)
{
    return (f == NULL) ? 0u : f->attempts;
}

bool ff_crewstart_has_precision(ff_crewstart_t const *f)
{
    return (f == NULL) ? false : f->has_precision;
}

uint32_t ff_crewstart_precision(ff_crewstart_t const *f)
{
    return (f == NULL) ? 0u : f->precision;
}

char const *ff_crewstart_state_name(ff_crewstart_state_t s)
{
    switch (s) {
    case FF_CREWSTART_IDLE: return "IDLE";
    case FF_CREWSTART_GENERATING: return "GENERATING";
    case FF_CREWSTART_WRITING: return "WRITING";
    case FF_CREWSTART_VERIFYING: return "VERIFYING";
    case FF_CREWSTART_READY: return "READY";
    case FF_CREWSTART_FAILED: return "FAILED";
    }
    return "?";
}

char const *ff_crewstart_op_name(ff_crewstart_op_t op)
{
    switch (op) {
    case FF_CREWSTART_OP_NONE: return "NONE";
    case FF_CREWSTART_OP_START: return "START";
    case FF_CREWSTART_OP_LEAVE: return "LEAVE";
    }
    return "?";
}

char const *ff_crewstart_fail_name(ff_crewstart_fail_t f)
{
    switch (f) {
    case FF_CREWSTART_FAIL_NONE: return "NONE";
    case FF_CREWSTART_FAIL_NO_LINK: return "NO_LINK";
    case FF_CREWSTART_FAIL_REGION_UNSET: return "REGION_UNSET";
    case FF_CREWSTART_FAIL_NO_ENTROPY: return "NO_ENTROPY";
    case FF_CREWSTART_FAIL_NO_SNAPSHOT: return "NO_SNAPSHOT";
    case FF_CREWSTART_FAIL_SEND: return "SEND";
    case FF_CREWSTART_FAIL_NAK: return "NAK";
    case FF_CREWSTART_FAIL_TIMEOUT_ACK: return "TIMEOUT_ACK";
    case FF_CREWSTART_FAIL_TIMEOUT_VERIFY: return "TIMEOUT_VERIFY";
    case FF_CREWSTART_FAIL_MISMATCH: return "MISMATCH";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* The pre-crew snapshot                                              */
/* ------------------------------------------------------------------ */

size_t ff_crewstart_snapshot_serialize(ff_crewstart_channel_t const *ch, uint8_t *buf, size_t n)
{
    if (ch == NULL || buf == NULL || n < FF_CREWSTART_SNAPSHOT_BLOB_LEN) return 0u;
    /* Faithful or nothing: a clipped snapshot restores a channel that is
     * not the one that was there. */
    if (ch->psk_len > FF_CREWSTART_PSK_MAX) return 0u;
    if (ch->position_precision > 0xFFFFu) return 0u;
    size_t name_len = 0u;
    while (name_len < FF_CREWSTART_NAME_MAX && ch->name[name_len] != '\0') name_len++;
    if (name_len >= FF_CREWSTART_NAME_MAX) return 0u; /* no NUL inside the field */

    memset(buf, 0, FF_CREWSTART_SNAPSHOT_BLOB_LEN);
    buf[0] = (uint8_t)(FF_CREWSTART_SNAPSHOT_MAGIC & 0xFFu);
    buf[1] = (uint8_t)((FF_CREWSTART_SNAPSHOT_MAGIC >> 8) & 0xFFu);
    buf[2] = (uint8_t)FF_CREWSTART_SNAPSHOT_VERSION;
    buf[3] = ch->index;
    memcpy(buf + 4, ch->name, name_len);
    memcpy(buf + 16, ch->psk, ch->psk_len);
    buf[48] = ch->psk_len;
    buf[49] = ch->is_primary ? 1u : 0u;
    buf[50] = (uint8_t)(ch->position_precision & 0xFFu);
    buf[51] = (uint8_t)((ch->position_precision >> 8) & 0xFFu);
    return FF_CREWSTART_SNAPSHOT_BLOB_LEN;
}

bool ff_crewstart_snapshot_deserialize(ff_crewstart_channel_t *out, uint8_t const *buf, size_t n)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out)); /* zeroed on EVERY failure path below */
    if (buf == NULL || n != FF_CREWSTART_SNAPSHOT_BLOB_LEN) return false;

    uint16_t const magic = (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
    if (magic != FF_CREWSTART_SNAPSHOT_MAGIC) return false;
    if (buf[2] != (uint8_t)FF_CREWSTART_SNAPSHOT_VERSION) return false;

    uint8_t const psk_len = buf[48];
    if (psk_len > FF_CREWSTART_PSK_MAX) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    /* The name field must carry its own terminator: a record whose 12
     * bytes are all non-NUL is corrupt, not a 12-character name. */
    bool terminated = false;
    for (size_t i = 0; i < FF_CREWSTART_NAME_MAX; i++) {
        if (buf[4 + i] == 0u) {
            terminated = true;
            break;
        }
    }
    if (!terminated) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    out->index = buf[3];
    memcpy(out->name, buf + 4, FF_CREWSTART_NAME_MAX);
    out->name[FF_CREWSTART_NAME_MAX - 1u] = '\0';
    memcpy(out->psk, buf + 16, FF_CREWSTART_PSK_MAX);
    out->psk_len = psk_len;
    out->is_primary = (buf[49] != 0u);
    out->position_precision = (uint32_t)buf[50] | ((uint32_t)buf[51] << 8);
    return true;
}
