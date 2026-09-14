/**
 * ff_crewcode.h — core/crewcode: the Firefly crew code, and the key it
 * derives.
 *
 * Spec: docs/specs/A02-crew-join.md §1 (shape, normalisation, why the
 * Meshtastic channel NAME *is* the code, HKDF derivation, the deep link)
 * and docs/specs/S02-core-crew.md's 2026-09-13 amendment §D (the puck
 * half — the puck derives the code it displays from its own channel
 * name, so there is no second source of truth and nothing extra to
 * persist).
 *
 * Byte-exact against `docs/specs/fixtures/A02-crew-codes.json`, which is
 * shared verbatim with the app's Swift `CrewCode` — one fixture, two
 * languages, no drift.
 *
 * ## What a crew code is
 *
 *     FIRE-4K9M7X
 *     └──┘ └────┘
 *      tag  6 symbols, Crockford base32, 30 bits
 *
 * The canonical form is exactly 11 ASCII characters, which is exactly
 * `ChannelSettings.name`'s usable budget ("Less than 12 bytes",
 * channel.proto). That identity is the whole design: a typed code is
 * sufficient to join, and a puck can read its own crew code off its own
 * channel table.
 *
 * ## Honest-data note
 *
 * Every entry point here either succeeds with a canonical result or
 * fails. There is no partial parse, no fallback code, and no checksum
 * (A02 §1.2: a mistyped code derives a different PSK, so nothing
 * decrypts — the honest feedback is "nobody heard yet", not a guess).
 * A channel name that is not a valid code is NOT a crew code, and the
 * UI says "no crew code yet" rather than rendering the name as one.
 *
 * ## Threat model, restated so nothing downstream over-claims
 *
 * 30 bits is a PRIVACY FENCE, not a security boundary (A02 §1.6): it
 * keeps other people at the festival out of your crew's traffic; it does
 * not stop anyone who actually wants in (the whole space brute-forces
 * offline from one captured packet). Never word a screen, a log line or
 * a doc as though it did.
 *
 * Pure C11, zero dependencies, no I/O, no allocation — same contract as
 * every other core module. The SHA-256/HMAC/HKDF implementation is
 * vendored inline in ff_crewcode.c (see that file's header for why core's
 * zero-dependency rule is paid for in ~140 lines here rather than by
 * pulling mbedTLS into a pure module).
 */
#ifndef FF_CREWCODE_H
#define FF_CREWCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The `FIRE-` tag, without its NUL. */
#define FF_CREWCODE_TAG "FIRE-"

/** Symbols after the tag — 6 × 5 bits = 30 bits of entropy (A02 §1.1). */
#define FF_CREWCODE_SYMBOLS 6u

/** Canonical length in ASCII characters: `FIRE-` + 6. Exactly the 11
 * usable bytes of `ChannelSettings.name`, which is what makes the
 * channel name and the code the same thing (A02 §1.3). */
#define FF_CREWCODE_LEN 11u

/** Derived pre-shared key length — AES256 (A02 §1.5). */
#define FF_CREWCODE_PSK_LEN 32u

/** Buffer size for `ff_crewcode_invite_url` including the NUL.
 *
 * `firefly://crew?v=1&code=FIRE-4K9M7X` is 35 chars; the optional
 * `&name=` clamps at 24 DECODED characters, each of which can
 * percent-encode to at most 12 bytes (a 4-byte UTF-8 code point ->
 * `%XX%XX%XX%XX`... worst case per decoded *character*, counted
 * conservatively as 3 bytes per input byte over a 24-BYTE clamp), so
 * 35 + 6 + 72 + 1 = 114. Rounded to 128 so a future parameter has room
 * without every caller's buffer changing size. */
#define FF_CREWCODE_URL_MAX 128u

/** Crockford base32, minus `I`, `L`, `O` and `U` (A02 §1.1). 32 symbols,
 * NUL-terminated, MSB-first when read as a 30-bit integer. */
extern char const ff_crewcode_alphabet[33];

