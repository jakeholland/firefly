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

/* String -> enum lookup over a {name, value} table. Returns `dflt` if the
 * token isn't a string or matches no table entry (unrecognized enum
 * strings are a fixture-authoring bug, not a parse failure — fixtures are
 * dev data, so this loader degrades to the documented default rather
 * than rejecting the whole file, matching festpack's "tolerant" style
 * for anything that isn't a hard structural error). */
typedef struct {
    char const *name;
    int value;
} fx_enum_entry_t;

static int fx_enum(fx_ctx_t const *c, int i, fx_enum_entry_t const *table, size_t n_entries, int dflt)
{
    if (i < 0 || i >= c->ntoks) return dflt;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_STRING) return dflt;
    int n = t->end - t->start;
    for (size_t k = 0; k < n_entries; k++) {
        size_t slen = strlen(table[k].name);
        if ((size_t)n == slen && memcmp(c->js + t->start, table[k].name, slen) == 0) return table[k].value;
    }
    return dflt;
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
        r->n_dots++;
        idx = fx_skip(c, obj_i);
    }
    return FF_FIXTURE_OK;
}

static ff_fixture_result_t fx_parse_radar(fx_ctx_t const *c, int obj_i, ff_radar_view_t *r)
{
    int t;
    if (fx_obj_get(c, obj_i, "mode", &t)) {
        r->mode = (radar_mode_t)fx_enum(c, t, fx_radar_mode_table,
                                         sizeof(fx_radar_mode_table) / sizeof(fx_radar_mode_table[0]), RADAR_NOSEL);
    }
    if (fx_obj_get(c, obj_i, "arrow_deg", &t)) r->arrow_deg = (float)fx_num(c, t, 0.0);
    if (fx_obj_get(c, obj_i, "arrow_valid", &t)) r->arrow_valid = fx_bool(c, t, false);
    if (fx_obj_get(c, obj_i, "name", &t)) fx_copy_str(c, t, r->name, sizeof(r->name));
    if (fx_obj_get(c, obj_i, "dist_str", &t)) fx_copy_str(c, t, r->dist_str, sizeof(r->dist_str));
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
}

static void fx_parse_lineup_item(fx_ctx_t const *c, int obj_i, ff_app_lineup_item_t *item)
{
    int t;
    if (fx_obj_get(c, obj_i, "artist", &t)) fx_copy_str(c, t, item->artist, sizeof(item->artist));
    if (fx_obj_get(c, obj_i, "stage_name", &t)) fx_copy_str(c, t, item->stage_name, sizeof(item->stage_name));
}

/* PR #21 code review finding #2/ruling: `now.state` replaces the earlier
 * `pack_loaded`+`tbd` bool pair, same string-enum convention as
 * fx_radar_mode_table above. Absent/unrecognized -> NOW_NO_PACK (the
 * enum's zero value — see now_state_t's own doc comment for why that's
 * the correct default, same reasoning as radar.mode's RADAR_NOSEL
 * default). */
static const fx_enum_entry_t fx_now_state_table[] = {
    {"no_pack", NOW_NO_PACK},
    {"tbd", NOW_TBD},
    {"mixed", NOW_MIXED},
    {"live", NOW_LIVE},
    {"nothing_playing", NOW_NOTHING_PLAYING},
};

/* fx_parse_now — same fail-loud-on-oversized-array treatment as
 * fx_parse_radar_dots above, for the `rows` array (cap
 * FF_APP_NOW_MAX_ROWS) and, as of S07 slice b, the `lineup` array (cap
 * FF_APP_NOW_MAX_LINEUP). */
