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

**But a PRESENT enum key must carry one of its documented strings**
(issue #28, orchestrator ruling). `face`, `radar.mode`, `now.state`,
`signals.items[].kind`, `compose.mode`, and `settings.share_mode` fail
the whole load with `FF_FIXTURE_ERR_BAD_ENUM` — `*out` fully zeroed, and
a stderr line naming the bad key and value — when the key is present but
its value isn't one of that key's documented strings (or isn't a JSON
string at all). No silent defaults: fixtures are the inputs to the
golden suite, so a typo'd enum (`"mixxed"`, `"no-pack"` for `no_pack`)
used to silently render a *different* state and then commit it as the
golden — a test green forever about the wrong screen. **Absent ≠
malformed:** an *omitted* enum key still takes its documented default,
exactly like every other omitted field; in particular an absent `face`
defaults to `radar` (PR #36's deliberate exception — a hand-written or
truncated snapshot gets the home face, not a blank screen). See
`test_fixture.c`'s `every_enum_key_fails_loud_on_unrecognized_string` /
`face_absent_still_defaults_to_radar`.

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
| `face` | string enum: `radar` \| `now` \| `signals` \| `settings` \| `compose` | `radar` (when the key is **absent** — an unrecognized string fails the load, see the fail-loud note above) | Which `ff_app_state_t.active_face` this snapshot represents; selects which section the S13 placeholder debug face's body renders. |
| `ui_settings_scroll_y` | integer | `0` | **Sim/golden render hint only (#bug5a), same category as `fixture`.** Scrolls the Settings list to this vertical offset (device points, clamped by LVGL to the scrollable range) before the screenshot, so a golden can capture a non-zero scroll position. Applied only to the `settings` face; `0` (the default, and the only value the live shell carries) is a no-op. See `settings_scrolled_bottom.json` / `settings_scrolled_mid.json`. |

## `radar` (mirrors `ff_radar_view_t`, `core/include/ff_radar.h`)

```json
"radar": {
  "mode": "live",
  "arrow_deg": 42.0,
  "arrow_valid": true,
  "name": "DANA",
  "dist_str": "320 m",
  "dist_imprecise": false,
  "age_str": "8 SEC",
  "trend": 0,
  "clock_str": "9:41",
  "batt_pct": 78,
  "mesh_ok": true,
  "dots": [
    {"ring_deg": 42.0, "initial": "D", "color_idx": 0, "stale": false, "place": false, "imprecise": false}
  ]
}
```

| Key | Type | Default |
|---|---|---|
| `mode` | string enum: `live`\|`stale`\|`lost`\|`place`\|`close`\|`nofix`\|`nosel` | `nosel` |
| `arrow_deg` | number | `0` |
| `arrow_valid` | bool | `false` |
| `name` | string (≤15 chars, `FF_APP_NAME_LEN`) | `""` |
| `dist_str` | string (≤11 chars) | `""` |
| `dist_imprecise` | bool | `false` | issue #47 — true when `dist_str` is a precision-degraded area estimate (e.g. `"~5.8 km"`), not a point-to-point distance. See `radar_imprecise.json` below. |
| `age_str` | string (≤11 chars) | `""` |
| `trend` | integer, -1/0/+1 | `0` |
| `clock_str` | string (≤5 chars) | `""` |
| `batt_pct` | integer | `0` (note: `-1` is the documented "unknown" sentinel elsewhere in this codebase — pass it explicitly if that's what a fixture needs) |
| `mesh_ok` | bool | `false` |
| `dots` | array of `{ring_deg, initial, color_idx, stale, place, imprecise}`, up to `FF_CREW_MAX` (8) — more than 8 fails the whole load (`FF_FIXTURE_ERR_TOO_BIG`) | `[]` |

`mode: "place"` is issue #33's addition (2026-08-24): the selected member's
latest position is an ASSERTION (Meshtastic `LOC_MANUAL`), not a
measurement — rendered as a landmark, never as LIVE/STALE/LOST no matter
its age. `age_str` is always `""` for this mode (see `ff_radar.h`'s
`RADAR_PLACE` doc comment for why an age can never be honestly shown for
an asserted position). A dot's `place: true` marks the same fact on the
crew ring — mutually exclusive with `stale`.

A dot's `imprecise: true` (issue #74, S17 slice a) marks that member's
latest fix as known-degraded precision — same gate as `dist_imprecise`
above, applied per-dot instead of only to the selection. Rendered with
the Map face's own fuzzy-ring idiom (hollow, no initial letter), never a
crisp point. See `radar_dot_precise.json`/`radar_dot_imprecise.json`
below for the golden pair.

Field names/semantics are transcribed 1:1 from `docs/specs/S06-radar-face.md`'s
`ff_radar_view_t` — as of S06, `ff_app_state_t.radar` *is* the real
`core/include/ff_radar.h` `ff_radar_view_t` (DRIFT GUARD resolution; see
`ff_app_state.h`'s header comment). `dots[]`'s cap is `FF_CREW_MAX` (8, from
`core/include/ff_crew.h`), not an app-local constant.

## `now` (flattened `ff_now_row_t`/`ff_next_t`, S07)

```json
"now": {
  "state": "live",
  "rows": [
    {"artist": "GRiZ", "stage_name": "Prehistoric Stage", "stage_color_rgb": "#ffc66b",
     "pct_done": 60}
  ],
  "next": {"artist": "Excision", "stage_name": "Prehistoric Stage", "mins_until": 45},
  "lineup": [
    {"artist": "NGHTMRE", "stage_name": ""}
  ]
}
```

| Key | Type | Default | Notes |
|---|---|---|---|
| `state` | string enum: `no_pack`\|`tbd`\|`mixed`\|`live`\|`nothing_playing`\|`time_unknown` | `no_pack` | **[api], PR #21 code review finding #2/ruling; `time_unknown` added by issue #48.** Replaces an earlier `pack_loaded`+`tbd` bool pair — see `now_state_t`'s doc comment in `ff_app_state.h` for why an explicit, mutually-exclusive-by-construction enum (same convention as `radar.mode`) replaced two independent bools. An absent key defaults to `no_pack`, the least-claiming state — same convention as `radar.mode`'s `nosel` default. An unrecognized string fails the load (`FF_FIXTURE_ERR_BAD_ENUM`, issue #28 — it used to silently take the same default). `time_unknown` (issue #48): a pack IS loaded but the clock is not — distinct from `no_pack` on purpose, see that member's own doc comment. |
| `rows` | array, up to `FF_APP_NOW_MAX_ROWS` (3) | `[]` | Populated for `state: "live"` and `state: "mixed"` (the day's known-time sets). A 4th entry fails the whole load with `FF_FIXTURE_ERR_TOO_BIG`. `stage_color_rgb` accepts a `"#rrggbb"` string (leading `#` optional) or a bare integer — both forms are exercised in `test_fixture.c` (`now_stage_color_rgb_hex_string_parses`, `now_stage_color_rgb_numeric_form_parses`). **`mins_left` was removed** (PR #21 code review finding #5a) — it was parsed and fixture-populated but never rendered (the row's progress bar is driven by `pct_done` alone; the spec's row requirement is a bar, not per-row minutes text). |
| `next` | object or omitted | omitted (`ff_app_next_t.valid = false`) | Populated for `state: "live"` and `state: "mixed"`. Omit entirely (not `null`) when there's no upcoming starred set — presence of the `next` object sets `valid = true`. Renders differently by state: `state: "live"` gives it the full next-card treatment (label/artist/stage/36px countdown); `state: "mixed"` (UX review round 2 finding #2) gives it a more compact but still countdown-LED block (22px amber countdown first, artist/stage below) — same field, deliberately still the visually prominent element in its own block, just scaled to share the screen with the mixed state's other sections. |
| `lineup` | array, up to `FF_APP_NOW_MAX_LINEUP` (32) | `[]` | The day's sets whose time is **not** known. For `state: "tbd"`, this is every set on the day (all of them lack a time, by definition). For `state: "mixed"` (PR #21 code review finding #1/ruling), this is just the still-unknown SUBSET — the known-time sets render via `rows`/`next` at the same time, so an unknown-time set is never silently dropped just because some other set on the same day got a real time. `stage_name: ""` means the set's stage is genuinely unknown in the source pack (`fp_set_t.stage_idx == -1`); rendered honestly (`"STAGE UNKNOWN"`), never guessed. A 33rd entry fails the whole load with `FF_FIXTURE_ERR_TOO_BIG`. |

`ff_app_now_row_t.stage_color_valid` (PR #21 code review finding #3, not
a fixture-authorable key — it's derived from whether `stage_color_rgb`
parsed successfully): a malformed `"#rrggbb"` string, or the key being
absent entirely, marks the row's color invalid rather than silently
defaulting to `0x000000` — the old behavior conflated "no color given" /
"malformed color" / "a genuinely black stage" into the same bit pattern.
See `test_fixture.c`'s `now_stage_color_rgb_valid_black_is_marked_valid`
/ `_malformed_hex_marks_invalid` / `_absent_key_marks_invalid`.

**Banner wording is state-aware** (UX review round 2 finding #3): `state:
"tbd"` shows "SET TIMES TBD"; `state: "mixed"` shows "SOME SET TIMES
TBD" instead — the unqualified wording sitting directly above a section
proving some times AREN'T unknown read as a self-contradiction at a
glance. Not a fixture field — `scr_now.c` picks the text from `state`
itself.

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

## `flare` (S10 slice b)

`ff_app_flare_t` mirrors `core/include/ff_flare.h`'s `ff_flare_t` shape:
THREE independent groups (outbound send, pending takeover, navigation
lock) that can each be true or false at once — see that header's
"Independent state" doc comment and `docs/specs/S10-flare.md`'s Amendments
(2026-08-23, PR #15) for why a single `state` enum (this section's
pre-slice-b shape) doesn't honestly represent this data. All three groups
can be present in the same fixture (I can be sending my own flare, have a
*different* crew member's takeover pending, and still be locked onto a
*third* member's earlier flare, simultaneously):

```json
"flare": {
  "sending": true, "send_expires_in_ms": 120000,
  "takeover_active": true, "takeover_from_name": "KEV",
  "takeover_bearing_valid": true, "takeover_bearing_deg": 90.0,
  "takeover_dist_str": "40 m",
  "takeover_expires_in_ms": 4200,
  "locked": true, "locked_from_name": "DANA", "locked_expires_in_ms": 9000
}
```

| Key | Type | Default |
|---|---|---|
| `sending` | bool | `false` |
| `send_expires_in_ms` | integer | `-1` ("n/a") |
| `takeover_active` | bool | `false` |
| `takeover_from_name` | string (≤15 chars) | `""` |
| `takeover_bearing_valid` | bool | `false` ("unknown" — see below) |
| `takeover_bearing_deg` | number, `[0, 360)` | `0`; meaningful only if `takeover_bearing_valid` |
| `takeover_dist_str` | string (≤11 chars) | `""` |
| `takeover_expires_in_ms` | integer | `-1` ("n/a") |
| `locked` | bool | `false` |
| `locked_from_name` | string (≤15 chars) | `""` |
| `locked_expires_in_ms` | integer | `-1` ("n/a") |

Each group's own `*_expires_in_ms` defaults to `-1` independently — both
when the whole `flare` section is absent, and when that one key is
omitted/`null` while the other two groups are present (see
`test_fixture.c`'s `flare_omitted_group_defaults_independently`).
`takeover_bearing_deg`/`takeover_dist_str` have no equivalent in core's
`ff_flare_t` (that module deliberately has zero `ff_crew`/`ff_radar`
dependency) — they're the app layer's own already-computed
bearing/distance-to-sender read for the takeover screen, same convention
as `radar.dist_str` above. Similarly, `takeover_from_name`/
`locked_from_name` carry only a display name, never the underlying mesh
node id core's `ff_flare_t` actually keys on (`takeover_node_id`/
`locked_node_id`) — two crew members sharing a display name are
indistinguishable in this flattened snapshot, which is fine for what it's
for (pixels). Until S16 slice c2, the one place a real lock DECISION gets
made (`ff_scr_flare_selection_locked`) consulted the live `ff_flare_t` via
`ff_flare_locked_node()` directly rather than this struct; that `[api]`
change moved it onto `flare.locked` (the shell computes the same fact once,
in `shell_project_flare`), so this struct is now the only thing every
screen-layer read of the lock — display or decision — consults.

`takeover_bearing_valid` (PR #20 code review, LOW finding) exists because
a bearing genuinely can be unknown (no position fix on either end) and
`takeover_bearing_deg` alone has no way to say so — `0.0` is
indistinguishable from "really due north." Defaults to `false`
independently of whether `takeover_bearing_deg` itself is present (see
`test_fixture.c`'s `flare_takeover_bearing_valid_defaults_false` —
providing the degree value without the flag still renders "bearing
unknown", never a fabricated compass point), same "prove you meant this"
convention `radar.arrow_valid` already uses.

## `compose` (S08, dump added S16 slice d)

```json
"compose": {
  "text": "omw!", "to_name": "DANA", "has_pending": false, "mode": "abc"
}
```

| Key | Type | Default |
|---|---|---|
| `text` | string (≤160 chars) | `""` |
| `to_name` | string (≤15 chars) | `""` ("" = broadcast, "TO: EVERYONE") |
| `has_pending` | bool | `false` |
| `mode` | string enum: `abc` \| `123` \| `sym` \| `pred` | `abc` |

### Predictive-T9 fields (`mode: "pred"`, S08 addendum)

Meaningful only in `pred` mode; the shell leaves them zeroed in the other
modes, so an `abc`/`123`/`sym` fixture simply omits them. This is
hand-authored golden data — on the live path the shell fills these from the
`core/ff_t9pred` engine (`from_pack` by pointer identity against the
festpack word table, `total_cand` the real engine count), never a
fabricated value. The screen renders exactly what is here: it never shows a
word the engine did not return.

```json
"compose": {
  "mode": "pred", "text": "omw to ", "word": "the",
  "cand": [ {"text": "the"}, {"text": "tie"}, {"text": "vie"} ],
  "sel": 0, "total_cand": 8
}
```

| Key | Type | Default |
|---|---|---|
| `word` | string (≤31 chars) | `""` — the in-progress predicted word (amber, underlined, with a caret). `""` = nothing predicted yet, or an honest no-match (see `word_nomatch`) |
| `word_nomatch` | bool | `false` — digits typed but the engine honestly returned NO word. The draft shows committed text + a neutral caret and NO amber word; the strip shows a dim "no match" affordance, never a fabricated chip |
| `cand` | array of `{ "text": string, "from_pack": bool }`, cap `FF_APP_COMPOSE_MAX_CAND` (6), fail-loud on over-cap | `[]`. Best-first candidate chips. `from_pack` (default `false`) badges festpack vocabulary with a ★ |
| `sel` / `sel_cand` | int | `0` — selection index among ALL candidates. Highlights `cand[sel]` amber-filled only when `sel < n_cand`; otherwise no chip is highlighted and `word` is the authoritative selection |
| `total_cand` | int | `0` — the real total match count. When it exceeds the shown chips, a trailing `›` cycle chip appears |

The LOADER has accepted this section since S08 (`fx_parse_compose`), but
`ff_fixture_dump_json` never wrote it back out until S16 slice d — a real
gap, not a deliberate omission: the ctl `state` dump could never show the
composer's own draft, which the S16 AC10 sequence test (draft typed ->
flare injected -> takeover renders -> cleared -> composer returns with
draft intact) needs to observe surviving a takeover through the socket,
not just by reading `ff_app_state_t` directly. `text` mirrors
`ff_t9_text()`: committed characters plus the live pending (uncommitted)
one, if any — the same value `scr_compose.c` renders.

## `settings` (S11)

```json
"settings": {
  "imperial": true, "share_mode": "live", "haptics": true, "night_glow": true,
  "water_min": 90, "quiet_from_min": 240, "quiet_to_min": 600, "my_name": "DANA",
  "utc_offset_set": true, "utc_offset_min": -420, "colorblind": false
}
```

`share_mode` is `live`\|`zones`\|`ghost` (maps to `FF_SHARE_LIVE`/`_ZONES`/`_GHOST`).

`utc_offset_set`/`utc_offset_min` (S11 slice b addition, mirrors
`ff_settings_t`'s S16-b0 amendment): `utc_offset_min` is MEANINGLESS unless
`utc_offset_set` is true — read `utc_offset_set` first, same "prove you
meant this" convention as `flare.takeover_bearing_valid`. Default (both
keys absent) is `utc_offset_set: false`, which `scr_settings.c` renders as
an honest "UNSET", not a fabricated "+0:00".

`colorblind` (S17 slice a addition, mirrors `ff_settings_t.colorblind`):
default `false` (the brand crew palette). This key is read regardless of
`face` — a `radar`/`map`-face fixture can set it too, since every screen
that draws a crew dot reads it (see `radar_crew8_colorblind.json` below).

## `map` (S09)

```json
"map": {
  "features": [
    {"kind": "stage", "label": "Prehistoric Stage", "color_rgb": "#ffc66b",
     "points": [[-100.0, -50.0], [100.0, -50.0], [100.0, 50.0], [-100.0, 50.0]]}
  ],
  "crew": [
    {"initial": "D", "color_idx": 0, "east_m": 20.0, "north_m": 35.0,
     "stale": false, "place": false, "imprecise": false}
  ],
  "rally": {"label": "MEETUP", "east_m": 0.0, "north_m": 0.0},
  "you": {"has_pos": true, "east_m": 0.0, "north_m": 0.0, "heading_valid": true, "heading_deg": 42.0}
}
```

Mirrors `ff_app_map_t` (`ff_app_state.h`) field-for-field — see that
struct's own doc comments for the full semantics. Same "fixture.c has
zero festpack dependency" convention as `now` above: `features[]` is a
flat `{kind, label, color_rgb, points}` list, never a live `fp_pack_t`.

| Key | Type | Default | Notes |
|---|---|---|---|
| `features` | array, up to `FF_APP_MAP_MAX_FEATURES` (8) | `[]` | `kind` is one of `unknown`\|`stage`\|`camping`\|`water`\|`path`\|`entrance`\|`vendor`\|`medical`\|`poi`. `points` is `[[east_m, north_m], ...]`, up to `FF_APP_MAP_MAX_POLY_PTS` (12) — a 13th point, or a malformed `[e, n]` pair, fails the whole load (`FF_FIXTURE_ERR_TOO_BIG`/`FF_FIXTURE_ERR_JSON`). `n_pts` drives the render policy (`scr_map.c`'s doc comment: `>= 3` a filled+stroked polygon, `2` a stroked line, `1` a stage's labeled 30 m stub circle or a label-only point for any other kind, `0` omitted entirely — never an invented shape). `color_rgb` (accepts `"#rrggbb"` or a bare integer, same convention as `now.rows[].stage_color_rgb`) is OMITTED, not a placeholder, when absent/malformed — `color_valid` then reads false and `scr_map.c` falls back to the kind's own theme color. |
| `crew` | array, up to `FF_CREW_MAX` (8) | `[]` | Presence of either `east_m`/`north_m` sets `has_pos`. `stale`/`place`/`imprecise` reuse Radar's own freshness/asserted/precision vocabulary (issue #33/#47) where it maps onto a map dot — see `ff_app_map_crew_t`'s doc comment for exactly how far that reuse goes (in particular: `imprecise` gets a MAP-SPECIFIC render, a larger fuzzy ring instead of the normal 18px pin-point dot, never a crisp point claim a degraded fix can't honestly support). |
| `rally` | object or omitted | omitted (`has_rally: false`) | Same "presence of the section is the flag" idiom as `now.next`. No live source exists yet (`ff_crew.h`'s own documented rally-selection gap) — fixtures/goldens are how this section is exercised until it does. |
| `you` | object or omitted | omitted (`you_has_pos: false`, `you_heading_valid: false`) | `has_pos` defaults to `true` when the section is present, overridable explicitly. `heading_valid` defaults `false` even with the section present (S09 AC5's least-claiming default, same convention as `radar.arrow_valid`) — omitted or `heading_valid: false` renders the arrow hidden with a "NO FIX" chip. |

### Map face fixtures (S09)

Four fixtures: `map_untraced.json` (AC3 — the real Lost Lands pack's
current shape: every stage feature carries just its known point, no
traced polygon, so the render is the honest "labels-only stub" state;
one non-stage feature with zero points to exercise the "otherwise
omitted" branch too), `map_traced.json` (AC4 — five synthetic polygons
across five different kinds, three crew dots covering live/stale/
imprecise, a rally pin, and a YOU arrow at a non-cardinal heading),
`map_heading.json` (AC5's positive half — YOU present with a distinct
heading, proving the arrow actually rotates rather than only ever
rendering north-up), and `map_nofix.json` (AC5's negative half — no
`you` section at all, so the arrow is hidden and the "NO FIX" chip
renders instead). All four are synthetic (no mockup artboards in-tree
for this agent to consult — see `ff_theme.h`'s top comment); real traced
geometry for Lost Lands is being surveyed in a separate, parallel effort
(fest-almanac) and was deliberately not blocked on here.

## Current fixtures

Ten radar fixtures exist as of S06 (compute in slice a, `scr_radar.c` +
`scr_nav.c` rendering in slice b/c/d): `radar_live.json`, `radar_stale.json`,
`radar_close.json` (S13/S14 slice b, goldens regenerated in S06 PR B once
the real radar face replaced the S13 debug placeholder — see this repo's
PR history for the before/after), `radar_nofix.json`, `radar_nosel.json`,
and `radar_never.json` (added in S06 PR B's first pass), plus
`radar_lost.json` and `radar_close_collision.json` (added in PR B's UX
review follow-up) and `radar_cluster.json`/`radar_cluster_stale.json` (added for issue
#18's cluster marker restyle, the second one in that PR's UX review
round). The first four cover S06 AC4's exact named fixtures
(`radar_live`/`stale`/`close`/`nofix`); the rest are additional coverage
beyond AC4's literal list, added because they're real, distinct render
states `scr_radar.c` has to handle honestly (empty-crew, a paired member
who has never sent a fix at all, a paired member with a genuinely old fix,
and a worst-case crew-ring layout).

| Fixture | mode | dist_str | age_str | notes |
|---|---|---|---|---|
| `radar_live.json` | `live` | `320 m` | `8 SEC` | fresh fix, arrow valid |
| `radar_stale.json` | `stale` | `320 m` (last known) | `4 MIN` | no new fix since; distance is honestly stale, not re-measured (CLAUDE.md: "never fake freshness, positions, or times") |
| `radar_lost.json` | `lost` (real fix) | `1.1 km` (rendered `~1.1 km`) | `42 MIN` | genuinely old fix — PR #16 UX review's top finding: this state had no fixture/golden at all in the first pass of this PR, so nobody had ever seen it rendered. Must read as a *different screen* from STALE, not a dimmer one (see `scr_radar.c`'s `radar_render_lost`) |
| `radar_close.json` | `close` | `15 m` | `3 SEC` | close-range predicate tripped; `arrow_valid: false` per S06 ("false in CLOSE/NOFIX/NOSEL") |
| `radar_cluster.json` | `live` | `320 m` | `8 SEC` | added for issue #18 (cluster marker styling). Four crew members 4 degrees apart on the ring, so all four resolve into ONE cluster marker — one wedge each, in all four **distinct** crew colors (pink/teal/violet/green), with the violet member stale so the mixed-freshness case is visible: three 6px wedges and one 2px one. `radar_close_collision.json` below only ever produces a 2-member cluster, which can't show whether the ring generalizes past a half-and-half split. **Not** the widest ring the marker can draw at the time this fixture was added — see `radar_cluster_8_samecolor.json` below (S17 slice a) for the 8-member case, closing the gap this note used to record |
| `radar_cluster_stale.json` | `stale` | `320 m` (last known) | `4 MIN` | same four clustered members, but **every one of them stale** — added in PR #41's UX review round, which found that the first implementation expressed staleness by dimming a wedge's opacity, and dimming is *relative*: with no fresh wedge in frame for contrast, an all-stale marker rendered indistinguishably from a live one. This is the fixture that pins the fix (2px hollow wedges at full crew color, plus a `FF_THEME_COLOR_MUTED` count digit). Deliberately paired with `radar_cluster.json` — the pair only means something read side by side, since the whole finding was that one of them used to look like the other |
| `radar_close_collision.json` | `close` | `15 m` | `3 SEC` | same scenario as `radar_close.json`, but with 4 crew-ring dots deliberately placed at worst-case bearings (one straight at the status bar, three clustered straight at the FLARE button / trend chip) to exercise `app/screens/radar_layout.c`'s layout resolver — the three southward dots resolve into a 1-dot + 1-cluster-of-2 outcome (a "2" marker, not a hidden member), and the northward dot lands clear of the status bar. Regressing the resolver will show up here even if it doesn't show up in the plain `radar_close` golden — though the authoritative regression coverage is `app/screens/tests/test_radar_layout.c`'s geometry-level sweep, not this golden (see that file's header comment for why) |
| `radar_nofix.json` | `nofix` | `""` (unknown — my position invalid) | `6 MIN` (the *selected member's* last-known age is still honestly known even though mine isn't) | arrow hidden, "NO FIX - RADIO ONLY" |
| `radar_nosel.json` | `nosel` | `""` | `""` | no paired crew member at all — empty-crew state, `mesh_ok: false` for variety |
| `radar_never.json` | `lost` (folded — see below) | `""` | `""` | selected member "JAMIE" is paired but has never sent a fix; `age_str[0] == '\0'` is what `scr_radar.c` keys off to show "NO FIX YET" instead of a "LAST SEEN" chip — NOT distinguishable from a genuinely-old fix by `mode` alone (both are `RADAR_LOST`; see `radar_lost.json` above for the other side of that same `mode`) |
| `radar_place.json` | `place` | `610 m` | `""` (always empty — see `ff_radar.h`'s `RADAR_PLACE` doc comment) | issue #33 — a landmark's asserted (`LOC_MANUAL`) position, "CAMP BASE". Solid arrow (a real coordinate exists), neutral "FIXED POSITION" chip — never "LIVE", never a rim tint, never an invented age. One ring dot (`"C"`, `place: true`) also carries the honest treatment alongside two ordinary live dots |
| `radar_imprecise.json` | `live` | `~5.8 km` | `8 SEC` | issue #47 — `dist_imprecise: true`; the selected member's precision is known-degraded (13 bits, the default-public-channel case measured on hardware). Freshness/mode are untouched (LIVE, fresh fix) — only the distance is an honest area estimate, dimmed in render and suffixed "- AREA" on the chip, never a metre-looking number |

**S17 slice a additions** (issue #43's 8-colour palette, the colorblind
toggle, and issue #74's radar-imprecise dot — `docs/specs/S17-usability-hardening.md`):

| Fixture | mode | notes |
|---|---|---|
| `radar_crew8.json` | `live` | eight crew-ring dots, `color_idx` 0-7, at 30/60/90/120/240/270/300/330 degrees — NOT an even 45-degree spread: this face's own reserved chrome (status bar, name/distance/chip stack, page dots — `radar_layout_build_registry`) blocks most of the band from ~130 to ~230 degrees and a sliver near 0, so two evenly-spaced attempts silently merged a pair into a cluster after push-away even though their *ideal* positions were 45 degrees apart. These eight angles were re-picked to clear all three reserved regions and render as eight separate lone dots, not a cluster — every brand-palette color visible at once, indices 0-3 the original four, 4-7 the new ones (issue #43) |
| `radar_crew8_colorblind.json` | `live` | byte-identical `radar` section to `radar_crew8.json` above, plus a top-level `"settings": {"colorblind": true}` — the SAME eight members rendered through the Okabe-Ito-derived safe palette instead. Meant to be diffed against `radar_crew8.json` directly: any pixel difference outside the eight dots is a bug |
| `radar_cluster_8_samecolor.json` | `live` | eight members clustered into ONE marker (4 degrees apart, same spacing idiom as `radar_cluster.json`), with the first two (`color_idx: 0` on both) deliberately adjacent in wedge order — the same-color-adjacent-wedge case AC3 exists for. Proves the wedge gap (`RADAR_LAYOUT_CLUSTER_WEDGE_GAP_DEG`, pre-existing since issue #18) keeps a same-colour pair countable, not just visually-distinct pairs |
| `radar_dot_precise.json` / `radar_dot_imprecise.json` | `live` | golden pair for issue #74 — identical except dot `"R"`'s `imprecise` flag (`false`/`true`). `"D"` stays an ordinary crisp dot in both; only `"R"` gains the fuzzy-ring treatment in the second fixture, isolating exactly the one thing that's supposed to change |
| `settings_colorblind_on.json` | (Settings face) | `settings_default.json` with `colorblind: true` — the toggle's ON render (chip reads "ON" in live-green, matching the haptics/night-glow on-state color convention) |

Four S10 slice b fixtures exist alongside the radar set, one per
`ff_scr_flare_*` builder plus one for the takeover/lock interaction PR #20
UX review flagged as unrepresented: `flare_takeover.json`
(`flare.takeover_active` — the full-screen receive takeover, which per
spec interrupts whatever `face`/`radar` content is also present in the
fixture, exercised here deliberately by pairing it with a live `radar`
section that never actually renders), `flare_takeover_locked.json` (same
takeover, but with `flare.locked` ALSO set to a *different* node than
`takeover_from_name` — exercises the GO-discloses-the-lock-cost chip;
see `ff_scr_flare_build_takeover`'s doc comment),
`flaring_self.json`
(`flare.sending` — the pulsing sender overlay on top of an otherwise-NOSEL
radar tile, whose own headline is dimmed while sending), and
`radar_flare_locked.json` (`flare.locked` on an otherwise-ordinary LIVE
radar render — the lock chip).

Two S11 slice b fixtures cover the Settings face's two visually-opposite
states, one apiece: `settings_default.json` (imperial, LIVE share,
haptics + night glow both on, the 90-min water preset, the 4a-10a quiet
preset, a set name and UTC offset — every chip/label in its "on"/named
state) and `settings_ghost.json` (metric, GHOST share, both toggles off,
water/quiet both OFF, an empty name, and an UNSET UTC offset — every
honest-absence render this face has: `NAME: (unset)`, `UNSET`, `OFF`
twice). Read side by side, the pair is the whole face's render surface at
a glance.

**Provenance note:** the specific numbers (DANA, 320 m, 8 s / 4 min / 15 m,
and PR B's additions) came from the task briefs that commissioned these
slices, not from a value transcribed directly out of
`docs/specs/S06-radar-face.md`'s own text — per `CLAUDE.md` ("Design
references: screen mockups and plan live as Claude artifacts (ask
Jake)"), the actual mockup artboards this repo's specs describe aren't
checked into the repo, so there's no in-tree source to transcribe
pixel/value tables from directly. These fixtures are therefore a
good-faith reconstruction consistent with the spec's *prose*, not a
byte-for-byte transcription of a mockup this agent could not access.
Flagged per AGENTS.md's "if blocked by a spec gap… note the
interpretation" — noted here and in the PR body.

### Now face fixtures (S07 slice b)

Six fixtures cover the Now face's six honestly-distinct `now_state_t`
values — one apiece. `now_nothing_live.json` was added in PR #21 UX
review round 1 (reachable in code from the first pass, no golden yet);
`now_mixed.json` was added in PR #21 code review round 2, alongside the
`now_state_t` enum itself replacing an earlier `pack_loaded`+`tbd` bool
pair that could only represent 3 of these states cleanly (see the
schema section above); `now_time_unknown.json` was added by issue #48
(PR #46 review, D3) for the sixth member, `NOW_TIME_UNKNOWN`.

| Fixture | `state` | What it exercises |
|---|---|---|
| `now_live.json` | `live` | Three concurrent now-playing rows (mocked artists/percentages; stage names + colors are the REAL Lost Lands stages — see the provenance note below, UX review round 1 flagged the original EDC-flavored placeholders) plus a starred-next card — "IN 33 MIN" is the literal countdown text the spec's own example transcribes. |
| `now_mixed.json` | `mixed` | Code review round 2's fix in action, refined in UX review round 2 into three visually distinct classes (never distinguished by an absent element): Excision (`rows[0]`, `pct_done: 22`) renders as a full **playing-now** row — the exact same stage-colored-label + progress-bar treatment `NOW_LIVE` uses, via the same `now_build_row()` call, because it's the same fact; TYNAN (`next`) renders as **scheduled, not started** — a countdown-LED block ("IN 58 MIN" first, biggest, amber, no progress bar); five more real Lost Lands day-1 artists (NGHTMRE, Borgore, Levity, Doctor P, Hairitage) stay visible as **time unknown** in the "STILL TBD" list under a "SOME SET TIMES TBD" banner (not the unqualified "SET TIMES TBD" `now_tbd.json` uses — see the schema section above), rather than silently vanishing the moment the day stopped being all-null. |
| `now_tbd.json` | `tbd` | The real 2026 Lost Lands pack's actual state today: every set's start/end is null. `lineup` is transcribed verbatim (artist + stage, in pack order) from day 1 (2026-09-18) of `firmware/festpack/tests/fixtures/lost-lands-2026.festpack.json` — 7 sets, most with `stage: null` (rendered as the explicit "STAGE UNKNOWN" fallback, not silently omitted — see the provenance note) except Excision (`prehistoric` → "Prehistoric Stage"). This is the pack-update story CLAUDE.md's honesty rule exists for: don't invent set times the source data doesn't have. |
| `now_nothing_live.json` | `nothing_playing` | Pack loaded, schedule known, but nothing is currently playing and nothing is starred upcoming — a genuinely reachable state (early morning between sets) distinct from every other state in this table. `rows`/`next`/`lineup` are all absent. |
| `now_empty.json` | `no_pack` (omitted — the default) | No festpack loaded at all — `now` is entirely absent from the fixture. Deliberately distinct from `now_tbd.json`/`now_mixed.json`: a puck with nothing loaded must never show schedule chrome (a "SET TIMES TBD" banner) that implies a pack exists. |
| `now_time_unknown.json` | `time_unknown` | Issue #48: a pack IS loaded but the wall clock is not (the normal cold-boot path, before a mesh timestamp latches) — distinct from `now_empty.json`'s true "nothing loaded" and from every TBD-flavored state (this isn't a claim about the DATA at all; the projection never got far enough to look at the schedule). `rows`/`next`/`lineup` are all absent, same as `now_nothing_live.json`'s shape, but the copy names the CLOCK as the missing fact, not the pack or the schedule. |

**Provenance note (`now_live.json`/`now_mixed.json`):** artist names and
set times/percentages are mocked test data (GRiZ, Wooli, Kompany, and
`now_mixed.json`'s illustrative "TYNAN just got a time and a stage"
scenario — none of this is a real Lost Lands 2026 announcement), same
"good-faith reconstruction, not a mockup transcription" category as the
radar fixtures above (no mockup artboards in-tree — see CLAUDE.md).
**Stage names + colors in `now_live.json`, however, are the real ones**
— `Prehistoric Stage` (`#ffc66b`), `Subsidia Stage` (`#ff5ca8`), `Forest
Stage` (`#9be07b`), copied from
`firmware/festpack/tests/fixtures/lost-lands-2026.festpack.json`'s
`stages[]`, after UX review round 1 (PR #21) flagged the original
fixture's "Bass Camp"/"Kinetic Field"/"The Grove" as reading like a
different festival's stage names — this PNG is permanent repo history,
worth getting right even though it's mocked data. `now_mixed.json`'s
still-unknown `lineup` entries (NGHTMRE, Borgore, Levity, Doctor P,
Hairitage) and its one known row's artist (Excision) ARE the real Lost
Lands day-1 names, continuing `now_tbd.json`'s same day — only the
*times/percentages/second stage* attached to them are invented, to
illustrate the day partway through the real pack-update process the
reviewer described. **`now_tbd.json` is the one Now fixture that is NOT
mocked at all** — its `lineup` entries are copied field-for-field from
the real vendored Lost Lands pack, specifically because the spec calls
out this exact case ("real Lost Lands pack") as the thing this fixture
must prove.

## Do not golden anything whose pixels depend on text truncation

**`LV_LABEL_LONG_MODE_DOTS` is not bit-reproducible across
architectures.** PR #41 added a `flare_takeover_wide_name` fixture — the
flare takeover with a locked name of fifteen `W`s, to make the
disclosure chip's pixel clamp visible — and CI caught it immediately:
**2.27% of pixels differed between an arm64 macOS render and CI's x86-64
Linux one, while all 25 other fixtures were byte-identical at
`0.0000%`.** Each platform is internally deterministic (the two-render
determinism check passes on both), so this is not flakiness; the
truncation point itself lands on a different character.

Only that one fixture exercised LVGL's dot-placement path, which is what
isolates the cause. The *behaviour* is portable — every assertion-level
test passes on both platforms, including under ASan/UBSan — but the
exact pixel LVGL chooses for the ellipsis is not, so it must never be
the subject of a byte comparison.

**And a per-fixture threshold override would be the wrong escape hatch.**
`run_goldens.sh` compares every fixture against ONE shared
`THRESHOLD_PCT` (0.5%). Keeping a 2.27% fixture means raising that to
~2.5% for all 25 others — blunting every golden gate five-fold to
accommodate the single fixture that cannot hold to it. Adding a
per-fixture override instead would keep the number but lose the
property: the whole value of a shared threshold is that no fixture gets
to negotiate its own standard. Neither trade is worth a picture.

The rendered proof lives at `docs/screens/flare-takeover-wide-name.png`
instead, where it is context rather than a gate, and the guarantee is
enforced by assertions that hold on any platform:
`S10_ACn_lock_disclosure_chip_stays_inside_the_round_glass` (a sweep over
width worst cases, checked against `ff_layout_rect_in_circle`) and
`S10_ACn_lock_disclosure_only_truncates_names_that_dont_fit`. This is the
same "assertion-level, not visual" discipline `test_radar_layout.c`'s
header argues for, arrived at the hard way.

**That PNG is an arm64 macOS render**, matching every other image in the
repo, and it is *the shipped side of the 2.27% discrepancy this section
describes* — so it is worth saying which side you are looking at. Render
the same state on x86-64 Linux and roughly a third of the chip's pixels
land differently, because the ellipsis falls on a different character.
Neither render is more correct than the other; on real hardware there is
exactly one platform and whatever it does is what users see. To check it
yourself, recreate the fixture (the flare takeover with
`locked_from_name` set to fifteen `W`s), render it, and diff against the
committed PNG with `build/compare_png` — on arm64 you will get
`0/207936 (0.0000%)`, and a non-zero result means you are on the other
side of the discrepancy, not that the image is stale.

## Adding a fixture

1. Write `tests/fixtures/<name>.json` per the schema above.
2. `./build/ffsim --headless --screenshot /tmp --fixture tests/fixtures/<name>.json` to sanity-check it renders.
3. `tests/run_goldens.sh --update-golden` to generate `tests/golden/<name>.png`, then review the PNG diff before committing.
