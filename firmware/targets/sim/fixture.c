/**
 * fixture.c — ff_app_state_t JSON fixture loader implementation.
 *
 * Vendored jsmn (firmware/third_party/jsmn.h) tokenizes the input into a
 * flat, zero-alloc token array; the extraction below walks that array by
 * hand, matching keys and copying/converting into the caller-provided
 * ff_app_state_t. Same shape as firmware/festpack/src/fp_pack.c's
 * fp_ctx_t/fp_obj_get/fp_skip helpers (duplicated here, not shared,
 * because fixture.c intentionally has zero dependency on festpack — see
 * fixture.h's header comment, and CMakeLists.txt: this loader is
 * targets/sim-only scaffolding, not an extraction-grade library).
 *
 * Tolerant of unknown keys and missing sections, matching fp_pack.c's
 * "schema will grow" philosophy — every ff_app_state_t section is
 * optional in the JSON; an absent section leaves that struct region
 * zeroed. See tests/fixtures/README.md for the documented schema.
 */
#define JSMN_STATIC
#include "jsmn.h"

#include "fixture.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ff_crew.h" /* FF_CREW_MAX — radar.dots[] cap, see fx_parse_radar_dots below */
#include "ff_settings.h" /* FF_BRIGHTNESS_DEFAULT_PCT — settings.brightness_pct default (#100) */

/* Input-size / token-arena budget. Fixtures are small, hand-authored
 * dev/test data (not attacker-controlled RF bytes like festpack's input),
 * but the same zero-alloc fixed-arena discipline applies per
 * docs/ARCHITECTURE.md ("no allocation surprises"). */
#define FIX_MAX_JSON_LEN (16u * 1024u)
#define FIX_MAX_TOKENS 2048

typedef struct {
    char const *js;
    jsmntok_t const *toks;
    int ntoks;
} fx_ctx_t;

/* ---------------------------------------------------------------------
 * Token-array helpers (see firmware/festpack/src/fp_pack.c's fp_* twins
 * for the original documented rationale; unchanged behavior here).
 * ------------------------------------------------------------------- */

static int fx_skip(fx_ctx_t const *c, int i)
{
    if (i < 0 || i >= c->ntoks) return c->ntoks;
    jsmntok_t const *t = &c->toks[i];
    int next = i + 1;
    if (t->type == JSMN_OBJECT) {
        for (int k = 0; k < t->size; k++) {
            next = fx_skip(c, next); /* key */
            next = fx_skip(c, next); /* value */
        }
    } else if (t->type == JSMN_ARRAY) {
        for (int k = 0; k < t->size; k++) {
            next = fx_skip(c, next);
        }
    }
    return next;
}

static bool fx_obj_get(fx_ctx_t const *c, int obj_i, char const *key, int *val_i)
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
        int val_index = key_i + 1; /* keys are plain strings: one token */
        if (kt->type == JSMN_STRING && (size_t)(kt->end - kt->start) == keylen &&
            memcmp(c->js + kt->start, key, keylen) == 0) {
            *val_i = val_index;
            return true;
        }
        i = fx_skip(c, val_index);
    }
    return false;
}

static bool fx_is_null(fx_ctx_t const *c, int i)
{
    if (i < 0 || i >= c->ntoks) return true;
    jsmntok_t const *t = &c->toks[i];
    return t->type == JSMN_PRIMITIVE && (t->end - t->start) == 4 &&
           memcmp(c->js + t->start, "null", 4) == 0;
}

/* Appends the UTF-8 encoding of a single BMP code point to *w (bounded by
 * end), advancing *w. Surrogate pairs (\uD800-\uDFFF) aren't handled —
 * S13c's dump function (fw_json_str) never emits them (every byte >=
 * 0x20 is passed through raw, UTF-8 and all; only control bytes get
 * \u-escaped, and control bytes are always single UTF-16 code units) —
 * so this is a non-goal here, not a silently-wrong path: a lone
 * surrogate from some other JSON source falls through the ASCII/2-byte
 * cases below and encodes as an (invalid but bounded, non-crashing)
 * 3-byte sequence, same as any other BMP code point. */
static void fx_append_utf8(char **w, char const *end, unsigned cp)
{
    if (cp < 0x80) {
        if ((end - *w) >= 1) *(*w)++ = (char)cp;
    } else if (cp < 0x800) {
        if ((end - *w) >= 2) {
            *(*w)++ = (char)(0xC0 | (cp >> 6));
            *(*w)++ = (char)(0x80 | (cp & 0x3F));
        }
    } else {
        if ((end - *w) >= 3) {
            *(*w)++ = (char)(0xE0 | (cp >> 12));
            *(*w)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *(*w)++ = (char)(0x80 | (cp & 0x3F));
        }
    }
}

static int fx_hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* fx_copy_str — copies a JSMN_STRING token, decoding standard JSON
 * backslash escapes (\" \\ \/ \n \r \t \b \f \uXXXX) rather than copying
 * jsmn's raw (still-escaped) substring verbatim.
 *
 * jsmn itself never decodes escapes (it only finds string boundaries) —
 * this was never a problem for the hand-authored fixtures under
 * tests/fixtures/ (none of their string fields use quotes/backslashes),
 * but it broke ff_fixture_dump_json's round-trip contract the moment a
 * dumped name/text field (which CAN contain arbitrary live mesh data —
 * see fixture.h's ff_fixture_dump_json doc comment) needed escaping: the
 * reloaded string came back with literal backslashes still in it instead
 * of the original characters. Decoding here, once, fixes every caller
 * (fixture loading AND the ctl socket's round-trip contract) instead of
 * papering over it only where it was first noticed.
 *
 * Truncates to `dst_sz` like before if needed; an incomplete/malformed
 * escape at the very end of the token (should never happen for
 * well-formed JSON, which is all jsmn will have accepted) is written out
 * literally rather than read past the token's bounds. */
static void fx_copy_str(fx_ctx_t const *c, int i, char *dst, size_t dst_sz)
{
    dst[0] = '\0';
    if (i < 0 || i >= c->ntoks || dst_sz == 0) return;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_STRING) return;

    char const *src = c->js + t->start;
    char const *src_end = c->js + t->end;
    char *w = dst;
    char *w_end = dst + (dst_sz - 1); /* reserve the final NUL */

    while (src < src_end && w < w_end) {
        if (*src != '\\') {
            *w++ = *src++;
            continue;
        }
        /* Backslash with nothing after it (shouldn't happen inside a
         * jsmn-validated string, but bounds-check regardless): stop
         * rather than read past src_end. */
        if (src + 1 >= src_end) break;
        char esc = src[1];
        switch (esc) {
            case '"': *w++ = '"'; src += 2; break;
            case '\\': *w++ = '\\'; src += 2; break;
            case '/': *w++ = '/'; src += 2; break;
            case 'n': *w++ = '\n'; src += 2; break;
            case 'r': *w++ = '\r'; src += 2; break;
            case 't': *w++ = '\t'; src += 2; break;
            case 'b': *w++ = '\b'; src += 2; break;
            case 'f': *w++ = '\f'; src += 2; break;
            case 'u': {
                if (src + 6 > src_end) {
                    /* Truncated \u escape: emit literally, bounded. */
                    *w++ = *src++;
                    break;
                }
                int d0 = fx_hex_digit(src[2]), d1 = fx_hex_digit(src[3]);
                int d2 = fx_hex_digit(src[4]), d3 = fx_hex_digit(src[5]);
                if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) {
                    *w++ = *src++;
                    break;
                }
                unsigned cp = ((unsigned)d0 << 12) | ((unsigned)d1 << 8) | ((unsigned)d2 << 4) | (unsigned)d3;
                fx_append_utf8(&w, w_end, cp);
                src += 6;
                break;
            }
            default:
                /* Unrecognized escape: emit the backslash literally
                 * (matches "tolerant, never a hard parse failure" style
                 * used throughout this loader) and let the next byte be
                 * read normally on the next loop iteration. */
                *w++ = *src++;
                break;
        }
    }
    *w = '\0';
}

static double fx_num(fx_ctx_t const *c, int i, double dflt)
{
    if (i < 0 || i >= c->ntoks) return dflt;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_PRIMITIVE) return dflt;
    int n = t->end - t->start;
    if (n == 4 && memcmp(c->js + t->start, "null", 4) == 0) return dflt;
    char buf[32];
    if (n <= 0 || (size_t)n >= sizeof(buf)) return dflt;
    memcpy(buf, c->js + t->start, (size_t)n);
    buf[n] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return dflt;
    return v;
}

static bool fx_bool(fx_ctx_t const *c, int i, bool dflt)
{
    if (i < 0 || i >= c->ntoks) return dflt;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_PRIMITIVE) return dflt;
    int n = t->end - t->start;
    if (n == 4 && memcmp(c->js + t->start, "true", 4) == 0) return true;
    if (n == 5 && memcmp(c->js + t->start, "false", 5) == 0) return false;
    return dflt;
}

/* String -> enum lookup over a {name, value} table. Fail-loud (issue
 * #28, orchestrator ruling — this used to silently return a default on
 * anything unrecognized): fixtures aren't just dev convenience, they're
 * the inputs to the golden suite, so a typo'd enum string ("mixxed",
 * "no-pack") didn't fail, it silently rendered a DIFFERENT state and
 * then committed that as the golden — a test green forever about the
 * wrong screen. Dev data that's wrong now refuses to load, with a
 * one-line stderr diagnostic naming the bad key and value, consistent
 * with the loader's existing FF_FIXTURE_ERR_TOO_BIG treatment of
 * over-cap arrays (tolerant enums were the inconsistent case within the
 * same file, not a deliberate global stance).
 *
 * On a match, writes the mapped value to *out and returns FF_FIXTURE_OK;
 * on an unmatched string OR a non-string token, prints the diagnostic
 * and returns FF_FIXTURE_ERR_BAD_ENUM. An ABSENT key never reaches this
 * function — callers only invoke it after fx_obj_get found the key — so
 * every "field omitted -> documented default" behavior is unchanged
 * (absent != malformed; see tests/fixtures/README.md). */
typedef struct {
    char const *name;
    int value;
} fx_enum_entry_t;

static ff_fixture_result_t fx_enum(fx_ctx_t const *c, int i, fx_enum_entry_t const *table, size_t n_entries,
                                   char const *key, int *out)
{
    if (i >= 0 && i < c->ntoks) {
        jsmntok_t const *t = &c->toks[i];
        if (t->type == JSMN_STRING) {
            int n = t->end - t->start;
            for (size_t k = 0; k < n_entries; k++) {
                size_t slen = strlen(table[k].name);
                if ((size_t)n == slen && memcmp(c->js + t->start, table[k].name, slen) == 0) {
                    *out = table[k].value;
                    return FF_FIXTURE_OK;
                }
            }
            fprintf(stderr, "fixture: unrecognized value \"%.*s\" for key \"%s\"\n", n, c->js + t->start, key);
            return FF_FIXTURE_ERR_BAD_ENUM;
        }
    }
    fprintf(stderr, "fixture: key \"%s\" must be a string enum, got a non-string value\n", key);
    return FF_FIXTURE_ERR_BAD_ENUM;
}

static int fx_hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* fx_color_rgb — reads `stage_color_rgb` (tests/fixtures/README.md:
 * "accepts a \"#rrggbb\" string or a bare integer"). A JSON string token
 * is parsed as "#rrggbb" (leading '#' optional) into 0x00RRGGBB, same
 * convention as festpack's fp_color_rgb(); a JSON number token is taken
 * as the packed integer directly.
 *
 * Returns true and writes the parsed value to *out on success; returns
 * false (and leaves *out untouched) for any other token type or a
 * malformed hex string. PR #21 code review finding #3: this used to
 * return a plain uint32_t with a `dflt` fallback (0) for failure — the
 * SAME 0 a legitimately black "#000000" stage color parses to, so a
 * malformed color and a real black one were bit-for-bit indistinguishable
 * downstream, and the renderer's "0 means unset" convention silently
 * masked BOTH a genuine black stage and a broken fixture value as the
 * same muted-grey fallback. Reporting success/failure explicitly lets the
 * caller (fx_parse_now_row) set ff_app_now_row_t.stage_color_valid
 * correctly instead of overloading the color value itself as its own
 * validity signal. (Review finding #1, still true here: a JSON_STRING
 * token is not a JSMN_PRIMITIVE, so the numeric path must not swallow the
 * string form — that bug predates this refactor and remains fixed.) */
