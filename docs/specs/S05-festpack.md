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

fp_result_t fp_parse(char const *json, size_t len, fp_pack_t *out,
                      jsmntok_t *toks, int ntoks); // OK / ERR_JSON / ERR_VERSION / ERR_TOO_BIG
```

## Behavior
- Parser: vendored **jsmn** (tokenizer, zero-alloc) + hand-rolled extraction. No dynamic allocation.
- `festpack` version must be `"0.1"` (`ERR_VERSION` otherwise).
- Null times → −1; null polygons → n_pts 0; unknown keys skipped. Overflowing any MAX ⇒ `ERR_TOO_BIG` (fail loudly; caps are generous for real festivals).
- Timezone: schema carries IANA name; device stores a UTC offset chosen at pack-load (sim/tools resolve it; device gets it via pack meta or setting — v1 uses `utc_offset_min` extension field, PR to fest-almanac schema noted).
- Lat/lon features are projected to east/north meters at parse time using `ff_geo_project` (map face consumes meters only).

## Acceptance criteria
1. Parses `packs/lost-lands/2026/festpack.json` (vendored as fixture): 7 stages, ≥27 sets, all times −1, names/colors exact. *(Superseded 2026-09-09 — the real pack now carries 222 sets with published start times; see this file's dated amendment below. The parse/stage/name/colour half of the criterion still holds.)*
2. Null-handling: fixture with nulls in every nullable slot parses; absent optional sections parse.
3. Wrong version, truncated JSON, non-JSON ⇒ correct errors, no crash (fuzz smoke 10k iters).
4. Overflow fixtures (13 stages, 257 sets) ⇒ ERR_TOO_BIG.
5. Feature polygons project: fixture with known lat/lon square → east/north within 1 m.
6. Struct fits in ≤48 KB (static assert) — must live in ESP32-S3 PSRAM comfortably.

## Slices
a) tokenizer+festival/stages/schedule · b) map/landmarks+projection · c) fuzz+fixtures.

## Amendments

- **2026-09-01, S26 slice a (PR #134) — caller-supplied jsmn token scratch.**
  `fp_parse()` gained two parameters, `jsmntok_t *toks, int ntoks`: the caller
  now owns the token buffer `fp_parse()` tokenizes into. It used to be a
  static 131,072-byte array living forever in `fp_pack.c`'s `.bss`; on the
  ESP32-S3 target that internal-RAM cost is reclaimed by moving it to a
  caller-owned buffer (stack, heap, or PSRAM) that can be sized, placed, or
  freed by the caller instead. `FP_MAX_TOKENS` (8192) in `fp_pack.h` is the
  recommended `ntoks` for callers with no reason to size differently —
  `FP_MAX_TOKENS * sizeof(jsmntok_t)`. A caller may legally pass a smaller
  buffer: `fp_parse()` returns `FP_ERR_TOO_BIG` rather than overrunning it,
  exactly the same error an oversized festpack (too many stages/sets/etc.)
  already produced — never a crash. This also makes `fp_parse()` reentrant,
  which the old static-arena version was not. Parse behavior and output are
  otherwise byte-identical (see `firmware/festpack/include/fp_pack.h`).

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

- **2026-09-09, real Lost Lands 2026 set times — after-midnight sets, day/time model.**

  fest-almanac refreshed the Lost Lands 2026 pack with the published
  set-time grid: 222 sets, `meta.complete.set_times` now `"full"` (it was
  `"none"`). `firmware/assets/field/lost-lands-2026.festpack.json` is a
  byte-identical `cp` of that pack, per
  `firmware/assets/field/README.md`'s standing "never hand-edit" rule.

  Real data answered a question `ff_sched.c` had flagged but left open
  (the now-resolved "KNOWN GAP" comment ahead of
  `sched_next_stage_start`, PR #65 review finding 3): **how is a set that
  runs after actual local midnight represented**, given `fp_set_t.day_doy`
  names one calendar date and `start_min`/`end_min` are minutes from that
  date's midnight? Lost Lands has 55 such sets — every stage's Friday and
  Saturday nights run a 00:15/01:10/02:05/03:00 tail past midnight, and
  those are billed on the grid under the night they belong to, not under
  the morning they technically occur on.

  **The pack encodes plain calendar facts.** No inflated hours, no
  invented dates:

  | field | meaning |
  |---|---|
  | `day` | ISO date the set STARTS on |
  | `start` | `"HH:MM"`, `HH` in **00..23**, local time on `day` |
  | `end` | `"HH:MM"`, `HH` in 00..23, or `null` (unknown — 221 of 222 are null; `ff_sched.c` derives the end from the next set on the stage) |
  | `night` | *optional* ISO date: the festival night the set is billed under. `== day` for an ordinary set, `== day - 1 day` for an after-midnight one. 55 of 222 differ from `day`. |
  | `end_day` | *optional* ISO date of `end`, present only when `end` falls on a later calendar date than `start`. Exactly one set has it: Excision, 2026-09-18 22:10 → 00:10 on 2026-09-19. |

  **The parser folds that into one night.** `fp_pack.c`'s
  `fp_parse_set_daytime()` stores a single `day_doy` — the *night* — with
  `start_min`/`end_min` measured from **that night's** local midnight. So
  an after-midnight set lands at `>= 1440` (00:15 becomes 1455) in the
  same number space as the same night's evening sets, and Excision's end
  becomes 1450 rather than an ambiguous 10.

  That target space is not a new invention: `ff_sched.h`'s festival-day
  contract is `[360, 1800)` rolling at 06:00, and
  `firmware/core/include/ff_wall.h`'s `ff_wall_split_local()` already
  resolves a real wall-clock reading of "01:00 local" to the *previous*
  calendar day's `day_doy` at `now_min == 1500` (1440 + 60) — see that
  header's "Composition with ff_sched / ff_settings" section. Folding the
  pack into the same space is what lets `ff_sched.c` compare `start_min`
  against `now_min`, and against other sets' `start_min`, as plain
  integers: no midnight special-casing, no runtime changes, and
  `sched_next_stage_start` orders 00:15 (1455) after 23:00 (1380) on the
  same night for free.

  **The rejected alternative** was filing a post-midnight set under the
  NEXT calendar day's `day_doy` at a small raw minute value.
  `ff_sched_now_playing`/`day_sets`/`day_tbd` filter by exact `day_doy`
  equality, so such a set would sit under a `day_doy` the wall-clock
  resolver never produces for that moment — silently absent from "now"
  and from the previous night's lineup, however live it actually was.

  (An earlier draft of this amendment instead widened
  `fp_min_from_hhmm`'s `HH` cap to 29 so a pack could literally write
  `"start": "24:15"`. That is **withdrawn**: it made the pack file carry
  a firmware-internal representation, put a non-ISO-8601 time string in a
  data interchange format, and could not have expressed Excision's end
  without also lying about its `day`. `HH` is 00..23, full stop; the fold
  belongs at parse time.)

  **Fallback when `night` is absent.** Older packs (and any hand-written
  one) may omit the field. `fp_parse_set_daytime()` then applies a
  documented best guess: **a set starting before 06:00 local belongs to
  the previous calendar day's night**; anything at or after 06:00 stays
  on its own `day`. 06:00 is `FP_NIGHT_FOLD_MIN`, mirroring
  `FF_SCHED_FESTIVAL_DAY_START_MIN` (festpack/ cannot include
  `ff_sched.h` — the dependency runs the other way — so it is a mirrored
  constant, the same arrangement `ff_wall.h` documents for
  `FF_WALL_DAY_START_MIN`). This is a guess, not a substitute for
  `night`: it cannot know that a pack deliberately bills a 02:00 set
  under its own day. An explicit `night` always wins. A `night` more than
  one day behind `day`, or ahead of it, is data corruption rather than a
  fold to guess at — the set groups under the authored night but its
  clock times are left unshifted.

  **Lint.** `tools/festpack_lint.py` (wired into ctest, so a bad pack
  fails the build) enforces what the tolerant parser deliberately does
  not: `HH` in 00..23; `night` is `day` or `day - 1`; `end_day` is
  exactly `day + 1` and only present when `end` really does cross
  midnight; `start < end` in the folded night space; no same-stage,
  same-night overlaps; stage references resolve; `day` inside the wall
  window; `note`/`artist` within their `fp_set_t` byte budgets; total
  `<= FP_MAX_SETS`. A bare `end` at or before `start` with no `end_day`
  stays legal — that is the older spelling `sched_effective_end` already
  folds (`ff_sched.h`, "Midnight-crossing sets"), and the demo pack uses
  it.

  **Regression coverage.** `test_festpack.c` asserts the fold on the real
  fixture (Sippy 00:15 → 1455 under Friday's 261, never Saturday's 262),
  the `end_day`-dated Excision end, the 55-set count, the absent-`night`
  fallback (including the Jan-1 wrap into the previous year's last
  day-of-year), and that an explicit `night` overrides the fallback in
  both directions. `test_field_pack.c` asserts the same against the
  actual embedded asset plus its 222/55 shape. `test_sched.c` runs
  `ff_sched_now_playing` at a fixed fake clock either side of midnight —
  Fri 2026-09-18 23:30 (`day_doy` Friday, `now_min` 1410; Excision live
  with an honest 40 minutes left) and Sat 2026-09-19 00:30 (`day_doy`
  still Friday, `now_min` 1470; Excision correctly over, Sippy live and
  sorting after the pre-midnight set it followed) — plus the "tonight"
  grouping case proving Friday's night is all 64 of its sets, in
  ascending start order per stage, with none of Saturday's daytime
  lineup leaking in.
