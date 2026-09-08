/**
 * fuzz_t9pred.c — fuzzes the T9 predictive dictionary/prediction engine
 * (ff_t9pred_match/_match_str/_session_*), the "T9 dictionary/prediction
 * inputs" fuzz target. Two related but distinct input surfaces:
 *
 *  1. ff_t9pred_match_ex() taking a raw uint8_t digit sequence directly
 *     (bytes are folded into 2..9 so the fuzzer spends its time inside
 *     digits_valid()'s accepted range, exercising the actual dictionary
 *     walk/match logic rather than bouncing off the first bounds check
 *     every call — an unfolded raw-byte variant would rarely land in
 *     2..9 by chance and mostly test digits_valid() itself, which is
 *     three lines).
 *  2. ff_t9pred_match_str(), which additionally parses a NUL-terminated
 *     ASCII '2'..'9' string — fed the raw fuzzer bytes UNFOLDED here
 *     specifically so out-of-range/non-digit bytes reach its own
 *     character-validation loop.
 *  3. The session API (ff_t9pred_session_key/backspace/cycle/select/
 *     current), driven by a short scripted sequence of fuzzer-controlled
 *     "keys" — exercises FF_T9PRED_MAX_DIGITS-boundary session state,
 *     not just one-shot matching.
 *
 * "extra" supplementary words (festpack artist names, etc. — see
 * ff_t9pred_match_ex()'s doc comment) are exercised with a small
 * fuzzer-influenced set of strings built from the same input, since the
 * de-dup logic in ff_t9pred.c (in_extra/ci_equal) is otherwise never
 * reached in a match-only fuzz corpus.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "ff_t9pred.h"

#define MAX_OUT 64u

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    if (size == 0) {
        return 0;
    }

    /* --- 1. Raw digit-sequence match, folded into the valid 2..9 range. */
    {
        uint8_t digits[FF_T9PRED_MAX_DIGITS + 4u];
        size_t n = size;
        if (n > sizeof(digits)) {
            n = sizeof(digits); /* intentionally sometimes > FF_T9PRED_MAX_DIGITS, to exercise the reject path */
        }
        for (size_t i = 0; i < n; i++) {
            digits[i] = (uint8_t)((data[i] % 8u) + 2u); /* 2..9 */
        }
        char const *out[MAX_OUT];
        size_t written = ff_t9pred_match(digits, n, out, MAX_OUT);
        if (written > MAX_OUT) {
            __builtin_trap();
        }
        size_t counted = ff_t9pred_count(digits, n);
        if (n <= FF_T9PRED_MAX_DIGITS && written < MAX_OUT && written != counted) {
            /* match() and count() must agree when the output cap wasn't hit. */
            __builtin_trap();
        }
    }

    /* --- 2. String-form match: raw bytes verbatim (not folded), so most
     *        inputs exercise the "invalid digit char" rejection path, and
     *        the rare all-'2'-'9' inputs exercise the real match. */
    {
        char qbuf[FF_T9PRED_MAX_DIGITS + 8u];
        size_t n = size;
        if (n > sizeof(qbuf) - 1u) {
            n = sizeof(qbuf) - 1u;
        }
        memcpy(qbuf, data, n);
        qbuf[n] = '\0';
        /* A NUL byte embedded earlier in `data` legitimately shortens the
         * string ff_t9pred_match_str() sees — that's real strlen()
         * semantics for a NUL-terminated C API, not a bug to flag. */
        char const *out[MAX_OUT];
        (void)ff_t9pred_match_str(qbuf, out, MAX_OUT);
    }

    /* --- 3. Session API driven by a short scripted sequence, plus a
     *        fuzzer-derived "extra" supplementary word list. */
    {
        char extra_buf[3][16];
        char const *extra[3];
        size_t off = 0;
        for (int w = 0; w < 3; w++) {
            size_t wl = 0;
            while (wl < sizeof(extra_buf[w]) - 1u && off < size) {
                uint8_t b = data[off++];
                if (b == 0u) {
                    break;
                }
                /* Keep it ASCII letters so word_matches()'s letter_digit()
                 * mapping is actually exercised (a non-letter byte just
                 * maps to digit 0, which can never match — legal but
                 * less interesting). */
                extra_buf[w][wl++] = (char)('a' + (b % 26u));
            }
            extra_buf[w][wl] = '\0';
            extra[w] = extra_buf[w];
        }

        ff_t9pred_session_t s;
        ff_t9pred_session_reset(&s);
        ff_t9pred_session_set_extra(&s, extra, 3);

        for (size_t i = 0; i < size && i < 32u; i++) {
            uint8_t op = data[i] & 0x07u;
            switch (op) {
            case 0:
            case 1:
            case 2:
                (void)ff_t9pred_session_key(&s, (uint8_t)((data[i] % 8u) + 2u));
                break;
            case 3:
                (void)ff_t9pred_session_backspace(&s);
                break;
            case 4:
                ff_t9pred_session_cycle(&s);
                break;
            case 5:
                ff_t9pred_session_select(&s, (uint16_t)data[i]);
                break;
            case 6: {
                char const *cur = ff_t9pred_session_current(&s);
                (void)cur;
                break;
            }
            default: {
                char const *out[MAX_OUT];
                size_t got = ff_t9pred_session_candidates(&s, out, MAX_OUT);
                if (got > MAX_OUT) {
                    __builtin_trap();
                }
                break;
            }
            }
            if (s.n > FF_T9PRED_MAX_DIGITS) {
                __builtin_trap();
            }
        }
    }

    return 0;
}
