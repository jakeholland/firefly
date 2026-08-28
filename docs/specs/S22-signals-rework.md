# S22 — Signals rework (unified crew list + targeting + actions)

Status: draft (2026-08-28). Supersedes the Signals-face UI of
[S08](S08-signals-t9.md) (the feed/T9 core stays); no protocol change
([S04](S04-firefly-protocol.md) is unchanged). Design canvas (authoritative
for layout, per CLAUDE.md): https://claude.ai/code/artifact/c8216bb6-4dbf-4f7a-a6d7-54baa8074dfd
(Page 1 · Direction A). The real Firefly bearing arrow referenced by the
"Crew Compass" alternative lives in the Firefly Screens artifact.

## Why

The current Signals face is an incoming-only feed with a corner `+` for
compose and a fixed OMW / 5 MIN / PULSE reply row. On-glass testing surfaced
three problems: (1) the `+`-is-compose affordance reads as odd; (2) you can
only reach people who have already messaged you; (3) the reply row is a
narrow, mode-y strip. This rework turns Signals into the one place to reach
anyone — recent signals **and** your whole crew — and makes "send a signal"
a first-class, unmistakable action.

## Model (all pure C11 in `core/`; the screen only renders)

A new **signals view-model** merges the two existing sources into one ordered
list the screen renders top-to-bottom:

- **Recent** — items from `ff_feed_t` ([ff_feed.h](../../firmware/core/include/ff_feed.h)),
  newest first, joined to a crew member by `from_node` for name / `initial` /
  `color_idx`. Carries kind (pulse/rally/text/…), age string, `unread`.
- **`· CREW ·` divider**, then **quiet crew** — paired members of
  `ff_crew_t` ([ff_crew.h](../../firmware/core/include/ff_crew.h)) that have
  **no** recent feed item, ordered by presence (freshest first). Rendered
  dimmed with an honest **presence** label derived from `ff_freshness_t`
  (`FF_FRESH_LIVE` / `STALE` / `LOST` / `NEVER`) + `rssi_age_ms`:
  - LIVE/STALE → `SEEN <age>` (e.g. `SEEN 6 MIN`).
  - LOST → `SEEN <age>` in the stale tint, or `LOST` past `FF_CREW_LOST_MS`.
  - NEVER → `LINKED` (paired but never heard) — never a fabricated time.

  **Honest-data (enforced in review, [[firefly-touch-cal-default]]):** presence
  is a freshness value. Never render a guessed "online"/"now"; a cold node shows
  its real last-heard age with the stale treatment, or `LINKED`/`LOST`.

### Targeting

A single **target** state lives in core: `WHOLE_CREW` (default) or one paired
`node_id`. Selecting any row (recent or quiet) sets the target to that member;
an explicit clear returns to `WHOLE_CREW`. The screen shows an always-visible
**target line** directly above the actions so a send never fires blind. Target
resets to `WHOLE_CREW` after any send.

### Actions

Three actions, each valid for the target (whole crew or one member), mapped to
the existing protocol ([ff_proto.h](../../firmware/core/include/ff_proto.h)):

- **RALLY** → `FF_PROTO_TYPE_RALLY` (gather + place). **Rally-to-WHOLE_CREW
  requires a confirm** (second tap / hold) — the one loud broadcast; rally to a
  single member and all pulse/compose sends do not.
- **PULSE** → `FF_PROTO_TYPE_PULSE` (empty-body ping).
- **COMPOSE** → opens the composer ([S08](S08-signals-t9.md) T9) targeting the
  current target; sends `FF_PROTO_TYPE_TEXT`.

The corner `+` is removed. Compose now lives in the COMPOSE action button.

### Composer start state (quick replies)

The old Signals reply row moves into the composer's **start state**: OMW /
5 MIN / PULSE render as the first suggestion chips (where predictive candidates
sit) before any key is pressed. Tapping one sends immediately (OMW/5 MIN =
canned `TEXT`, PULSE = `FF_PROTO_TYPE_PULSE`); typing switches to predictive T9.
Canned strings come from the festpack, never hardcoded outside test fixtures.

## Layout (round 412 glass — `ff_layout_safe_margin_x` per row/band)

Feed list (scrolls under a docked footer) → docked **target line** → three
large action buttons (kind-colored: Rally violet, Pulse amber, Compose green) →
nav page-dots. Every band's width derived from its worst-case y, verified by
`test_face_hit_targets.c` (not hand math — PR #86's lesson). All controls
≥ `FF_THEME_MIN_HIT_PX` (44) with ≥ `FF_HIT_MIN_GAP_PX` (8) adjacency.

## Acceptance criteria

- **AC1** Unified list: recent feed rows, a CREW divider, then paired crew with
  no recent item, ordered by presence; joins feed `from_node` → crew identity.
- **AC2** Presence labels are honest (`SEEN <age>` / stale / `LOST` / `LINKED`),
  derived from `ff_freshness_t` + `rssi_age_ms`; never a fabricated freshness.
- **AC3** Target defaults to WHOLE_CREW; selecting a row targets that member;
  clear returns to WHOLE_CREW; the target line always shows the current target;
  target resets to WHOLE_CREW after a send.
- **AC4** RALLY/PULSE/COMPOSE act on the current target; rally-to-WHOLE_CREW
  requires a confirm; the others send on first tap.
- **AC5** Composer start state shows OMW / 5 MIN / PULSE as the first
  suggestions; tap sends (text / pulse); typing switches to predictive T9.
- **AC6** All hit targets ≥ 44px, adjacency ≥ 8px, nothing clips the round
  glass — verified by the hit-target sweep. UI PR attaches rendered goldens.
- **AC7** Core view-model + targeting are pure C11 with unit tests; the screen
  only renders and forwards intents.

## Slice plan

- **(a) core view-model + targeting** — new `core/` module: merge feed+crew into
  the ordered list, presence-label derivation, target state + transitions.
  Unity tests, no UI. (First PR.)
- **(b) scr_signals.c** — render the unified list + target line + 3 actions to
  the model; round-fit geometry; new golden fixtures. Emits intents.
- **(c) composer quick-reply start** — start-state suggestions + send wiring in
  `scr_compose.c` / shell.
- **(d) send wiring** — actions → `ff_proto` encode + dispatch; rally-to-crew
  confirm; target routing. (Blocked only by whatever real send seam exists.)

## Open / out of scope

- Bearing arrows ("Crew Compass" alternative, canvas Page 2) are **not** in this
  spec — Signals is not bearing-based (that's [S06](S06-radar-face.md) /
  [S09](S09-map-face.md)); revisit if we want a compass Signals variant.
- Pairing UI (adding heard nodes to crew) stays out — `ff_heard_t` exists but
  the pairing screen is separate, unbuilt.
