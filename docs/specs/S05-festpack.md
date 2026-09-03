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

## Amendments

### Wrong-typed numeric fields are honest unknowns, never "known" positions/offsets

A field can be present with the *wrong* JSON type — e.g. `"lat": "43.7"`
(a string, where the schema promises a number) — distinct from being
absent or explicitly `null`. Found as a P0 honesty bug: `fp_num()`
silently returned its caller-supplied default on any type mismatch, with
no failure signal, while three call sites separately flipped a "verified"
boolean (`origin_known`, `has_pos`, `utc_offset_assumed`) based only on
the key's *presence*, not on the value actually having parsed. A quoted
`festival.venue.lat` therefore produced `origin_known == true` with a
silently-defaulted `(0, lon)` origin; a quoted landmark `lat` produced
`has_pos == true` at a fabricated `(0,0)`-projected position; a quoted
`utc_offset_min` produced `utc_offset_assumed == false` ("explicit")
while quietly using the -240 default — outranking the user's own S18
manual clock-trust setting.

**Policy, decided per field against this spec and `fp_pack.h`'s own
documented contract** (CLAUDE.md: "honest data over pretty data" — unknown
must read as unknown, never as verified):

- **`festival.venue.lat`/`.lon` (festival origin).** `fp_pack.h` already
  documents these as *nullable* — "the schema allows either to be null
  ('unknown venue')" — with `origin_known` as the dedicated flag for
  exactly that case (see AC2's "null-handling" and the null-venue test).
  Nothing in this spec marks the origin required. A wrong-typed lat/lon is
  therefore folded into that SAME existing honest-unknown path: parsing
  continues, `origin_known` stays `false`, `origin` stays `{0,0}`, and the
  rest of the pack (stages/schedule/etc., which do not depend on the
  venue) still parses normally. This differs from `fp_parse_polygon()`
  (below) precisely because origin already has a per-field "unknown" slot
  built for this; a polygon point does not.
- **Landmark `lat`/`lon` (optional, `fp_landmark_t.has_pos`).** Same
  treatment: a wrong-typed value is indistinguishable from absent/null —
  `has_pos` stays `false`, the landmark's other fields (`id`/`name`)
  still parse.
- **`utc_offset_min` (optional v1 extension field).** Same treatment: a
  wrong-typed value at either the top-level or nested-under-`festival`
  location is treated as not-present-there and falls through (to the
  other location, then to the documented -240 default), leaving
  `utc_offset_assumed == true`. It must never read as `false` ("explicit")
  while actually holding a default — see `ff_shell.c`'s S18 wall-clock-
  trust consumer of this flag.
- **Polygon points (`fp_parse_polygon`), unchanged policy, tightened
  enforcement.** A polygon point has no per-point "unknown" representation
  to fall back to — `fp_feature_t.pts_en[]` is committed geometry, not a
  nullable slot — so a malformed point (wrong shape, or now also a
  non-numeric primitive such as a bare `true`/`false` in tuple position)
  fails the whole pack with `FP_ERR_JSON`, exactly as the existing
  object-format-point rejection already did.

**Mechanism:** `fp_num()` gained a strict sibling, `fp_num_checked()`,
which returns success/failure instead of silently substituting a default
(JSMN type must be `JSMN_PRIMITIVE`, not `null`/`true`/`false`, and
`strtod()` must consume the token's entire text). Any call site that
gates a "known"/"verified"/"assumed" flag on a value's *correctness* — not
just the key's presence — must use `fp_num_checked()`, never `fp_num()`;
`fp_num()` remains the lenient wrapper for plain data fields with no
downstream honesty flag (e.g. `festival.year`, `schedule[].starred`),
where silently defaulting on a type mismatch is the same tolerant posture
already taken for unknown keys.
