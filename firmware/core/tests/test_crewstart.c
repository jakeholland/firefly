/**
 * test_crewstart.c — the puck-initiated crew, driven end to end by a
 * scripted mc-events stub (docs/specs/S02-core-crew.md's 2026-09-14
 * amendment, acceptance criteria **S02_AC16** (START), **S02_AC17**
 * (LEAVE + snapshot) and **S02_AC18** (honest failure)).
 *
 * ## How this file is built, and why
 *
 * `ff_crewstart_t` has no radio, no clock and no RNG of its own, so the
 * whole sequence is drivable from a plain struct: `mc_stub_t` below is
 * the "radio", a scripted list of what it will do in response to the
 * write (accept or refuse, ACK or NAK or silence, and which channel row
 * it then reports on the read-back). `stub_run` is the tick loop —
 * `ff_crewstart_tick`, `ff_crewstart_take_action`, perform, report —
 * which is exactly the loop `ff_shell.c` runs against a real one.
 *
 * That matters for the proxy check (docs/review/code-review.md item 6).
 * The property under test is "READY means the radio, re-read, reports
 * the channel we asked for" — and the cheap proxy for it is "READY
 * means we sent something and nothing complained". So the stub can be
 * told to ACK the write and then report a DIFFERENT channel back
 * (`S02_AC18_readback_mismatch_is_a_failure_not_a_success`), which
 * satisfies the proxy and violates the property. A machine that
 * declared victory on the ACK passes every other test in this file and
 * fails that one.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ff_crewcode.h"
#include "ff_crewstart.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* The scripted "radio"                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    STUB_REFUSE = 0,   /* the write is not even accepted for sending */
    STUB_ACCEPT_ACK,   /* accepted, then a routing ACK */
    STUB_ACCEPT_NAK,   /* accepted, then a routing NAK */
    STUB_ACCEPT_SILENT /* accepted, and no routing reply ever arrives */
} stub_write_t;

typedef struct {
    /* What the radio does with each write attempt, in order. Attempts
     * past `n_writes` reuse the last entry — a script that only
     * describes the first attempt still describes an infinite retry. */
    stub_write_t writes[4];
    uint8_t      n_writes;

    /* Does the write also hand back a packet id? A real radio does; a
     * transport that lost the id is a separate, tested path. */
    bool         gives_packet_id;

    /* The channel row the read-back reports, and whether it reports one
     * at all. */
    bool                   readback;
    ff_crewstart_channel_t readback_row;
    /* Report the row as the ECHO of whatever was written, rather than as
     * `readback_row`. The honest-radio case. */
    bool                   readback_echo;

    /* Observed by the test. */
    uint8_t                writes_seen;
    uint8_t                rereads_seen;
    ff_crewstart_channel_t last_written;
    uint32_t               next_packet_id;
} mc_stub_t;

static mc_stub_t stub_ok(void)
{
    mc_stub_t s;
    memset(&s, 0, sizeof(s));
    s.writes[0] = STUB_ACCEPT_ACK;
    s.n_writes = 1u;
    s.gives_packet_id = true;
    s.readback = true;
    s.readback_echo = true;
    s.next_packet_id = 0x1000u;
    return s;
}

static stub_write_t stub_write_for(mc_stub_t const *s, uint8_t attempt_idx)
{
    uint8_t const n = (s->n_writes == 0u) ? 1u : s->n_writes;
    uint8_t const i = (attempt_idx < n) ? attempt_idx : (uint8_t)(n - 1u);
    return s->writes[i];
}

/* One tick of the loop ff_shell.c runs: advance deadlines, take at most
 * one action, perform it, report the result back. `now_ms` is the
 * caller's clock — this module has none. */
static void stub_tick(ff_crewstart_t *f, mc_stub_t *s, uint32_t now_ms)
{
    ff_crewstart_tick(f, now_ms);

    ff_crewstart_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ff_crewstart_action_t const act = ff_crewstart_take_action(f, &ch);

    if (act == FF_CREWSTART_ACT_WRITE) {
        uint8_t const attempt = s->writes_seen;
        s->writes_seen++;
        s->last_written = ch;

        stub_write_t const w = stub_write_for(s, attempt);
        if (w == STUB_REFUSE) {
            ff_crewstart_on_write_result(f, /*accepted=*/false, false, 0u, now_ms);
            return;
        }
        uint32_t const pid = s->next_packet_id++;
        ff_crewstart_on_write_result(f, /*accepted=*/true, s->gives_packet_id, pid, now_ms);
        if (!s->gives_packet_id) return; /* no id => no reply to correlate */

        if (w == STUB_ACCEPT_ACK) {
            ff_crewstart_on_routing_ack(f, pid, /*ok=*/true, now_ms);
        } else if (w == STUB_ACCEPT_NAK) {
            ff_crewstart_on_routing_ack(f, pid, /*ok=*/false, now_ms);
        }
        return;
    }

    if (act == FF_CREWSTART_ACT_REREAD) {
        s->rereads_seen++;
        if (!s->readback) return; /* the radio never came back */
        ff_crewstart_channel_t row = s->readback_echo ? s->last_written : s->readback_row;
        /* A real handshake replays the WHOLE table; the rows that are
         * not the crew index must cost nothing. */
        ff_crewstart_channel_t other;
        memset(&other, 0, sizeof(other));
        other.index = 3u;
        other.psk_len = 1u;
        other.psk[0] = 0x01u;
        ff_crewstart_on_channel(f, &other, now_ms);
        ff_crewstart_on_channel(f, &row, now_ms);
    }
}

