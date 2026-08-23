/**
 * fp_pack.c — festpack.json parser implementation.
 *
 * Vendored jsmn (firmware/third_party/jsmn.h) tokenizes the input into a
 * flat, zero-alloc token array; the extraction below walks that array by
 * hand, matching keys and copying/converting into the caller-provided
 * fp_pack_t. No malloc, no recursion beyond the JSON's own (small,
 * bounded) nesting depth in fp_skip().
 */
#define JSMN_STATIC
#include "jsmn.h"

#include "fp_pack.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Parser budget. Independent of the fp_pack_t struct's 48KB output
 * budget (S05 AC6) — this bounds the *input* JSON text and the transient
 * jsmn token arena used only during fp_parse(). A too-large input or a
 * token count that overruns the arena both map to FP_ERR_TOO_BIG (never
 * a crash) — see fp_parse().
 * ------------------------------------------------------------------- */
#define FP_MAX_JSON_LEN (64u * 1024u)
#define FP_MAX_TOKENS 8192

/* ---------------------------------------------------------------------
 * fp_project — local equirectangular lat/lon -> east/north projection.
 *
 * TODO(S01): S01 (core/geo, ff_geo_project) is being built in parallel
 * with this slice; per the S05 brief we must not depend on it. Swap this
 * for `ff_geo_project()` once S01 lands — same signature shape, so the
 * call sites below need no change beyond the #include.
 *
 * Constants:
 *   111320 m/deg — meters per degree of longitude at the equator
 *     (WGS84 mean meridian length / 360), scaled by cos(lat0) to account
 *     for meridian convergence at the origin's latitude.
 *   110540 m/deg — meters per degree of latitude (WGS84 mean; the true
 *     value varies from ~110,574 m/deg at the equator to ~111,694 m/deg
 *     at the poles — 110540 is the standard mid-latitude approximation
 *     used by equirectangular "flat-earth" projections).
 * Valid at festival scale (<10 km from origin) per S01's own bound.
 * ------------------------------------------------------------------- */
#define FP_DEG_TO_RAD (3.14159265358979323846 / 180.0)

static void fp_project(ff_latlon_t origin, ff_latlon_t p, float *east_m, float *north_m)
{
    double lat0_rad = origin.lat * FP_DEG_TO_RAD;
    *east_m = (float)((p.lon - origin.lon) * cos(lat0_rad) * 111320.0);
    *north_m = (float)((p.lat - origin.lat) * 110540.0);
}

/* ---------------------------------------------------------------------
 * Token-array helpers.
 * ------------------------------------------------------------------- */
typedef struct {
    char const *js;
    jsmntok_t const *toks;
    int ntoks;
} fp_ctx_t;

/* Returns the index of the token immediately after tok i's subtree
 * (i's own value plus, for containers, all descendants). */