/**
 * ff_crewcode_parse — normalise anything a human typed (or a channel
 * name) into the canonical code, or fail.
 *
 * The steps, in this order and no other (A02 §1.2 — the ordering is
 * pinned, not incidental):
 *   1. trim ASCII whitespace, uppercase;
 *   2. strip every space and `-`;
 *   3. strip a LEADING literal `FIRE` if present — before step 4, so the
 *      tag is matched literally. Consequence, pinned rather than
 *      discovered: a crew whose six symbols are `F1RE9X` parses from the
 *      full spelling `FIRE-FIRE9X`, but the tagless `FIRE9X` is rejected
 *      rather than guessed at;
 *   4. apply Crockford's DECODING aliases `I`->`1`, `L`->`1`, `O`->`0`.
 *      `U` is deliberately NOT aliased — it is rejected, so a typo lands
 *      on an error instead of silently on somebody else's crew;
 *   5. require exactly 6 remaining characters, all in the alphabet;
 *   6. re-render as `FIRE-` + the six symbols.
 *
 * `out` receives the canonical 11 characters plus a NUL on success and
 * is left UNTOUCHED on failure (a caller that ignores the return value
 * cannot end up holding a fabricated code from a stale buffer, because
 * there is nothing written for it to mistake for one).
 *
 * Returns false for NULL arguments and for any input that is not a code.
 */
bool ff_crewcode_parse(char const *in, char out[FF_CREWCODE_LEN + 1u]);

/**
 * ff_crewcode_valid — true iff `s` is ALREADY in canonical form
 * (`FIRE-` + 6 alphabet symbols, uppercase, exactly 11 chars, NUL
 * terminated).
 *
 * Deliberately stricter than `ff_crewcode_parse`: this is the question
 * "is this channel name a crew code", where accepting `fire 4k9m7x`
 * would be wrong — the on-air channel hash folds the name's exact bytes
 * (A02 §1.3), so a channel named anything but the canonical spelling is
 * a different channel, not a sloppily-typed crew.
 */
bool ff_crewcode_valid(char const *s);

/**
 * ff_crewcode_psk — derive the crew channel's 32-byte AES256 key from a
 * canonical code (A02 §1.4):
 *
 *     PSK = HKDF-SHA256(salt = "firefly-crew-v1",
 *                       ikm  = the canonical code, 11 ASCII bytes,
 *                       info = "firefly-crew-psk-v1",
 *                       L    = 32)
 *
 * Both `salt` and `info` are CONSTANTS, and both deliberately so:
 *  - the salt is version-tagged rather than random because both sides
 *    derive independently from nothing but the code (HKDF explicitly
 *    permits a non-secret constant salt), and a `-v2` tag is the escape
 *    hatch if the format ever changes;
 *  - the info is NOT the crew's human name. The human name is optional
 *    (A02 §1.3 — a typed-code join has none), so binding it into the key
 *    would make that join underivable. `info` is here to domain-separate
 *    this key from any future key derived from the same code.
 *
 * Returns false (leaving `psk` untouched) unless `canonical` passes
 * `ff_crewcode_valid` — a key derived from a non-code would be a
 * perfectly plausible-looking 32 bytes that no other Firefly would ever
 * derive, which is the worst kind of wrong.
 */
bool ff_crewcode_psk(char const *canonical, uint8_t psk[FF_CREWCODE_PSK_LEN]);

/**
 * ff_crewcode_invite_url — build the Firefly deep link a QR encodes
 * (A02 §1.8):
 *
 *     firefly://crew?v=1&code=FIRE-4K9M7X&name=Camp%20Firefly
 *
 * Parameter order is FIXED (`v`, `code`, `name`) so the payload is
 * byte-reproducible across platforms and testable by fixture.
 *
 * `name` is optional display text and is percent-encoded UTF-8, clamped
 * to 24 bytes before encoding. It is never derived from and never
 * affects the key. **The puck passes NULL**: it has no human crew name
 * (that lives on the phone, A02 §1.3), and inventing one for the QR
 * would put a fabricated crew name on a joiner's screen. The parameter
 * exists so the C side can reproduce the fixture's own `deep_link`
 * strings byte-for-byte, which is what pins this encoder against the
 * app's.
 *
 * Returns the number of characters written (excluding the NUL), or 0 on
 * any failure — an invalid code, a NULL/short buffer. On failure `buf`
 * is set to "" when it is non-NULL and `n > 0`, never left holding a
 * half-built link.
 */
size_t ff_crewcode_invite_url(char const *canonical, char const *name, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* FF_CREWCODE_H */
