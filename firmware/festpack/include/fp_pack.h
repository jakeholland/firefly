/**
 * fp_pack.h — festpack.json (fest-almanac schema v0.1) parser.
 *
 * Loads a festpack.json document into fixed-size C structs with zero
 * dynamic allocation (vendored jsmn tokenizer + hand-rolled extraction,
 * see firmware/festpack/src/fp_pack.c). Tolerant of unknown fields
 * (schema will grow); strict about the types it does read.
 *
 * Pure C11, no I/O — the caller supplies the JSON buffer (e.g. read from
 * flash/SD/network) and a caller-owned fp_pack_t to fill in.
 *
 * Spec: docs/specs/S05-festpack.md
 */
#ifndef FP_PACK_H
#define FP_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff_latlon.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FP_MAX_STAGES 12
#define FP_MAX_SETS 256
#define FP_MAX_FEATURES 24
#define FP_MAX_LANDMARKS 12
#define FP_MAX_POLY_PTS 24

/** Result of fp_parse(). */
typedef enum {
    FP_OK = 0,
    FP_ERR_JSON,    /* malformed/truncated/non-JSON input */
    FP_ERR_VERSION, /* "festpack" field missing or != "0.1" */
    FP_ERR_TOO_BIG, /* an array exceeded its FP_MAX_* cap, or input/token
                        budget exceeded */
} fp_result_t;

/** Map/feature kind. Mirrors the fest-almanac schema's closed `kind` enum
 *  exactly (["stage","camping","water","path","entrance","vendor",
 *  "medical","poi"] — see schema/festpack.schema.json). Any other JSON
 *  string (schema additions, typos, etc.) maps to FP_KIND_UNKNOWN rather
 *  than failing the parse, so the parser stays forward-tolerant even
 *  though the enum itself is closed today. */
typedef enum {
    FP_KIND_UNKNOWN = 0,
    FP_KIND_STAGE,
    FP_KIND_CAMPING,
    FP_KIND_WATER,
    FP_KIND_PATH,
    FP_KIND_ENTRANCE,
    FP_KIND_VENDOR,
    FP_KIND_MEDICAL,
    FP_KIND_POI,
} fp_feature_kind_t;

typedef struct {
    char id[16];
    char name[28];
    uint32_t color_rgb; /* 0x00RRGGBB */
} fp_stage_t;

typedef struct {
    char artist[32];
    int8_t stage_idx;    /* index into fp_pack_t.stages, -1 unknown */
    uint16_t day_doy;    /* day-of-year, 1..366 */
    int16_t start_min;   /* minutes from local midnight, -1 null */
    int16_t end_min;     /* minutes from local midnight, -1 null */
    char note[24];
    bool starred;
} fp_set_t;

typedef struct {
    uint8_t kind;      /* fp_feature_kind_t */
    int8_t stage_idx;  /* index into fp_pack_t.stages, -1 unknown/none */
    char label[32];
    uint8_t n_pts;
    float pts_en[FP_MAX_POLY_PTS][2]; /* [east_m, north_m], projected at parse time */
} fp_feature_t;

typedef struct {
    char id[16];
    char name[28];
    bool has_pos;
    float east_m, north_m; /* valid only if has_pos */
} fp_landmark_t;

typedef struct {
    char name[32];
    uint16_t year;
    ff_latlon_t origin; /* festival.venue lat/lon — projection origin.
                            Meaningless (reads as {0,0}) unless origin_known
                            is true — see origin_known below. */
    bool origin_known;  /* true iff festival.venue.lat AND .lon were both
                            present and non-null. The schema allows either
                            to be null ("unknown venue"), distinct from
                            origin_approx ("known, but guesswork"). Callers
                            (map/radar screens) MUST check this before
                            trusting `origin` or any projected east_m/
                            north_m in features[]/landmarks[] — when false,
                            those were computed against a (0,0) fallback
                            origin and are not real positions. */
    bool origin_approx; /* true iff festival.venue.approximate == true:
                            venue-center guesswork, not surveyed. Only
                            meaningful when origin_known is also true. */
    uint16_t start_doy, end_doy;
    int16_t utc_offset_min; /* minutes east of UTC; defaults to -240 (EDT)
                                when the pack omits utc_offset_min — see
                                docs/specs/S05-festpack.md */
    bool utc_offset_assumed; /* true iff utc_offset_min was absent from the
                                 pack and the -240 default was applied
                                 (rather than an explicit value read from
                                 the pack). Callers should treat schedule
                                 times as EDT-assumed, not verified, when
                                 this is true. */

    fp_stage_t stages[FP_MAX_STAGES];
    uint8_t n_stages;

    fp_set_t sets[FP_MAX_SETS];
    uint16_t n_sets;

    fp_feature_t features[FP_MAX_FEATURES];
    uint8_t n_features;

    fp_landmark_t landmarks[FP_MAX_LANDMARKS];
    uint8_t n_landmarks;
} fp_pack_t;

/* Must live comfortably in ESP32-S3 PSRAM (S05 AC6). */
_Static_assert(sizeof(fp_pack_t) <= 48 * 1024,
               "fp_pack_t exceeds the 48KB pack struct budget (S05 AC6)");

/**
 * fp_parse — parse `json[0..len)` into `*out`.
 *
 * Zero dynamic allocation. Not reentrant (uses an internal static token
 * arena) — call from a single context at a time, matching the "one pack
 * loaded at a time" device usage pattern.
 *
 * On any return other than FP_OK, `*out` is left zeroed (memset), not
 * partially populated.
 */
fp_result_t fp_parse(char const *json, size_t len, fp_pack_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FP_PACK_H */