/* Run the loop until the machine settles or `max_ticks` elapse. Ticks
 * advance the clock by `step_ms`. */
static uint32_t stub_run(ff_crewstart_t *f, mc_stub_t *s, uint32_t start_ms, uint32_t step_ms, unsigned max_ticks)
{
    uint32_t now = start_ms;
    for (unsigned i = 0; i < max_ticks; i++) {
        if (!ff_crewstart_busy(f)) break;
        stub_tick(f, s, now);
        now += step_ms;
    }
    return now;
}

static uint32_t rng_fixed(void *ctx)
{
    return *(uint32_t *)ctx;
}

/* ------------------------------------------------------------------ */
/* S02_AC16 — START                                                   */
/* ------------------------------------------------------------------ */

static void S02_AC16_start_mints_writes_verifies_and_lands_ready(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();

    /* 0x0A4D2E7 is an arbitrary 30-bit draw; the point is that the code
     * and the key come from IT and from nothing else. */
    uint32_t bits = 0x0A4D2E7u;
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 1000u));
    TEST_ASSERT_EQUAL(FF_CREWSTART_GENERATING, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_OP_START, ff_crewstart_op(&f));

    /* The minted code is a real, parseable crew code — not a plausible
     * string that this tree's own decoder would reject. */
    char const *code = ff_crewstart_code(&f);
    TEST_ASSERT_TRUE(ff_crewcode_valid(code));
    TEST_ASSERT_EQUAL_size_t(FF_CREWCODE_LEN, strlen(code));

    (void)stub_run(&f, &s, 1000u, 100u, 20u);

    TEST_ASSERT_EQUAL(FF_CREWSTART_READY, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_NONE, ff_crewstart_failure(&f));
    TEST_ASSERT_EQUAL_UINT8(1u, s.writes_seen);
    TEST_ASSERT_EQUAL_UINT8(1u, s.rereads_seen);
}

/* The channel that goes on the wire is A02 §1.5's, field by field. This
 * is the test that would catch a crew minted at the import path's
 * cautious precision 0, or parked on a secondary slot. */
static void S02_AC16_written_channel_is_the_spec_channel(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    uint32_t bits = 0x12345u;

    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    (void)stub_run(&f, &s, 0u, 100u, 20u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_READY, ff_crewstart_state(&f));

    TEST_ASSERT_EQUAL_UINT8(0u, s.last_written.index);
    TEST_ASSERT_TRUE(s.last_written.is_primary);
    TEST_ASSERT_EQUAL_UINT32(32u, s.last_written.position_precision);
    TEST_ASSERT_EQUAL_UINT8(32u, s.last_written.psk_len);
    /* A02 §1.3 — the channel NAME *is* the code, verbatim. */
    TEST_ASSERT_EQUAL_STRING(ff_crewstart_code(&f), s.last_written.name);
}

/* The key on the wire is the one ff_crewcode derives from the code on
 * the screen. A generator that minted a code and a key independently
 * would put a crew on a channel nobody who typed the code could join. */
static void S02_AC16_written_psk_is_the_one_the_code_derives(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    uint32_t bits = 0x3FFFFFFFu;

    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    (void)stub_run(&f, &s, 0u, 100u, 20u);

    uint8_t want[FF_CREWCODE_PSK_LEN];
    TEST_ASSERT_TRUE(ff_crewcode_psk(ff_crewstart_code(&f), want));
    TEST_ASSERT_EQUAL_MEMORY(want, s.last_written.psk, sizeof(want));
}

/* The draw is masked to 30 bits HERE, not in the codec, so a 32-bit
 * CSPRNG word is usable directly. Both extremes must produce the
 * fixture's own boundary codes. */
static void S02_AC16_thirty_bits_are_taken_from_a_thirty_two_bit_draw(void)
{
    ff_crewstart_t f;
    uint32_t all_ones = 0xFFFFFFFFu;
    ff_crewstart_init(&f);
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &all_ones, 0u));
    TEST_ASSERT_EQUAL_STRING("FIRE-ZZZZZZ", ff_crewstart_code(&f));

    uint32_t zero = 0u;
    ff_crewstart_init(&f);
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &zero, 0u));
    TEST_ASSERT_EQUAL_STRING("FIRE-000000", ff_crewstart_code(&f));
}

