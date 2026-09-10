/**
 * ff_base64.h — 2026-09-09 amendment (fix/s31-beat-real-audio): a tiny,
 * pure, host-testable RFC 4648 base64 ENCODER, split out as its own
 * `firmware/core/` module (CLAUDE.md's placement rule: zero I/O, zero
 * hardware knowledge) so the `mic dump <secs>` bench console command
 * (`firmware/targets/esp32s3/main/app_main.c`) has a tested encoder to
 * call rather than hand-rolling one inline in device-only code no host
 * test ever exercises.
 *
 * DECODE has no C-side consumer: the coordinator's own capture workflow
 * decodes a dump on the HOST, in Python (`tools/beat_replay.py`, this
 * repo's own top-level tools/ directory), using the standard library's
 * `base64` module — writing a decoder here would be dead code. This
 * header is encode-only, deliberately.
 *
 * Standard alphabet (`A-Za-z0-9+/`), `=` padding, no line wrapping (a
 * dump's own console transport already frames one video frame's worth
 * of encoded bytes per LINE — see `ff_dbgconsole_mic_fn`'s own doc
 * comment, `ff_debug_console.h`, "mic dump" — wrapping inside that
 * would just be redundant).
 */
#ifndef FF_BASE64_H
#define FF_BASE64_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_base64_encoded_len — the exact number of base64 characters
 * (EXCLUDING a NUL terminator) `ff_base64_encode` produces for `n`
 * input bytes: `4 * ceil(n/3)`. Exposed so a caller can size its own
 * output buffer exactly (`ff_base64_encoded_len(n) + 1` for the NUL)
 * rather than guessing/over-allocating.
 */
size_t ff_base64_encoded_len(size_t n);

/**
 * ff_base64_encode — encode `n` bytes of `in` into `out` as standard
 * (RFC 4648, `+`/`/`, `=`-padded) base64 text, NUL-terminated. `out`
 * must be at least `ff_base64_encoded_len(n) + 1` bytes; `out_cap` is
 * checked against that requirement and the function does nothing (does
 * NOT partially fill `out`) if it is too small — an honest failure, not
 * a silently truncated encoding a downstream base64 DECODER would
 * misinterpret as complete, valid text.
 *
 * `in == NULL` with `n > 0`, or `out == NULL`, is a safe no-op.
 * `n == 0` writes just a NUL terminator (an empty string) and returns
 * true, provided `out_cap >= 1`.
 *
 * Returns true iff `out` was fully written (encoding succeeded);
 * false iff `out_cap` was too small, or an input arg was invalid per
 * the paragraph above.
 */
bool ff_base64_encode(uint8_t const *in, size_t n, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* FF_BASE64_H */
