/**
 * test_t9.c — S08 core/t9 acceptance criteria, slice (a) only.
 *
 * Test names follow docs/specs/S08-signals-t9.md's numbered acceptance
 * criteria: S08_ACn_description. Criteria 3-6 (feed / wiring / goldens /
 * canned replies) are out of scope for this slice — see the PR checklist.
 *
 * Tests below inspect `ff_t9_t` fields directly (buf/len/has_pending/...)
 * in addition to `ff_t9_text()` where that's the only way to distinguish
 * "committed" from "pending-but-displays-the-same-string" states — the
 * struct is fully defined (not opaque) specifically so callers/tests can
 * do this, matching the ff_crew_t precedent in this codebase.
 */
#include <string.h>

#include "unity.h"

#include "ff_t9.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* AC1 — multi-tap core: omw sequence, cycling, timer/key commits,      */
/* backspace                                                            */
/* ------------------------------------------------------------------- */

static void S08_AC1_omw_sequence_with_waits(void)
{
    /* "omw": 6=mno cycled twice to 'o' (666 with short waits), a long
     * wait (timer) commits it; 6 pressed once more for 'm', a different
     * key (9) commits it; 9 pressed once for 'w', a final timer commit
     * flushes it. Mirrors the spec's own worked example verbatim. */
    ff_t9_t t;
    ff_t9_reset(&t);

    ff_t9_key(&t, 6, 0);    /* pending 'm' */
    ff_t9_key(&t, 6, 200);  /* +200ms < 900 -> cycle 'n' */
    ff_t9_key(&t, 6, 400);  /* +200ms < 900 -> cycle 'o' */
    ff_t9_tick(&t, 1400);   /* +1000ms >= 900 -> commit 'o' */
    TEST_ASSERT_EQUAL_STRING("o", ff_t9_text(&t));

    ff_t9_key(&t, 6, 1500); /* fresh pending 'm' (timer already expired) */
    ff_t9_tick(&t, 2500);   /* +1000ms -> commit 'm' */
    TEST_ASSERT_EQUAL_STRING("om", ff_t9_text(&t));

    ff_t9_key(&t, 9, 2600); /* pending 'w' */
    ff_t9_tick(&t, 3600);   /* +1000ms -> commit 'w' */

    TEST_ASSERT_EQUAL_STRING("omw", ff_t9_text(&t));
    TEST_ASSERT_EQUAL_UINT8(3, t.len);
    TEST_ASSERT_FALSE(t.has_pending);
}

static void S08_AC1_rapid_same_key_cycles_a_b_c_and_wraps(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    ff_t9_key(&t, 2, 0);
    TEST_ASSERT_EQUAL_STRING("a", ff_t9_text(&t));

    ff_t9_key(&t, 2, 100); /* well under 900ms -> cycles, does not commit */
    TEST_ASSERT_EQUAL_STRING("b", ff_t9_text(&t));
    TEST_ASSERT_EQUAL_UINT8(0, t.len); /* nothing committed yet */

    ff_t9_key(&t, 2, 200);
    TEST_ASSERT_EQUAL_STRING("c", ff_t9_text(&t));

    ff_t9_key(&t, 2, 300); /* 4th press of a 3-letter table -> wraps */
    TEST_ASSERT_EQUAL_STRING("a", ff_t9_text(&t));
    TEST_ASSERT_EQUAL_UINT8(0, t.len);
}

static void S08_AC1_timer_boundary_899ms_pending_900ms_commits(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    ff_t9_key(&t, 2, 0); /* pending 'a' at t=0 */

    ff_t9_tick(&t, 899); /* elapsed 899 < 900 -> still pending */
    TEST_ASSERT_TRUE(t.has_pending);
    TEST_ASSERT_EQUAL_UINT8(0, t.len);
    TEST_ASSERT_EQUAL_STRING("a", ff_t9_text(&t));

    ff_t9_tick(&t, 900); /* elapsed 900 >= 900 -> commits */
    TEST_ASSERT_FALSE(t.has_pending);
    TEST_ASSERT_EQUAL_UINT8(1, t.len);
    TEST_ASSERT_EQUAL_STRING("a", ff_t9_text(&t));
}