static ff_fixture_result_t fx_parse_now(fx_ctx_t const *c, int obj_i, ff_app_now_t *now)
{
    int t;
    if (fx_obj_get(c, obj_i, "state", &t)) {
        now->state = (now_state_t)fx_enum(c, t, fx_now_state_table,
                                           sizeof(fx_now_state_table) / sizeof(fx_now_state_table[0]), NOW_NO_PACK);
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

static const fx_enum_entry_t fx_feed_kind_table[] = {
    {"pulse", FF_APP_FEED_PULSE}, {"text", FF_APP_FEED_TEXT}, {"rally", FF_APP_FEED_RALLY},
    {"status", FF_APP_FEED_STATUS}, {"flare", FF_APP_FEED_FLARE},
};

/* fx_parse_signals — same fail-loud-on-oversized-array treatment as
 * fx_parse_radar_dots above, for the `items` array (cap
 * FF_APP_SIGNALS_MAX_ITEMS). */
static ff_fixture_result_t fx_parse_signals(fx_ctx_t const *c, int obj_i, ff_app_signals_t *sig)
{
    int t;
    int items_i;
    if (fx_obj_get(c, obj_i, "items", &items_i) && !fx_is_null(c, items_i)) {
        jsmntok_t const *at = &c->toks[items_i];
        if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
        if (at->size > FF_APP_SIGNALS_MAX_ITEMS) return FF_FIXTURE_ERR_TOO_BIG;
        int idx = items_i + 1;
        for (int i = 0; i < at->size; i++) {
            int item_i = idx;
            ff_app_feed_item_t *it = &sig->items[sig->n_items];
            memset(it, 0, sizeof(*it));
            int kt;
            if (fx_obj_get(c, item_i, "kind", &kt))
                it->kind = (ff_app_feed_kind_t)fx_enum(c, kt, fx_feed_kind_table,
                                                        sizeof(fx_feed_kind_table) / sizeof(fx_feed_kind_table[0]),
                                                        FF_APP_FEED_TEXT);
            if (fx_obj_get(c, item_i, "from_name", &kt)) fx_copy_str(c, kt, it->from_name, sizeof(it->from_name));
            if (fx_obj_get(c, item_i, "text", &kt)) fx_copy_str(c, kt, it->text, sizeof(it->text));
            if (fx_obj_get(c, item_i, "age_str", &kt)) fx_copy_str(c, kt, it->age_str, sizeof(it->age_str));
            if (fx_obj_get(c, item_i, "unread", &kt)) it->unread = fx_bool(c, kt, false);
            sig->n_items++;
            idx = fx_skip(c, item_i);
        }
    }
    if (fx_obj_get(c, obj_i, "unread_count", &t)) sig->unread_count = (uint8_t)fx_num(c, t, 0.0);
    return FF_FIXTURE_OK;
}

static const fx_enum_entry_t fx_compose_mode_table[] = {
    {"abc", FF_APP_COMPOSE_ABC},
    {"123", FF_APP_COMPOSE_123},
    {"sym", FF_APP_COMPOSE_SYM},
};

static void fx_parse_compose(fx_ctx_t const *c, int obj_i, ff_app_compose_t *cp)
{
    int t;
    if (fx_obj_get(c, obj_i, "text", &t)) fx_copy_str(c, t, cp->text, sizeof(cp->text));
    if (fx_obj_get(c, obj_i, "to_name", &t)) fx_copy_str(c, t, cp->to_name, sizeof(cp->to_name));
    if (fx_obj_get(c, obj_i, "has_pending", &t)) cp->has_pending = fx_bool(c, t, false);
    if (fx_obj_get(c, obj_i, "mode", &t))
        cp->mode = (ff_app_compose_mode_t)fx_enum(c, t, fx_compose_mode_table,
                                                    sizeof(fx_compose_mode_table) / sizeof(fx_compose_mode_table[0]),
                                                    FF_APP_COMPOSE_ABC);
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

static void fx_parse_settings(fx_ctx_t const *c, int obj_i, ff_app_settings_t *s)
{
    int t;
    if (fx_obj_get(c, obj_i, "imperial", &t)) s->imperial = fx_bool(c, t, true);
    if (fx_obj_get(c, obj_i, "share_mode", &t))
        s->share_mode = (uint8_t)fx_enum(c, t, fx_share_mode_table,
                                          sizeof(fx_share_mode_table) / sizeof(fx_share_mode_table[0]), 0);
    if (fx_obj_get(c, obj_i, "haptics", &t)) s->haptics = fx_bool(c, t, true);
    if (fx_obj_get(c, obj_i, "night_glow", &t)) s->night_glow = fx_bool(c, t, true);
    if (fx_obj_get(c, obj_i, "water_min", &t)) s->water_min = (uint16_t)fx_num(c, t, 90.0);
    if (fx_obj_get(c, obj_i, "quiet_from_min", &t)) s->quiet_from_min = (uint16_t)fx_num(c, t, 240.0);
    if (fx_obj_get(c, obj_i, "quiet_to_min", &t)) s->quiet_to_min = (uint16_t)fx_num(c, t, 600.0);
    if (fx_obj_get(c, obj_i, "my_name", &t)) fx_copy_str(c, t, s->my_name, sizeof(s->my_name));
}

static const fx_enum_entry_t fx_face_table[] = {
    {"radar", FF_APP_FACE_RADAR},
    {"now", FF_APP_FACE_NOW},
    {"signals", FF_APP_FACE_SIGNALS},
    {"settings", FF_APP_FACE_SETTINGS},
    {"compose", FF_APP_FACE_COMPOSE},
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
    if (fx_obj_get(&ctx, 0, "face", &t))
        out->active_face = (ff_app_face_t)fx_enum(&ctx, t, fx_face_table,
                                                    sizeof(fx_face_table) / sizeof(fx_face_table[0]),
                                                    FF_APP_FACE_RADAR);

    /* radar/now/signals can fail-loud on an oversized array (see their
     * parsers' doc comments) — on any such failure, re-zero `out` before
     * returning, matching this function's documented "non-OK return
     * leaves *out fully zeroed" contract (same as fp_parse()'s). */
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
    if (fx_obj_get(&ctx, 0, "compose", &sec_i) && !fx_is_null(&ctx, sec_i))
        fx_parse_compose(&ctx, sec_i, &out->compose);
    if (fx_obj_get(&ctx, 0, "flare", &sec_i) && !fx_is_null(&ctx, sec_i)) fx_parse_flare(&ctx, sec_i, &out->flare);
    /* else: out->flare's three -1 "n/a" defaults (set above) stand. */
    if (fx_obj_get(&ctx, 0, "settings", &sec_i) && !fx_is_null(&ctx, sec_i))
        fx_parse_settings(&ctx, sec_i, &out->settings);

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
    fw_raw(w, d->stale ? ",\"stale\":true}" : ",\"stale\":false}");
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
    fw_fmt(w, ",\"pct_done\":%u}", (unsigned)r->pct_done);
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

static void fw_feed_item(fw_cur_t *w, ff_app_feed_item_t const *it)
{
    fw_raw(w, "{\"kind\":\"");
    fw_raw(w, fx_enum_name(fx_feed_kind_table, sizeof(fx_feed_kind_table) / sizeof(fx_feed_kind_table[0]), it->kind,
                            "text"));
    fw_raw(w, "\",\"from_name\":");
    fw_json_str(w, it->from_name);
    fw_raw(w, ",\"text\":");
    fw_json_str(w, it->text);
    fw_raw(w, ",\"age_str\":");
    fw_json_str(w, it->age_str);
    fw_raw(w, it->unread ? ",\"unread\":true}" : ",\"unread\":false}");
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

    /* signals */
    fw_raw(&w, ",\"signals\":{\"items\":[");
    for (uint8_t i = 0; i < s->signals.n_items; i++) {
        if (i > 0) fw_raw(&w, ",");
        fw_feed_item(&w, &s->signals.items[i]);
    }
    fw_fmt(&w, "],\"unread_count\":%u}", (unsigned)s->signals.unread_count);

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
    fw_raw(&w, "}");

    fw_raw(&w, "}"); /* close root */

    if (w.overflow) return -1;
    *w.p = '\0';
    return (int)(w.p - buf);
}
