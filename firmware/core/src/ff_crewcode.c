/**
 * ff_crewcode.c — the crew code codec and its HKDF-SHA256 key derivation.
 *
 * Spec: docs/specs/A02-crew-join.md §1; vectors in
 * docs/specs/fixtures/A02-crew-codes.json.
 *
 * ## Why SHA-256 is vendored here
 *
 * core/ is pure C11 with zero dependencies (CLAUDE.md). The device has
 * mbedTLS and the app has CryptoKit, but core is the one place that must
 * not reach for either — and the derivation has to live in core because
 * both the shell (resolving which channel is the crew channel, by name
 * AND key) and the tests need it with no platform underneath.
 *
 * A02 slice A sized this honestly: "a vendored 40-line HMAC-SHA256 +
 * expand in core for the sim/tests (core stays zero-dependency — this is
 * the one place that rule costs us, and 40 lines with byte-exact vectors
 * is the cheap way to pay it)". It came out nearer 140 with the
 * compression function written out, which does not change the trade: the
 * whole of it is pinned byte-for-byte by the fixture's four distinct PSK
 * vectors, so a transcription error cannot pass the test suite.
 *
 * This implementation is for key derivation from an 11-byte input, not a
 * general-purpose hash service: it is straightforward, not constant-time
 * beyond what the operations themselves give, and it is deliberately NOT
 * exported outside this translation unit. There is no secret-dependent
 * branching or table indexing here, and the one input is a code the user
 * is about to show on screen as a QR anyway.
 */
#include "ff_crewcode.h"

#include <string.h>

/* -------------------------------------------------------------------- */
/* SHA-256 (FIPS 180-4)                                                  */
/* -------------------------------------------------------------------- */

typedef struct {
    uint32_t h[8];
    uint64_t len_bits;
    uint8_t  buf[64];
    size_t   buf_len;
} ff_sha256_t;

