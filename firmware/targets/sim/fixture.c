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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

static void fx_copy_str(fx_ctx_t const *c, int i, char *dst, size_t dst_sz)
{
    dst[0] = '\0';
    if (i < 0 || i >= c->ntoks || dst_sz == 0) return;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_STRING) return;
    size_t n = (size_t)(t->end - t->start);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, c->js + t->start, n);
    dst[n] = '\0';
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
 * as the packed integer directly. Any other token type, or a malformed
 * hex string, returns `dflt`. (Review finding #1: this used to route
 * unconditionally through fx_num(), which silently returned 0 for the
 * documented "#rrggbb" string form — a JSON_STRING token is not a
 * JSMN_PRIMITIVE, so fx_num() rejected it without any signal.) */
static uint32_t fx_color_rgb(fx_ctx_t const *c, int i, uint32_t dflt)
{
    if (i < 0 || i >= c->ntoks) return dflt;
    jsmntok_t const *t = &c->toks[i];
    if (t->type == JSMN_STRING) {
        char const *s = c->js + t->start;
        int n = t->end - t->start;
        if (n > 0 && s[0] == '#') {
            s++;
            n--;
        }
        if (n != 6) return dflt;
        uint32_t v = 0;
        for (int k = 0; k < 6; k++) {
            int nib = fx_hex_nibble(s[k]);
            if (nib < 0) return dflt;
            v = (v << 4) | (uint32_t)nib;
        }
        return v;
    }
    if (t->type == JSMN_PRIMITIVE) return (uint32_t)fx_num(c, i, (double)dflt);
    return dflt;
}

/* ---------------------------------------------------------------------
 * Section parsers.
 * ------------------------------------------------------------------- */

static const fx_enum_entry_t fx_radar_mode_table[] = {
    {"live", FF_APP_RADAR_LIVE}, {"stale", FF_APP_RADAR_STALE}, {"lost", FF_APP_RADAR_LOST},
    {"close", FF_APP_RADAR_CLOSE}, {"nofix", FF_APP_RADAR_NOFIX}, {"nosel", FF_APP_RADAR_NOSEL},
};

/* fx_parse_radar_dots — fail-loud on an oversized array (orchestrator
 * ruling on PR review finding #3/#4: consistent with fp_parse's
 * FP_ERR_TOO_BIG and the honest-data culture, an over-cap array is
 * rejected outright rather than silently truncated — a fixture that
 * grows a 9th dot should get a loud "fixture X has 9 items, cap is 8"
 * failure, not a quietly-dropped entry that surfaces later as an
 * unrelated-looking golden diff). The size check runs BEFORE any writes
 * into `r->dots[]`, so `r->n_dots` never exceeds FF_APP_RADAR_MAX_DOTS
 * on any code path through this function — see test_fixture.c's
 * `radar_dots_over_cap_fails_loud` for the regression test (and the
 * mutation-testing rationale: if this check is ever deleted, that test
 * stops observing FF_FIXTURE_ERR_TOO_BIG and fails, rather than the
 * cap-overflow going unnoticed as before). */
static ff_fixture_result_t fx_parse_radar_dots(fx_ctx_t const *c, int arr_i, ff_app_radar_t *r)
{
    jsmntok_t const *at = &c->toks[arr_i];
    if (at->type != JSMN_ARRAY) return FF_FIXTURE_ERR_JSON;
    if (at->size > FF_APP_RADAR_MAX_DOTS) return FF_FIXTURE_ERR_TOO_BIG;
    int idx = arr_i + 1;
    for (int i = 0; i < at->size; i++) {
        int obj_i = idx;
        ff_app_radar_dot_t *d = &r->dots[r->n_dots];
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

static ff_fixture_result_t fx_parse_radar(fx_ctx_t const *c, int obj_i, ff_app_radar_t *r)
{
    int t;
    if (fx_obj_get(c, obj_i, "mode", &t)) {
        r->mode = (ff_app_radar_mode_t)fx_enum(c, t, fx_radar_mode_table,
                                                sizeof(fx_radar_mode_table) / sizeof(fx_radar_mode_table[0]),
                                                FF_APP_RADAR_NOSEL);
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
    /* Review finding #1: this used to be fx_num() directly, which
     * silently returned 0 for the documented "#rrggbb" string form. */
    if (fx_obj_get(c, obj_i, "stage_color_rgb", &t)) row->stage_color_rgb = fx_color_rgb(c, t, 0);
    if (fx_obj_get(c, obj_i, "mins_left", &t)) row->mins_left = (int16_t)fx_num(c, t, 0.0);
    if (fx_obj_get(c, obj_i, "pct_done", &t)) row->pct_done = (uint8_t)fx_num(c, t, 0.0);
}

/* fx_parse_now — same fail-loud-on-oversized-array treatment as
 * fx_parse_radar_dots above, for the `rows` array (cap
 * FF_APP_NOW_MAX_ROWS). */
static ff_fixture_result_t fx_parse_now(fx_ctx_t const *c, int obj_i, ff_app_now_t *now)
{
    int t;
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
    if (fx_obj_get(c, obj_i, "tbd", &t)) now->tbd = fx_bool(c, t, false);
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

static const fx_enum_entry_t fx_flare_state_table[] = {
    {"idle", FF_APP_FLARE_IDLE},
    {"sending", FF_APP_FLARE_SENDING},
    {"received", FF_APP_FLARE_RECEIVED},
    {"locked", FF_APP_FLARE_LOCKED},
};

static void fx_parse_flare(fx_ctx_t const *c, int obj_i, ff_app_flare_t *fl)
{
    int t;
    fl->expires_in_ms = -1;
    if (fx_obj_get(c, obj_i, "state", &t))
        fl->state = (ff_app_flare_state_t)fx_enum(c, t, fx_flare_state_table,
                                                   sizeof(fx_flare_state_table) / sizeof(fx_flare_state_table[0]),
                                                   FF_APP_FLARE_IDLE);
    if (fx_obj_get(c, obj_i, "from_name", &t)) fx_copy_str(c, t, fl->from_name, sizeof(fl->from_name));
    if (fx_obj_get(c, obj_i, "expires_in_ms", &t) && !fx_is_null(c, t))
        fl->expires_in_ms = (int32_t)fx_num(c, t, -1.0);
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
     * (FF_APP_RADAR_LIVE == 0), a much stronger, more misleading claim
     * than "no fixture data provided" (CLAUDE.md: "unknown = explicitly
     * unknown... never fake freshness, positions, or times" — LIVE is
     * exactly the kind of fake-freshness claim that principle rules
     * out). Section parsers below may still override these from
     * explicit JSON fields. */
    out->radar.mode = FF_APP_RADAR_NOSEL;
    out->flare.expires_in_ms = -1;

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
    if (fx_obj_get(&ctx, 0, "flare", &sec_i) && !fx_is_null(&ctx, sec_i)) fx_parse_flare(&ctx, sec_i, &out->flare);
    /* else: out->flare.expires_in_ms's -1 "n/a" default (set above) stands. */
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