static void S08_AC1_same_key_repeat_at_exactly_900ms_commits_not_cycles(void)
{
    /* Half-open window: a same-key repeat landing at EXACTLY 900ms is
     * treated as "the old pending already timed out", so it commits the
     * old char and starts a brand-new pending cycle (index 0 again) —
     * it must NOT cycle 'a'->'b'. */
    ff_t9_t t;
    ff_t9_reset(&t);

    ff_t9_key(&t, 2, 0);   /* pending 'a' */
    ff_t9_key(&t, 2, 900); /* elapsed == 900 -> commit 'a', fresh pending 'a' */

    TEST_ASSERT_EQUAL_UINT8(1, t.len);
    TEST_ASSERT_EQUAL_STRING("a", t.buf);
    TEST_ASSERT_TRUE(t.has_pending);
    TEST_ASSERT_EQUAL_CHAR('a', t.pending_char); /* fresh cycle, not 'b' */
    TEST_ASSERT_EQUAL_STRING("aa", ff_t9_text(&t));
}

static void S08_AC1_different_key_commits_pending(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    ff_t9_key(&t, 2, 0);   /* pending 'a' */
    ff_t9_key(&t, 3, 100); /* different key -> commit 'a', pending 'd' */

    TEST_ASSERT_EQUAL_UINT8(1, t.len);
    TEST_ASSERT_EQUAL_STRING("a", t.buf);
    TEST_ASSERT_TRUE(t.has_pending);
    TEST_ASSERT_EQUAL_CHAR('d', t.pending_char);
    TEST_ASSERT_EQUAL_STRING("ad", ff_t9_text(&t));
}

static void S08_AC1_backspace_removes_pending_first_then_committed(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    ff_t9_key(&t, 2, 0);   /* pending 'a' */
    ff_t9_key(&t, 3, 100); /* commits 'a', pending 'd' */
    TEST_ASSERT_EQUAL_STRING("ad", ff_t9_text(&t));

    ff_t9_backspace(&t); /* removes pending 'd' only */
    TEST_ASSERT_FALSE(t.has_pending);
    TEST_ASSERT_EQUAL_UINT8(1, t.len);
    TEST_ASSERT_EQUAL_STRING("a", ff_t9_text(&t));

    ff_t9_backspace(&t); /* now removes committed 'a' */
    TEST_ASSERT_EQUAL_UINT8(0, t.len);
    TEST_ASSERT_EQUAL_STRING("", ff_t9_text(&t));

    ff_t9_backspace(&t); /* empty: no-op, must not underflow/crash */
    TEST_ASSERT_EQUAL_UINT8(0, t.len);
    TEST_ASSERT_EQUAL_STRING("", ff_t9_text(&t));
}

static void S08_AC1_reset_clears_pending_and_committed(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);
    ff_t9_key(&t, 2, 0);
    ff_t9_key(&t, 3, 100);
    TEST_ASSERT_EQUAL_STRING("ad", ff_t9_text(&t));

    ff_t9_reset(&t);
    TEST_ASSERT_EQUAL_UINT8(0, t.len);
    TEST_ASSERT_FALSE(t.has_pending);
    TEST_ASSERT_EQUAL_STRING("", ff_t9_text(&t));
}

/* ------------------------------------------------------------------- */
/* AC2 — punctuation, space, 160-char cap                               */
/* ------------------------------------------------------------------- */