static bool fx_color_rgb(fx_ctx_t const *c, int i, uint32_t *out)
{
    if (i < 0 || i >= c->ntoks) return false;
    jsmntok_t const *t = &c->toks[i];
    if (t->type == JSMN_STRING) {
        char const *s = c->js + t->start;
        int n = t->end - t->start;
        if (n > 0 && s[0] == '#') {
            s++;
            n--;
        }
        if (n != 6) return false;
        uint32_t v = 0;
        for (int k = 0; k < 6; k++) {
            int nib = fx_hex_nibble(s[k]);
            if (nib < 0) return false;
            v = (v << 4) | (uint32_t)nib;
        }
        *out = v;
        return true;
    }
    if (t->type == JSMN_PRIMITIVE) {
        /* fx_num() itself has its own null/malformed-primitive fallback
         * (returns its `dflt` argument) — pass a sentinel we can never
         * confuse with a real result and check for it explicitly, rather
         * than trusting fx_num()'s success implicitly. */
        double dflt_sentinel = -1.0;
        double v = fx_num(c, i, dflt_sentinel);
        if (v == dflt_sentinel) {
            return false;
        }
        *out = (uint32_t)v;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------
 * Section parsers.
 * ------------------------------------------------------------------- */

static const fx_enum_entry_t fx_radar_mode_table[] = {
    {"live", RADAR_LIVE}, {"stale", RADAR_STALE}, {"lost", RADAR_LOST},
    {"place", RADAR_PLACE}, /* issue #33 */
    {"close", RADAR_CLOSE}, {"nofix", RADAR_NOFIX}, {"nosel", RADAR_NOSEL},
};

/* fx_parse_radar_dots — fail-loud on an oversized array (orchestrator
 * ruling on PR review finding #3/#4: consistent with fp_parse's
 * FP_ERR_TOO_BIG and the honest-data culture, an over-cap array is
 * rejected outright rather than silently truncated — a fixture that
 * grows a 9th dot should get a loud "fixture X has 9 items, cap is 8"
 * failure, not a quietly-dropped entry that surfaces later as an
 * unrelated-looking golden diff). The size check runs BEFORE any writes
 * into `r->dots[]`, so `r->n_dots` never exceeds FF_CREW_MAX
 * on any code path through this function — see test_fixture.c's
 * `radar_dots_over_cap_fails_loud` for the regression test (and the
 * mutation-testing rationale: if this check is ever deleted, that test
 * stops observing FF_FIXTURE_ERR_TOO_BIG and fails, rather than the
 * cap-overflow going unnoticed as before). */
static ff_fixture_result_t fx_parse_radar_dots(fx_ctx_t const *c, int arr_i, ff_radar_view_t *r)
{
    jsmntok_t const *at = &c->toks[arr_i];
    if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
    if (at->size > FF_CREW_MAX) return FF_FIXTURE_ERR_TOO_BIG;
    int idx = arr_i + 1;
    for (int i = 0; i < at->size; i++) {
        int obj_i = idx;
        ff_radar_dot_t *d = &r->dots[r->n_dots];
        memset(d, 0, sizeof(*d));
        int t;
        if (fx_obj_get(c, obj_i, "ring_deg", &t)) d->ring_deg = (float)fx_num(c, t, 0.0);
        if (fx_obj_get(c, obj_i, "initial", &t)) {
            char buf[2];
            fx_copy_str(c, t, buf, sizeof(buf));
            d->initial = buf[0];
        }
        if (fx_obj_get(c, obj_i, "color_idx", &t)) d->color_idx = (uint8_t)fx_num(c, t, 0.0);
        if (fx_obj_get(c, obj_i, "stale", &t)) d->stale = fx_bool(c, t, false);
        if (fx_obj_get(c, obj_i, "place", &t)) d->place = fx_bool(c, t, false); /* issue #33 */
        if (fx_obj_get(c, obj_i, "imprecise", &t)) d->imprecise = fx_bool(c, t, false); /* issue #74 */
        r->n_dots++;
        idx = fx_skip(c, obj_i);
    }
    return FF_FIXTURE_OK;
}

static ff_fixture_result_t fx_parse_radar(fx_ctx_t const *c, int obj_i, ff_radar_view_t *r)
{
    int t;
    if (fx_obj_get(c, obj_i, "mode", &t)) {
        int v;
        ff_fixture_result_t rc = fx_enum(c, t, fx_radar_mode_table,
                                          sizeof(fx_radar_mode_table) / sizeof(fx_radar_mode_table[0]),
                                          "radar.mode", &v);
        if (rc != FF_FIXTURE_OK) return rc;
        r->mode = (radar_mode_t)v;
    }
    if (fx_obj_get(c, obj_i, "arrow_deg", &t)) r->arrow_deg = (float)fx_num(c, t, 0.0);
    if (fx_obj_get(c, obj_i, "arrow_valid", &t)) r->arrow_valid = fx_bool(c, t, false);
    if (fx_obj_get(c, obj_i, "name", &t)) fx_copy_str(c, t, r->name, sizeof(r->name));
    if (fx_obj_get(c, obj_i, "dist_str", &t)) fx_copy_str(c, t, r->dist_str, sizeof(r->dist_str));
    if (fx_obj_get(c, obj_i, "dist_imprecise", &t)) r->dist_imprecise = fx_bool(c, t, false); /* issue #47 */
    if (fx_obj_get(c, obj_i, "age_str", &t)) fx_copy_str(c, t, r->age_str, sizeof(r->age_str));
    if (fx_obj_get(c, obj_i, "trend", &t)) r->trend = (int8_t)fx_num(c, t, 0.0);
    if (fx_obj_get(c, obj_i, "clock_str", &t)) fx_copy_str(c, t, r->clock_str, sizeof(r->clock_str));
    if (fx_obj_get(c, obj_i, "batt_pct", &t)) r->batt_pct = (int8_t)fx_num(c, t, -1.0);
    if (fx_obj_get(c, obj_i, "mesh_ok", &t)) r->mesh_ok = fx_bool(c, t, false);
    int dots_i;
    if (fx_obj_get(c, obj_i, "dots", &dots_i) && !fx_is_null(c, dots_i)) {
        ff_fixture_result_t rc = fx_parse_radar_dots(c, dots_i, r);
        if (rc != FF_FIXTURE_OK) return rc;
    }
    return FF_FIXTURE_OK;
}

static void fx_parse_now_row(fx_ctx_t const *c, int obj_i, ff_app_now_row_t *row)
{
    int t;
    if (fx_obj_get(c, obj_i, "artist", &t)) fx_copy_str(c, t, row->artist, sizeof(row->artist));
    if (fx_obj_get(c, obj_i, "stage_name", &t)) fx_copy_str(c, t, row->stage_name, sizeof(row->stage_name));
    /* PR #21 code review finding #3: stage_color_valid is set from
     * fx_color_rgb's own success/failure report, not inferred from
     * whether the parsed value is nonzero — see that function's doc
     * comment for why the old "0 == unset" convention was a bug (masked
     * both a genuine black stage AND a malformed fixture value the same
     * way). Key left absent entirely -> stays false (the zeroed default),
     * same as every other "not provided" field in this parser. */
    if (fx_obj_get(c, obj_i, "stage_color_rgb", &t)) {
        uint32_t color = 0;
        row->stage_color_valid = fx_color_rgb(c, t, &color);
        row->stage_color_rgb = row->stage_color_valid ? color : 0;
    }
    if (fx_obj_get(c, obj_i, "pct_done", &t)) row->pct_done = (uint8_t)fx_num(c, t, 0.0);
    /* 2026-08-24: pct_valid absent -> stays false (the zeroed default
     * from fx_parse_now's memset before this call) — same "absent ->
     * least-claiming" convention as stage_color_valid/arrow_valid. A
     * fixture authored before this field existed (now_live.json,
     * now_mixed.json) must set this explicitly to keep rendering its bar
     * — see ff_app_now_row_t's doc comment. */
    if (fx_obj_get(c, obj_i, "pct_valid", &t)) row->pct_valid = fx_bool(c, t, false);
}

static void fx_parse_lineup_item(fx_ctx_t const *c, int obj_i, ff_app_lineup_item_t *item)
{
    int t;
    if (fx_obj_get(c, obj_i, "artist", &t)) fx_copy_str(c, t, item->artist, sizeof(item->artist));
    if (fx_obj_get(c, obj_i, "stage_name", &t)) fx_copy_str(c, t, item->stage_name, sizeof(item->stage_name));
}

/* PR #21 code review finding #2/ruling: `now.state` replaces the earlier
 * `pack_loaded`+`tbd` bool pair, same string-enum convention as
 * fx_radar_mode_table above. Absent -> NOW_NO_PACK (the enum's zero
 * value — see now_state_t's own doc comment for why that's the correct
 * default, same reasoning as radar.mode's RADAR_NOSEL default).
 * Unrecognized -> FF_FIXTURE_ERR_BAD_ENUM as of issue #28 (it used to
 * silently take the same default; see fx_enum's doc comment). */
static const fx_enum_entry_t fx_now_state_table[] = {
    {"no_pack", NOW_NO_PACK},
    {"tbd", NOW_TBD},
    {"mixed", NOW_MIXED},
    {"live", NOW_LIVE},
    {"nothing_playing", NOW_NOTHING_PLAYING},
    {"time_unknown", NOW_TIME_UNKNOWN}, /* issue #48 */
};

/* fx_parse_now — same fail-loud-on-oversized-array treatment as
 * fx_parse_radar_dots above, for the `rows` array (cap
 * FF_APP_NOW_MAX_ROWS) and, as of S07 slice b, the `lineup` array (cap
 * FF_APP_NOW_MAX_LINEUP). */
static ff_fixture_result_t fx_parse_now(fx_ctx_t const *c, int obj_i, ff_app_now_t *now)
{
    int t;
    if (fx_obj_get(c, obj_i, "state", &t)) {
        int v;
        ff_fixture_result_t rc = fx_enum(c, t, fx_now_state_table,
                                          sizeof(fx_now_state_table) / sizeof(fx_now_state_table[0]),
                                          "now.state", &v);
        if (rc != FF_FIXTURE_OK) return rc;
        now->state = (now_state_t)v;
    }

    int rows_i;
    if (fx_obj_get(c, obj_i, "rows", &rows_i) && !fx_is_null(c, rows_i)) {
        jsmntok_t const *at = &c->toks[rows_i];
        if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
        if (at->size > FF_APP_NOW_MAX_ROWS) return FF_FIXTURE_ERR_TOO_BIG;
        int idx = rows_i + 1;
        for (int i = 0; i < at->size; i++) {
            int row_obj_i = idx;
            memset(&now->rows[now->n_rows], 0, sizeof(now->rows[0]));
            fx_parse_now_row(c, row_obj_i, &now->rows[now->n_rows]);
            now->n_rows++;
            idx = fx_skip(c, row_obj_i);
        }
    }
    int next_i;
    if (fx_obj_get(c, obj_i, "next", &next_i) && !fx_is_null(c, next_i)) {
        now->next.valid = true;
        if (fx_obj_get(c, next_i, "artist", &t)) fx_copy_str(c, t, now->next.artist, sizeof(now->next.artist));
        if (fx_obj_get(c, next_i, "stage_name", &t))
            fx_copy_str(c, t, now->next.stage_name, sizeof(now->next.stage_name));
        if (fx_obj_get(c, next_i, "mins_until", &t)) now->next.mins_until = (int16_t)fx_num(c, t, 0.0);
    }

    int lineup_i;
    if (fx_obj_get(c, obj_i, "lineup", &lineup_i) && !fx_is_null(c, lineup_i)) {
        jsmntok_t const *at = &c->toks[lineup_i];
        if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
        if (at->size > FF_APP_NOW_MAX_LINEUP) return FF_FIXTURE_ERR_TOO_BIG;
        int idx = lineup_i + 1;
        for (int i = 0; i < at->size; i++) {
            int item_obj_i = idx;
            memset(&now->lineup[now->n_lineup], 0, sizeof(now->lineup[0]));
            fx_parse_lineup_item(c, item_obj_i, &now->lineup[now->n_lineup]);
            now->n_lineup++;
            idx = fx_skip(c, item_obj_i);
        }
    }
    return FF_FIXTURE_OK;
}

/* S24: the `signals` fixture section describes an `ff_app_signals_t`
 * directly — the sub-view selector + the core `ff_inbox_t` conversation
 * list + the kept S22(d) target fields — the same "fixture is a view
 * snapshot" convention the `radar` section uses; see ff_app_state.h's
 * signals doc comment. Tables: `fx_subview_table`'s values are
 * FF_SIG_SUB_*, `fx_conv_kind_table`'s FF_CONV_*, `fx_feed_kind_table`'s
 * ff_feed_kind_t (S08's FEED_*), `fx_feed_dir_table`'s ff_feed_dir_t
 * (S24 slice a's FEED_DIR_*), `fx_presence_table`'s FF_PRESENCE_*, and
 * `fx_target_kind_table`'s FF_TARGET_*. */
static const fx_enum_entry_t fx_subview_table[] = {
    {"inbox", FF_SIG_SUB_INBOX},   {"picker", FF_SIG_SUB_PICKER}, {"thread", FF_SIG_SUB_THREAD},
    {"popup", FF_SIG_SUB_POPUP},   {"rally", FF_SIG_SUB_RALLY},
};

static const fx_enum_entry_t fx_conv_kind_table[] = {
    {"crew", FF_CONV_CREW},
    {"member", FF_CONV_MEMBER},
};

/* S24 slice d — the Rally WHEN chip. */
static const fx_enum_entry_t fx_rally_when_table[] = {
    {"now", FF_RALLY_WHEN_NOW},
    {"15", FF_RALLY_WHEN_15},
    {"30", FF_RALLY_WHEN_30},
};

static const fx_enum_entry_t fx_feed_kind_table[] = {
    {"pulse", FEED_PULSE}, {"text", FEED_TEXT}, {"rally", FEED_RALLY},
    {"status", FEED_STATUS}, {"flare", FEED_FLARE},
};

static const fx_enum_entry_t fx_feed_dir_table[] = {
    {"unknown", FEED_DIR_UNKNOWN},
    {"broadcast", FEED_DIR_BROADCAST},
    {"direct", FEED_DIR_DIRECT},
    {"out", FEED_DIR_OUT},
};

static const fx_enum_entry_t fx_presence_table[] = {
    {"seen", FF_PRESENCE_SEEN},
    {"lost", FF_PRESENCE_LOST},
    {"linked", FF_PRESENCE_LINKED},
};

static const fx_enum_entry_t fx_target_kind_table[] = {
    {"whole_crew", FF_TARGET_WHOLE_CREW},
    {"member", FF_TARGET_MEMBER},
};

/* fx_parse_signals — same fail-loud-on-oversized-array treatment as
 * fx_parse_radar_dots above, for the `convs` array (cap
 * FF_INBOX_MAX_CONVS). Derived-but-not-independent facts follow the
 * model's own invariants rather than being separately authorable:
 * `has_preview` is implied by `item_count > 0` (ff_inbox.h's contract),
 * `presence_valid` by the conversation being a member, and
 * `preview_from_known` by a `preview_from` key being present (a fixture
 * that writes an empty `preview_from` still means "known but unnamed" —
 * a paired member whose NodeInfo never arrived). */
static ff_fixture_result_t fx_parse_signals(fx_ctx_t const *c, int obj_i, ff_app_signals_t *sig)
{
    int t;

    if (fx_obj_get(c, obj_i, "subview", &t)) {
        int v;
        ff_fixture_result_t rc = fx_enum(c, t, fx_subview_table,
                                          sizeof(fx_subview_table) / sizeof(fx_subview_table[0]),
                                          "signals.subview", &v);
        if (rc != FF_FIXTURE_OK) return rc;
        sig->subview = (ff_sig_subview_t)v;
    }
    if (fx_obj_get(c, obj_i, "thread_node", &t)) sig->thread_node = (uint32_t)fx_num(c, t, 0.0);
    if (fx_obj_get(c, obj_i, "thread_name", &t)) fx_copy_str(c, t, sig->thread_name, sizeof(sig->thread_name));
    if (fx_obj_get(c, obj_i, "thread_color_idx", &t)) sig->thread_color_idx = (uint8_t)fx_num(c, t, 0.0);

    if (fx_obj_get(c, obj_i, "target_kind", &t)) {
        int v;
        ff_fixture_result_t rc = fx_enum(c, t, fx_target_kind_table,
                                          sizeof(fx_target_kind_table) / sizeof(fx_target_kind_table[0]),
                                          "signals.target_kind", &v);
        if (rc != FF_FIXTURE_OK) return rc;
        sig->target_kind = (ff_target_kind_t)v;
    }
    if (fx_obj_get(c, obj_i, "target_node", &t)) sig->target_node = (uint32_t)fx_num(c, t, 0.0);
    /* S22 slice d — the RALLY-to-WHOLE_CREW confirm display flag (AC4). */
    if (fx_obj_get(c, obj_i, "rally_confirm_armed", &t)) sig->rally_confirm_armed = fx_bool(c, t, false);

    int convs_i;
    if (fx_obj_get(c, obj_i, "convs", &convs_i) && !fx_is_null(c, convs_i)) {
        jsmntok_t const *at = &c->toks[convs_i];
        if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
        if (at->size > FF_INBOX_MAX_CONVS) return FF_FIXTURE_ERR_TOO_BIG;
        int idx = convs_i + 1;
        for (int i = 0; i < at->size; i++) {
            int conv_i = idx;
            ff_inbox_conv_t *cv = &sig->inbox.convs[sig->inbox.conv_count];
            memset(cv, 0, sizeof(*cv));

            int kt;
            if (fx_obj_get(c, conv_i, "conv", &kt)) {
                int v;
                ff_fixture_result_t rc = fx_enum(c, kt, fx_conv_kind_table,
                                                  sizeof(fx_conv_kind_table) / sizeof(fx_conv_kind_table[0]),
                                                  "signals.convs[].conv", &v);
                if (rc != FF_FIXTURE_OK) return rc;
                cv->kind = (ff_conv_kind_t)v;
            }
            if (fx_obj_get(c, conv_i, "node_id", &kt)) cv->node_id = (uint32_t)fx_num(c, kt, 0.0);
            if (fx_obj_get(c, conv_i, "name", &kt)) fx_copy_str(c, kt, cv->name, sizeof(cv->name));
            if (fx_obj_get(c, conv_i, "initial", &kt)) {
                char buf[2];
                fx_copy_str(c, kt, buf, sizeof(buf));
                cv->initial = buf[0];
            }
            if (fx_obj_get(c, conv_i, "color_idx", &kt)) cv->color_idx = (uint8_t)fx_num(c, kt, 0.0);
            if (fx_obj_get(c, conv_i, "unread", &kt)) cv->unread = (uint16_t)fx_num(c, kt, 0.0);
            if (fx_obj_get(c, conv_i, "item_count", &kt)) cv->item_count = (uint8_t)fx_num(c, kt, 0.0);

            if (fx_obj_get(c, conv_i, "preview_kind", &kt)) {
                int v;
                ff_fixture_result_t rc = fx_enum(c, kt, fx_feed_kind_table,
                                                  sizeof(fx_feed_kind_table) / sizeof(fx_feed_kind_table[0]),
                                                  "signals.convs[].preview_kind", &v);
                if (rc != FF_FIXTURE_OK) return rc;
                cv->preview_kind = (ff_feed_kind_t)v;
            }
            if (fx_obj_get(c, conv_i, "preview_dir", &kt)) {
                int v;
                ff_fixture_result_t rc = fx_enum(c, kt, fx_feed_dir_table,
                                                  sizeof(fx_feed_dir_table) / sizeof(fx_feed_dir_table[0]),
                                                  "signals.convs[].preview_dir", &v);
                if (rc != FF_FIXTURE_OK) return rc;
                cv->preview_dir = (ff_feed_dir_t)v;
            }
            if (fx_obj_get(c, conv_i, "preview_text", &kt))
                fx_copy_str(c, kt, cv->preview_text, sizeof(cv->preview_text));
            if (fx_obj_get(c, conv_i, "preview_age_ms", &kt))
                cv->preview_age_ms = (uint32_t)fx_num(c, kt, 0.0);
            if (fx_obj_get(c, conv_i, "preview_from", &kt)) {
                cv->preview_from_known = true;
                fx_copy_str(c, kt, cv->preview_from_name, sizeof(cv->preview_from_name));
            }

            if (fx_obj_get(c, conv_i, "presence", &kt)) {
                int v;
                ff_fixture_result_t rc = fx_enum(c, kt, fx_presence_table,
                                                  sizeof(fx_presence_table) / sizeof(fx_presence_table[0]),
                                                  "signals.convs[].presence", &v);
                if (rc != FF_FIXTURE_OK) return rc;
                cv->presence = (ff_sigview_presence_t)v;
            }
            if (fx_obj_get(c, conv_i, "presence_age_ms", &kt))
                cv->presence_age_ms = (uint32_t)fx_num(c, kt, 0.0);

            /* Model invariants, not independent fixture facts. */
            cv->has_preview    = (cv->item_count > 0);
            cv->presence_valid = (cv->kind == FF_CONV_MEMBER);

            sig->inbox.conv_count++;
            idx = fx_skip(c, conv_i);
        }
    }

    /* S24 slice (c) — the open thread's messages (`ff_inbox_msg_t`),
     * oldest first, same fail-loud over-cap treatment as `convs`. The
     * loader DERIVES `identity_known` from the presence of a `from` key
     * (mirroring `preview_from`: writing `"from": ""` still means "known
     * but unnamed" — a paired member whose NodeInfo never arrived; no
     * key at all is an honest unjoined sender). */
    int msgs_i;
    if (fx_obj_get(c, obj_i, "msgs", &msgs_i) && !fx_is_null(c, msgs_i)) {
        jsmntok_t const *at = &c->toks[msgs_i];
        if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
        if (at->size > FF_INBOX_MAX_MSGS) return FF_FIXTURE_ERR_TOO_BIG;
        int idx = msgs_i + 1;
        for (int i = 0; i < at->size; i++) {
            int msg_i = idx;
            ff_inbox_msg_t *m = &sig->thread.msgs[sig->thread.msg_count];
            memset(m, 0, sizeof(*m));

            int kt;
            if (fx_obj_get(c, msg_i, "kind", &kt)) {
                int v;
                ff_fixture_result_t rc = fx_enum(c, kt, fx_feed_kind_table,
                                                  sizeof(fx_feed_kind_table) / sizeof(fx_feed_kind_table[0]),
                                                  "signals.msgs[].kind", &v);
                if (rc != FF_FIXTURE_OK) return rc;
                m->kind = (ff_feed_kind_t)v;
            }
            if (fx_obj_get(c, msg_i, "dir", &kt)) {
                int v;
                ff_fixture_result_t rc = fx_enum(c, kt, fx_feed_dir_table,
                                                  sizeof(fx_feed_dir_table) / sizeof(fx_feed_dir_table[0]),
                                                  "signals.msgs[].dir", &v);
                if (rc != FF_FIXTURE_OK) return rc;
                m->dir = (ff_feed_dir_t)v;
            }
            if (fx_obj_get(c, msg_i, "node_id", &kt)) m->node_id = (uint32_t)fx_num(c, kt, 0.0);
            if (fx_obj_get(c, msg_i, "from", &kt)) {
                m->identity_known = true;
                fx_copy_str(c, kt, m->name, sizeof(m->name));
            }
            if (fx_obj_get(c, msg_i, "initial", &kt)) {
                char buf[2];
                fx_copy_str(c, kt, buf, sizeof(buf));
                m->initial = buf[0];
            }
            if (fx_obj_get(c, msg_i, "color_idx", &kt)) m->color_idx = (uint8_t)fx_num(c, kt, 0.0);
            if (fx_obj_get(c, msg_i, "text", &kt)) fx_copy_str(c, kt, m->text, sizeof(m->text));
            if (fx_obj_get(c, msg_i, "age_ms", &kt)) m->age_ms = (uint32_t)fx_num(c, kt, 0.0);
            if (fx_obj_get(c, msg_i, "unread", &kt)) m->unread = fx_bool(c, kt, false);

            sig->thread.msg_count++;
            idx = fx_skip(c, msg_i);
        }
    }

    /* S24 slice d — the Rally sub-view's projected WHERE/WHEN/Send state
     * (a view snapshot: the shell derives these from the pack live, but a
     * fixture authors them directly, same convention as convs/msgs). */
    int rally_i;
    if (fx_obj_get(c, obj_i, "rally", &rally_i) && !fx_is_null(c, rally_i)) {
        ff_app_rally_t *r = &sig->rally;
        int rt;
        if (fx_obj_get(c, rally_i, "on_me_ok", &rt)) r->on_me_ok = fx_bool(c, rt, false);
        if (fx_obj_get(c, rally_i, "sel", &rt)) r->sel = (uint8_t)fx_num(c, rt, 0.0);
        if (fx_obj_get(c, rally_i, "when", &rt)) {
            int v;
            ff_fixture_result_t rc = fx_enum(c, rt, fx_rally_when_table,
                                              sizeof(fx_rally_when_table) / sizeof(fx_rally_when_table[0]),
                                              "signals.rally.when", &v);
            if (rc != FF_FIXTURE_OK) return rc;
            r->when = (ff_rally_when_t)v;
        }
        if (fx_obj_get(c, rally_i, "echo_place", &rt)) fx_copy_str(c, rt, r->echo_place, sizeof(r->echo_place));
        if (fx_obj_get(c, rally_i, "echo_when", &rt)) fx_copy_str(c, rt, r->echo_when, sizeof(r->echo_when));
        if (fx_obj_get(c, rally_i, "can_send", &rt)) r->can_send = fx_bool(c, rt, false);
        if (fx_obj_get(c, rally_i, "confirm_armed", &rt)) r->confirm_armed = fx_bool(c, rt, false);

        int places_i;
        if (fx_obj_get(c, rally_i, "places", &places_i) && !fx_is_null(c, places_i)) {
            jsmntok_t const *at = &c->toks[places_i];
            if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
            if (at->size > FF_APP_RALLY_MAX_PLACES) return FF_FIXTURE_ERR_TOO_BIG;
            int pidx = places_i + 1;
            for (int i = 0; i < at->size; i++) {
                fx_copy_str(c, pidx, r->place_names[r->place_count], sizeof(r->place_names[0]));
                r->place_count++;
                pidx = fx_skip(c, pidx);
            }
        }
    }
    return FF_FIXTURE_OK;
}

static const fx_enum_entry_t fx_compose_mode_table[] = {
    {"abc", FF_APP_COMPOSE_ABC},
    {"123", FF_APP_COMPOSE_123},
    {"sym", FF_APP_COMPOSE_SYM},
    {"pred", FF_APP_COMPOSE_PRED}, /* S08 predictive addendum (PR2) */
};

/* fx_parse_compose_cand — the predictive candidate array (S08 addendum):
 * `[{"text": "the", "from_pack": false}, ...]`, best-first, cap
 * FF_APP_COMPOSE_MAX_CAND. Same fail-loud-on-oversized-array treatment as
 * fx_parse_radar_dots/fx_parse_now above. `from_pack` defaults false (a
 * dictionary word, not festpack vocabulary) when the key is absent. This is
 * hand-authored golden data: on the live path the shell fills these fields
 * from the engine by pointer identity (see ff_app_state.h), never here. */
static ff_fixture_result_t fx_parse_compose_cand(fx_ctx_t const *c, int arr_i, ff_app_compose_t *cp)
{
    jsmntok_t const *at = &c->toks[arr_i];
    if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
    if (at->size > FF_APP_COMPOSE_MAX_CAND) return FF_FIXTURE_ERR_TOO_BIG;
    int idx = arr_i + 1;
    for (int i = 0; i < at->size; i++) {
        int obj_i = idx;
        int t;
        if (fx_obj_get(c, obj_i, "text", &t))
            fx_copy_str(c, t, cp->cand[cp->n_cand].text, sizeof(cp->cand[cp->n_cand].text));
        if (fx_obj_get(c, obj_i, "from_pack", &t)) cp->cand[cp->n_cand].from_pack = fx_bool(c, t, false);
        cp->n_cand++;
        idx = fx_skip(c, obj_i);
    }
    return FF_FIXTURE_OK;
}

/* Returns non-OK only for a present-but-unrecognized `mode` (issue #28 —
 * see fx_enum's doc comment) or an oversized `cand` array; every other
 * field stays tolerant. */
static ff_fixture_result_t fx_parse_compose(fx_ctx_t const *c, int obj_i, ff_app_compose_t *cp)
{
    int t;
    if (fx_obj_get(c, obj_i, "text", &t)) fx_copy_str(c, t, cp->text, sizeof(cp->text));
    if (fx_obj_get(c, obj_i, "to_name", &t)) fx_copy_str(c, t, cp->to_name, sizeof(cp->to_name));
    if (fx_obj_get(c, obj_i, "has_pending", &t)) cp->has_pending = fx_bool(c, t, false);
    if (fx_obj_get(c, obj_i, "mode", &t)) {
        int v;
        ff_fixture_result_t rc = fx_enum(c, t, fx_compose_mode_table,
                                          sizeof(fx_compose_mode_table) / sizeof(fx_compose_mode_table[0]),
                                          "compose.mode", &v);
        if (rc != FF_FIXTURE_OK) return rc;
        cp->mode = (ff_app_compose_mode_t)v;
    }

    /* Predictive-T9 projection (S08 addendum, PR2). Meaningful only in PRED
     * mode; a fixture in another mode simply omits these (they stay zeroed,
     * exactly as the shell leaves them). */
    if (fx_obj_get(c, obj_i, "word", &t)) fx_copy_str(c, t, cp->word, sizeof(cp->word));
    if (fx_obj_get(c, obj_i, "word_nomatch", &t)) cp->word_nomatch = fx_bool(c, t, false);
    int cand_i;
    if (fx_obj_get(c, obj_i, "cand", &cand_i) && !fx_is_null(c, cand_i)) {
        ff_fixture_result_t rc = fx_parse_compose_cand(c, cand_i, cp);
        if (rc != FF_FIXTURE_OK) return rc;
    }
    /* Selection index: accept both `sel` and `sel_cand` spellings. */
    if (fx_obj_get(c, obj_i, "sel", &t)) cp->sel_cand = (uint8_t)fx_num(c, t, 0.0);
    if (fx_obj_get(c, obj_i, "sel_cand", &t)) cp->sel_cand = (uint8_t)fx_num(c, t, 0.0);
    if (fx_obj_get(c, obj_i, "total_cand", &t)) cp->total_cand = (uint16_t)fx_num(c, t, 0.0);
    return FF_FIXTURE_OK;
}

/* fx_parse_flare — [api] S10 slice b: the three independent groups on
 * ff_app_flare_t (see ff_app_state.h's updated doc comment). Each group's
 * own "n/a" sentinel is set BEFORE reading that group's fields, same
 * pattern as ff_fixture_load_json's top-level flare.expires_in_ms default
 * used to be — now three independent defaults instead of one, since the
 * three groups no longer share a single `expires_in_ms`.
 *
 * (Merge note: the old string->enum `fx_flare_state_table` for the
 * removed single-`state` IDLE/SENDING/RECEIVED/LOCKED shape is gone —
 * S10 slice b's Amendments replaced that enum with the three independent
 * groups this function parses; nothing maps a bare "state" string to
 * anything any more.) */
static void fx_parse_flare(fx_ctx_t const *c, int obj_i, ff_app_flare_t *fl)
{
    int t;

    fl->send_expires_in_ms = -1;
    if (fx_obj_get(c, obj_i, "sending", &t)) fl->sending = fx_bool(c, t, false);
    if (fx_obj_get(c, obj_i, "send_expires_in_ms", &t) && !fx_is_null(c, t))
        fl->send_expires_in_ms = (int32_t)fx_num(c, t, -1.0);

    fl->takeover_expires_in_ms = -1;
    if (fx_obj_get(c, obj_i, "takeover_active", &t)) fl->takeover_active = fx_bool(c, t, false);
    if (fx_obj_get(c, obj_i, "takeover_from_name", &t))
        fx_copy_str(c, t, fl->takeover_from_name, sizeof(fl->takeover_from_name));
    /* takeover_bearing_valid defaults to false (unknown) — see
     * ff_app_state.h's doc comment on this field, same "prove you meant
     * this" convention as ff_radar_view_t's arrow_valid. Read BEFORE
     * takeover_bearing_deg so a fixture author who sets the degree value
     * but forgets the validity flag gets an honest "unknown, ignore the
     * degrees" render rather than a silently-fabricated compass point. */
    if (fx_obj_get(c, obj_i, "takeover_bearing_valid", &t)) fl->takeover_bearing_valid = fx_bool(c, t, false);
    if (fx_obj_get(c, obj_i, "takeover_bearing_deg", &t)) fl->takeover_bearing_deg = (float)fx_num(c, t, 0.0);
    if (fx_obj_get(c, obj_i, "takeover_dist_str", &t))
        fx_copy_str(c, t, fl->takeover_dist_str, sizeof(fl->takeover_dist_str));
    if (fx_obj_get(c, obj_i, "takeover_expires_in_ms", &t) && !fx_is_null(c, t))
        fl->takeover_expires_in_ms = (int32_t)fx_num(c, t, -1.0);

    fl->locked_expires_in_ms = -1;
    if (fx_obj_get(c, obj_i, "locked", &t)) fl->locked = fx_bool(c, t, false);
    if (fx_obj_get(c, obj_i, "locked_from_name", &t))
        fx_copy_str(c, t, fl->locked_from_name, sizeof(fl->locked_from_name));
    if (fx_obj_get(c, obj_i, "locked_expires_in_ms", &t) && !fx_is_null(c, t))
        fl->locked_expires_in_ms = (int32_t)fx_num(c, t, -1.0);
}

static const fx_enum_entry_t fx_share_mode_table[] = {
    {"live", 0}, {"zones", 1}, {"ghost", 2},
};

/* Returns non-OK only for a present-but-unrecognized `share_mode`
 * (issue #28 — see fx_enum's doc comment). */
static ff_fixture_result_t fx_parse_settings(fx_ctx_t const *c, int obj_i, ff_app_settings_t *s)
{
    int t;
    if (fx_obj_get(c, obj_i, "imperial", &t)) s->imperial = fx_bool(c, t, true);
    if (fx_obj_get(c, obj_i, "share_mode", &t)) {
        int v;
        ff_fixture_result_t rc = fx_enum(c, t, fx_share_mode_table,
                                          sizeof(fx_share_mode_table) / sizeof(fx_share_mode_table[0]),
                                          "settings.share_mode", &v);
        if (rc != FF_FIXTURE_OK) return rc;
        s->share_mode = (uint8_t)v;
    }
    if (fx_obj_get(c, obj_i, "haptics", &t)) s->haptics = fx_bool(c, t, true);
    if (fx_obj_get(c, obj_i, "night_glow", &t)) s->night_glow = fx_bool(c, t, true);
    if (fx_obj_get(c, obj_i, "water_min", &t)) s->water_min = (uint16_t)fx_num(c, t, 90.0);
    if (fx_obj_get(c, obj_i, "quiet_from_min", &t)) s->quiet_from_min = (uint16_t)fx_num(c, t, 240.0);
    if (fx_obj_get(c, obj_i, "quiet_to_min", &t)) s->quiet_to_min = (uint16_t)fx_num(c, t, 600.0);
    if (fx_obj_get(c, obj_i, "my_name", &t)) fx_copy_str(c, t, s->my_name, sizeof(s->my_name));
    /* utc_offset_set read BEFORE utc_offset_min, same "prove you meant
     * this" ordering as fx_parse_flare's takeover_bearing_valid — a
     * fixture author who sets the minutes but forgets the flag gets an
     * honest UNSET render rather than a fabricated offset (S11 slice b,
     * ff_app_state.h's doc comment on this field). */
    if (fx_obj_get(c, obj_i, "utc_offset_set", &t)) s->utc_offset_set = fx_bool(c, t, false);
    if (fx_obj_get(c, obj_i, "utc_offset_min", &t)) s->utc_offset_min = (int16_t)fx_num(c, t, 0.0);
    if (fx_obj_get(c, obj_i, "colorblind", &t)) s->colorblind = fx_bool(c, t, false); /* S17 slice a */
    /* #100: brightness percent. The ff_app_state_t is memset(0) before
     * parse, so an omitted brightness would render as 0 (below the floor) —
     * default it to the shell's own FF_BRIGHTNESS_DEFAULT_PCT here so a
     * fixture that doesn't care renders the same as a fresh puck. (S21
     * removed the `page` field: Settings is one scrolling list now.) */
    s->brightness_pct = FF_BRIGHTNESS_DEFAULT_PCT;
    if (fx_obj_get(c, obj_i, "brightness_pct", &t))
        s->brightness_pct = (uint8_t)fx_num(c, t, (double)FF_BRIGHTNESS_DEFAULT_PCT);
    return FF_FIXTURE_OK;
}

/* ---------------------------------------------------------------------
 * map (S09) — mirrors ff_app_map_t field-for-field. Same "fixture.c has
 * zero festpack dependency" convention as `now` above: features are
 * plain {kind, label, color, points} objects, never a live fp_pack_t.
 * ------------------------------------------------------------------- */

static const fx_enum_entry_t fx_map_kind_table[] = {
    {"unknown", FF_APP_MAP_KIND_UNKNOWN},   {"stage", FF_APP_MAP_KIND_STAGE},
    {"camping", FF_APP_MAP_KIND_CAMPING},   {"water", FF_APP_MAP_KIND_WATER},
    {"path", FF_APP_MAP_KIND_PATH},         {"entrance", FF_APP_MAP_KIND_ENTRANCE},
    {"vendor", FF_APP_MAP_KIND_VENDOR},     {"medical", FF_APP_MAP_KIND_MEDICAL},
    {"poi", FF_APP_MAP_KIND_POI},
};

/* fx_parse_map_points — `[[east_m, north_m], ...]`, fail-loud on an
 * oversized array (same convention as fx_parse_radar_dots/fx_parse_now).
 * Each element must itself be a 2-element JSON array; anything else is a
 * malformed fixture (FF_FIXTURE_ERR_JSON), not a silently-skipped point —
 * a point silently dropped here is exactly the "no invented geometry"
 * rule's opposite failure (inventing an OMISSION nobody asked for). */
static ff_fixture_result_t fx_parse_map_points(fx_ctx_t const *c, int arr_i, ff_app_map_feature_t *f)
{
    jsmntok_t const *at = &c->toks[arr_i];
    if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
    if (at->size > FF_APP_MAP_MAX_POLY_PTS) return FF_FIXTURE_ERR_TOO_BIG;
    int idx = arr_i + 1;
    for (int i = 0; i < at->size; i++) {
        int pair_i = idx;
        jsmntok_t const *pt = &c->toks[pair_i];
        if (pt->type != JSMN_ARRAY || pt->size != 2) return FF_FIXTURE_ERR_JSON;
        int e_i = pair_i + 1;
        int n_i = fx_skip(c, e_i);
        f->pts_en[f->n_pts][0] = (float)fx_num(c, e_i, 0.0);
        f->pts_en[f->n_pts][1] = (float)fx_num(c, n_i, 0.0);
        f->n_pts++;
        idx = fx_skip(c, pair_i);
    }
    return FF_FIXTURE_OK;
}

static ff_fixture_result_t fx_parse_map_feature(fx_ctx_t const *c, int obj_i, ff_app_map_feature_t *f)
{
    memset(f, 0, sizeof(*f));
    int t;
    if (fx_obj_get(c, obj_i, "kind", &t)) {
        int v;
        ff_fixture_result_t rc = fx_enum(c, t, fx_map_kind_table,
                                          sizeof(fx_map_kind_table) / sizeof(fx_map_kind_table[0]),
                                          "map.features[].kind", &v);
        if (rc != FF_FIXTURE_OK) return rc;
        f->kind = (ff_app_map_kind_t)v;
    }
    if (fx_obj_get(c, obj_i, "label", &t)) fx_copy_str(c, t, f->label, sizeof(f->label));
    if (fx_obj_get(c, obj_i, "color_rgb", &t)) {
        uint32_t rgb;
        if (fx_color_rgb(c, t, &rgb)) {
            f->color_rgb = rgb;
            f->color_valid = true;
        }
        /* A present-but-malformed color, same as fx_color_rgb's other
         * callers: leaves color_valid false rather than guessing. */
    }
    int pts_i;
    if (fx_obj_get(c, obj_i, "points", &pts_i) && !fx_is_null(c, pts_i)) {
        ff_fixture_result_t rc = fx_parse_map_points(c, pts_i, f);
        if (rc != FF_FIXTURE_OK) return rc;
    }
    return FF_FIXTURE_OK;
}

static ff_fixture_result_t fx_parse_map_features(fx_ctx_t const *c, int arr_i, ff_app_map_t *m)
{
    jsmntok_t const *at = &c->toks[arr_i];
    if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
    if (at->size > FF_APP_MAP_MAX_FEATURES) return FF_FIXTURE_ERR_TOO_BIG;
    int idx = arr_i + 1;
    for (int i = 0; i < at->size; i++) {
        int obj_i2 = idx;
        ff_fixture_result_t rc = fx_parse_map_feature(c, obj_i2, &m->features[m->n_features]);
        if (rc != FF_FIXTURE_OK) return rc;
        m->n_features++;
        idx = fx_skip(c, obj_i2);
    }
    return FF_FIXTURE_OK;
}

static ff_fixture_result_t fx_parse_map_crew(fx_ctx_t const *c, int arr_i, ff_app_map_t *m)
{
    jsmntok_t const *at = &c->toks[arr_i];
    if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
    if (at->size > FF_CREW_MAX) return FF_FIXTURE_ERR_TOO_BIG;
    int idx = arr_i + 1;
    for (int i = 0; i < at->size; i++) {
        int obj_i2 = idx;
        ff_app_map_crew_t *o = &m->crew[m->n_crew];
        memset(o, 0, sizeof(*o));
        int t;
        if (fx_obj_get(c, obj_i2, "initial", &t)) {
            char buf[2];
            fx_copy_str(c, t, buf, sizeof(buf));
            o->initial = buf[0];
        }
        if (fx_obj_get(c, obj_i2, "color_idx", &t)) o->color_idx = (uint8_t)fx_num(c, t, 0.0);
        /* Presence of EITHER coordinate is the has_pos signal — same
         * "prove you meant this" idiom used throughout this loader (see
         * e.g. flare.takeover_bearing_valid); a crew entry with no
         * coordinates at all is a malformed fixture, not a silent
         * (0,0) placement. */
        if (fx_obj_get(c, obj_i2, "east_m", &t)) {
            o->east_m = (float)fx_num(c, t, 0.0);
            o->has_pos = true;
        }
        if (fx_obj_get(c, obj_i2, "north_m", &t)) {
            o->north_m = (float)fx_num(c, t, 0.0);
            o->has_pos = true;
        }
        if (fx_obj_get(c, obj_i2, "stale", &t)) o->stale = fx_bool(c, t, false);
        if (fx_obj_get(c, obj_i2, "place", &t)) o->place = fx_bool(c, t, false);
        if (fx_obj_get(c, obj_i2, "imprecise", &t)) o->imprecise = fx_bool(c, t, false);
        m->n_crew++;
        idx = fx_skip(c, obj_i2);
    }
    return FF_FIXTURE_OK;
}

static ff_fixture_result_t fx_parse_map(fx_ctx_t const *c, int obj_i, ff_app_map_t *m)
{
    int feat_i;
    if (fx_obj_get(c, obj_i, "features", &feat_i) && !fx_is_null(c, feat_i)) {
        ff_fixture_result_t rc = fx_parse_map_features(c, feat_i, m);
        if (rc != FF_FIXTURE_OK) return rc;
    }
    /* PR #73 review finding #1: authorable directly, so a golden can
     * exercise scr_map.c's "+N MORE" render without needing an actual
     * >cap pack (fixture.c's own array-cap check would reject that
     * outright — see fx_parse_map_features — this is the intentional,
     * pack-independent way to author the truncated STATE itself). */
    int t0;
    if (fx_obj_get(c, obj_i, "truncated", &t0)) m->truncated = fx_bool(c, t0, false);
    if (fx_obj_get(c, obj_i, "features_omitted", &t0)) m->features_omitted = (uint8_t)fx_num(c, t0, 0.0);
    int crew_i;
    if (fx_obj_get(c, obj_i, "crew", &crew_i) && !fx_is_null(c, crew_i)) {
        ff_fixture_result_t rc = fx_parse_map_crew(c, crew_i, m);
        if (rc != FF_FIXTURE_OK) return rc;
    }
    int rally_i;
    if (fx_obj_get(c, obj_i, "rally", &rally_i) && !fx_is_null(c, rally_i)) {
        m->has_rally = true; /* presence of the section IS the flag — same "next" idiom as ff_app_now_t */
        int t;
        if (fx_obj_get(c, rally_i, "label", &t)) fx_copy_str(c, t, m->rally_label, sizeof(m->rally_label));
        if (fx_obj_get(c, rally_i, "east_m", &t)) m->rally_east_m = (float)fx_num(c, t, 0.0);
        if (fx_obj_get(c, rally_i, "north_m", &t)) m->rally_north_m = (float)fx_num(c, t, 0.0);
    }
    int you_i;
    if (fx_obj_get(c, obj_i, "you", &you_i) && !fx_is_null(c, you_i)) {
        m->you_has_pos = true; /* presence of the section defaults has_pos true, overridable below */
        int t;
        if (fx_obj_get(c, you_i, "has_pos", &t)) m->you_has_pos = fx_bool(c, t, true);
        if (fx_obj_get(c, you_i, "east_m", &t)) m->you_east_m = (float)fx_num(c, t, 0.0);
        if (fx_obj_get(c, you_i, "north_m", &t)) m->you_north_m = (float)fx_num(c, t, 0.0);
        /* heading_valid defaults false even with the section present —
         * S09 AC5's "hidden + NO FIX chip" is the least-claiming default,
         * same convention as radar.arrow_valid. */
        if (fx_obj_get(c, you_i, "heading_valid", &t)) m->you_heading_valid = fx_bool(c, t, false);
        if (fx_obj_get(c, you_i, "heading_deg", &t)) m->you_heading_deg = (float)fx_num(c, t, 0.0);
    }
    /* else: `you` absent entirely -> you_has_pos/you_heading_valid stay
     * false (the whole-struct zeroing at load start) — the honest "no
     * fix at all" case AC5 also covers. */
    return FF_FIXTURE_OK;
}

static const fx_enum_entry_t fx_face_table[] = {
    {"radar", FF_APP_FACE_RADAR},
    {"now", FF_APP_FACE_NOW},
    {"signals", FF_APP_FACE_SIGNALS},
    {"settings", FF_APP_FACE_SETTINGS},
    {"compose", FF_APP_FACE_COMPOSE},
    {"map", FF_APP_FACE_MAP},
    /* S26 slice b — the PWR-button power menu modal. No JSON section of
     * its own (unlike compose/flare/map): the face renders fixed content
     * (scr_power_menu.h's top comment), so `"face": "power_menu"` is the
     * entire fixture. */
    {"power_menu", FF_APP_FACE_POWER_MENU},
};

ff_fixture_result_t ff_fixture_load_json(char const *json, size_t len, ff_app_state_t *out)
{
    if (out == NULL) return FF_FIXTURE_ERR_JSON;
    memset(out, 0, sizeof(*out));

    if (json == NULL || len == 0) return FF_FIXTURE_ERR_JSON;
    if (len > FIX_MAX_JSON_LEN) return FF_FIXTURE_ERR_TOO_BIG;

    static jsmntok_t toks[FIX_MAX_TOKENS];
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, len, toks, FIX_MAX_TOKENS);
    if (r == JSMN_ERROR_NOMEM) return FF_FIXTURE_ERR_TOO_BIG;
    if (r < 0 || r == 0) return FF_FIXTURE_ERR_JSON;

    fx_ctx_t ctx = {json, toks, r};
    if (toks[0].type != JSMN_OBJECT) {
        memset(out, 0, sizeof(*out));
        return FF_FIXTURE_ERR_JSON;
    }

    /* Non-zero documented defaults (tests/fixtures/README.md's field
     * tables), applied only once we know we have a genuine JSON object
     * to load — every non-OK return above leaves `out` fully zeroed per
     * this function's documented contract. These defaults apply whether
     * the enclosing section is present with the field omitted, or the
     * whole section is absent from the document: plain memset(0) above
     * would otherwise silently give radar.mode the *first* enum value
     * (RADAR_LIVE == 0), a much stronger, more misleading claim
     * than "no fixture data provided" (CLAUDE.md: "unknown = explicitly
     * unknown... never fake freshness, positions, or times" — LIVE is
     * exactly the kind of fake-freshness claim that principle rules
     * out). Section parsers below may still override these from
     * explicit JSON fields. */
    out->radar.mode = RADAR_NOSEL;
    /* S16 slice a: `face` defaults to RADAR — and it must STAY RADAR,
     * which runs deliberately AGAINST the "least-claiming first enum
     * member" convention this block otherwise applies (radar.mode above
     * picks NOSEL over LIVE for exactly that reason). Context that
     * would not announce itself: FF_APP_FACE_NONE = 0 renumbered
     * ff_app_face_t, so the memset(0) above no longer leaves
     * active_face on RADAR by coincidence. Without this assignment, a
     * fixture that omits `face` would silently start rendering as NONE
     * — which face_dispatch.c routes to the S13 debug placeholder
     * instead of the real nav shell.
     *
     * This default covers ONLY the absent-key case (PR #36's deliberate
     * exception: absent != malformed). A face string that is present
     * but unrecognized — the typo'd `"face": "radr"` this fallback once
     * silently absorbed — now fails the whole load with
     * FF_FIXTURE_ERR_BAD_ENUM instead (issue #28; see fx_enum's doc
     * comment and the `face` parse below).
     *
     * RADAR is right here because this is a FIXTURE default, not a
     * claim about live data: every committed fixture names its face
     * explicitly (all 25 do), so this path is only reached by a
     * hand-written or truncated snapshot, where "show me the home face"
     * is more useful than "show me nothing". NONE would be the honest
     * answer if this field described the world; it describes which
     * screen to draw. Documented in tests/fixtures/README.md's field
     * table and asserted by test_fixture.c's
     * absent_sections_default_to_zero /
     * face_absent_still_defaults_to_radar. */
    out->active_face = FF_APP_FACE_RADAR;
    /* Three independent "n/a" defaults (S10 slice b) — see fx_parse_flare's
     * doc comment for why there are three now instead of one. Set here too
     * (not only inside fx_parse_flare) so they hold even when the "flare"
     * section is absent entirely from the document (fx_parse_flare is
     * never called in that case — see the call site below). */
    out->flare.send_expires_in_ms = -1;
    out->flare.takeover_expires_in_ms = -1;
    out->flare.locked_expires_in_ms = -1;

    int t;
    if (fx_obj_get(&ctx, 0, "fixture", &t)) fx_copy_str(&ctx, t, out->fixture_name, sizeof(out->fixture_name));
    /* #bug5a — sim-only Settings scroll render hint (see ff_app_state.h's
     * ui_settings_scroll_y). Top-level, like `fixture`/`face`; absent -> 0
     * (no scroll), the ordinary case for every fixture but the scrolled
     * Settings goldens. */
    if (fx_obj_get(&ctx, 0, "ui_settings_scroll_y", &t))
        out->ui_settings_scroll_y = (int32_t)fx_num(&ctx, t, 0.0);
    if (fx_obj_get(&ctx, 0, "face", &t)) {
        int v;
        ff_fixture_result_t rc =
            fx_enum(&ctx, t, fx_face_table, sizeof(fx_face_table) / sizeof(fx_face_table[0]), "face", &v);
        if (rc != FF_FIXTURE_OK) {
            memset(out, 0, sizeof(*out));
            return rc;
        }
        out->active_face = (ff_app_face_t)v;
    }
    /* else: the RADAR default set above stands — absent is a documented
     * default, not a parse failure (PR #36 / issue #28). */

    /* radar/now/signals can fail-loud on an oversized array (see their
     * parsers' doc comments), and every section with an enum key can
     * fail-loud on an unrecognized enum string (issue #28 — see
     * fx_enum's doc comment; compose/settings below get the same
     * treatment). On any such failure, re-zero `out` before returning,
     * matching this function's documented "non-OK return leaves *out
     * fully zeroed" contract (same as fp_parse()'s). */
    int sec_i;
    if (fx_obj_get(&ctx, 0, "radar", &sec_i) && !fx_is_null(&ctx, sec_i)) {
        ff_fixture_result_t rc = fx_parse_radar(&ctx, sec_i, &out->radar);
        if (rc != FF_FIXTURE_OK) {
            memset(out, 0, sizeof(*out));
            return rc;
        }
    }
    if (fx_obj_get(&ctx, 0, "now", &sec_i) && !fx_is_null(&ctx, sec_i)) {
        ff_fixture_result_t rc = fx_parse_now(&ctx, sec_i, &out->now);
        if (rc != FF_FIXTURE_OK) {
            memset(out, 0, sizeof(*out));
            return rc;
        }
    }
    if (fx_obj_get(&ctx, 0, "signals", &sec_i) && !fx_is_null(&ctx, sec_i)) {
        ff_fixture_result_t rc = fx_parse_signals(&ctx, sec_i, &out->signals);
        if (rc != FF_FIXTURE_OK) {
            memset(out, 0, sizeof(*out));
            return rc;
        }
    }
    if (fx_obj_get(&ctx, 0, "compose", &sec_i) && !fx_is_null(&ctx, sec_i)) {
        ff_fixture_result_t rc = fx_parse_compose(&ctx, sec_i, &out->compose);
        if (rc != FF_FIXTURE_OK) {
            memset(out, 0, sizeof(*out));
            return rc;
        }
    }
    if (fx_obj_get(&ctx, 0, "flare", &sec_i) && !fx_is_null(&ctx, sec_i)) fx_parse_flare(&ctx, sec_i, &out->flare);
    /* else: out->flare's three -1 "n/a" defaults (set above) stand. */
    if (fx_obj_get(&ctx, 0, "settings", &sec_i) && !fx_is_null(&ctx, sec_i)) {
        ff_fixture_result_t rc = fx_parse_settings(&ctx, sec_i, &out->settings);
        if (rc != FF_FIXTURE_OK) {
            memset(out, 0, sizeof(*out));
            return rc;
        }
    }
    if (fx_obj_get(&ctx, 0, "map", &sec_i) && !fx_is_null(&ctx, sec_i)) {
        ff_fixture_result_t rc = fx_parse_map(&ctx, sec_i, &out->map);
        if (rc != FF_FIXTURE_OK) {
            memset(out, 0, sizeof(*out));
            return rc;
        }
    }

    return FF_FIXTURE_OK;
}

ff_fixture_result_t ff_fixture_load_file(char const *path, ff_app_state_t *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (path == NULL) return FF_FIXTURE_ERR_IO;

    FILE *f = fopen(path, "rb");
    if (f == NULL) return FF_FIXTURE_ERR_IO;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return FF_FIXTURE_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > FIX_MAX_JSON_LEN) {
        fclose(f);
        return sz < 0 ? FF_FIXTURE_ERR_IO : FF_FIXTURE_ERR_TOO_BIG;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return FF_FIXTURE_ERR_IO;
    }

    char buf[FIX_MAX_JSON_LEN];
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) return FF_FIXTURE_ERR_IO;

    return ff_fixture_load_json(buf, n, out);
}

