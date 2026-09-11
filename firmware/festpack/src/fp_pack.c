/**
 * fp_pack.c — festpack.json parser implementation.
 *
 * Vendored jsmn (firmware/third_party/jsmn.h) tokenizes the input into a
 * flat, zero-alloc token array; the extraction below walks that array by
 * hand, matching keys and copying/converting into the caller-provided
 * fp_pack_t. No malloc. fp_skip()'s recursion depth is explicitly capped
 * (FP_MAX_JSON_DEPTH) — untrusted/attacker-controlled input (this device
 * "eats untrusted RF bytes") could otherwise nest JSON deep enough to
 * overflow a small ESP32-S3 task stack; see fp_skip_depth().
 */
#define JSMN_STATIC
#include "jsmn.h"

#include "fp_pack.h"

#include "ff_geo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Parser budget. Independent of the fp_pack_t struct's 48KB output
 * budget (S05 AC6) — this bounds the *input* JSON text and the caller-
 * supplied jsmn token scratch used only during fp_parse(). A too-large
 * input or a token count that overruns the caller's buffer both map to
 * FP_ERR_TOO_BIG (never an overrun) — see fp_parse(). FP_MAX_TOKENS
 * (the recommended scratch capacity) lives in fp_pack.h now — callers
 * need it to size their own buffer.
 * ------------------------------------------------------------------- */
#define FP_MAX_JSON_LEN (64u * 1024u)

/* Real festpacks nest ~7-8 levels deep at most (root -> map -> features ->
 * [i] -> polygon -> [k] -> [lat,lon] -> number). 16 is generous headroom;
 * beyond it fp_skip_depth() bails out rather than recursing further,
 * capping fp_skip()'s own C call-stack usage regardless of how deeply an
 * attacker nests input JSON. See fp_skip_depth(). */
#define FP_MAX_JSON_DEPTH 16

/* ---------------------------------------------------------------------
 * Token-array helpers.
 * ------------------------------------------------------------------- */
typedef struct {
    char const *js;
    jsmntok_t const *toks;
    int ntoks;
    bool *depth_exceeded; /* set true (never cleared) if fp_skip_depth() ever
                              hits FP_MAX_JSON_DEPTH. fp_parse() checks this
                              once at the end and forces FP_ERR_JSON if set,
                              regardless of what fp_parse_inner() otherwise
                              returned — a depth-capped skip means we may
                              have stopped short partway through some
                              subtree, so nothing downstream of it can be
                              trusted. */
} fp_ctx_t;

/* Returns the index of the token immediately after tok i's subtree (i's
 * own value plus, for containers, all descendants), or c->ntoks if the
 * subtree's nesting exceeds FP_MAX_JSON_DEPTH (and sets *depth_exceeded).
 * `depth` is the nesting depth of token i itself (0 at any top-level call
 * site) — bounding it bounds this function's own recursion, which is the
 * only attacker-controlled-depth recursion in this file. */
static int fp_skip_depth(fp_ctx_t const *c, int i, int depth)
{
    if (depth > FP_MAX_JSON_DEPTH) {
        *c->depth_exceeded = true;
        return c->ntoks;
    }
    if (i < 0 || i >= c->ntoks) return c->ntoks;
    jsmntok_t const *t = &c->toks[i];
    int next = i + 1;
    if (t->type == JSMN_OBJECT) {
        for (int k = 0; k < t->size; k++) {
            next = fp_skip_depth(c, next, depth + 1); /* key */
            next = fp_skip_depth(c, next, depth + 1); /* value */
        }
    } else if (t->type == JSMN_ARRAY) {
        for (int k = 0; k < t->size; k++) {
            next = fp_skip_depth(c, next, depth + 1);
        }
    }
    return next;
}

static int fp_skip(fp_ctx_t const *c, int i)
{
    return fp_skip_depth(c, i, 0);
}

/* Looks up `key` among the top-level members of the object at index
 * obj_i. Unknown keys are simply never looked up — they're skipped by
 * construction, satisfying the schema's "tolerant of unknown fields"
 * requirement without any extra bookkeeping. */
static bool fp_obj_get(fp_ctx_t const *c, int obj_i, char const *key, int *val_i)
{
    if (obj_i < 0 || obj_i >= c->ntoks) return false;
    jsmntok_t const *t = &c->toks[obj_i];
    if (t->type != JSMN_OBJECT) return false;
    size_t keylen = strlen(key);
    int i = obj_i + 1;
    for (int k = 0; k < t->size; k++) {
        int key_i = i;
        if (key_i < 0 || key_i >= c->ntoks) return false;
        jsmntok_t const *kt = &c->toks[key_i];
        int val_index = fp_skip(c, key_i); /* keys are plain strings: key_i + 1 */
        if (kt->type == JSMN_STRING && (size_t)(kt->end - kt->start) == keylen &&
            memcmp(c->js + kt->start, key, keylen) == 0) {
            *val_i = val_index;
            return true;
        }
        i = fp_skip(c, val_index);
    }
    return false;
}

static bool fp_is_null(fp_ctx_t const *c, int i)
{
    if (i < 0 || i >= c->ntoks) return true;
    jsmntok_t const *t = &c->toks[i];
    return t->type == JSMN_PRIMITIVE && (t->end - t->start) == 4 &&
           memcmp(c->js + t->start, "null", 4) == 0;
}

static bool fp_tok_eq(fp_ctx_t const *c, int i, char const *s)
{
    if (i < 0 || i >= c->ntoks) return false;
    jsmntok_t const *t = &c->toks[i];
    size_t slen = strlen(s);
    return t->type == JSMN_STRING && (size_t)(t->end - t->start) == slen &&
           memcmp(c->js + t->start, s, slen) == 0;
}