static void S08_AC2_punctuation_key_cycles_in_spec_order_and_wraps(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    ff_t9_key(&t, 1, 0);
    TEST_ASSERT_EQUAL_STRING(".", ff_t9_text(&t));
    ff_t9_key(&t, 1, 100);
    TEST_ASSERT_EQUAL_STRING(",", ff_t9_text(&t));
    ff_t9_key(&t, 1, 200);
    TEST_ASSERT_EQUAL_STRING("?", ff_t9_text(&t));
    ff_t9_key(&t, 1, 300);
    TEST_ASSERT_EQUAL_STRING("!", ff_t9_text(&t));
    ff_t9_key(&t, 1, 400); /* 5th press of a 4-char table -> wraps */
    TEST_ASSERT_EQUAL_STRING(".", ff_t9_text(&t));
}

static void S08_AC2_space_commits_pending_and_appends_space(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    ff_t9_key(&t, 2, 0); /* pending 'a' */
    ff_t9_space(&t);

    TEST_ASSERT_FALSE(t.has_pending);
    TEST_ASSERT_EQUAL_UINT8(2, t.len);
    TEST_ASSERT_EQUAL_STRING("a ", ff_t9_text(&t));
}

static void S08_AC2_key0_and_ff_t9_space_are_equivalent(void)
{
    ff_t9_t a, b;
    ff_t9_reset(&a);
    ff_t9_reset(&b);

    ff_t9_key(&a, 2, 0);
    ff_t9_key(&b, 2, 0);

    ff_t9_key(&a, 0, 999999u); /* arbitrary now_ms: must not matter */
    ff_t9_space(&b);

    TEST_ASSERT_EQUAL_STRING(ff_t9_text(&b), ff_t9_text(&a));
    TEST_ASSERT_EQUAL_UINT8(b.len, a.len);
    TEST_ASSERT_EQUAL(b.has_pending, a.has_pending);
}

static void S08_AC2_160_char_cap_exactly_160_accepted_161st_rejected(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    for (int i = 0; i < FF_T9_MAX_LEN; i++) {
        ff_t9_space(&t);
    }
    TEST_ASSERT_EQUAL_UINT8(FF_T9_MAX_LEN, t.len);
    TEST_ASSERT_EQUAL_UINT(FF_T9_MAX_LEN, (unsigned)strlen(ff_t9_text(&t)));
    for (int i = 0; i < FF_T9_MAX_LEN; i++) {
        TEST_ASSERT_EQUAL_CHAR(' ', t.buf[i]);
    }

    ff_t9_space(&t); /* 161st char attempt via space: rejected */
    TEST_ASSERT_EQUAL_UINT8(FF_T9_MAX_LEN, t.len);
    TEST_ASSERT_EQUAL_UINT(FF_T9_MAX_LEN, (unsigned)strlen(ff_t9_text(&t)));

    ff_t9_key(&t, 2, 12345u); /* 161st char attempt via letter key: rejected too */
    TEST_ASSERT_FALSE(t.has_pending);
    TEST_ASSERT_EQUAL_UINT8(FF_T9_MAX_LEN, t.len);
    TEST_ASSERT_EQUAL_UINT(FF_T9_MAX_LEN, (unsigned)strlen(ff_t9_text(&t)));
}

static void S08_AC2_pending_char_committed_exactly_at_cap_blocks_next_pending(void)
{
    /* Boundary case distinct from the "already full" test above: the
     * pending char is what PUSHES the committed length from 159 to
     * exactly 160 — verifies the cap check runs AFTER that commit, not
     * before, and that a fresh pending is then correctly refused. */
    ff_t9_t t;
    ff_t9_reset(&t);

    for (int i = 0; i < FF_T9_MAX_LEN - 1; i++) {
        ff_t9_space(&t);
    }
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(FF_T9_MAX_LEN - 1), t.len);

    ff_t9_key(&t, 2, 0); /* pending 'a': len still 159, cap not yet hit */
    TEST_ASSERT_TRUE(t.has_pending);
    TEST_ASSERT_EQUAL_UINT(FF_T9_MAX_LEN, (unsigned)strlen(ff_t9_text(&t)));

    ff_t9_key(&t, 3, 100); /* different key: commits 'a' -> len hits 160 exactly */
    TEST_ASSERT_EQUAL_UINT8(FF_T9_MAX_LEN, t.len);
    TEST_ASSERT_EQUAL_CHAR('a', t.buf[FF_T9_MAX_LEN - 1]);
    /* No pending 'd' got started: the cap check ran after the commit. */
    TEST_ASSERT_FALSE(t.has_pending);
    TEST_ASSERT_EQUAL_UINT(FF_T9_MAX_LEN, (unsigned)strlen(ff_t9_text(&t)));
}