void ff_fixture_stem(char const *path, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) return;
    out[0] = '\0';
    if (path == NULL) return;

    char const *base = path;
    for (char const *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    size_t len = strlen(base);
    static const char suffix[] = ".json";
    size_t const slen = sizeof(suffix) - 1;
    if (len > slen && memcmp(base + len - slen, suffix, slen) == 0) len -= slen;

    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

/* ---------------------------------------------------------------------
 * S13c — ff_fixture_dump_json: ff_app_state_t -> JSON (inverse of the
 * loader above). See fixture.h's doc comment for the round-trip contract;
 * reuses the same fx_*_table[] enum tables the loader defines above (this
 * is the one direction where sharing beats "duplicated here, not shared" —
 * same translation unit, same tables, zero risk of the two directions
 * drifting apart on an enum string).
 * ------------------------------------------------------------------- */

/* Bounded, allocation-free cursor writer. `end` reserves exactly one byte
 * for the final NUL terminator ff_fixture_dump_json writes on success, so
 * every fw_* helper below can treat [p, end) as the full usable range and
 * never has to special-case the terminator itself. */
typedef struct {
    char *p;
    char *end;
    bool overflow;
} fw_cur_t;

static void fw_init(fw_cur_t *w, char *buf, size_t buf_sz)
{
    w->overflow = (buf == NULL || buf_sz == 0);
    w->p = buf;
    w->end = w->overflow ? buf : buf + (buf_sz - 1);
}

static void fw_raw(fw_cur_t *w, char const *s)
{
    if (w->overflow) return;
    size_t n = strlen(s);
    if ((size_t)(w->end - w->p) < n) {
        w->overflow = true;
        return;
    }
    memcpy(w->p, s, n);
    w->p += n;
}

/* printf-style append, bounded to the remaining [p, end) span. Safe to
 * call even when nearly full: vsnprintf is given exactly enough room to
 * write its own trailing NUL at `end` (the byte fw_init reserved for
 * *this* function's final terminator), which is always within `buf`. */
static void fw_fmt(fw_cur_t *w, char const *fmt, ...)
{
    if (w->overflow) return;
    size_t avail = (size_t)(w->end - w->p);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->p, avail + 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n > avail) {
        w->overflow = true;
        return;
    }
    w->p += (size_t)n;
}