/* S14 hardening pass (bounds/wraparound audit): the largest prefix
 * length <= `cap` bytes of `s` (which need not be NUL-terminated at
 * `cap`) that does not split a UTF-8 code point. `n` is `s`'s actual
 * length; only called when `n > cap` (an actual truncation is
 * happening). A festpack string field is real festival-authored text —
 * artist/stage/landmark names routinely carry non-ASCII (accents,
 * emoji) — so a plain byte-offset cut like the old `n = dst_sz - 1` can
 * land mid-sequence and leave a dangling lead byte or orphaned
 * continuation byte(s) at the end of the field, which is not valid
 * UTF-8 and can render as a replacement glyph or worse depending on the
 * consumer. Standard trailing-continuation-byte backup: a UTF-8
 * continuation byte always matches the bit pattern 10xxxxxx
 * (`(byte & 0xC0) == 0x80`), which a UTF-8 lead byte (0xxxxxxx ASCII,
 * 110xxxxx, 1110xxxx, 11110xxx) never does — so backing up over
 * continuation bytes always stops exactly at the start of whatever code
 * point we're sitting inside, ASCII included (an ASCII byte never
 * matches the continuation pattern, so the loop is a no-op whenever
 * `cap` already lands on a clean boundary). The code point found at
 * that stopping point is then excluded entirely, even if it's a lead
 * byte with no room left for its own continuation bytes, rather than
 * ever emitting a partial one. Already-malformed input (a stray
 * continuation byte with no preceding lead byte, e.g. non-UTF-8 bytes)
 * degrades safely to an empty/shorter-than-expected prefix, never past
 * `cap` and never a crash — this function has no opinion on whether `s`
 * was valid UTF-8 to begin with, only on not making a valid prefix
 * invalid. */
static size_t fp_utf8_truncate_len(char const *s, size_t cap)
{
    size_t end = cap;
    while (end > 0 && ((unsigned char)s[end] & 0xC0u) == 0x80u) {
        end--;
    }
    return end;
}

/* Bounded copy, always NUL-terminated. Oversized field VALUES (as
 * opposed to oversized ARRAYS) are truncated, not treated as
 * FP_ERR_TOO_BIG — only the counted collections (stages/sets/features/
 * landmarks/polygon points) are overflow-checked per the spec. The
 * truncation point itself is UTF-8-code-point-safe (fp_utf8_truncate_len
 * above) — never shorter than necessary, but also never splitting a
 * multi-byte character. */
static void fp_copy_str(fp_ctx_t const *c, int i, char *dst, size_t dst_sz)
{
    dst[0] = '\0';
    if (i < 0 || i >= c->ntoks || dst_sz == 0) return;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_STRING) return;
    size_t n = (size_t)(t->end - t->start);
    if (n >= dst_sz) n = fp_utf8_truncate_len(c->js + t->start, dst_sz - 1);
    memcpy(dst, c->js + t->start, n);
    dst[n] = '\0';
}

/* Strict numeric extraction: succeeds ONLY if token i exists, is a JSON
 * number (JSMN_PRIMITIVE, not "null"/"true"/"false"), strtod() consumes
 * the token's ENTIRE text with no trailing garbage, AND the result is
 * finite. Writes *out and returns true on success; leaves *out untouched
 * and returns false on any mismatch (wrong JSMN type — e.g. a quoted
 * string — a boolean literal, null, malformed number text, or a
 * non-finite result).
 *
 * Non-finite rejection matters even though JSON's own grammar has no
 * Infinity/NaN literals: strtod() is a C-locale float parser, not a JSON
 * validator, so it happily consumes JSON-legal-looking-but-huge numeric
 * text like "1e400" (out of double's range -> HUGE_VAL) as well as
 * bareword "Infinity"/"NaN" tokens (which jsmn still tokenizes as
 * JSMN_PRIMITIVE, since it doesn't validate number grammar either). A
 * non-finite value is treated exactly like any other wrong-typed value:
 * an honest unknown, not a number to hand to a caller — see
 * docs/specs/S05-festpack.md's Amendments entry. This also forecloses
 * the UB a non-finite (or simply out-of-range) double would cause at an
 * int16_t/uint16_t cast site downstream; see fp_i16_checked()/fp_u16()
 * below for the additional range check those sites need even after this
 * filter (a finite but huge value like 1e10 is still not
 * representable).
 *
 * Any call site that gates a "known"/"verified"/"assumed" flag on a
 * value's *correctness* (not just the key's presence) MUST call this
 * directly (or fp_i16_checked() below, for an int16-typed field) rather
 * than only checking fp_obj_get()+!fp_is_null() and then converting
 * separately — that split is exactly the bug fixed here for
 * origin/landmark-position/utc_offset_min: those sites used to decide
 * the flag from presence alone, then convert with something that could
 * *also* silently default or invoke UB on an out-of-range/non-finite
 * value — so a wrong-typed-but-present field looked "verified" while
 * actually holding a default (or worse). See docs/specs/S05-festpack.md's
 * Amendments entry "Wrong-typed numeric fields are honest unknowns". */
static bool fp_num_checked(fp_ctx_t const *c, int i, double *out)
{
    if (i < 0 || i >= c->ntoks) return false;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_PRIMITIVE) return false;
    int n = t->end - t->start;
    if (n == 4 && memcmp(c->js + t->start, "null", 4) == 0) return false;
    if (n == 4 && memcmp(c->js + t->start, "true", 4) == 0) return false;
    if (n == 5 && memcmp(c->js + t->start, "false", 5) == 0) return false;
    char buf[32];
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
    memcpy(buf, c->js + t->start, (size_t)n);
    buf[n] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf || *end != '\0') return false; /* no digits, or trailing junk */
    if (!isfinite(v)) return false; /* +-Infinity, NaN, or magnitude overflow (1e400) */
    *out = v;
    return true;
}