/* ------------------------------------------------------------------- */
/* ff_t9_insert_text (S08 slice c/d, SYM-page ASCII-emoticon shortcuts) */
/* ------------------------------------------------------------------- */

static void S08_insert_text_appends_atomically_to_empty_buffer(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    bool ok = ff_t9_insert_text(&t, ":)");

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING(":)", ff_t9_text(&t));
    TEST_ASSERT_EQUAL_UINT8(2, t.len);
    TEST_ASSERT_FALSE(t.has_pending);
}

static void S08_insert_text_commits_pending_char_first(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    ff_t9_key(&t, 2, 0); /* pending 'a', not yet committed */
    TEST_ASSERT_EQUAL_UINT8(0, t.len);

    bool ok = ff_t9_insert_text(&t, "<3");

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(t.has_pending);
    TEST_ASSERT_EQUAL_STRING("a<3", ff_t9_text(&t));
}

static void S08_insert_text_that_fits_exactly_at_cap_is_accepted(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    char filler[FF_T9_MAX_LEN - 1]; /* leaves exactly 2 bytes of budget */
    memset(filler, 'x', sizeof(filler));
    filler[sizeof(filler) - 1] = '\0';
    TEST_ASSERT_TRUE(ff_t9_insert_text(&t, filler));
    TEST_ASSERT_EQUAL_UINT8(FF_T9_MAX_LEN - 2, t.len);

    bool ok = ff_t9_insert_text(&t, ":)"); /* exactly the remaining 2 bytes */

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(FF_T9_MAX_LEN, t.len);
    TEST_ASSERT_EQUAL_UINT(FF_T9_MAX_LEN, (unsigned)strlen(ff_t9_text(&t)));
}

static void S08_insert_text_that_overflows_cap_is_rejected_and_leaves_t_untouched(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    char filler[FF_T9_MAX_LEN]; /* fills the buffer completely, 1 byte short */
    memset(filler, 'x', sizeof(filler) - 1);
    filler[sizeof(filler) - 1] = '\0';
    TEST_ASSERT_TRUE(ff_t9_insert_text(&t, filler));
    TEST_ASSERT_EQUAL_UINT8(FF_T9_MAX_LEN - 1, t.len);

    /* A pending char is still legal to start (len < cap)... */
    ff_t9_key(&t, 2, 0); /* pending 'a' */
    TEST_ASSERT_TRUE(t.has_pending);

    /* ...but ":)" needs 2 committed bytes and only 0 remain once that
     * pending char also commits (len would become MAX_LEN, leaving 0
     * budget) — must be rejected all-or-nothing, and MUST NOT silently
     * commit the pending char as a side effect of the failed attempt
     * (mutation-check: a buggy "commit first, THEN check" ordering would
     * pass a length-only assertion but corrupt has_pending). */
    uint8_t len_before = t.len;
    bool has_pending_before = t.has_pending;
    char pending_char_before = t.pending_char;

    bool ok = ff_t9_insert_text(&t, ":)");

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_UINT8(len_before, t.len);
    TEST_ASSERT_EQUAL(has_pending_before, t.has_pending);
    TEST_ASSERT_EQUAL(pending_char_before, t.pending_char);
}

static void S08_insert_text_empty_string_still_commits_pending(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);
    ff_t9_key(&t, 2, 0); /* pending 'a' */

    bool ok = ff_t9_insert_text(&t, "");

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(t.has_pending);
    TEST_ASSERT_EQUAL_STRING("a", ff_t9_text(&t));
}