/* Writes `s` as a quoted, escaped JSON string. `s` is untrusted-ish: it
 * can carry live mesh data (node long/short names, free-text messages)
 * that isn't developer-authored, so every byte is escaped rather than
 * assumed JSON-safe — an unescaped `"` or control byte here would corrupt
 * the ctl socket's response framing (see ctl_server.c, which embeds this
 * output directly into its own response line). */
static void fw_json_str(fw_cur_t *w, char const *s)
{
    if (w->overflow) return;
    fw_raw(w, "\"");
    if (s == NULL) s = "";
    for (unsigned char const *p = (unsigned char const *)s; *p != '\0' && !w->overflow; p++) {
        switch (*p) {
            case '"': fw_raw(w, "\\\""); break;
            case '\\': fw_raw(w, "\\\\"); break;
            case '\n': fw_raw(w, "\\n"); break;
            case '\r': fw_raw(w, "\\r"); break;
            case '\t': fw_raw(w, "\\t"); break;
            default:
                if (*p < 0x20) {
                    fw_fmt(w, "\\u%04x", (unsigned)*p);
                } else {
                    char one[2] = {(char)*p, '\0'};
                    fw_raw(w, one);
                }
                break;
        }
    }
    fw_raw(w, "\"");
}

static char const *fx_enum_name(fx_enum_entry_t const *table, size_t n_entries, int value, char const *dflt)
{
    for (size_t k = 0; k < n_entries; k++) {
        if (table[k].value == value) return table[k].name;
    }
    return dflt;
}

