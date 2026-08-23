# Fixture JSON schema

Fixtures are standalone JSON snapshots of `ff_app_state_t`
(`firmware/app/include/ff_app_state.h`), loaded by
`firmware/targets/sim/fixture.c` (`ff_fixture_load_file` /
`ff_fixture_load_json`) via `ffsim --fixture FILE.json`. They are **not**
live wiring output — they're hand-authored dev/test data used to drive the
headless per-fixture renderer (`ffsim --headless --screenshot DIR --fixture
FILE.json` → `DIR/<stem>.png`) and golden-screenshot tests
(`tests/run_goldens.sh`), independent of a live meshtasticd connection or a
real festpack.

Every top-level key and every field within every section is **optional**.
Anything omitted is left at its zero value in the loaded `ff_app_state_t`
(`false` / `0` / `""` / the first-listed enum member where noted) — the
loader is tolerant of missing/unknown keys, matching
`firmware/festpack/src/fp_pack.c`'s "schema will grow" philosophy. Unknown
keys anywhere are silently skipped, not errors.

**Array caps are enforced fail-loud, not by silent truncation.** A section
array (`radar.dots`, `now.rows`, `signals.items`) that exceeds its
documented cap makes the whole load fail with `FF_FIXTURE_ERR_TOO_BIG` —
`*out` comes back fully zeroed, same as any other load failure — rather
than quietly keeping only the first N entries. (Ruled on during PR #12
review: consistent with `fp_parse`'s `FP_ERR_TOO_BIG` and this repo's
honest-data culture — a fixture that grows past a cap should get a loud,
attributable failure, not a dropped entry that only shows up later as an
unrelated-looking golden diff.)

## Top level

```json
{
  "fixture": "radar_live",
  "face": "radar",
  "radar": { ... },
  "now": { ... },
  "signals": { ... },
  "flare": { ... },
  "settings": { ... }
}
```

| Key | Type | Default | Notes |
|---|---|---|---|
| `fixture` | string | `""` | Debug-only provenance name. The S13 placeholder debug face renders this verbatim as its title — real S06+ screens ignore it. Conventionally matches the filename stem. |
| `face` | string enum: `radar` \| `now` \| `signals` \| `settings` | `radar` | Which `ff_app_state_t.active_face` this snapshot represents; selects which section the S13 placeholder debug face's body renders. |

## `radar` (mirrors `ff_radar_view_t`, `core/include/ff_radar.h`)

```json
"radar": {
  "mode": "live",
  "arrow_deg": 42.0,
  "arrow_valid": true,
  "name": "DANA",
  "dist_str": "320 m",
  "age_str": "8 SEC",
  "trend": 0,
  "clock_str": "9:41",
  "batt_pct": 78,
  "mesh_ok": true,
  "dots": [
    {"ring_deg": 42.0, "initial": "D", "color_idx": 0, "stale": false}
  ]
}
```

