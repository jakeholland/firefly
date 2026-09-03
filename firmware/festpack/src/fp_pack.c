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

/* Strict numeric extraction: succeeds ONLY if token i exists, is a JSON
 * number (JSMN_PRIMITIVE, not "null"/"true"/"false"), and strtod()
 * consumes the token's ENTIRE text with no trailing garbage. Writes *out
 * and returns true on success; leaves *out untouched and returns false on
 * any mismatch (wrong JSMN type — e.g. a quoted string — a boolean
 * literal, null, or malformed number text).
 *
 * Any call site that gates a "known"/"verified"/"assumed" flag on a
 * value's *correctness* (not just the key's presence) MUST use this, not
 * fp_num() below — fp_num() silently substitutes a default on a type
 * mismatch, which is fine for a plain data field with no separate honesty
 * flag, but was exactly the bug fixed here for origin/landmark-position/
 * utc_offset_min: those sites used to call fp_obj_get()+!fp_is_null() to
 * decide the flag, then fp_num() (which can *also* silently default) for
 * the value — so a wrong-typed-but-present field looked "verified" while
 * actually holding the default. See docs/specs/S05-festpack.md's
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
    *out = v;
    return true;
}

/* Lenient wrapper over fp_num_checked(): returns `dflt` on any mismatch
 * instead of signaling failure. Correct for plain data fields that have
 * no separate "was this verified" flag downstream (e.g. `year`, a set's
 * `starred` bool) — silently defaulting there is the same tolerant
 * posture the parser already takes for unknown keys. NOT correct for a
 * field whose presence flips a "known"/"assumed" bool elsewhere; use
 * fp_num_checked() directly for those (see its comment). */
static double fp_num(fp_ctx_t const *c, int i, double dflt)
{
    double v;
    if (fp_num_checked(c, i, &v)) return v;
    return dflt;
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
     * A wrong-typed value (e.g. a quoted "-240") at either location is
     * treated exactly like the field being absent there — it falls
     * through to the next location, and ultimately to the default —
     * rather than marking utc_offset_assumed false while quietly holding
     * the -240 default. ff_shell.c reads utc_offset_assumed as the S18
     * wall-clock-trust signal, so a bad-but-present value must not
     * outrank the user's manual setting. See docs/specs/S05-festpack.md's
     * Amendments entry. */
    {
        int t;
        double v;
        if (fp_obj_get(c, 0, "utc_offset_min", &t) && !fp_is_null(c, t) && fp_num_checked(c, t, &v)) {
            out->utc_offset_min = (int16_t)v;
            out->utc_offset_assumed = false;
        } else if (fest_i >= 0 && fp_obj_get(c, fest_i, "utc_offset_min", &t) && !fp_is_null(c, t) &&
                   fp_num_checked(c, t, &v)) {
            out->utc_offset_min = (int16_t)v;
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