/* No CSPRNG wired up is a refusal, not a fallback to something
 * guessable. A02 §1.6's threat model does not survive a predictable 30
 * bits, so there is deliberately no "well, use the tick count". */
static void S02_AC18_no_entropy_source_mints_nothing(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    TEST_ASSERT_FALSE(ff_crewstart_begin_start(&f, NULL, NULL, 500u));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAILED, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_NO_ENTROPY, ff_crewstart_failure(&f));
    TEST_ASSERT_EQUAL_STRING("", ff_crewstart_code(&f));
}

/* A second press while a write is in the air is ignored, not queued: two
 * channel writes racing each other's reboot is how a radio ends up in a
 * configuration neither press asked for. */
static void S02_AC16_a_second_start_while_busy_is_ignored(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    s.writes[0] = STUB_ACCEPT_SILENT;
    uint32_t bits = 0x777u;

    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    ff_crewstart_tick(&f, 10u);
    ff_crewstart_channel_t ch;
    TEST_ASSERT_EQUAL(FF_CREWSTART_ACT_WRITE, ff_crewstart_take_action(&f, &ch));
    ff_crewstart_on_write_result(&f, true, true, 42u, 10u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_WRITING, ff_crewstart_state(&f));

    char code_before[FF_CREWCODE_LEN + 1u];
    snprintf(code_before, sizeof(code_before), "%s", ff_crewstart_code(&f));

    uint32_t other = 0x999u;
    TEST_ASSERT_FALSE(ff_crewstart_begin_start(&f, rng_fixed, &other, 20u));
    TEST_ASSERT_EQUAL(FF_CREWSTART_WRITING, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL_STRING(code_before, ff_crewstart_code(&f));
    (void)s;
}

/* ------------------------------------------------------------------ */
/* S02_AC17 — LEAVE, and the pre-crew snapshot                        */
/* ------------------------------------------------------------------ */

static ff_crewstart_channel_t a_snapshot(void)
{
    ff_crewstart_channel_t c;
    memset(&c, 0, sizeof(c));
    c.index = 0u;
    snprintf(c.name, sizeof(c.name), "LongFast");
    c.psk[0] = 0x01u;
    c.psk_len = 1u;
    c.is_primary = true;
    c.position_precision = 13u;
    return c;
}

static void S02_AC17_leave_restores_the_snapshot_verbatim(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    ff_crewstart_channel_t snap = a_snapshot();

    TEST_ASSERT_TRUE(ff_crewstart_begin_leave(&f, &snap, 0u));
    TEST_ASSERT_EQUAL(FF_CREWSTART_OP_LEAVE, ff_crewstart_op(&f));
    /* There is no code after leaving, and there must not appear to be
     * one — the SHOW CODE face reads exactly this. */
    TEST_ASSERT_EQUAL_STRING("", ff_crewstart_code(&f));

    (void)stub_run(&f, &s, 0u, 100u, 20u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_READY, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL_STRING("LongFast", s.last_written.name);
    TEST_ASSERT_EQUAL_UINT8(1u, s.last_written.psk_len);
    TEST_ASSERT_EQUAL_UINT32(13u, s.last_written.position_precision);
}

/* Leaving with nothing recorded fails honestly rather than inventing a
 * configuration for somebody else's radio. */
static void S02_AC17_leave_with_no_snapshot_fails_honestly(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    TEST_ASSERT_FALSE(ff_crewstart_begin_leave(&f, NULL, 0u));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAILED, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_NO_SNAPSHOT, ff_crewstart_failure(&f));
}

/* The stock default is available, but only to a caller that asks for it
 * by name — it is a default, not a guess about this radio's history. */
static void S02_AC17_default_primary_is_meshtastics_stock_channel(void)
{
    ff_crewstart_channel_t c;
    ff_crewstart_default_primary(&c);
    TEST_ASSERT_EQUAL_UINT8(0u, c.index);
    TEST_ASSERT_EQUAL_STRING("", c.name);
    TEST_ASSERT_EQUAL_UINT8(1u, c.psk_len);
    TEST_ASSERT_EQUAL_UINT8(0x01u, c.psk[0]);
    TEST_ASSERT_TRUE(c.is_primary);
    TEST_ASSERT_EQUAL_UINT32(0u, c.position_precision);
}

static void S02_AC17_snapshot_round_trips_through_a_blob(void)
{
    ff_crewstart_channel_t in = a_snapshot();
    uint8_t blob[FF_CREWSTART_SNAPSHOT_BLOB_LEN];
    TEST_ASSERT_EQUAL_size_t(FF_CREWSTART_SNAPSHOT_BLOB_LEN,
                             ff_crewstart_snapshot_serialize(&in, blob, sizeof(blob)));

    ff_crewstart_channel_t out;
    TEST_ASSERT_TRUE(ff_crewstart_snapshot_deserialize(&out, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(in.index, out.index);
    TEST_ASSERT_EQUAL_STRING(in.name, out.name);
    TEST_ASSERT_EQUAL_UINT8(in.psk_len, out.psk_len);
    TEST_ASSERT_EQUAL_MEMORY(in.psk, out.psk, sizeof(in.psk));
    TEST_ASSERT_EQUAL(in.is_primary, out.is_primary);
    TEST_ASSERT_EQUAL_UINT32(in.position_precision, out.position_precision);
}

/* A 32-byte crew key round-trips too — the snapshot must be able to hold
 * whatever was actually there, including another crew's channel. */
static void S02_AC17_snapshot_holds_a_full_length_key(void)
{
    ff_crewstart_channel_t in;
    memset(&in, 0, sizeof(in));
    snprintf(in.name, sizeof(in.name), "FIRE-4K9M7X");
    TEST_ASSERT_TRUE(ff_crewcode_psk("FIRE-4K9M7X", in.psk));
    in.psk_len = 32u;
    in.is_primary = true;
    in.position_precision = 32u;

    uint8_t blob[FF_CREWSTART_SNAPSHOT_BLOB_LEN];
    TEST_ASSERT_EQUAL_size_t(FF_CREWSTART_SNAPSHOT_BLOB_LEN,
                             ff_crewstart_snapshot_serialize(&in, blob, sizeof(blob)));
    ff_crewstart_channel_t out;
    TEST_ASSERT_TRUE(ff_crewstart_snapshot_deserialize(&out, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_STRING("FIRE-4K9M7X", out.name);
    TEST_ASSERT_EQUAL_MEMORY(in.psk, out.psk, 32u);
}

/* Corruption loads EMPTY, never partially: a half-read snapshot written
 * onto a radio is worse than the honest "there is no snapshot". */
static void S02_AC17_corrupt_snapshot_loads_nothing(void)
{
    ff_crewstart_channel_t in = a_snapshot();
    uint8_t blob[FF_CREWSTART_SNAPSHOT_BLOB_LEN];
    (void)ff_crewstart_snapshot_serialize(&in, blob, sizeof(blob));

    ff_crewstart_channel_t out;

    uint8_t bad_magic[FF_CREWSTART_SNAPSHOT_BLOB_LEN];
    memcpy(bad_magic, blob, sizeof(blob));
    bad_magic[0] ^= 0xFFu;
    TEST_ASSERT_FALSE(ff_crewstart_snapshot_deserialize(&out, bad_magic, sizeof(bad_magic)));
    TEST_ASSERT_EQUAL_UINT8(0u, out.psk_len);

    uint8_t bad_version[FF_CREWSTART_SNAPSHOT_BLOB_LEN];
    memcpy(bad_version, blob, sizeof(blob));
    bad_version[2] = 99u;
    TEST_ASSERT_FALSE(ff_crewstart_snapshot_deserialize(&out, bad_version, sizeof(bad_version)));

    uint8_t bad_psk_len[FF_CREWSTART_SNAPSHOT_BLOB_LEN];
    memcpy(bad_psk_len, blob, sizeof(blob));
    bad_psk_len[48] = 33u;
    TEST_ASSERT_FALSE(ff_crewstart_snapshot_deserialize(&out, bad_psk_len, sizeof(bad_psk_len)));

    /* A name field with no terminator anywhere in it is corruption, not
     * a 12-character name. */
    uint8_t no_nul[FF_CREWSTART_SNAPSHOT_BLOB_LEN];
    memcpy(no_nul, blob, sizeof(blob));
    memset(no_nul + 4, 'A', FF_CREWSTART_NAME_MAX);
    TEST_ASSERT_FALSE(ff_crewstart_snapshot_deserialize(&out, no_nul, sizeof(no_nul)));

    /* A short buffer is not a short snapshot. */
    TEST_ASSERT_FALSE(ff_crewstart_snapshot_deserialize(&out, blob, sizeof(blob) - 1u));
    TEST_ASSERT_FALSE(ff_crewstart_snapshot_deserialize(&out, NULL, sizeof(blob)));
}

/* ------------------------------------------------------------------ */
/* S02_AC18 — honest failure                                          */
/* ------------------------------------------------------------------ */

/* THE PROXY TEST. The radio accepts the write and ACKs it, then reports
 * back a channel that is not the one we asked for. "We sent it and
 * nothing complained" is satisfied; "the radio holds our crew" is not.
 * READY here would be a confidently-wrong screen: a code on the glass
 * that nobody can join. */
static void S02_AC18_readback_mismatch_is_a_failure_not_a_success(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    s.readback_echo = false;
    memset(&s.readback_row, 0, sizeof(s.readback_row));
    s.readback_row.index = 0u;
    snprintf(s.readback_row.name, sizeof(s.readback_row.name), "LongFast");
    s.readback_row.psk_len = 1u;
    s.readback_row.psk[0] = 0x01u;
    s.readback_row.is_primary = true;

    uint32_t bits = 0x2222u;
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    (void)stub_run(&f, &s, 0u, 100u, 20u);

    TEST_ASSERT_EQUAL(FF_CREWSTART_FAILED, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_MISMATCH, ff_crewstart_failure(&f));
}

/* The same shape, one field apart: the name matches and the KEY does
 * not. This is the case a name-only comparison would wave through, and
 * it is precisely the crew nobody can decrypt. */
static void S02_AC18_readback_with_the_right_name_and_wrong_key_fails(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    uint32_t bits = 0x2223u;
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));

    /* Drive one tick so the write lands, then hand back a row that
     * copies the written name but flips one key byte. */
    stub_tick(&f, &s, 0u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_VERIFYING, ff_crewstart_state(&f));

    ff_crewstart_channel_t row = s.last_written;
    row.psk[7] ^= 0x01u;
    ff_crewstart_on_channel(&f, &row, 100u);

    TEST_ASSERT_EQUAL(FF_CREWSTART_FAILED, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_MISMATCH, ff_crewstart_failure(&f));
}

/* A NAK retries, bounded, and then reports the NAK — not a generic
 * "gave up", because which of the three things went wrong is the whole
 * content of the message. */
static void S02_AC18_nak_retries_then_fails_as_nak(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    s.writes[0] = STUB_ACCEPT_NAK;
    s.n_writes = 1u; /* every attempt NAKs */

    uint32_t bits = 0x4444u;
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    (void)stub_run(&f, &s, 0u, 100u, 30u);

    TEST_ASSERT_EQUAL(FF_CREWSTART_FAILED, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_NAK, ff_crewstart_failure(&f));
    TEST_ASSERT_EQUAL_UINT8(FF_CREWSTART_MAX_ATTEMPTS, s.writes_seen);
    TEST_ASSERT_EQUAL_UINT8(FF_CREWSTART_MAX_ATTEMPTS, ff_crewstart_attempts(&f));
}

/* A NAK on the first attempt and an ACK on the second succeeds — the
 * retry is a real retry, not a countdown to failure. */
static void S02_AC18_a_nak_then_an_ack_still_lands_ready(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    s.writes[0] = STUB_ACCEPT_NAK;
    s.writes[1] = STUB_ACCEPT_ACK;
    s.n_writes = 2u;

    uint32_t bits = 0x5555u;
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    (void)stub_run(&f, &s, 0u, 100u, 30u);

    TEST_ASSERT_EQUAL(FF_CREWSTART_READY, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL_UINT8(2u, s.writes_seen);
}

/* Silence is a failure with its own name. A radio that never answers is
 * a different problem from one that answered no, and the face says so. */
static void S02_AC18_ack_timeout_retries_then_fails_as_timeout(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    s.writes[0] = STUB_ACCEPT_SILENT;
    s.n_writes = 1u;

    uint32_t bits = 0x6666u;
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    /* Ticks of a full ack timeout each, so every retry is exercised. */
    (void)stub_run(&f, &s, 0u, FF_CREWSTART_ACK_TIMEOUT_MS, 30u);

    TEST_ASSERT_EQUAL(FF_CREWSTART_FAILED, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_TIMEOUT_ACK, ff_crewstart_failure(&f));
    TEST_ASSERT_EQUAL_UINT8(FF_CREWSTART_MAX_ATTEMPTS, s.writes_seen);
}

/* A refused send retries and then reports the refusal. */
static void S02_AC18_refused_send_fails_as_send(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    s.writes[0] = STUB_REFUSE;
    s.n_writes = 1u;

    uint32_t bits = 0x7777u;
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    (void)stub_run(&f, &s, 0u, 100u, 30u);

    TEST_ASSERT_EQUAL(FF_CREWSTART_FAILED, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_SEND, ff_crewstart_failure(&f));
    TEST_ASSERT_EQUAL_UINT8(FF_CREWSTART_MAX_ATTEMPTS, s.writes_seen);
}

/* A radio that never comes back to be read is unproven, and unproven is
 * not success. Verification is deliberately NOT retried — see
 * FF_CREWSTART_MAX_ATTEMPTS' own comment. */
static void S02_AC18_verify_timeout_fails_and_does_not_rewrite(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    s.readback = false;

    uint32_t bits = 0x8888u;
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    (void)stub_run(&f, &s, 0u, FF_CREWSTART_VERIFY_TIMEOUT_MS, 10u);

    TEST_ASSERT_EQUAL(FF_CREWSTART_FAILED, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_TIMEOUT_VERIFY, ff_crewstart_failure(&f));
    TEST_ASSERT_EQUAL_UINT8(1u, s.writes_seen);
}

/* A transport that lost the packet id cannot correlate a routing reply,
 * so the read-back is the only proof — and it still is proof. */
static void S02_AC18_no_packet_id_still_verifies_by_readback(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    s.gives_packet_id = false;

    uint32_t bits = 0x9999u;
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    (void)stub_run(&f, &s, 0u, 100u, 20u);

    TEST_ASSERT_EQUAL(FF_CREWSTART_READY, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL_UINT8(1u, s.rereads_seen);
}

/* A routing reply for somebody else's packet must not be mistaken for
 * ours. */
static void S02_AC18_a_foreign_routing_ack_is_ignored(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    s.writes[0] = STUB_ACCEPT_SILENT;
    uint32_t bits = 0xABCDu;

    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    ff_crewstart_tick(&f, 10u);
    ff_crewstart_channel_t ch;
    TEST_ASSERT_EQUAL(FF_CREWSTART_ACT_WRITE, ff_crewstart_take_action(&f, &ch));
    ff_crewstart_on_write_result(&f, true, true, 500u, 10u);

    ff_crewstart_on_routing_ack(&f, 501u, true, 20u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_WRITING, ff_crewstart_state(&f));
    ff_crewstart_on_routing_ack(&f, 500u, true, 30u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_VERIFYING, ff_crewstart_state(&f));
    (void)s;
}

/* Preconditions the machine cannot see (no link, no region) stop it
 * before anything reaches the radio, each with its own reason. */
static void S02_AC18_preconditions_stop_before_any_write(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    ff_crewstart_fail_now(&f, FF_CREWSTART_OP_START, FF_CREWSTART_FAIL_NO_LINK, 0u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAILED, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_NO_LINK, ff_crewstart_failure(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_OP_START, ff_crewstart_op(&f));

    /* No action is ever produced from a failed machine. */
    ff_crewstart_channel_t ch;
    ff_crewstart_tick(&f, 100u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_ACT_NONE, ff_crewstart_take_action(&f, &ch));

    ff_crewstart_init(&f);
    ff_crewstart_fail_now(&f, FF_CREWSTART_OP_START, FF_CREWSTART_FAIL_REGION_UNSET, 0u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_REGION_UNSET, ff_crewstart_failure(&f));
}

/* A frame already in the air cannot be un-sent, so a late precondition
 * failure must not rewrite the face into a state the radio does not
 * share. */
static void S02_AC18_fail_now_never_overrides_a_write_in_flight(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    s.writes[0] = STUB_ACCEPT_SILENT;
    uint32_t bits = 0xDEADu;

    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    stub_tick(&f, &s, 0u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_WRITING, ff_crewstart_state(&f));

    ff_crewstart_fail_now(&f, FF_CREWSTART_OP_START, FF_CREWSTART_FAIL_NO_LINK, 10u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_WRITING, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_NONE, ff_crewstart_failure(&f));
}

/* Dismiss returns a finished run to IDLE, and refuses to abandon one
 * that is still in flight. */
static void S02_AC18_dismiss_only_clears_a_finished_run(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    mc_stub_t s = stub_ok();
    uint32_t bits = 0xBEEFu;

    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    stub_tick(&f, &s, 0u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_VERIFYING, ff_crewstart_state(&f));
    ff_crewstart_dismiss(&f);
    TEST_ASSERT_EQUAL(FF_CREWSTART_VERIFYING, ff_crewstart_state(&f));

    (void)stub_run(&f, &s, 100u, 100u, 10u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_READY, ff_crewstart_state(&f));
    ff_crewstart_dismiss(&f);
    TEST_ASSERT_EQUAL(FF_CREWSTART_IDLE, ff_crewstart_state(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_OP_NONE, ff_crewstart_op(&f));
    TEST_ASSERT_EQUAL_STRING("", ff_crewstart_code(&f));
}

/* Channel rows arriving outside a verification cost nothing — the
 * ordinary every-handshake table replay must not be able to flip this
 * machine into READY. */
static void S02_AC18_channel_rows_outside_verifying_do_nothing(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);
    ff_crewstart_channel_t row;
    ff_crewstart_default_primary(&row);
    ff_crewstart_on_channel(&f, &row, 0u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_IDLE, ff_crewstart_state(&f));
}

/* Every NULL entry point is a no-op with a defined answer. */
static void S02_AC18_null_is_never_a_crash_and_never_a_claim(void)
{
    TEST_ASSERT_EQUAL(FF_CREWSTART_IDLE, ff_crewstart_state(NULL));
    TEST_ASSERT_EQUAL(FF_CREWSTART_OP_NONE, ff_crewstart_op(NULL));
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_NONE, ff_crewstart_failure(NULL));
    TEST_ASSERT_EQUAL_STRING("", ff_crewstart_code(NULL));
    TEST_ASSERT_FALSE(ff_crewstart_busy(NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, ff_crewstart_attempts(NULL));
    TEST_ASSERT_EQUAL(FF_CREWSTART_ACT_NONE, ff_crewstart_take_action(NULL, NULL));
    ff_crewstart_init(NULL);
    ff_crewstart_tick(NULL, 0u);
    ff_crewstart_dismiss(NULL);
    ff_crewstart_on_channel(NULL, NULL, 0u);
    ff_crewstart_on_routing_ack(NULL, 0u, true, 0u);
    ff_crewstart_on_write_result(NULL, true, true, 0u, 0u);
    ff_crewstart_default_primary(NULL);
    uint32_t bits = 1u;
    TEST_ASSERT_FALSE(ff_crewstart_begin_start(NULL, rng_fixed, &bits, 0u));
    TEST_ASSERT_FALSE(ff_crewstart_begin_leave(NULL, NULL, 0u));
}

/* Names are for logs and bench output; a missing one is "?" rather than
 * a NULL a printf would take personally. */
static void S02_AC18_every_state_and_reason_has_a_distinct_name(void)
{
    TEST_ASSERT_EQUAL_STRING("READY", ff_crewstart_state_name(FF_CREWSTART_READY));
    TEST_ASSERT_EQUAL_STRING("LEAVE", ff_crewstart_op_name(FF_CREWSTART_OP_LEAVE));
    TEST_ASSERT_EQUAL_STRING("MISMATCH", ff_crewstart_fail_name(FF_CREWSTART_FAIL_MISMATCH));
    TEST_ASSERT_EQUAL_STRING("?", ff_crewstart_state_name((ff_crewstart_state_t)99));
    TEST_ASSERT_EQUAL_STRING("?", ff_crewstart_op_name((ff_crewstart_op_t)99));
    TEST_ASSERT_EQUAL_STRING("?", ff_crewstart_fail_name((ff_crewstart_fail_t)99));

    /* Distinctness, so a bench log can never report two different
     * failures with one word. */
    char const *seen[10];
    for (int i = 0; i < 10; i++) {
        seen[i] = ff_crewstart_fail_name((ff_crewstart_fail_t)i);
        for (int j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(seen[i], seen[j]));
        }
    }
}

/* ------------------------------------------------------------------ */
/* The codec's encode half (A02 §1.1)                                 */
/* ------------------------------------------------------------------ */

static void S02_AC16_from_bits_is_msb_first_and_round_trips(void)
{
    char out[FF_CREWCODE_LEN + 1u];

    TEST_ASSERT_TRUE(ff_crewcode_from_bits(0u, out));
    TEST_ASSERT_EQUAL_STRING("FIRE-000000", out);

    TEST_ASSERT_TRUE(ff_crewcode_from_bits((1u << FF_CREWCODE_BITS) - 1u, out));
    TEST_ASSERT_EQUAL_STRING("FIRE-ZZZZZZ", out);

    /* MSB-first: the top five bits pick the FIRST symbol. */
    TEST_ASSERT_TRUE(ff_crewcode_from_bits(1u << 25, out));
    TEST_ASSERT_EQUAL_STRING("FIRE-100000", out);
    TEST_ASSERT_TRUE(ff_crewcode_from_bits(1u, out));
    TEST_ASSERT_EQUAL_STRING("FIRE-000001", out);

    /* Everything it mints, the decoder reads back as the same code. */
    for (uint32_t b = 0; b < 4096u; b++) {
        uint32_t const bits = b * 262143u % (1u << FF_CREWCODE_BITS);
        TEST_ASSERT_TRUE(ff_crewcode_from_bits(bits, out));
        TEST_ASSERT_TRUE(ff_crewcode_valid(out));
        char canon[FF_CREWCODE_LEN + 1u];
        TEST_ASSERT_TRUE(ff_crewcode_parse(out, canon));
        TEST_ASSERT_EQUAL_STRING(out, canon);
    }
}

/* Out-of-range is rejected, not masked — a caller handing over 32
 * meaningful bits finds out rather than silently getting 30. */
static void S02_AC16_from_bits_rejects_out_of_range(void)
{
    char out[FF_CREWCODE_LEN + 1u];
    memset(out, 'x', sizeof(out));
    TEST_ASSERT_FALSE(ff_crewcode_from_bits(1u << FF_CREWCODE_BITS, out));
    TEST_ASSERT_FALSE(ff_crewcode_from_bits(0xFFFFFFFFu, out));
    TEST_ASSERT_EQUAL_CHAR('x', out[0]); /* untouched on failure */
    TEST_ASSERT_FALSE(ff_crewcode_from_bits(0u, NULL));
}

/* `consider` is what a confirm face uses: it names the operation without
 * starting it, and it must never re-label a write already in the air. */
static void S02_AC16_consider_names_the_op_without_starting_it(void)
{
    ff_crewstart_t f;
    ff_crewstart_init(&f);

    ff_crewstart_consider(&f, FF_CREWSTART_OP_LEAVE);
    TEST_ASSERT_EQUAL(FF_CREWSTART_OP_LEAVE, ff_crewstart_op(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_IDLE, ff_crewstart_state(&f));
    ff_crewstart_channel_t ch;
    ff_crewstart_tick(&f, 10u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_ACT_NONE, ff_crewstart_take_action(&f, &ch));

    /* It also clears a finished run's result, so a fresh confirm face
     * never shows the last run's failure. */
    ff_crewstart_fail_now(&f, FF_CREWSTART_OP_START, FF_CREWSTART_FAIL_NAK, 20u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_NAK, ff_crewstart_failure(&f));
    ff_crewstart_consider(&f, FF_CREWSTART_OP_START);
    TEST_ASSERT_EQUAL(FF_CREWSTART_FAIL_NONE, ff_crewstart_failure(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_IDLE, ff_crewstart_state(&f));

    /* And it is a no-op mid-run. */
    mc_stub_t s = stub_ok();
    s.writes[0] = STUB_ACCEPT_SILENT;
    uint32_t bits = 0x1234u;
    TEST_ASSERT_TRUE(ff_crewstart_begin_start(&f, rng_fixed, &bits, 0u));
    stub_tick(&f, &s, 0u);
    TEST_ASSERT_EQUAL(FF_CREWSTART_WRITING, ff_crewstart_state(&f));
    ff_crewstart_consider(&f, FF_CREWSTART_OP_LEAVE);
    TEST_ASSERT_EQUAL(FF_CREWSTART_OP_START, ff_crewstart_op(&f));
    TEST_ASSERT_EQUAL(FF_CREWSTART_WRITING, ff_crewstart_state(&f));

    ff_crewstart_consider(NULL, FF_CREWSTART_OP_START);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S02_AC16_consider_names_the_op_without_starting_it);
    RUN_TEST(S02_AC16_start_mints_writes_verifies_and_lands_ready);
    RUN_TEST(S02_AC16_written_channel_is_the_spec_channel);
    RUN_TEST(S02_AC16_written_psk_is_the_one_the_code_derives);
    RUN_TEST(S02_AC16_thirty_bits_are_taken_from_a_thirty_two_bit_draw);
    RUN_TEST(S02_AC16_a_second_start_while_busy_is_ignored);
    RUN_TEST(S02_AC16_from_bits_is_msb_first_and_round_trips);
    RUN_TEST(S02_AC16_from_bits_rejects_out_of_range);
    RUN_TEST(S02_AC17_leave_restores_the_snapshot_verbatim);
    RUN_TEST(S02_AC17_leave_with_no_snapshot_fails_honestly);
    RUN_TEST(S02_AC17_default_primary_is_meshtastics_stock_channel);
    RUN_TEST(S02_AC17_snapshot_round_trips_through_a_blob);
    RUN_TEST(S02_AC17_snapshot_holds_a_full_length_key);
    RUN_TEST(S02_AC17_corrupt_snapshot_loads_nothing);
    RUN_TEST(S02_AC18_no_entropy_source_mints_nothing);
    RUN_TEST(S02_AC18_readback_mismatch_is_a_failure_not_a_success);
    RUN_TEST(S02_AC18_readback_with_the_right_name_and_wrong_key_fails);
    RUN_TEST(S02_AC18_nak_retries_then_fails_as_nak);
    RUN_TEST(S02_AC18_a_nak_then_an_ack_still_lands_ready);
    RUN_TEST(S02_AC18_ack_timeout_retries_then_fails_as_timeout);
    RUN_TEST(S02_AC18_refused_send_fails_as_send);
    RUN_TEST(S02_AC18_verify_timeout_fails_and_does_not_rewrite);
    RUN_TEST(S02_AC18_no_packet_id_still_verifies_by_readback);
    RUN_TEST(S02_AC18_a_foreign_routing_ack_is_ignored);
    RUN_TEST(S02_AC18_preconditions_stop_before_any_write);
    RUN_TEST(S02_AC18_fail_now_never_overrides_a_write_in_flight);
    RUN_TEST(S02_AC18_dismiss_only_clears_a_finished_run);
    RUN_TEST(S02_AC18_channel_rows_outside_verifying_do_nothing);
    RUN_TEST(S02_AC18_null_is_never_a_crash_and_never_a_claim);
    RUN_TEST(S02_AC18_every_state_and_reason_has_a_distinct_name);
    return UNITY_END();
}