| Key | Type | Default |
|---|---|---|
| `mode` | string enum: `live`\|`stale`\|`lost`\|`close`\|`nofix`\|`nosel` | `nosel` |
| `arrow_deg` | number | `0` |
| `arrow_valid` | bool | `false` |
| `name` | string (≤15 chars, `FF_APP_NAME_LEN`) | `""` |
| `dist_str` | string (≤11 chars) | `""` |
| `age_str` | string (≤11 chars) | `""` |
| `trend` | integer, -1/0/+1 | `0` |
| `clock_str` | string (≤5 chars) | `""` |
| `batt_pct` | integer | `0` (note: `-1` is the documented "unknown" sentinel elsewhere in this codebase — pass it explicitly if that's what a fixture needs) |
| `mesh_ok` | bool | `false` |
| `dots` | array of `{ring_deg, initial, color_idx, stale}`, up to `FF_CREW_MAX` (8) — more than 8 fails the whole load (`FF_FIXTURE_ERR_TOO_BIG`) | `[]` |

Field names/semantics are transcribed 1:1 from `docs/specs/S06-radar-face.md`'s
`ff_radar_view_t` — as of S06, `ff_app_state_t.radar` *is* the real
`core/include/ff_radar.h` `ff_radar_view_t` (DRIFT GUARD resolution; see
`ff_app_state.h`'s header comment). `dots[]`'s cap is `FF_CREW_MAX` (8, from
`core/include/ff_crew.h`), not an app-local constant.

## `now` (flattened `ff_now_row_t`/`ff_next_t`, S07)

```json
"now": {
  "rows": [
    {"artist": "GRiZ", "stage_name": "Bass Camp", "stage_color_rgb": "#ffc66b",
     "mins_left": 12, "pct_done": 60}
  ],
  "next": {"artist": "Subtronics", "stage_name": "Grand Illusion", "mins_until": 45},
  "tbd": false
}
```

`rows` holds up to `FF_APP_NOW_MAX_ROWS` (3) entries (a 4th fails the whole
load with `FF_FIXTURE_ERR_TOO_BIG`); `stage_color_rgb` accepts a
`"#rrggbb"` string (leading `#` optional) or a bare integer — both forms
are exercised in `test_fixture.c` (`now_stage_color_rgb_hex_string_parses`,
`now_stage_color_rgb_numeric_form_parses`). `next` is omitted entirely
(not `null`) when there's no upcoming starred set — presence of the `next`
object sets `ff_app_next_t.valid = true`.

## `signals` (flattened `ff_feed_item_t`, S08)

```json
"signals": {
  "items": [
    {"kind": "pulse", "from_name": "RILEY", "text": "omw", "age_str": "2 MIN", "unread": true}
  ],
  "unread_count": 1
}
```

`kind` is one of `pulse`\|`text`\|`rally`\|`status`\|`flare`. `items` holds
up to `FF_APP_SIGNALS_MAX_ITEMS` (8), newest first — a 9th fails the whole
load with `FF_FIXTURE_ERR_TOO_BIG`.

## `flare` (S10)

```json
"flare": {"state": "received", "from_name": "MAX", "expires_in_ms": 4200}
```

`state` is one of `idle`\|`sending`\|`received`\|`locked`. `expires_in_ms`
defaults to `-1` ("n/a") both when the `flare` section is entirely absent
and when the key itself is omitted or `null` within it.

## `settings` (S11)

```json
"settings": {
  "imperial": true, "share_mode": "live", "haptics": true, "night_glow": true,
  "water_min": 90, "quiet_from_min": 240, "quiet_to_min": 600, "my_name": "DANA"
}
```

`share_mode` is `live`\|`zones`\|`ghost` (maps to `FF_SHARE_LIVE`/`_ZONES`/`_GHOST`).

## Current fixtures

Three radar fixtures exist as of S13/S14 slice b — one per S06 radar mode
called out in the spec's acceptance criteria (AC4): `radar_live.json`,
`radar_stale.json`, `radar_close.json`. All three follow one synthetic
scenario (crew member "DANA", tracked over a few minutes) so the three
goldens read as a coherent before/after sequence:

| Fixture | mode | dist_str | age_str | notes |
|---|---|---|---|---|
| `radar_live.json` | `live` | `320 m` | `8 SEC` | fresh fix, arrow valid |
| `radar_stale.json` | `stale` | `320 m` (last known) | `4 MIN` | no new fix since; distance is honestly stale, not re-measured (CLAUDE.md: "never fake freshness, positions, or times") |
| `radar_close.json` | `close` | `15 m` | `3 SEC` | close-range predicate tripped; `arrow_valid: false` per S06 ("false in CLOSE/NOFIX/NOSEL") |

**Provenance note:** the specific numbers (DANA, 320 m, 8 s / 4 min / 15 m)
came from the task brief that commissioned this slice, not from a value
transcribed directly out of `docs/specs/S06-radar-face.md`'s own text —
per `CLAUDE.md` ("Design references: screen mockups and plan live as
Claude artifacts (ask Jake)"), the actual mockup artboards this repo's
specs describe aren't checked into the repo, so there's no in-tree source
to transcribe pixel/value tables from directly. These three fixtures are
therefore a good-faith reconstruction consistent with the spec's *prose*
(the three named modes, S06 AC4's exact three fixture names, S02's
close-range/freshness thresholds), not a byte-for-byte transcription of a
mockup this agent could not access. Flagged per AGENTS.md's "if blocked by
a spec gap… note the interpretation" — noted here and in the PR body.
`radar_nofix.json`/`radar_nosel.json` (also named in S06 AC4/AC1) are not
included in this slice; S06 itself (Wave 3) is expected to add the
remaining mode fixtures alongside its real `ff_radar_compute()` /
`scr_radar.c` — the loader and schema above already support them (just
`"mode": "nofix"` / `"nosel"`, `"arrow_valid": false`).

## Adding a fixture

1. Write `tests/fixtures/<name>.json` per the schema above.
2. `./build/ffsim --headless --screenshot /tmp --fixture tests/fixtures/<name>.json` to sanity-check it renders.
3. `tests/run_goldens.sh --update-golden` to generate `tests/golden/<name>.png`, then review the PNG diff before committing.