static void fw_radar_dot(fw_cur_t *w, ff_radar_dot_t const *d)
{
    fw_raw(w, "{\"ring_deg\":");
    fw_fmt(w, "%g", (double)d->ring_deg);
    char initial[2] = {d->initial, '\0'}; /* '\0' initial -> writes "" */
    fw_raw(w, ",\"initial\":");
    fw_json_str(w, initial);
    fw_fmt(w, ",\"color_idx\":%u", (unsigned)d->color_idx);
    fw_raw(w, d->stale ? ",\"stale\":true" : ",\"stale\":false");
    fw_raw(w, d->place ? ",\"place\":true" : ",\"place\":false"); /* issue #33 */
    fw_raw(w, d->imprecise ? ",\"imprecise\":true}" : ",\"imprecise\":false}"); /* issue #74 */
}

static void fw_now_row(fw_cur_t *w, ff_app_now_row_t const *r)
{
    fw_raw(w, "{\"artist\":");
    fw_json_str(w, r->artist);
    fw_raw(w, ",\"stage_name\":");
    fw_json_str(w, r->stage_name);
    /* PR #21 code review finding #3: stage_color_rgb is only meaningful
     * when stage_color_valid — the key is OMITTED entirely rather than
     * written as "#000000" (or any other placeholder) when invalid, so a
     * dump->reload round-trip doesn't accidentally turn "no valid color"
     * into "a real, valid black color" (fx_color_rgb parses "#000000"
     * successfully, so writing it unconditionally here would silently
     * flip stage_color_valid from false to true on reload). Same
     * "absent key -> invalid, by construction" contract fx_parse_now_row
     * already gives the loader side. */
    if (r->stage_color_valid) {
        fw_fmt(w, ",\"stage_color_rgb\":\"#%06x\"", (unsigned)(r->stage_color_rgb & 0xFFFFFFu));
    }
    fw_fmt(w, ",\"pct_done\":%u", (unsigned)r->pct_done);
    fw_raw(w, r->pct_valid ? ",\"pct_valid\":true}" : ",\"pct_valid\":false}");
}