static int fp_skip(fp_ctx_t const *c, int i)
{
    if (i < 0 || i >= c->ntoks) return c->ntoks;
    jsmntok_t const *t = &c->toks[i];
    int next = i + 1;
    if (t->type == JSMN_OBJECT) {
        for (int k = 0; k < t->size; k++) {
            next = fp_skip(c, next); /* key */
            next = fp_skip(c, next); /* value */
        }
    } else if (t->type == JSMN_ARRAY) {
        for (int k = 0; k < t->size; k++) {
            next = fp_skip(c, next);
        }
    }
    return next;
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

/* Bounded copy, always NUL-terminated. Oversized field VALUES (as
 * opposed to oversized ARRAYS) are truncated, not treated as
 * FP_ERR_TOO_BIG — only the counted collections (stages/sets/features/
 * landmarks/polygon points) are overflow-checked per the spec. */
static void fp_copy_str(fp_ctx_t const *c, int i, char *dst, size_t dst_sz)
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

static double fp_num(fp_ctx_t const *c, int i, double dflt)
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

/* "YYYY-MM-DD" -> day-of-year (1..366). Returns 0 on malformed input
 * rather than failing the whole parse — a bad date is a data-quality
 * problem for the schedule engine (S07), not a parse-time crash. */
static uint16_t fp_doy_from_iso_date(char const *s, size_t len)
{
    if (len != 10 || s[4] != '-' || s[7] != '-') return 0;
    int y = 0, m = 0, d = 0;
    for (int i = 0; i < 4; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        y = y * 10 + (s[i] - '0');
    }
    for (int i = 5; i < 7; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        m = m * 10 + (s[i] - '0');
    }
    for (int i = 8; i < 10; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        d = d * 10 + (s[i] - '0');
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
    uint16_t doy = fp_cum_days[m - 1] + (uint16_t)d;
    if (m > 2 && fp_is_leap_year(y)) doy += 1;
    return doy;
}

/* "HH:MM" -> minutes from midnight, or -1 (null / malformed). */
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
    static const struct {
        char const *s;
        fp_feature_kind_t k;
    } table[] = {
        {"entrance", FP_KIND_ENTRANCE}, {"stage", FP_KIND_STAGE}, {"water", FP_KIND_WATER},
        {"vendor", FP_KIND_VENDOR},     {"camping", FP_KIND_CAMPING}, {"parking", FP_KIND_PARKING},
        {"medical", FP_KIND_MEDICAL},   {"food", FP_KIND_FOOD},     {"gate", FP_KIND_GATE},
        {"poi", FP_KIND_POI},
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
    if (fp_obj_get(c, fest_i, "year", &t)) out->year = (uint16_t)fp_num(c, t, 0.0);
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
        int lt;
        if (fp_obj_get(c, venue_i, "lat", &lt)) origin->lat = fp_num(c, lt, 0.0);
        if (fp_obj_get(c, venue_i, "lon", &lt)) origin->lon = fp_num(c, lt, 0.0);
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
        if (fp_obj_get(c, obj_i, "day", &t) && !fp_is_null(c, t)) {
            jsmntok_t const *tt = &c->toks[t];
            s->day_doy = fp_doy_from_iso_date(c->js + tt->start, (size_t)(tt->end - tt->start));
        }
        if (fp_obj_get(c, obj_i, "start", &t) && !fp_is_null(c, t)) {
            jsmntok_t const *tt = &c->toks[t];
            s->start_min = fp_min_from_hhmm(c->js + tt->start, (size_t)(tt->end - tt->start));
        }
        if (fp_obj_get(c, obj_i, "end", &t) && !fp_is_null(c, t)) {
            jsmntok_t const *tt = &c->toks[t];
            s->end_min = fp_min_from_hhmm(c->js + tt->start, (size_t)(tt->end - tt->start));
        }
        if (fp_obj_get(c, obj_i, "note", &t)) fp_copy_str(c, t, s->note, sizeof(s->note));
        if (fp_obj_get(c, obj_i, "starred", &t)) s->starred = fp_bool(c, t, false);
        out->n_sets++;
        idx = fp_skip(c, obj_i);
    }
    return FP_OK;
}

static fp_result_t fp_parse_polygon(fp_ctx_t const *c, int poly_i, ff_latlon_t origin, fp_feature_t *f)
{
    jsmntok_t const *pt = &c->toks[poly_i];
    if (pt->type != JSMN_ARRAY) return FP_ERR_JSON;
    if (pt->size > FP_MAX_POLY_PTS) return FP_ERR_TOO_BIG;
    int idx = poly_i + 1;
    for (int k = 0; k < pt->size; k++) {
        int pt_obj = idx;
        int lt;
        ff_latlon_t p = {0.0, 0.0};
        if (fp_obj_get(c, pt_obj, "lat", &lt)) p.lat = fp_num(c, lt, 0.0);
        if (fp_obj_get(c, pt_obj, "lon", &lt)) p.lon = fp_num(c, lt, 0.0);
        fp_project(origin, p, &f->pts_en[f->n_pts][0], &f->pts_en[f->n_pts][1]);
        f->n_pts++;
        idx = fp_skip(c, pt_obj);
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
        if (have_lat && have_lon) {
            ff_latlon_t p = {fp_num(c, lat_i, 0.0), fp_num(c, lon_i, 0.0)};
            fp_project(origin, p, &lm->east_m, &lm->north_m);
            lm->has_pos = true;
        }
        out->n_landmarks++;
        idx = fp_skip(c, obj_i);
    }
    return FP_OK;
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
     * the S05 PR body for the interpretation call. */
    {
        int t;
        if (fp_obj_get(c, 0, "utc_offset_min", &t) && !fp_is_null(c, t)) {
            out->utc_offset_min = (int16_t)fp_num(c, t, -240.0);
        } else if (fest_i >= 0 && fp_obj_get(c, fest_i, "utc_offset_min", &t) && !fp_is_null(c, t)) {
            out->utc_offset_min = (int16_t)fp_num(c, t, -240.0);
        } else {
            out->utc_offset_min = -240;
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

    return FP_OK;
}

fp_result_t fp_parse(char const *json, size_t len, fp_pack_t *out)
{
    if (out == NULL) return FP_ERR_JSON;
    memset(out, 0, sizeof(*out));

    if (json == NULL || len == 0) return FP_ERR_JSON;
    if (len > FP_MAX_JSON_LEN) return FP_ERR_TOO_BIG;

    /* Static, not stack: the token arena is a "fixed arena" per
     * docs/ARCHITECTURE.md's no-allocation-surprises rule, sized well
     * beyond real festpacks (see FP_MAX_TOKENS above). This makes
     * fp_parse() non-reentrant — callers load one pack at a time, which
     * matches the device's actual usage pattern. */
    static jsmntok_t toks[FP_MAX_TOKENS];
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, len, toks, FP_MAX_TOKENS);
    if (r == JSMN_ERROR_NOMEM) return FP_ERR_TOO_BIG;
    if (r < 0 || r == 0) return FP_ERR_JSON; /* INVAL / PART / empty */

    fp_ctx_t ctx = {json, toks, r};
    if (toks[0].type != JSMN_OBJECT) return FP_ERR_JSON;

    fp_result_t res = fp_parse_inner(&ctx, out);
    if (res != FP_OK) memset(out, 0, sizeof(*out));
    return res;
}