/* Strict int16 extraction built on fp_num_checked(): fails (false) if the
 * value is wrong-typed/non-finite (fp_num_checked's job) OR finite but
 * outside INT16_MIN..INT16_MAX. Casting an out-of-range double to
 * int16_t is undefined behavior in C regardless of whether the double is
 * finite — fp_num_checked() alone (Infinity/NaN/overflow) is not enough
 * for a field like utc_offset_min that then gets cast; this closes that
 * gap. Used wherever a strict/flagged int16 field is read — see
 * fp_num_checked()'s comment. */
static bool fp_i16_checked(fp_ctx_t const *c, int i, int16_t *out)
{
    double v;
    if (!fp_num_checked(c, i, &v)) return false;
    if (v < (double)INT16_MIN || v > (double)INT16_MAX) return false;
    *out = (int16_t)v;
    return true;
}

/* Lenient uint16 extraction, for plain fields with no downstream "known"
 * flag (e.g. `year`) — same range-safety as fp_i16_checked() (no
 * out-of-range-or-non-finite-double-to-uint16_t cast, which is UB
 * exactly like the int16 case), but returns `dflt` on any mismatch
 * instead of a success/failure signal: silently defaulting is the same
 * tolerant posture already taken for unknown keys, correct here because
 * nothing downstream treats `year` as "verified" the way origin_known/
 * has_pos/utc_offset_assumed do. NOT correct for a field whose presence
 * flips a "known"/"assumed" bool elsewhere; use fp_num_checked() or
 * fp_i16_checked() directly for those. */
static uint16_t fp_u16(fp_ctx_t const *c, int i, uint16_t dflt)
{
    double v;
    if (!fp_num_checked(c, i, &v)) return dflt;
    if (v < 0.0 || v > (double)UINT16_MAX) return dflt;
    return (uint16_t)v;
}

static bool fp_bool(fp_ctx_t const *c, int i, bool dflt)
{
    if (i < 0 || i >= c->ntoks) return dflt;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_PRIMITIVE) return dflt;
    int n = t->end - t->start;
    if (n == 4 && memcmp(c->js + t->start, "true", 4) == 0) return true;
    if (n == 5 && memcmp(c->js + t->start, "false", 5) == 0) return false;
    return dflt;
}

/* ---------------------------------------------------------------------
 * Domain-specific field parsing.
 * ------------------------------------------------------------------- */
static const uint16_t fp_cum_days[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

static bool fp_is_leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/* "YYYY-MM-DD" -> (y, m, d). Returns false on malformed input rather
 * than failing the whole parse — a bad date is a data-quality problem
 * for the schedule engine (S07), not a parse-time crash. */
static bool fp_ymd_from_iso_date(char const *s, size_t len, int *out_y, int *out_m, int *out_d)
{
    if (len != 10 || s[4] != '-' || s[7] != '-') return false;
    int y = 0, m = 0, d = 0;
    for (int i = 0; i < 4; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        y = y * 10 + (s[i] - '0');
    }
    for (int i = 5; i < 7; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        m = m * 10 + (s[i] - '0');
    }
    for (int i = 8; i < 10; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        d = d * 10 + (s[i] - '0');
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) return false;
    *out_y = y;
    *out_m = m;
    *out_d = d;
    return true;
}

/* day-of-year (1..366) for a (y, m, d) already validated by
 * fp_ymd_from_iso_date. */
static uint16_t fp_doy_from_ymd(int y, int m, int d)
{
    uint16_t doy = fp_cum_days[m - 1] + (uint16_t)d;
    if (m > 2 && fp_is_leap_year(y)) doy += 1;
    return doy;
}

/* "YYYY-MM-DD" -> day-of-year (1..366), 0 on malformed input. */
static uint16_t fp_doy_from_iso_date(char const *s, size_t len)
{
    int y, m, d;
    if (!fp_ymd_from_iso_date(s, len, &y, &m, &d)) return 0;
    return fp_doy_from_ymd(y, m, d);
}

/* Days since 1970-01-01 for a proleptic-Gregorian (y, m, d) — Howard
 * Hinnant's days_from_civil, integer-only. Used for one thing only:
 * taking an EXACT difference between two ISO dates that appear in the
 * same schedule entry (`day` vs `night`, `day` vs `end_day`), which
 * day-of-year arithmetic alone cannot do across a year boundary. */
static int32_t fp_days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int32_t const era = (int32_t)((y >= 0 ? y : y - 399) / 400);
    uint32_t const yoe = (uint32_t)(y - era * 400);                            /* [0, 399] */
    uint32_t const doy_ = (uint32_t)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1); /* [0, 365] */
    uint32_t const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy_;               /* [0, 146096] */
    return era * 146097 + (int32_t)doe - 719468;
}

/* The day-of-year one calendar day BEFORE (y, m, d) — the fallback fold
 * target when a schedule entry omits `night` (see fp_parse_schedule).
 * Handles the Jan-1 wrap into the previous year's own last day-of-year
 * (365, or 366 if that year was a leap year). */
static uint16_t fp_doy_prev_day(int y, int m, int d)
{
    uint16_t doy = fp_doy_from_ymd(y, m, d);
    if (doy > 1) return (uint16_t)(doy - 1);
    return fp_is_leap_year(y - 1) ? 366u : 365u;
}

/* "HH:MM" -> minutes from midnight, or -1 (null / malformed). HH is
 * 00..23 — a plain wall-clock time on its entry's own `day` calendar
 * date. A set that runs past actual local midnight is NOT encoded with
 * an inflated hour here; it carries the next calendar date in `day` and
 * names the festival night it belongs to in `night` (and, for an `end`
 * on a later date than `start`, `end_day`). fp_parse_schedule() does the
 * folding into fp_set_t's festival-night minute space — see its comment
 * and docs/specs/S05-festpack.md's 2026-09-09 amendment. */