/* fw_lineup_item — one entry of ff_app_now_t.lineup (the day's
 * still-unknown-time sets, S07 slice b round 2's now_state_t /
 * NOW_MIXED shape). Mirrors fx_parse_lineup_item's fields exactly. */
static void fw_lineup_item(fw_cur_t *w, ff_app_lineup_item_t const *item)
{
    fw_raw(w, "{\"artist\":");
    fw_json_str(w, item->artist);
    fw_raw(w, ",\"stage_name\":");
    fw_json_str(w, item->stage_name);
    fw_raw(w, "}");
}

static void fw_map_feature(fw_cur_t *w, ff_app_map_feature_t const *f)
{
    fw_raw(w, "{\"kind\":\"");
    fw_raw(w, fx_enum_name(fx_map_kind_table, sizeof(fx_map_kind_table) / sizeof(fx_map_kind_table[0]), f->kind,
                            "unknown"));
    fw_raw(w, "\",\"label\":");
    fw_json_str(w, f->label);
    /* color_rgb OMITTED entirely when invalid — same round-trip-safe
     * contract fw_now_row already gives stage_color_rgb (writing a
     * placeholder would silently flip color_valid true on reload). */
    if (f->color_valid) {
        fw_fmt(w, ",\"color_rgb\":\"#%06x\"", (unsigned)(f->color_rgb & 0xFFFFFFu));
    }
    fw_raw(w, ",\"points\":[");
    for (uint8_t i = 0; i < f->n_pts; i++) {
        if (i > 0) fw_raw(w, ",");
        fw_fmt(w, "[%g,%g]", (double)f->pts_en[i][0], (double)f->pts_en[i][1]);
    }
    fw_raw(w, "]}");
}