static void S08_insert_text_then_backspace_removes_one_char_at_a_time(void)
{
    /* The classic multi-char-shortcut bug: backspace must remove exactly
     * one character of the inserted string per press, not the whole
     * shortcut atomically and not a partial/corrupted remainder. Since
     * ff_t9_insert_text only ever accepts single-byte ASCII (see its
     * header doc), the existing byte-at-a-time ff_t9_backspace is already
     * correct for it — this test pins that down explicitly so a future
     * change can't silently break it. */
    ff_t9_t t;
    ff_t9_reset(&t);
    ff_t9_insert_text(&t, ":)");

    ff_t9_backspace(&t);
    TEST_ASSERT_EQUAL_STRING(":", ff_t9_text(&t));

    ff_t9_backspace(&t);
    TEST_ASSERT_EQUAL_STRING("", ff_t9_text(&t));
}

static void S08_insert_text_null_args_are_safe_and_return_false(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    TEST_ASSERT_FALSE(ff_t9_insert_text(NULL, ":)"));
    TEST_ASSERT_FALSE(ff_t9_insert_text(&t, NULL));
    TEST_ASSERT_EQUAL_STRING("", ff_t9_text(&t)); /* untouched */
}

/* PR #25 code review, LOW finding: the ASCII-only contract is enforced
 * at runtime, not just documented — a future caller reusing this
 * function with multi-byte UTF-8 must be rejected outright, not
 * silently corrupt a later byte-at-a-time backspace. */
static void S08_insert_text_rejects_a_high_bit_byte_and_leaves_t_untouched(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);
    ff_t9_insert_text(&t, "hi "); /* some prior committed state to prove is untouched */

    /* "\xF0\x9F\x94\xA5" is the (well-formed) 4-byte UTF-8 encoding of
     * U+1F525 FIRE — exactly the kind of string a hypothetical real-emoji
     * caller (issue #22) might mistakenly pass to this ASCII-only
     * function. Every byte has its high bit set. */
    bool ok = ff_t9_insert_text(&t, "\xF0\x9F\x94\xA5");

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("hi ", ff_t9_text(&t)); /* unchanged, not partially appended */
    TEST_ASSERT_EQUAL_UINT8(3, t.len);
}

static void S08_insert_text_rejects_if_ANY_byte_is_high_bit_even_with_ascii_around_it(void)
{
    /* Mutation-check: a check that only looks at s[0] (or only the LAST
     * byte) would pass this — the reject must scan every byte. */
    ff_t9_t t;
    ff_t9_reset(&t);

    bool ok = ff_t9_insert_text(&t, "ok\x80ok");

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("", ff_t9_text(&t));
    TEST_ASSERT_EQUAL_UINT8(0, t.len);
}

static void S08_insert_text_rejects_high_bit_byte_even_when_pending_char_exists(void)
{
    /* The all-or-nothing/untouched contract must hold for the ASCII
     * check too, not just the capacity check — a pending char must
     * survive a rejected non-ASCII insert exactly like it survives a
     * rejected over-cap insert. */
    ff_t9_t t;
    ff_t9_reset(&t);
    ff_t9_key(&t, 2, 0); /* pending 'a' */

    bool ok = ff_t9_insert_text(&t, "\xFF");

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(t.has_pending);
    TEST_ASSERT_EQUAL_STRING("a", ff_t9_text(&t)); /* pending char still live, not committed or lost */
}

static void S08_insert_text_accepts_full_printable_ascii_range(void)
{
    /* The boundary immediately below the rejection threshold (0x7F DEL,
     * the highest 7-bit ASCII value) must still be accepted — this isn't
     * a "reject anything unusual" filter, specifically "reject >= 0x80". */
    ff_t9_t t;
    ff_t9_reset(&t);

    char s[2] = {(char)0x7F, '\0'};
    bool ok = ff_t9_insert_text(&t, s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(1, t.len);
}

/* ------------------------------------------------------------------- */
/* Guard paths (not spec-numbered, but load-bearing)                    */
/* ------------------------------------------------------------------- */

static void S08_guard_null_pointer_calls_are_safe_noops(void)
{
    ff_t9_reset(NULL);
    ff_t9_key(NULL, 2, 0);
    ff_t9_tick(NULL, 1000);
    ff_t9_backspace(NULL);
    ff_t9_space(NULL);
    TEST_ASSERT_EQUAL_STRING("", ff_t9_text(NULL));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9_suggest(NULL, NULL, 0));
}