static int16_t fp_min_from_hhmm(char const *s, size_t len)
{
    if (len != 5 || s[2] != ':') return -1;
    int h = 0, m = 0;
    for (int i = 0; i < 2; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        h = h * 10 + (s[i] - '0');
    }
    for (int i = 3; i < 5; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        m = m * 10 + (s[i] - '0');
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return (int16_t)(h * 60 + m);
}

static int fp_hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* "#rrggbb" (leading '#' optional) -> 0x00RRGGBB, or 0 if malformed. */
static uint32_t fp_color_rgb(fp_ctx_t const *c, int i)
{
    if (i < 0 || i >= c->ntoks) return 0;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_STRING) return 0;
    char const *s = c->js + t->start;
    int n = t->end - t->start;
    if (n > 0 && s[0] == '#') {
        s++;
        n--;
    }
    if (n != 6) return 0;
    uint32_t v = 0;
    for (int k = 0; k < 6; k++) {
        int nib = fp_hex_nibble(s[k]);
        if (nib < 0) return 0;
        v = (v << 4) | (uint32_t)nib;
    }
    return v;
}

static fp_feature_kind_t fp_kind_from_tok(fp_ctx_t const *c, int i)
{
    if (i < 0 || i >= c->ntoks) return FP_KIND_UNKNOWN;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_STRING) return FP_KIND_UNKNOWN;
    /* Matches schema/festpack.schema.json's closed `kind` enum exactly:
     * ["stage","camping","water","path","entrance","vendor","medical","poi"] */
    static const struct {
        char const *s;
        fp_feature_kind_t k;
    } table[] = {
        {"stage", FP_KIND_STAGE},     {"camping", FP_KIND_CAMPING}, {"water", FP_KIND_WATER},
        {"path", FP_KIND_PATH},       {"entrance", FP_KIND_ENTRANCE}, {"vendor", FP_KIND_VENDOR},
        {"medical", FP_KIND_MEDICAL}, {"poi", FP_KIND_POI},
    };
    int n = t->end - t->start;
    for (size_t k = 0; k < sizeof(table) / sizeof(table[0]); k++) {
        size_t slen = strlen(table[k].s);
        if ((size_t)n == slen && memcmp(c->js + t->start, table[k].s, slen) == 0) return table[k].k;
    }
    return FP_KIND_UNKNOWN;
}

/* Resolve a stage-id string token against the already-parsed
 * out->stages[] table. -1 if null/absent/unmatched. */
static int8_t fp_stage_idx_lookup(fp_ctx_t const *c, int i, fp_pack_t const *out)
{
    if (i < 0 || i >= c->ntoks) return -1;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_STRING) return -1;
    int n = t->end - t->start;
    for (uint8_t k = 0; k < out->n_stages; k++) {
        size_t slen = strlen(out->stages[k].id);
        if ((size_t)n == slen && memcmp(c->js + t->start, out->stages[k].id, slen) == 0) return (int8_t)k;
    }
    return -1;
}

/* ---------------------------------------------------------------------
 * Section parsers.
 * ------------------------------------------------------------------- */
static fp_result_t fp_parse_festival(fp_ctx_t const *c, int fest_i, fp_pack_t *out, ff_latlon_t *origin)
{
    int t;
    if (fp_obj_get(c, fest_i, "name", &t)) fp_copy_str(c, t, out->name, sizeof(out->name));
    if (fp_obj_get(c, fest_i, "year", &t)) out->year = fp_u16(c, t, 0); /* range-safe cast, see fp_u16() */
    if (fp_obj_get(c, fest_i, "start", &t) && !fp_is_null(c, t)) {
        jsmntok_t const *tt = &c->toks[t];
        out->start_doy = fp_doy_from_iso_date(c->js + tt->start, (size_t)(tt->end - tt->start));
    }
    if (fp_obj_get(c, fest_i, "end", &t) && !fp_is_null(c, t)) {
        jsmntok_t const *tt = &c->toks[t];
        out->end_doy = fp_doy_from_iso_date(c->js + tt->start, (size_t)(tt->end - tt->start));
    }
    int venue_i = -1;
    fp_obj_get(c, fest_i, "venue", &venue_i);
    if (venue_i >= 0 && !fp_is_null(c, venue_i)) {
        int lat_i = -1, lon_i = -1;
        bool have_lat = fp_obj_get(c, venue_i, "lat", &lat_i) && !fp_is_null(c, lat_i);
        bool have_lon = fp_obj_get(c, venue_i, "lon", &lon_i) && !fp_is_null(c, lon_i);
        double lat_v = 0.0, lon_v = 0.0;
        if (have_lat && have_lon && fp_num_checked(c, lat_i, &lat_v) && fp_num_checked(c, lon_i, &lon_v)) {
            origin->lat = lat_v;
            origin->lon = lon_v;
            out->origin_known = true;
        }
        /* venue.lat/lon may be null (schema: "unknown venue") — origin
         * stays {0,0} and origin_known stays false, per fp_pack_t's
         * documented contract. Don't silently present that as real.
         * Same treatment for a wrong-typed lat/lon (e.g. a quoted
         * "43.7"): fp_pack_t already has a dedicated honest-unknown slot
         * for this exact "present but not usable" case, so a type
         * mismatch here is folded into that existing null-venue path
         * rather than failing the whole pack — see docs/specs/
         * S05-festpack.md's Amendments entry for the policy and why this
         * differs from fp_parse_polygon()'s FP_ERR_JSON (a polygon point
         * has no per-point "unknown" slot to fall back to). */
        int lt;
        if (fp_obj_get(c, venue_i, "approximate", &lt)) out->origin_approx = fp_bool(c, lt, false);
    }
    return FP_OK;
}