static void fw_map_crew(fw_cur_t *w, ff_app_map_crew_t const *c)
{
    char initial[2] = {c->initial, '\0'};
    fw_raw(w, "{\"initial\":");
    fw_json_str(w, initial);
    fw_fmt(w, ",\"color_idx\":%u", (unsigned)c->color_idx);
    fw_fmt(w, ",\"east_m\":%g,\"north_m\":%g", (double)c->east_m, (double)c->north_m);
    fw_raw(w, c->stale ? ",\"stale\":true" : ",\"stale\":false");
    fw_raw(w, c->place ? ",\"place\":true" : ",\"place\":false");
    fw_raw(w, c->imprecise ? ",\"imprecise\":true}" : ",\"imprecise\":false}");
}

/* S24: serialize one ff_inbox_conv_t. Mirrors fx_parse_signals
 * field-for-field so a dump round-trips through the loader. Fields the
 * loader DERIVES (has_preview, presence_valid, preview_from_known) are
 * not written as independent facts: `preview_from` is written only when
 * known (its presence IS the flag), and has_preview/presence_valid
 * re-derive from item_count / conv kind on reload — writing them too
 * would let a hand-edited fixture desync an invariant the model itself
 * cannot express. */
/* One-char-or-empty string for an `initial`: '\0' -> "" so it round-trips
 * back to '\0' through fx_copy_str (a written space would reload as ' '). */
static char const *fw_char1(char ch, char *buf)
{
    if (ch == '\0') {
        buf[0] = '\0';
        return buf;
    }
    buf[0] = ch;
    buf[1] = '\0';
    return buf;
}

static void fw_conv(fw_cur_t *w, ff_inbox_conv_t const *cv)
{
    char cbuf[2];
    fw_raw(w, "{\"conv\":\"");
    fw_raw(w, fx_enum_name(fx_conv_kind_table, sizeof(fx_conv_kind_table) / sizeof(fx_conv_kind_table[0]),
                            cv->kind, "crew"));
    fw_raw(w, "\"");
    fw_fmt(w, ",\"node_id\":%u", (unsigned)cv->node_id);
    fw_raw(w, ",\"name\":");
    fw_json_str(w, cv->name);
    fw_raw(w, ",\"initial\":");
    fw_json_str(w, fw_char1(cv->initial, cbuf));
    fw_fmt(w, ",\"color_idx\":%u", (unsigned)cv->color_idx);
    fw_fmt(w, ",\"unread\":%u", (unsigned)cv->unread);
    fw_fmt(w, ",\"item_count\":%u", (unsigned)cv->item_count);
    fw_raw(w, ",\"preview_kind\":\"");
    fw_raw(w, fx_enum_name(fx_feed_kind_table, sizeof(fx_feed_kind_table) / sizeof(fx_feed_kind_table[0]),
                            cv->preview_kind, "text"));
    fw_raw(w, "\",\"preview_dir\":\"");
    fw_raw(w, fx_enum_name(fx_feed_dir_table, sizeof(fx_feed_dir_table) / sizeof(fx_feed_dir_table[0]),
                            cv->preview_dir, "unknown"));
    fw_raw(w, "\",\"preview_text\":");
    fw_json_str(w, cv->preview_text);
    fw_fmt(w, ",\"preview_age_ms\":%u", (unsigned)cv->preview_age_ms);
    if (cv->preview_from_known) {
        fw_raw(w, ",\"preview_from\":");
        fw_json_str(w, cv->preview_from_name);
    }
    fw_raw(w, ",\"presence\":\"");
    fw_raw(w, fx_enum_name(fx_presence_table, sizeof(fx_presence_table) / sizeof(fx_presence_table[0]),
                            cv->presence, "seen"));
    fw_raw(w, "\"");
    fw_fmt(w, ",\"presence_age_ms\":%u}", (unsigned)cv->presence_age_ms);
}

/* S24 slice (c): serialize one ff_inbox_msg_t. Mirrors the `msgs` loader
 * field-for-field; `from` is written only when the identity is known
 * (its presence IS the identity_known flag on reload, the preview_from
 * convention). */
static void fw_msg(fw_cur_t *w, ff_inbox_msg_t const *m)
{
    char cbuf[2];
    fw_raw(w, "{\"kind\":\"");
    fw_raw(w, fx_enum_name(fx_feed_kind_table, sizeof(fx_feed_kind_table) / sizeof(fx_feed_kind_table[0]),
                            m->kind, "text"));
    fw_raw(w, "\",\"dir\":\"");
    fw_raw(w, fx_enum_name(fx_feed_dir_table, sizeof(fx_feed_dir_table) / sizeof(fx_feed_dir_table[0]),
                            m->dir, "unknown"));
    fw_raw(w, "\"");
    fw_fmt(w, ",\"node_id\":%u", (unsigned)m->node_id);
    if (m->identity_known) {
        fw_raw(w, ",\"from\":");
        fw_json_str(w, m->name);
    }
    fw_raw(w, ",\"initial\":");
    fw_json_str(w, fw_char1(m->initial, cbuf));
    fw_fmt(w, ",\"color_idx\":%u", (unsigned)m->color_idx);
    fw_raw(w, ",\"text\":");
    fw_json_str(w, m->text);
    fw_fmt(w, ",\"age_ms\":%u", (unsigned)m->age_ms);
    fw_raw(w, m->unread ? ",\"unread\":true}" : ",\"unread\":false}");
}