static uint32_t ff_ror32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static uint32_t const FF_SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static void ff_sha256_block(ff_sha256_t *s, uint8_t const *p)
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)p[4u * i] << 24) | ((uint32_t)p[4u * i + 1u] << 16) |
               ((uint32_t)p[4u * i + 2u] << 8) | (uint32_t)p[4u * i + 3u];
    }
    for (unsigned i = 16u; i < 64u; i++) {
        uint32_t const s0 = ff_ror32(w[i - 15u], 7) ^ ff_ror32(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
        uint32_t const s1 = ff_ror32(w[i - 2u], 17) ^ ff_ror32(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], hh = s->h[7];

    for (unsigned i = 0; i < 64u; i++) {
        uint32_t const S1 = ff_ror32(e, 6) ^ ff_ror32(e, 11) ^ ff_ror32(e, 25);
        uint32_t const ch = (e & f) ^ ((~e) & g);
        uint32_t const t1 = hh + S1 + ch + FF_SHA256_K[i] + w[i];
        uint32_t const S0 = ff_ror32(a, 2) ^ ff_ror32(a, 13) ^ ff_ror32(a, 22);
        uint32_t const maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t const t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += hh;
}

static void ff_sha256_init(ff_sha256_t *s)
{
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u; s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu; s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->len_bits = 0u;
    s->buf_len = 0u;
    memset(s->buf, 0, sizeof(s->buf));
}

static void ff_sha256_update(ff_sha256_t *s, void const *data, size_t n)
{
    uint8_t const *p = (uint8_t const *)data;
    s->len_bits += (uint64_t)n * 8u;
    while (n > 0u) {
        size_t take = 64u - s->buf_len;
        if (take > n) take = n;
        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += take;
        p += take;
        n -= take;
        if (s->buf_len == 64u) {
            ff_sha256_block(s, s->buf);
            s->buf_len = 0u;
        }
    }
}

static void ff_sha256_final(ff_sha256_t *s, uint8_t out[32])
{
    uint64_t const bits = s->len_bits;
    uint8_t const pad = 0x80u;
    ff_sha256_update(s, &pad, 1u);
    uint8_t const zero = 0x00u;
    while (s->buf_len != 56u) {
        ff_sha256_update(s, &zero, 1u);
    }
    uint8_t len_be[8];
    for (unsigned i = 0; i < 8u; i++) {
        len_be[i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    ff_sha256_update(s, len_be, sizeof(len_be));
    for (unsigned i = 0; i < 8u; i++) {
        out[4u * i]      = (uint8_t)(s->h[i] >> 24);
        out[4u * i + 1u] = (uint8_t)(s->h[i] >> 16);
        out[4u * i + 2u] = (uint8_t)(s->h[i] >> 8);
        out[4u * i + 3u] = (uint8_t)(s->h[i]);
    }
}

/* HMAC-SHA256 (RFC 2104). Keys here are never longer than the 32-byte
 * block-shortening threshold in practice (the salt is 15 bytes, the PRK
 * is 32), but the >64 case is implemented anyway rather than left as an
 * undocumented precondition. */
static void ff_hmac_sha256(uint8_t const *key, size_t key_len, uint8_t const *msg, size_t msg_len,
                            uint8_t out[32])
{
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64u) {
        ff_sha256_t s;
        ff_sha256_init(&s);
        ff_sha256_update(&s, key, key_len);
        ff_sha256_final(&s, k);
    } else {
        memcpy(k, key, key_len);
    }

    uint8_t pad[64];
    for (unsigned i = 0; i < 64u; i++) pad[i] = (uint8_t)(k[i] ^ 0x36u);

    uint8_t inner[32];
    ff_sha256_t s;
    ff_sha256_init(&s);
    ff_sha256_update(&s, pad, sizeof(pad));
    ff_sha256_update(&s, msg, msg_len);
    ff_sha256_final(&s, inner);

    for (unsigned i = 0; i < 64u; i++) pad[i] = (uint8_t)(k[i] ^ 0x5cu);
    ff_sha256_init(&s);
    ff_sha256_update(&s, pad, sizeof(pad));
    ff_sha256_update(&s, inner, sizeof(inner));
    ff_sha256_final(&s, out);
}

/* -------------------------------------------------------------------- */
/* The code itself                                                       */
/* -------------------------------------------------------------------- */

char const ff_crewcode_alphabet[33] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static char ff_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool ff_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool ff_in_alphabet(char c)
{
    for (unsigned i = 0; i < 32u; i++) {
        if (ff_crewcode_alphabet[i] == c) return true;
    }
    return false;
}

bool ff_crewcode_valid(char const *s)
{
    if (s == NULL) return false;
    if (strlen(s) != FF_CREWCODE_LEN) return false;
    if (memcmp(s, FF_CREWCODE_TAG, sizeof(FF_CREWCODE_TAG) - 1u) != 0) return false;
    for (unsigned i = 0; i < FF_CREWCODE_SYMBOLS; i++) {
        if (!ff_in_alphabet(s[(sizeof(FF_CREWCODE_TAG) - 1u) + i])) return false;
    }
    return true;
}

bool ff_crewcode_parse(char const *in, char out[FF_CREWCODE_LEN + 1u])
{
    if (in == NULL || out == NULL) return false;

    /* Steps 1+2 in one pass: uppercase, drop whitespace and '-'. Bounded
     * by the widest input that could still be a code once the separators
     * are gone — anything longer cannot possibly reduce to 6 symbols plus
     * an optional 4-character tag, so it is rejected rather than
     * truncated (truncating is how you silently join the wrong crew). */
    char sym[FF_CREWCODE_LEN + 1u];
    size_t n = 0u;
    for (char const *p = in; *p != '\0'; p++) {
        char c = ff_upper(*p);
        if (ff_is_space(c) || c == '-') continue;
        if (n >= sizeof(sym) - 1u) return false; /* too long to be a code */
        sym[n++] = c;
    }
    sym[n] = '\0';

    /* Step 3: strip a LEADING literal "FIRE", before aliasing, so the tag
     * matches as typed. See the header's pinned consequence for the
     * `F1RE9X` case this ordering deliberately produces. */
    char const *body = sym;
    size_t body_len = n;
    if (body_len >= 4u && body[0] == 'F' && body[1] == 'I' && body[2] == 'R' && body[3] == 'E') {
        body += 4;
        body_len -= 4u;
    }

    /* Step 5 (length) checked before the per-character work so a wrong
     * length can never be reported as a bad symbol. */
    if (body_len != FF_CREWCODE_SYMBOLS) return false;

    char canon[FF_CREWCODE_LEN + 1u];
    memcpy(canon, FF_CREWCODE_TAG, sizeof(FF_CREWCODE_TAG) - 1u);
    for (unsigned i = 0; i < FF_CREWCODE_SYMBOLS; i++) {
        char c = body[i];
        /* Step 4: Crockford DECODING aliases. `U` is absent on purpose —
         * it is rejected below, never remapped, so a typo lands on an
         * error rather than on somebody else's crew. */
        if (c == 'I' || c == 'L') c = '1';
        else if (c == 'O') c = '0';
        if (!ff_in_alphabet(c)) return false;
        canon[(sizeof(FF_CREWCODE_TAG) - 1u) + i] = c;
    }
    canon[FF_CREWCODE_LEN] = '\0';

    /* Written only now that the whole input has passed — `out` is
     * untouched on every failure path above. */
    memcpy(out, canon, sizeof(canon));
    return true;
}

bool ff_crewcode_psk(char const *canonical, uint8_t psk[FF_CREWCODE_PSK_LEN])
{
    if (psk == NULL || !ff_crewcode_valid(canonical)) return false;

    static char const salt[] = "firefly-crew-v1";      /* 15 bytes, no NUL on the wire */
    static char const info[] = "firefly-crew-psk-v1";  /* 19 bytes, no NUL on the wire */

    /* RFC 5869 extract. */
    uint8_t prk[32];
    ff_hmac_sha256((uint8_t const *)salt, sizeof(salt) - 1u,
                   (uint8_t const *)canonical, FF_CREWCODE_LEN, prk);

    /* RFC 5869 expand. L = 32 == HashLen, so exactly ONE block: T(1) =
     * HMAC(PRK, info || 0x01), with the empty T(0) prefix. A second block
     * would need T(1) prepended; there is deliberately no loop here
     * because there is deliberately no second block — a future L > 32
     * must add the loop rather than silently truncate. */
    uint8_t block_in[sizeof(info) - 1u + 1u];
    memcpy(block_in, info, sizeof(info) - 1u);
    block_in[sizeof(info) - 1u] = 0x01u;
    ff_hmac_sha256(prk, sizeof(prk), block_in, sizeof(block_in), psk);
    return true;
}

/* Percent-encoding for the optional `name` parameter (A02 §1.8). RFC 3986
 * unreserved set stays literal; everything else, byte by byte, becomes
 * %XX with UPPERCASE hex — the app's own encoder does the same, and the
 * fixture's `Camp%20Firefly` pins it. */
static bool ff_pct_append(char *buf, size_t n, size_t *len, uint8_t byte)
{
    static char const hex[] = "0123456789ABCDEF";
    bool const unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                             (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                             byte == '.' || byte == '~';
    if (unreserved) {
        if (*len + 1u >= n) return false;
        buf[(*len)++] = (char)byte;
        return true;
    }
    if (*len + 3u >= n) return false;
    buf[(*len)++] = '%';
    buf[(*len)++] = hex[(byte >> 4) & 0x0Fu];
    buf[(*len)++] = hex[byte & 0x0Fu];
    return true;
}

size_t ff_crewcode_invite_url(char const *canonical, char const *name, char *buf, size_t n)
{
    if (buf == NULL || n == 0u) return 0u;
    buf[0] = '\0';
    if (!ff_crewcode_valid(canonical)) return 0u;

    static char const prefix[] = "firefly://crew?v=1&code=";
    size_t len = 0u;
    size_t const prefix_len = sizeof(prefix) - 1u;
    if (prefix_len + FF_CREWCODE_LEN + 1u > n) return 0u;
    memcpy(buf, prefix, prefix_len);
    len = prefix_len;
    memcpy(buf + len, canonical, FF_CREWCODE_LEN);
    len += FF_CREWCODE_LEN;
    buf[len] = '\0';

    if (name != NULL && name[0] != '\0') {
        static char const key[] = "&name=";
        size_t const key_len = sizeof(key) - 1u;
        if (len + key_len + 1u > n) {
            buf[0] = '\0';
            return 0u;
        }
        memcpy(buf + len, key, key_len);
        len += key_len;

        /* Clamped to 24 BYTES of input (A02 §1.8's "clamped to 24
         * characters after decoding", applied to the bytes we are given —
         * this module has no UTF-8 decoder and will not pretend to count
         * code points; the clamp is a bound, and a caller passing a
         * longer name gets a shortened link rather than an overflowing
         * one).
         *
         * The cut is then backed off any UTF-8 continuation byte
         * (0b10xxxxxx), so a clamp landing mid-character drops the whole
         * character instead of emitting a percent-encoded fragment that
         * decodes to mojibake on the joiner's screen. */
        size_t name_max = 24u;
        size_t const name_len = strlen(name);
        if (name_max > name_len) name_max = name_len;
        while (name_max > 0u && ((uint8_t)name[name_max] & 0xC0u) == 0x80u) name_max--;
        for (size_t i = 0; i < name_max; i++) {
            if (!ff_pct_append(buf, n, &len, (uint8_t)name[i])) {
                buf[0] = '\0';
                return 0u;
            }
        }
        buf[len] = '\0';
    }

    return len;
}

bool ff_crewcode_from_bits(uint32_t bits, char out[FF_CREWCODE_LEN + 1u])
{
    if (out == NULL) return false;
    /* Rejected, never masked — see this function's doc comment
     * (ff_crewcode.h): silently keeping the low 30 bits of a 32-bit draw
     * would hand the caller a code it did not think it drew. */
    if (bits >= (1u << FF_CREWCODE_BITS)) return false;

    char canon[FF_CREWCODE_LEN + 1u];
    memcpy(canon, FF_CREWCODE_TAG, sizeof(FF_CREWCODE_TAG) - 1u);
    for (unsigned i = 0; i < FF_CREWCODE_SYMBOLS; i++) {
        unsigned const shift = FF_CREWCODE_BITS - 5u * (i + 1u); /* MSB-first */
        uint32_t const sym = (bits >> shift) & 0x1Fu;
        canon[(sizeof(FF_CREWCODE_TAG) - 1u) + i] = ff_crewcode_alphabet[sym];
    }
    canon[FF_CREWCODE_LEN] = '\0';

    memcpy(out, canon, sizeof(canon));
    return true;
}