static fp_result_t fp_parse_stages(fp_ctx_t const *c, int arr_i, fp_pack_t *out)
{
    jsmntok_t const *at = &c->toks[arr_i];
    if (at->type != JSMN_ARRAY) return FP_ERR_JSON;
    if (at->size > FP_MAX_STAGES) return FP_ERR_TOO_BIG;
    int idx = arr_i + 1;
    for (int i = 0; i < at->size; i++) {
        int obj_i = idx;
        fp_stage_t *st = &out->stages[out->n_stages];
        memset(st, 0, sizeof(*st));
        int t;
        if (fp_obj_get(c, obj_i, "id", &t)) fp_copy_str(c, t, st->id, sizeof(st->id));
        if (fp_obj_get(c, obj_i, "name", &t)) fp_copy_str(c, t, st->name, sizeof(st->name));
        if (fp_obj_get(c, obj_i, "color", &t)) st->color_rgb = fp_color_rgb(c, t);
        out->n_stages++;
        idx = fp_skip(c, obj_i);
    }
    return FP_OK;
}

/* Minutes-from-midnight below which a set with no explicit `night` is
 * folded onto the PREVIOUS calendar day's festival night (06:00). Must
 * equal ff_sched.h's FF_SCHED_FESTIVAL_DAY_START_MIN — festpack/ cannot
 * include ff_sched.h from here (ff_sched.h includes fp_pack.h, not the
 * other way round), so this is the same deliberate mirrored-constant
 * arrangement ff_wall.h documents for FF_WALL_DAY_START_MIN. */
#define FP_NIGHT_FOLD_MIN 360

/* Reads one schedule entry's day/time fields into `s`, folding the
 * pack's plain-calendar encoding into fp_set_t's festival-NIGHT minute
 * space (see fp_pack.h's fp_set_t doc comment and
 * docs/specs/S05-festpack.md's 2026-09-09 amendment).
 *
 * The pack encodes ordinary calendar facts:
 *   `day`      ISO date the set STARTS on
 *   `start`    "HH:MM", HH 00..23, local time on `day`
 *   `end`      "HH:MM", HH 00..23, or null (unknown — derived by
 *               ff_sched.c from the next set on the stage)
 *   `night`    optional ISO date: the festival night the set is billed
 *               under. Equals `day` for an ordinary set; equals
 *               `day` - 1 day for an after-midnight set (Sippy's 00:15
 *               on Sat 2026-09-19 is billed under Friday 2026-09-18).
 *   `end_day`  optional ISO date of `end`, present only when the set's
 *               end falls on a later calendar date than its start
 *               (Excision 2026-09-18 22:10 -> 00:10 on 2026-09-19).
 *
 * fp_set_t instead stores ONE day_doy (the night) with start_min/end_min
 * measured from that night's local midnight, so an after-midnight set
 * lands at >= 1440 in the same number space as that night's evening
 * sets. That is the space ff_sched.h's festival-day contract ([360,
 * 1800), rolling at 06:00) and ff_wall.h's wall-clock resolution both
 * already work in, so ff_sched.c's plain integer comparisons order
 * 00:15 (1455) after 23:00 (1380) on the same night with no special
 * casing.
 *
 * FALLBACK when `night` is absent: a set starting before 06:00
 * (FP_NIGHT_FOLD_MIN) local belongs to the PREVIOUS calendar day's
 * night; anything else belongs to its own `day`. This is a documented
 * best guess for packs predating the `night` field, not a substitute
 * for it — fest-almanac emits `night` on every entry, and
 * tools/festpack_lint.py requires night == day or day - 1. */
static void fp_parse_set_daytime(fp_ctx_t const *c, int obj_i, fp_set_t *s)
{
    int t;
    int day_y = 0, day_m = 0, day_d = 0;
    bool day_ok = false;
    if (fp_obj_get(c, obj_i, "day", &t) && !fp_is_null(c, t)) {
        jsmntok_t const *tt = &c->toks[t];
        day_ok = fp_ymd_from_iso_date(c->js + tt->start, (size_t)(tt->end - tt->start),
                                      &day_y, &day_m, &day_d);
    }

    int16_t start_raw = -1, end_raw = -1;
    if (fp_obj_get(c, obj_i, "start", &t) && !fp_is_null(c, t)) {
        jsmntok_t const *tt = &c->toks[t];
        start_raw = fp_min_from_hhmm(c->js + tt->start, (size_t)(tt->end - tt->start));
    }
    if (fp_obj_get(c, obj_i, "end", &t) && !fp_is_null(c, t)) {
        jsmntok_t const *tt = &c->toks[t];
        end_raw = fp_min_from_hhmm(c->js + tt->start, (size_t)(tt->end - tt->start));
    }

    if (!day_ok) {
        /* No usable `day`. `night` alone can still name the bucket; times
         * stay in their raw 0..1439 space because there is no calendar
         * date to fold them against. */
        s->day_doy = 0;
        if (fp_obj_get(c, obj_i, "night", &t) && !fp_is_null(c, t)) {
            jsmntok_t const *tt = &c->toks[t];
            s->day_doy = fp_doy_from_iso_date(c->js + tt->start, (size_t)(tt->end - tt->start));
        }
        s->start_min = start_raw;
        s->end_min = end_raw;
        return;
    }

    int32_t const day_days = fp_days_from_civil(day_y, day_m, day_d);

    /* fold_days = how many days `day` sits AFTER the festival night. 0 for
     * an ordinary set, 1 for an after-midnight one. */
    int32_t fold_days = 0;
    bool night_resolved = false;
    if (fp_obj_get(c, obj_i, "night", &t) && !fp_is_null(c, t)) {
        jsmntok_t const *tt = &c->toks[t];
        int ny, nm, nd;
        if (fp_ymd_from_iso_date(c->js + tt->start, (size_t)(tt->end - tt->start), &ny, &nm, &nd)) {
            int32_t const diff = day_days - fp_days_from_civil(ny, nm, nd);
            /* Out-of-contract `night` (ahead of `day`, or more than one
             * night behind it) is data corruption, not a fold we should
             * guess at: keep the night as authored for grouping but do
             * not shift the clock times by a bogus multi-day offset.
             * tools/festpack_lint.py rejects exactly this shape. */
            fold_days = (diff == 1) ? 1 : 0;
            s->day_doy = fp_doy_from_ymd(ny, nm, nd);
            night_resolved = true;
        }
    }

    if (!night_resolved) {
        /* Documented fallback — see this function's comment. */
        if (start_raw >= 0 && start_raw < FP_NIGHT_FOLD_MIN) {
            fold_days = 1;
            s->day_doy = fp_doy_prev_day(day_y, day_m, day_d);
        } else {
            s->day_doy = fp_doy_from_ymd(day_y, day_m, day_d);
        }
    }

    /* `end_day`, when present, dates the END; the extra days it adds are
     * on top of the start's own fold. Clamped to [0, 1]: a schedule entry
     * whose end is more than one calendar day after its start is not a
     * set, it is bad data (lint rejects it), and letting it through would
     * push end_min past int16_t. */
    int32_t end_extra_days = 0;
    if (fp_obj_get(c, obj_i, "end_day", &t) && !fp_is_null(c, t)) {
        jsmntok_t const *tt = &c->toks[t];
        int ey, em, ed;
        if (fp_ymd_from_iso_date(c->js + tt->start, (size_t)(tt->end - tt->start), &ey, &em, &ed)) {
            int32_t const diff = fp_days_from_civil(ey, em, ed) - day_days;
            end_extra_days = (diff == 1) ? 1 : 0;
        }
    }

    s->start_min = (start_raw < 0) ? (int16_t)-1
                                   : (int16_t)(start_raw + fold_days * 1440);
    s->end_min = (end_raw < 0) ? (int16_t)-1
                               : (int16_t)(end_raw + (fold_days + end_extra_days) * 1440);
}

