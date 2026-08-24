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
| `state` | string enum: `no_pack`\|`tbd`\|`mixed`\|`live`\|`nothing_playing` | `no_pack` | **[api], PR #21 code review finding #2/ruling.** Replaces an earlier `pack_loaded`+`tbd` bool pair — see `now_state_t`'s doc comment in `ff_app_state.h` for why an explicit, mutually-exclusive-by-construction enum (same convention as `radar.mode`) replaced two independent bools. An absent or unrecognized string defaults to `no_pack`, the least-claiming state — same convention as `radar.mode`'s `nosel` default. |
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
for (pixels), but the one place a real lock DECISION gets made
(`ff_scr_flare_selection_locked`) always consults the live `ff_flare_t`
via `ff_flare_locked_node()` directly, never this struct.

`takeover_bearing_valid` (PR #20 code review, LOW finding) exists because
a bearing genuinely can be unknown (no position fix on either end) and
`takeover_bearing_deg` alone has no way to say so — `0.0` is
indistinguishable from "really due north." Defaults to `false`
independently of whether `takeover_bearing_deg` itself is present (see
`test_fixture.c`'s `flare_takeover_bearing_valid_defaults_false` —
providing the degree value without the flag still renders "bearing
unknown", never a fabricated compass point), same "prove you meant this"
convention `radar.arrow_valid` already uses.

## `settings` (S11)

```json
"settings": {
  "imperial": true, "share_mode": "live", "haptics": true, "night_glow": true,
  "water_min": 90, "quiet_from_min": 240, "quiet_to_min": 600, "my_name": "DANA"
}
```

`share_mode` is `live`\|`zones`\|`ghost` (maps to `FF_SHARE_LIVE`/`_ZONES`/`_GHOST`).

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
| `radar_cluster.json` | `live` | `320 m` | `8 SEC` | added for issue #18 (cluster marker styling). Four crew members 4 degrees apart on the ring, so all four resolve into ONE cluster marker — one wedge each, in all four **distinct** crew colors (pink/teal/violet/green), with the violet member stale so the mixed-freshness case is visible: three 6px wedges and one 2px one. `radar_close_collision.json` below only ever produces a 2-member cluster, which can't show whether the ring generalizes past a half-and-half split. **Not** the widest ring the marker can draw — `FF_CREW_MAX` is 8 while the palette has 4 colors, so a 5+ member cluster repeats colors; nothing above 4 members has a golden, and that gap is recorded in `docs/specs/S06-radar-face.md`'s Amendments |
| `radar_cluster_stale.json` | `stale` | `320 m` (last known) | `4 MIN` | same four clustered members, but **every one of them stale** — added in PR #41's UX review round, which found that the first implementation expressed staleness by dimming a wedge's opacity, and dimming is *relative*: with no fresh wedge in frame for contrast, an all-stale marker rendered indistinguishably from a live one. This is the fixture that pins the fix (2px hollow wedges at full crew color, plus a `FF_THEME_COLOR_MUTED` count digit). Deliberately paired with `radar_cluster.json` — the pair only means something read side by side, since the whole finding was that one of them used to look like the other |
| `radar_close_collision.json` | `close` | `15 m` | `3 SEC` | same scenario as `radar_close.json`, but with 4 crew-ring dots deliberately placed at worst-case bearings (one straight at the status bar, three clustered straight at the FLARE button / trend chip) to exercise `app/screens/radar_layout.c`'s layout resolver — the three southward dots resolve into a 1-dot + 1-cluster-of-2 outcome (a "2" marker, not a hidden member), and the northward dot lands clear of the status bar. Regressing the resolver will show up here even if it doesn't show up in the plain `radar_close` golden — though the authoritative regression coverage is `app/screens/tests/test_radar_layout.c`'s geometry-level sweep, not this golden (see that file's header comment for why) |
| `radar_nofix.json` | `nofix` | `""` (unknown — my position invalid) | `6 MIN` (the *selected member's* last-known age is still honestly known even though mine isn't) | arrow hidden, "NO FIX - RADIO ONLY" |
| `radar_nosel.json` | `nosel` | `""` | `""` | no paired crew member at all — empty-crew state, `mesh_ok: false` for variety |
| `radar_never.json` | `lost` (folded — see below) | `""` | `""` | selected member "JAMIE" is paired but has never sent a fix; `age_str[0] == '\0'` is what `scr_radar.c` keys off to show "NO FIX YET" instead of a "LAST SEEN" chip — NOT distinguishable from a genuinely-old fix by `mode` alone (both are `RADAR_LOST`; see `radar_lost.json` above for the other side of that same `mode`) |

Four S10 slice b fixtures exist alongside the radar set, one per
`ff_scr_flare_*` builder plus one for the takeover/lock interaction PR #20
UX review flagged as unrepresented: `flare_takeover.json`
(`flare.takeover_active` — the full-screen receive takeover, which per
spec interrupts whatever `face`/`radar` content is also present in the
fixture, exercised here deliberately by pairing it with a live `radar`
section that never actually renders), `flare_takeover_locked.json` (same
takeover, but with `flare.locked` ALSO set to a *different* node than
`takeover_from_name` — exercises the GO-discloses-the-lock-cost chip;
see `ff_scr_flare_build_takeover`'s doc comment), `flaring_self.json`
(`flare.sending` — the pulsing sender overlay on top of an otherwise-NOSEL
radar tile, whose own headline is dimmed while sending), and
`radar_flare_locked.json` (`flare.locked` on an otherwise-ordinary LIVE
radar render — the lock chip).

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

Five fixtures cover the Now face's five honestly-distinct `now_state_t`
values — one apiece. `now_nothing_live.json` was added in PR #21 UX
review round 1 (reachable in code from the first pass, no golden yet);
`now_mixed.json` was added in PR #21 code review round 2, alongside the
`now_state_t` enum itself replacing an earlier `pack_loaded`+`tbd` bool
pair that could only represent 3 of these states cleanly (see the
schema section above).

| Fixture | `state` | What it exercises |
|---|---|---|
| `now_live.json` | `live` | Three concurrent now-playing rows (mocked artists/percentages; stage names + colors are the REAL Lost Lands stages — see the provenance note below, UX review round 1 flagged the original EDC-flavored placeholders) plus a starred-next card — "IN 33 MIN" is the literal countdown text the spec's own example transcribes. |
| `now_mixed.json` | `mixed` | Code review round 2's fix in action, refined in UX review round 2 into three visually distinct classes (never distinguished by an absent element): Excision (`rows[0]`, `pct_done: 22`) renders as a full **playing-now** row — the exact same stage-colored-label + progress-bar treatment `NOW_LIVE` uses, via the same `now_build_row()` call, because it's the same fact; TYNAN (`next`) renders as **scheduled, not started** — a countdown-LED block ("IN 58 MIN" first, biggest, amber, no progress bar); five more real Lost Lands day-1 artists (NGHTMRE, Borgore, Levity, Doctor P, Hairitage) stay visible as **time unknown** in the "STILL TBD" list under a "SOME SET TIMES TBD" banner (not the unqualified "SET TIMES TBD" `now_tbd.json` uses — see the schema section above), rather than silently vanishing the moment the day stopped being all-null. |
| `now_tbd.json` | `tbd` | The real 2026 Lost Lands pack's actual state today: every set's start/end is null. `lineup` is transcribed verbatim (artist + stage, in pack order) from day 1 (2026-09-18) of `firmware/festpack/tests/fixtures/lost-lands-2026.festpack.json` — 7 sets, most with `stage: null` (rendered as the explicit "STAGE UNKNOWN" fallback, not silently omitted — see the provenance note) except Excision (`prehistoric` → "Prehistoric Stage"). This is the pack-update story CLAUDE.md's honesty rule exists for: don't invent set times the source data doesn't have. |
| `now_nothing_live.json` | `nothing_playing` | Pack loaded, schedule known, but nothing is currently playing and nothing is starred upcoming — a genuinely reachable state (early morning between sets) distinct from every other state in this table. `rows`/`next`/`lineup` are all absent. |
| `now_empty.json` | `no_pack` (omitted — the default) | No festpack loaded at all — `now` is entirely absent from the fixture. Deliberately distinct from `now_tbd.json`/`now_mixed.json`: a puck with nothing loaded must never show schedule chrome (a "SET TIMES TBD" banner) that implies a pack exists. |

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

## Adding a fixture

1. Write `tests/fixtures/<name>.json` per the schema above.
2. `./build/ffsim --headless --screenshot /tmp --fixture tests/fixtures/<name>.json` to sanity-check it renders.
3. `tests/run_goldens.sh --update-golden` to generate `tests/golden/<name>.png`, then review the PNG diff before committing.
