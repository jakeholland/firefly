# S05 · festpack — pack parser

## Purpose
Load a `festpack.json` (fest-almanac schema v0.1) into fixed-size C structs. Tolerant of unknown fields (schema will grow), strict about types it does read.

## Interface (`festpack/include/fp_pack.h`)
```c
#define FP_MAX_STAGES 12
#define FP_MAX_SETS 256
#define FP_MAX_FEATURES 24
#define FP_MAX_LANDMARKS 12
#define FP_MAX_POLY_PTS 24
typedef struct { char id[16]; char name[28]; uint32_t color_rgb; } fp_stage_t;
typedef struct { char artist[32]; int8_t stage_idx; /* -1 unknown */
                 uint16_t day_doy; int16_t start_min, end_min; /* minutes from local midnight, -1 null */
                 char note[24]; bool starred; } fp_set_t;
typedef struct { uint8_t kind; int8_t stage_idx; char label[32];
                 uint8_t n_pts; float pts_en[FP_MAX_POLY_PTS][2]; /* projected m, via S01 */ } fp_feature_t;
typedef struct { char id[16]; char name[28]; bool has_pos; float east_m, north_m; } fp_landmark_t;
typedef struct { char name[32]; uint16_t year; ff_latlon_t origin; bool origin_approx;
                 uint16_t start_doy, end_doy; int16_t utc_offset_min;
                 fp_stage_t stages[FP_MAX_STAGES]; uint8_t n_stages;
                 fp_set_t sets[FP_MAX_SETS]; uint16_t n_sets;
                 fp_feature_t features[FP_MAX_FEATURES]; uint8_t n_features;
                 fp_landmark_t landmarks[FP_MAX_LANDMARKS]; uint8_t n_landmarks; } fp_pack_t;

fp_result_t fp_parse(char const *json, size_t len, fp_pack_t *out); // OK / ERR_JSON / ERR_VERSION / ERR_TOO_BIG
```

## Behavior
- Parser: vendored **jsmn** (tokenizer, zero-alloc) + hand-rolled extraction. No dynamic allocation.
- `festpack` version must be `"0.1"` (`ERR_VERSION` otherwise).
- Null times → −1; null polygons → n_pts 0; unknown keys skipped. Overflowing any MAX ⇒ `ERR_TOO_BIG` (fail loudly; caps are generous for real festivals).
- Timezone: schema carries IANA name; device stores a UTC offset chosen at pack-load (sim/tools resolve it; device gets it via pack meta or setting — v1 uses `utc_offset_min` extension field, PR to fest-almanac schema noted).
- Lat/lon features are projected to east/north meters at parse time using `ff_geo_project` (map face consumes meters only).

## Acceptance criteria
1. Parses `packs/lost-lands/2026/festpack.json` (vendored as fixture): 7 stages, ≥27 sets, all times −1, names/colors exact.
2. Null-handling: fixture with nulls in every nullable slot parses; absent optional sections parse.
3. Wrong version, truncated JSON, non-JSON ⇒ correct errors, no crash (fuzz smoke 10k iters).
4. Overflow fixtures (13 stages, 257 sets) ⇒ ERR_TOO_BIG.
5. Feature polygons project: fixture with known lat/lon square → east/north within 1 m.
6. Struct fits in ≤48 KB (static assert) — must live in ESP32-S3 PSRAM comfortably.

## Slices
a) tokenizer+festival/stages/schedule · b) map/landmarks+projection · c) fuzz+fixtures.