static fp_result_t fp_parse_schedule(fp_ctx_t const *c, int arr_i, fp_pack_t *out)
{
    jsmntok_t const *at = &c->toks[arr_i];
    if (at->type != JSMN_ARRAY) return FP_ERR_JSON;
    if (at->size > FP_MAX_SETS) return FP_ERR_TOO_BIG;
    int idx = arr_i + 1;
    for (int i = 0; i < at->size; i++) {
        int obj_i = idx;
        fp_set_t *s = &out->sets[out->n_sets];
        memset(s, 0, sizeof(*s));
        s->stage_idx = -1;
        s->start_min = -1;
        s->end_min = -1;
        int t;
        if (fp_obj_get(c, obj_i, "artist", &t)) fp_copy_str(c, t, s->artist, sizeof(s->artist));
        if (fp_obj_get(c, obj_i, "stage", &t) && !fp_is_null(c, t)) s->stage_idx = fp_stage_idx_lookup(c, t, out);
        fp_parse_set_daytime(c, obj_i, s);
        if (fp_obj_get(c, obj_i, "note", &t)) fp_copy_str(c, t, s->note, sizeof(s->note));
        if (fp_obj_get(c, obj_i, "starred", &t)) s->starred = fp_bool(c, t, false);
        out->n_sets++;
        idx = fp_skip(c, obj_i);
    }
    return FP_OK;
}

/* polygon: [[lat, lon], ...] per schema/festpack.schema.json's
 * `prefixItems: [{number},{number}]` — each point is a 2-element tuple
 * ARRAY, not a {"lat":,"lon":} object. A point that isn't a well-formed
 * 2-number tuple is a schema violation and must fail the parse
 * (FP_ERR_JSON) rather than silently projecting a (0,0) fallback as if
 * it were real data — see CLAUDE.md's "honest data over pretty data". */
static fp_result_t fp_parse_polygon(fp_ctx_t const *c, int poly_i, ff_latlon_t origin, fp_feature_t *f)
{
    jsmntok_t const *pt = &c->toks[poly_i];
    if (pt->type != JSMN_ARRAY) return FP_ERR_JSON;
    if (pt->size > FP_MAX_POLY_PTS) return FP_ERR_TOO_BIG;
    int idx = poly_i + 1;
    for (int k = 0; k < pt->size; k++) {
        int pt_tok_i = idx;
        if (pt_tok_i < 0 || pt_tok_i >= c->ntoks) return FP_ERR_JSON;
        jsmntok_t const *pt_tok = &c->toks[pt_tok_i];
        if (pt_tok->type != JSMN_ARRAY || pt_tok->size != 2) return FP_ERR_JSON;

        int lat_i = pt_tok_i + 1;
        int lon_i = fp_skip(c, lat_i);
        if (lat_i >= c->ntoks || lon_i >= c->ntoks) return FP_ERR_JSON;

        /* fp_num_checked (not fp_num) so a boolean literal in tuple
         * position — well-formed JSMN_PRIMITIVE, but not a number, e.g.
         * [true, -82.4] — is also rejected rather than silently
         * projecting a fabricated 0.0. Same policy as the JSMN_ARRAY/
         * size checks above: a malformed point fails the whole pack,
         * because a polygon point has no per-point "unknown" slot to
         * honestly fall back to (unlike origin/landmark/utc_offset_min —
         * see docs/specs/S05-festpack.md's Amendments entry). */
        double lat_v, lon_v;
        if (!fp_num_checked(c, lat_i, &lat_v) || !fp_num_checked(c, lon_i, &lon_v)) return FP_ERR_JSON;

        ff_latlon_t p = {lat_v, lon_v};
        ff_geo_project(origin, p, &f->pts_en[f->n_pts][0], &f->pts_en[f->n_pts][1]);
        f->n_pts++;
        idx = fp_skip(c, pt_tok_i);
    }
    return FP_OK;
}