int ff_fixture_dump_json(ff_app_state_t const *s, char *buf, size_t buf_sz)
{
    if (s == NULL || buf == NULL || buf_sz == 0) return -1;

    fw_cur_t w;
    fw_init(&w, buf, buf_sz);

    fw_raw(&w, "{\"fixture\":");
    fw_json_str(&w, s->fixture_name);
    fw_raw(&w, ",\"face\":\"");
    fw_raw(&w, fx_enum_name(fx_face_table, sizeof(fx_face_table) / sizeof(fx_face_table[0]), s->active_face,
                             "radar"));
    fw_raw(&w, "\"");
    /* #bug5a — mirror the sim-only scroll hint so a dump round-trips through
     * the loader (see ff_fixture_dump_json's contract). Always 0 for a live
     * shell dump; carried for the scrolled Settings fixtures. */
    fw_fmt(&w, ",\"ui_settings_scroll_y\":%d", (int)s->ui_settings_scroll_y);

    /* radar */
    fw_raw(&w, ",\"radar\":{\"mode\":\"");
    fw_raw(&w, fx_enum_name(fx_radar_mode_table, sizeof(fx_radar_mode_table) / sizeof(fx_radar_mode_table[0]),
                             s->radar.mode, "nosel"));
    fw_raw(&w, "\"");
    fw_fmt(&w, ",\"arrow_deg\":%g", (double)s->radar.arrow_deg);
    fw_raw(&w, s->radar.arrow_valid ? ",\"arrow_valid\":true" : ",\"arrow_valid\":false");
    fw_raw(&w, ",\"name\":");
    fw_json_str(&w, s->radar.name);
    fw_raw(&w, ",\"dist_str\":");
    fw_json_str(&w, s->radar.dist_str);
    fw_raw(&w, s->radar.dist_imprecise ? ",\"dist_imprecise\":true" : ",\"dist_imprecise\":false"); /* issue #47 */
    fw_raw(&w, ",\"age_str\":");
    fw_json_str(&w, s->radar.age_str);
    fw_fmt(&w, ",\"trend\":%d", (int)s->radar.trend);
    fw_raw(&w, ",\"clock_str\":");
    fw_json_str(&w, s->radar.clock_str);
    fw_fmt(&w, ",\"batt_pct\":%d", (int)s->radar.batt_pct);
    fw_raw(&w, s->radar.mesh_ok ? ",\"mesh_ok\":true" : ",\"mesh_ok\":false");
    fw_raw(&w, ",\"dots\":[");
    for (uint8_t i = 0; i < s->radar.n_dots; i++) {
        if (i > 0) fw_raw(&w, ",");
        fw_radar_dot(&w, &s->radar.dots[i]);
    }
    fw_raw(&w, "]}");

    /* now (S07 slice b round 2's now_state_t shape — see ff_app_state.h's
     * doc comment and fx_parse_now/fx_now_state_table above, which this
     * mirrors field-for-field so a dump round-trips through the loader.
     * [api] merge note: this section used to write the old
     * `pack_loaded`+`tbd` bool-pair shape (pre-round-2 scaffolding);
     * rewritten here to match that PR's breaking replacement (the
     * `now_state_t` enum + the `lineup` array's generalized meaning),
     * landed while PR #19 (this dumper's own PR) was in flight — same
     * class of merge fixup the flare section just above needed for
     * S10b's reshape. */
    fw_raw(&w, ",\"now\":{\"state\":\"");
    fw_raw(&w, fx_enum_name(fx_now_state_table, sizeof(fx_now_state_table) / sizeof(fx_now_state_table[0]),
                             s->now.state, "no_pack"));
    fw_raw(&w, "\",\"rows\":[");
    for (uint8_t i = 0; i < s->now.n_rows; i++) {
        if (i > 0) fw_raw(&w, ",");
        fw_now_row(&w, &s->now.rows[i]);
    }
    fw_raw(&w, "]");
    if (s->now.next.valid) {
        fw_raw(&w, ",\"next\":{\"artist\":");
        fw_json_str(&w, s->now.next.artist);
        fw_raw(&w, ",\"stage_name\":");
        fw_json_str(&w, s->now.next.stage_name);
        fw_fmt(&w, ",\"mins_until\":%d}", (int)s->now.next.mins_until);
    }
    fw_raw(&w, ",\"lineup\":[");
    for (uint8_t i = 0; i < s->now.n_lineup; i++) {
        if (i > 0) fw_raw(&w, ",");
        fw_lineup_item(&w, &s->now.lineup[i]);
    }
    fw_raw(&w, "]}");

    /* signals (S24 — ff_app_signals_t: sub-view + the ff_inbox_t
     * conversation list + the kept S22(d) target fields) */
    fw_raw(&w, ",\"signals\":{\"subview\":\"");
    fw_raw(&w, fx_enum_name(fx_subview_table, sizeof(fx_subview_table) / sizeof(fx_subview_table[0]),
                             s->signals.subview, "inbox"));
    fw_raw(&w, "\"");
    fw_fmt(&w, ",\"thread_node\":%u", (unsigned)s->signals.thread_node);
    fw_raw(&w, ",\"thread_name\":");
    fw_json_str(&w, s->signals.thread_name);
    fw_fmt(&w, ",\"thread_color_idx\":%u", (unsigned)s->signals.thread_color_idx);
    fw_raw(&w, ",\"target_kind\":\"");
    fw_raw(&w, fx_enum_name(fx_target_kind_table, sizeof(fx_target_kind_table) / sizeof(fx_target_kind_table[0]),
                             s->signals.target_kind, "whole_crew"));
    fw_raw(&w, "\"");
    fw_fmt(&w, ",\"target_node\":%u", (unsigned)s->signals.target_node);
    fw_raw(&w, s->signals.rally_confirm_armed ? ",\"rally_confirm_armed\":true" : ",\"rally_confirm_armed\":false");
    fw_raw(&w, ",\"convs\":[");
    for (uint8_t i = 0; i < s->signals.inbox.conv_count; i++) {
        if (i > 0) fw_raw(&w, ",");
        fw_conv(&w, &s->signals.inbox.convs[i]);
    }
    fw_raw(&w, "]");
    /* S24 slice (c): the open thread's messages. */
    fw_raw(&w, ",\"msgs\":[");
    for (uint8_t i = 0; i < s->signals.thread.msg_count; i++) {
        if (i > 0) fw_raw(&w, ",");
        fw_msg(&w, &s->signals.thread.msgs[i]);
    }
    fw_raw(&w, "]}");

    /* flare (S10 slice b's three-independent-groups shape — see
     * ff_app_state.h's doc comment and fx_parse_flare above, which this
     * mirrors field-for-field so a dump round-trips through the loader.
     * [api] merge note: this section used to write the old single-`state`
     * enum shape (S10 slice a scaffolding); rewritten here to match S10b's
     * breaking replacement, landed on main while this PR was in flight. */
    fw_raw(&w, ",\"flare\":{");
    fw_raw(&w, s->flare.sending ? "\"sending\":true" : "\"sending\":false");
    fw_fmt(&w, ",\"send_expires_in_ms\":%d", (int)s->flare.send_expires_in_ms);

    fw_raw(&w, s->flare.takeover_active ? ",\"takeover_active\":true" : ",\"takeover_active\":false");
    fw_raw(&w, ",\"takeover_from_name\":");
    fw_json_str(&w, s->flare.takeover_from_name);
    fw_raw(&w, s->flare.takeover_bearing_valid ? ",\"takeover_bearing_valid\":true"
                                                : ",\"takeover_bearing_valid\":false");
    fw_fmt(&w, ",\"takeover_bearing_deg\":%g", (double)s->flare.takeover_bearing_deg);
    fw_raw(&w, ",\"takeover_dist_str\":");
    fw_json_str(&w, s->flare.takeover_dist_str);
    fw_fmt(&w, ",\"takeover_expires_in_ms\":%d", (int)s->flare.takeover_expires_in_ms);

    fw_raw(&w, s->flare.locked ? ",\"locked\":true" : ",\"locked\":false");
    fw_raw(&w, ",\"locked_from_name\":");
    fw_json_str(&w, s->flare.locked_from_name);
    fw_fmt(&w, ",\"locked_expires_in_ms\":%d}", (int)s->flare.locked_expires_in_ms);

    /* compose (S16 slice d: fx_parse_compose above has accepted this
     * section on LOAD since S08, but nothing ever wrote it back out —
     * the ctl `state` dump could never show the composer's own draft,
     * which the S16 AC10 sequence test needs to observe surviving a
     * flare takeover THROUGH THE SOCKET, not just by reading the C
     * struct directly. Field-for-field mirror of fx_parse_compose so a
     * dump still round-trips through the loader. */
    fw_raw(&w, ",\"compose\":{\"text\":");
    fw_json_str(&w, s->compose.text);
    fw_raw(&w, ",\"to_name\":");
    fw_json_str(&w, s->compose.to_name);
    fw_raw(&w, s->compose.has_pending ? ",\"has_pending\":true" : ",\"has_pending\":false");
    fw_raw(&w, ",\"mode\":\"");
    fw_raw(&w, fx_enum_name(fx_compose_mode_table, sizeof(fx_compose_mode_table) / sizeof(fx_compose_mode_table[0]),
                             s->compose.mode, "abc"));
    fw_raw(&w, "\"}");

    /* settings */
    fw_raw(&w, ",\"settings\":{");
    fw_raw(&w, s->settings.imperial ? "\"imperial\":true" : "\"imperial\":false");
    fw_raw(&w, ",\"share_mode\":\"");
    fw_raw(&w, fx_enum_name(fx_share_mode_table, sizeof(fx_share_mode_table) / sizeof(fx_share_mode_table[0]),
                             s->settings.share_mode, "live"));
    fw_raw(&w, "\"");
    fw_raw(&w, s->settings.haptics ? ",\"haptics\":true" : ",\"haptics\":false");
    fw_raw(&w, s->settings.night_glow ? ",\"night_glow\":true" : ",\"night_glow\":false");
    fw_fmt(&w, ",\"water_min\":%u", (unsigned)s->settings.water_min);
    fw_fmt(&w, ",\"quiet_from_min\":%u", (unsigned)s->settings.quiet_from_min);
    fw_fmt(&w, ",\"quiet_to_min\":%u", (unsigned)s->settings.quiet_to_min);
    fw_raw(&w, ",\"my_name\":");
    fw_json_str(&w, s->settings.my_name);
    fw_raw(&w, s->settings.utc_offset_set ? ",\"utc_offset_set\":true" : ",\"utc_offset_set\":false");
    fw_fmt(&w, ",\"utc_offset_min\":%d", (int)s->settings.utc_offset_min);
    fw_raw(&w, s->settings.colorblind ? ",\"colorblind\":true" : ",\"colorblind\":false"); /* S17 slice a */
    fw_fmt(&w, ",\"brightness_pct\":%u", (unsigned)s->settings.brightness_pct); /* #100 */
    fw_raw(&w, "}");

    /* map (S09) — field-for-field mirror of fx_parse_map so a dump
     * round-trips through the loader. */
    fw_raw(&w, ",\"map\":{\"features\":[");
    for (uint8_t i = 0; i < s->map.n_features; i++) {
        if (i > 0) fw_raw(&w, ",");
        fw_map_feature(&w, &s->map.features[i]);
    }
    fw_raw(&w, "]");
    fw_raw(&w, s->map.truncated ? ",\"truncated\":true" : ",\"truncated\":false");
    fw_fmt(&w, ",\"features_omitted\":%u", (unsigned)s->map.features_omitted);
    fw_raw(&w, ",\"crew\":[");
    for (uint8_t i = 0; i < s->map.n_crew; i++) {
        if (i > 0) fw_raw(&w, ",");
        fw_map_crew(&w, &s->map.crew[i]);
    }
    fw_raw(&w, "]");
    if (s->map.has_rally) {
        fw_raw(&w, ",\"rally\":{\"label\":");
        fw_json_str(&w, s->map.rally_label);
        fw_fmt(&w, ",\"east_m\":%g,\"north_m\":%g}", (double)s->map.rally_east_m, (double)s->map.rally_north_m);
    }
    /* `you` OMITTED entirely when there's no position at all — same
     * round-trip-safe "absent key -> honest default" contract as
     * flare's optional groups (mirrors fx_parse_map's own "you absent ->
     * has_pos/heading_valid stay false" default exactly). */
    if (s->map.you_has_pos) {
        fw_fmt(&w, ",\"you\":{\"has_pos\":true,\"east_m\":%g,\"north_m\":%g", (double)s->map.you_east_m,
               (double)s->map.you_north_m);
        fw_raw(&w, s->map.you_heading_valid ? ",\"heading_valid\":true" : ",\"heading_valid\":false");
        fw_fmt(&w, ",\"heading_deg\":%g}", (double)s->map.you_heading_deg);
    }
    fw_raw(&w, "}");

    fw_raw(&w, "}"); /* close root */

    if (w.overflow) return -1;
    *w.p = '\0';
    return (int)(w.p - buf);
}