static void S08_guard_invalid_key_above_9_is_ignored(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);

    ff_t9_key(&t, 10, 0); /* first invalid value above the valid 0-9 range */

    TEST_ASSERT_FALSE(t.has_pending);
    TEST_ASSERT_EQUAL_UINT8(0, t.len);
    TEST_ASSERT_EQUAL_STRING("", ff_t9_text(&t));
}

static void S08_guard_suggest_stub_returns_zero_and_leaves_out_untouched(void)
{
    ff_t9_t t;
    ff_t9_reset(&t);
    ff_t9_key(&t, 2, 0); /* non-empty state, in case that ever mattered */

    char const *sentinel = "sentinel";
    char const *out[2] = {sentinel, sentinel};

    size_t n = ff_t9_suggest(&t, out, 2);

    TEST_ASSERT_EQUAL_UINT(0, (unsigned)n);
    TEST_ASSERT_TRUE(out[0] == sentinel);
    TEST_ASSERT_TRUE(out[1] == sentinel);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S08_AC1_omw_sequence_with_waits);
    RUN_TEST(S08_AC1_rapid_same_key_cycles_a_b_c_and_wraps);
    RUN_TEST(S08_AC1_timer_boundary_899ms_pending_900ms_commits);
    RUN_TEST(S08_AC1_same_key_repeat_at_exactly_900ms_commits_not_cycles);
    RUN_TEST(S08_AC1_different_key_commits_pending);
    RUN_TEST(S08_AC1_backspace_removes_pending_first_then_committed);
    RUN_TEST(S08_AC1_reset_clears_pending_and_committed);

    RUN_TEST(S08_AC2_punctuation_key_cycles_in_spec_order_and_wraps);
    RUN_TEST(S08_AC2_space_commits_pending_and_appends_space);
    RUN_TEST(S08_AC2_key0_and_ff_t9_space_are_equivalent);
    RUN_TEST(S08_AC2_160_char_cap_exactly_160_accepted_161st_rejected);
    RUN_TEST(S08_AC2_pending_char_committed_exactly_at_cap_blocks_next_pending);

    RUN_TEST(S08_insert_text_appends_atomically_to_empty_buffer);
    RUN_TEST(S08_insert_text_commits_pending_char_first);
    RUN_TEST(S08_insert_text_that_fits_exactly_at_cap_is_accepted);
    RUN_TEST(S08_insert_text_that_overflows_cap_is_rejected_and_leaves_t_untouched);
    RUN_TEST(S08_insert_text_empty_string_still_commits_pending);
    RUN_TEST(S08_insert_text_then_backspace_removes_one_char_at_a_time);
    RUN_TEST(S08_insert_text_null_args_are_safe_and_return_false);
    RUN_TEST(S08_insert_text_rejects_a_high_bit_byte_and_leaves_t_untouched);
    RUN_TEST(S08_insert_text_rejects_if_ANY_byte_is_high_bit_even_with_ascii_around_it);
    RUN_TEST(S08_insert_text_rejects_high_bit_byte_even_when_pending_char_exists);
    RUN_TEST(S08_insert_text_accepts_full_printable_ascii_range);

    RUN_TEST(S08_guard_null_pointer_calls_are_safe_noops);
    RUN_TEST(S08_guard_invalid_key_above_9_is_ignored);
    RUN_TEST(S08_guard_suggest_stub_returns_zero_and_leaves_out_untouched);

    return UNITY_END();
}