static fp_result_t fp_parse_features(fp_ctx_t const *c, int arr_i, ff_latlon_t origin, fp_pack_t *out)
{
    jsmntok_t const *at = &c->toks[arr_i];
    if (at->type != JSMN_ARRAY) return FP_ERR_JSON;
    if (at->size > FP_MAX_FEATURES) return FP_ERR_TOO_BIG;
    int idx = arr_i + 1;
    for (int i = 0; i < at->size; i++) {
        int obj_i = idx;
        fp_feature_t *f = &out->features[out->n_features];
        memset(f, 0, sizeof(*f));
        f->stage_idx = -1;
        int t;
        if (fp_obj_get(c, obj_i, "kind", &t)) f->kind = (uint8_t)fp_kind_from_tok(c, t);
        if (fp_obj_get(c, obj_i, "stage", &t) && !fp_is_null(c, t)) f->stage_idx = fp_stage_idx_lookup(c, t, out);
        if (fp_obj_get(c, obj_i, "label", &t)) fp_copy_str(c, t, f->label, sizeof(f->label));
        int poly_i;
        if (fp_obj_get(c, obj_i, "polygon", &poly_i) && !fp_is_null(c, poly_i)) {
            fp_result_t r = fp_parse_polygon(c, poly_i, origin, f);
            if (r != FP_OK) return r;
        }
        out->n_features++;
        idx = fp_skip(c, obj_i);
    }
    return FP_OK;
}

static fp_result_t fp_parse_landmarks(fp_ctx_t const *c, int arr_i, ff_latlon_t origin, fp_pack_t *out)
{
    jsmntok_t const *at = &c->toks[arr_i];
    if (at->type != JSMN_ARRAY) return FP_ERR_JSON;
    if (at->size > FP_MAX_LANDMARKS) return FP_ERR_TOO_BIG;
    int idx = arr_i + 1;
    for (int i = 0; i < at->size; i++) {
        int obj_i = idx;
        fp_landmark_t *lm = &out->landmarks[out->n_landmarks];
        memset(lm, 0, sizeof(*lm));
        int t;
        if (fp_obj_get(c, obj_i, "id", &t)) fp_copy_str(c, t, lm->id, sizeof(lm->id));
        if (fp_obj_get(c, obj_i, "name", &t)) fp_copy_str(c, t, lm->name, sizeof(lm->name));
        int lat_i = -1, lon_i = -1;
        bool have_lat = fp_obj_get(c, obj_i, "lat", &lat_i) && !fp_is_null(c, lat_i);
        bool have_lon = fp_obj_get(c, obj_i, "lon", &lon_i) && !fp_is_null(c, lon_i);
        double lat_v = 0.0, lon_v = 0.0;
        /* Wrong-typed lat/lon (e.g. a quoted string) is treated exactly
         * like an absent/null position: has_pos stays false rather than
         * projecting a fabricated (0,0)-derived east/north as if it were
         * real. See docs/specs/S05-festpack.md's Amendments entry. */
        if (have_lat && have_lon && fp_num_checked(c, lat_i, &lat_v) && fp_num_checked(c, lon_i, &lon_v)) {
            ff_latlon_t p = {lat_v, lon_v};
            ff_geo_project(origin, p, &lm->east_m, &lm->north_m);
            lm->has_pos = true;
        }
        out->n_landmarks++;
        idx = fp_skip(c, obj_i);
    }
    return FP_OK;
}

/* 2026-09-11 S05 amendment — see fp_meta_t's own doc comment (fp_pack.h).
 * Purely additive and tolerant: every sub-field defaults to empty/zero
 * (already true from fp_parse()'s memset) and is only ever overwritten
 * on a successfully-typed match, exactly like every other optional field
 * in this file. Never returns an error — a malformed "meta" object is
 * simply an absent one for whichever of its sub-fields didn't parse. */
static void fp_parse_meta(fp_ctx_t const *c, int meta_i, fp_pack_t *out)
{
    out->meta.present = true;
    int t;
    if (fp_obj_get(c, meta_i, "updated", &t) && !fp_is_null(c, t)) {
        fp_copy_str(c, t, out->meta.updated, sizeof(out->meta.updated));
    }
    int arr_i;
    if (fp_obj_get(c, meta_i, "sources", &arr_i) && !fp_is_null(c, arr_i)) {
        jsmntok_t const *at = &c->toks[arr_i];
        if (at->type == JSMN_ARRAY) {
            int idx = arr_i + 1;
            for (int i = 0; i < at->size; i++) {
                if (out->meta.n_sources < FP_MAX_META_SOURCES) {
                    fp_copy_str(c, idx, out->meta.sources[out->meta.n_sources], FP_META_SOURCE_LEN);
                    out->meta.n_sources++;
                }
                idx = fp_skip(c, idx);
            }
        }
    }
    int complete_i;
    if (fp_obj_get(c, meta_i, "complete", &complete_i) && !fp_is_null(c, complete_i)) {
        if (fp_obj_get(c, complete_i, "lineup", &t))
            fp_copy_str(c, t, out->meta.complete_lineup, sizeof(out->meta.complete_lineup));
        if (fp_obj_get(c, complete_i, "set_times", &t))
            fp_copy_str(c, t, out->meta.complete_set_times, sizeof(out->meta.complete_set_times));
        if (fp_obj_get(c, complete_i, "map", &t))
            fp_copy_str(c, t, out->meta.complete_map, sizeof(out->meta.complete_map));
    }
}

static fp_result_t fp_parse_map(fp_ctx_t const *c, int map_i, ff_latlon_t origin, fp_pack_t *out)
{
    int arr_i;
    if (fp_obj_get(c, map_i, "features", &arr_i) && !fp_is_null(c, arr_i)) {
        fp_result_t r = fp_parse_features(c, arr_i, origin, out);
        if (r != FP_OK) return r;
    }
    if (fp_obj_get(c, map_i, "landmarks", &arr_i) && !fp_is_null(c, arr_i)) {
        fp_result_t r = fp_parse_landmarks(c, arr_i, origin, out);
        if (r != FP_OK) return r;
    }
    return FP_OK;
}

/* ---------------------------------------------------------------------
 * fp_parse_inner — populates `out` directly (caller-provided, already
 * zeroed by fp_parse). Any non-OK return leaves `out` partially
 * written; fp_parse() re-zeros it before returning to the caller, so
 * that partial state never escapes this translation unit.
 * ------------------------------------------------------------------- */
static fp_result_t fp_parse_inner(fp_ctx_t const *c, fp_pack_t *out)
{
    int vi;
    if (!fp_obj_get(c, 0, "festpack", &vi) || !fp_tok_eq(c, vi, "0.1")) return FP_ERR_VERSION;

    int fest_i = -1;
    fp_obj_get(c, 0, "festival", &fest_i);
    ff_latlon_t origin = {0.0, 0.0};
    if (fest_i >= 0 && !fp_is_null(c, fest_i)) {
        fp_result_t r = fp_parse_festival(c, fest_i, out, &origin);
        if (r != FP_OK) return r;
    }
    out->origin = origin;

    /* utc_offset_min is an optional v1 extension field (not yet in every
     * real-world pack — e.g. the vendored Lost Lands 2026 fixture omits
     * it entirely). Check top-level first, then nested under "festival"
     * as a fallback, then default to -240 (EDT, the Lost Lands venue's
     * standard September UTC offset). See docs/specs/S05-festpack.md and
     * the S05 PR body for the interpretation call.
     *
     * A wrong-typed value (e.g. a quoted "-240"), a non-finite value
     * (Infinity/NaN/1e400), or a finite-but-out-of-int16-range value at
     * either location is treated exactly like the field being absent
     * there — it falls through to the next location, and ultimately to
     * the default — rather than marking utc_offset_assumed false while
     * quietly holding the -240 default (or, for the range/finiteness
     * cases, invoking undefined behavior by casting an unrepresentable
     * double to int16_t). ff_shell.c reads utc_offset_assumed as the S18
     * wall-clock-trust signal, so a bad-but-present value must not
     * outrank the user's manual setting. fp_i16_checked() does the
     * finite+range-checked extraction; see its comment and
     * fp_num_checked()'s. See docs/specs/S05-festpack.md's Amendments
     * entry. */
    {
        int t;
        int16_t v;
        if (fp_obj_get(c, 0, "utc_offset_min", &t) && !fp_is_null(c, t) && fp_i16_checked(c, t, &v)) {
            out->utc_offset_min = v;
            out->utc_offset_assumed = false;
        } else if (fest_i >= 0 && fp_obj_get(c, fest_i, "utc_offset_min", &t) && !fp_is_null(c, t) &&
                   fp_i16_checked(c, t, &v)) {
            out->utc_offset_min = v;
            out->utc_offset_assumed = false;
        } else {
            out->utc_offset_min = -240;
            out->utc_offset_assumed = true;
        }
    }

    int arr_i;
    if (fp_obj_get(c, 0, "stages", &arr_i) && !fp_is_null(c, arr_i)) {
        fp_result_t r = fp_parse_stages(c, arr_i, out);
        if (r != FP_OK) return r;
    }
    if (fp_obj_get(c, 0, "schedule", &arr_i) && !fp_is_null(c, arr_i)) {
        fp_result_t r = fp_parse_schedule(c, arr_i, out);
        if (r != FP_OK) return r;
    }

    int map_i = -1;
    fp_obj_get(c, 0, "map", &map_i);
    if (map_i >= 0 && !fp_is_null(c, map_i)) {
        fp_result_t r = fp_parse_map(c, map_i, origin, out);
        if (r != FP_OK) return r;
    }

    /* 2026-09-11 amendment — additive, never fails the parse. */
    int meta_i = -1;
    fp_obj_get(c, 0, "meta", &meta_i);
    if (meta_i >= 0 && !fp_is_null(c, meta_i)) {
        fp_parse_meta(c, meta_i, out);
    }

    return FP_OK;
}

fp_result_t fp_parse(char const *json, size_t len, fp_pack_t *out, jsmntok_t *toks, int ntoks)
{
    if (out == NULL) return FP_ERR_JSON;
    memset(out, 0, sizeof(*out));

    if (json == NULL || len == 0) return FP_ERR_JSON;
    if (len > FP_MAX_JSON_LEN) return FP_ERR_TOO_BIG;
    /* Caller-supplied scratch (S26 slice a — see fp_pack.h). A NULL
     * buffer or non-positive capacity is treated the same as jsmn
     * running out of tokens mid-parse: FP_ERR_TOO_BIG, never a deref of
     * a null/undersized array. */
    if (toks == NULL || ntoks <= 0) return FP_ERR_TOO_BIG;

    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, len, toks, (unsigned int)ntoks);
    if (r == JSMN_ERROR_NOMEM) return FP_ERR_TOO_BIG;
    if (r < 0 || r == 0) return FP_ERR_JSON; /* INVAL / PART / empty */

    bool depth_exceeded = false;
    fp_ctx_t ctx = {json, toks, r, &depth_exceeded};
    if (toks[0].type != JSMN_OBJECT) return FP_ERR_JSON;

    fp_result_t res = fp_parse_inner(&ctx, out);
    /* A depth-capped fp_skip() means some subtree was walked only
     * partway (see fp_skip_depth()) — nothing extracted downstream of
     * it can be trusted, even if fp_parse_inner() otherwise reported
     * FP_OK. Force the honest answer: malformed/hostile input, not a
     * successful parse. */
    if (depth_exceeded) res = FP_ERR_JSON;
    if (res != FP_OK) memset(out, 0, sizeof(*out));
    return res;
}
