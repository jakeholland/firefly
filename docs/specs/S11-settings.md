# S11 · settings & persistence

## Purpose
User prefs + durable state behind a tiny key-value seam. Mockup "Settings" is layout authority.

## Interface (`core/include/ff_store.h`, `core/include/ff_settings.h`)
```c
typedef struct {           // storage vtable: sim=file, esp32=NVS
  int (*get)(void *io, char const *key, void *buf, size_t n);   // returns len or <0
  int (*set)(void *io, char const *key, void const *buf, size_t n);
  void *io;
} ff_store_t;
typedef struct {
  bool imperial;            // default true (US festival)
  uint8_t share_mode;       // 0 LIVE / 1 ZONES / 2 GHOST (v1: LIVE/GHOST honored; ZONES=LIVE + issue)
  bool haptics, night_glow; // defaults true
  uint16_t water_min;       // 0 off, default 90
  uint16_t quiet_from_min, quiet_to_min; // local minutes, default 240→600 (4a–10a)
  char my_name[16];
  ff_geo_cal_t compass_cal; bool cal_valid;
} ff_settings_t;
void ff_settings_load(ff_settings_t *s, ff_store_t const *st);   // defaults on missing/corrupt
void ff_settings_save(ff_settings_t const *s, ff_store_t const *st);
bool ff_quiet_now(ff_settings_t const *s, int16_t now_min);      // handles wrap (from>to)
```
Also persisted: paired crew list (S02), starred sets (S07), selected pack id.

## Behavior
- GHOST: app stops honoring outbound STATUS + suppresses `mc_send_position`; radio-level position broadcast is the comms brain's — v1 documents that GHOST silences firefly-layer sharing and sets Meshtastic position broadcast off via admin message (slice c; if flaky, GHOST ships firefly-layer-only with README honesty note).
- Settings face renders per mockup: FT/M segmented, share row, two toggles, water/quiet value rows (tap cycles presets v1: water off/45/90/120; quiet off/2a-8a/4a-10a). Long-press-anywhere opens settings; back = swipe.
- Water nudge: haptic + toast every `water_min` while awake, suppressed in quiet hours.

## Acceptance criteria
1. Load with empty store yields exact defaults; corrupt blob (wrong size/magic) yields defaults not garbage.
2. Round-trip save/load equality for full struct incl. calibration.
3. `ff_quiet_now`: table incl. wrap 23:00→02:00 boundaries inclusive-exclusive documented.
4. Water tick fires at interval, resets on settings change, silent in quiet hours.
5. Golden: `settings.json` matches mockup.
6. Store mock records single write per save (no write-amplification loops).

## Slices
a) store seam + settings struct + tests · b) face render + interactions + golden · c) GHOST admin-message wiring (e2e).

## Amendments

- **2026-08-23, PR #37 (S16 slice b0) — `ff_settings_t` gains a UTC offset `[api]`.** Foreseen by S16's "Wall clock" section, which records the amendment here. Quiet hours is a settings feature with no festival dependency, but before this there was no path to local time at all without a loaded festpack, so on a puck with no pack `ff_quiet_now` silently could not be evaluated.

  Two new fields: `int16_t utc_offset_min` (minutes east of UTC) and its own `bool utc_offset_set`. The flag is not redundant — `int16_t` has no free sentinel here because **0 is legitimately UTC**, so absence cannot be encoded as a value that already means something. Same ruling as `stage_color_valid` and `FF_FRESH_NEVER`. Defaults to *unset*, which keeps a never-configured puck honestly `FF_WALL_UNKNOWN` rather than guessing a zone.

  Resolution order against a loaded pack lives in `ff_wall_resolve_offset` (`core/include/ff_wall.h`), not here: pack's **stated** offset → settings offset when set → pack's assumed default → `FF_WALL_UNKNOWN`. The settings value deliberately outranks `fp_parse`'s −240 fallback; a value the user configured must not lose to a parser default.

  **Release note — a v2 settings blob is discarded, not migrated.** `FF_SETTINGS_FORMAT_VERSION` goes 2 → 3, and `ff_settings_load` rejects any blob whose version does not match, falling back to the full default struct. There is no migration path. The blast radius is wider than the new field: units, share mode, haptics, night glow, water interval, quiet hours, `my_name` **and compass calibration** all reset — and the calibration is the one a user would actually notice having to redo (S12's calibration ritual). Accepted rather than mitigated: this is pre-v1 firmware with no fielded devices, `sizeof(ff_settings_t)` changed anyway so the payload-size check would have rejected the blob regardless, and it matches the precedent set by the v1 → v2 bump. Flagged in the review of PR #37 (D6) as needing to be written down where the next version bump will look for it, which is here.

- **2026-09-03 — Settings audit finding: slice c (GHOST admin-message
  wiring) is still unimplemented.** A read-only audit
  (`feat/settings-audit-sections`, trace every consumer of every
  `ff_settings_t` field) confirmed what this file's own "Slices" list
  already implies but never called out explicitly: `share_mode` is
  written and projected by every layer that touches settings (screen,
  shell, store), but **nothing gates outbound position sharing on it** —
  not on the sim, not on the device. A wearer can set SHARE to GHOST and
  the puck keeps sharing position exactly as if it were LIVE. Slice c
  ("GHOST admin-message wiring (e2e)") is the slice that was always
  supposed to close this gap (see this spec's own "Behavior" section:
  "v1 documents that GHOST silences firefly-layer sharing and sets
  Meshtastic position broadcast off via admin message (slice c; if
  flaky, GHOST ships firefly-layer-only with README honesty note)") — it
  never landed.

  **Consequence, this audit:** because SHARE currently has no effect on
  either target, `feat/settings-audit-sections` hides the SHARE row from
  the Settings UI (behind `scr_settings.c`'s
  `FF_SETTINGS_ROW_ENABLE_SHARE`, default off) rather than leave a
  control on screen that lies about doing something — the "honest data
  over pretty data" rule (`CLAUDE.md`) applied to a CONTROL, not just a
  displayed value: a toggle nobody's code reads is exactly as dishonest
  as a stale timestamp presented as fresh. `share_mode` itself stays
  persisted and the `FF_SETTING_SHARE_MODE` intent stays wired at the
  shell level — flipping the row's flag back to `1` is the entire
  re-enable once slice c actually lands. See
  `docs/specs/S21-settings-rework.md`'s own 2026-09-03 Amendment for the
  full audit (all four hidden rows, not just this one) and the section
  layout that replaced them.

  **Not resolved by this audit** — this is a note, not slice c's
  implementation. Whoever picks up slice c still owns: the admin-message
  wire format to the comms brain, the "if flaky, GHOST ships
  firefly-layer-only with README honesty note" fallback this spec
  already describes, and un-hiding the SHARE row (one flag flip) once
  the wiring is real.

- **2026-09-06 — NAME in Settings lands (`feat/settings-my-name-owner`):
  `my_name` gets an editor and a mesh push.** Before this PR, `my_name`
  was a real, persisted `ff_settings_t` field with a working write-through
  seam (`FF_INTENT_SETTING_SET`/`FF_SETTING_MY_NAME`, wired since S16
  slice e) — but no UI ever emitted it (`scr_settings.c`'s own header
  comment used to say so explicitly: "`my_name` is NOT editable in this
  slice") and nothing pushed it anywhere the crew could see: the caption
  under SETTINGS was purely local, never reaching the comms brain's
  Meshtastic owner identity (what every other node/phone app on the mesh
  actually displays for this puck).

  **What landed:**
  1. A **NAME** row (its own single-row section, S21's "UNITS" precedent
     for a lone-member category, placed directly above CREW) showing the
     stored name — DOTS-ellipsized at the 15-char cap, same `compose_to`
     precedent scr_compose.c's own TO row uses — plus a small honest
     status pill: `LV_SYMBOL_OK` once confirmed, "..." while pending,
     "N/A" when there is nothing to confirm yet (name unset). Tapping
     either half opens a full-screen T9 editor
     (`FF_SETTINGS_SUB_NAME_EDIT`) that reuses core's `ff_t9.h` engine
     through a SECOND, independent `ff_t9_t` (`ff_shell.c`'s
     `name_draft`) — deliberately not scr_compose.c's own keypad
     renderer or its `compose_draft`; see `ff_intent.h`'s
     `FF_INTENT_SETTINGS_OPEN_NAME_EDIT` doc comment for why. ABC/123
     modes only (letters/digits/space — this spec's own "charset A-Z0-9
     space" rule, no SYM/PRED). BACK cancels (discards the draft); DONE
     commits.
  2. **Commit path**: DONE (or the bench console's `name <text>`)
     sanitizes the draft (`ff_meshname_sanitize`, core) through the
     EXISTING `FF_SETTING_MY_NAME` string-payload seam — persisted on
     change, no second persistence path — then pushes a Meshtastic
     `AdminMessage.set_owner` (long name verbatim, short name derived by
     `ff_meshname_derive_short`: first 4 alphanumeric characters,
     uppercased, non-alphanumerics dropped BEFORE truncating — "Taylor"
     -> "TAYL", "Jake" -> "JAKE", "Jo" -> "JO", never padded with
     anything fabricated) to this node's own id, over the SAME local
     client connection this device already is to the comms brain — no
     admin key needed for a message addressed to yourself (see
     `mc_send_set_owner`'s own doc comment, `meshclient/include/
     mc_client.h`, for the exact firmware citation). Independent of
     whether the LOCAL value changed: re-pressing DONE with the same
     text is the retry mechanism for a push that failed silently on a
     flaky link.
  3. **Honesty**: the row's status pill is NEVER assumed confirmed
     merely because a push was accepted for send — only an actual
     self NodeInfo reporting a MATCHING `long_name` flips it
     (`ff_shell_mesh_name_status`/`shell_mesh_name_confirmed`,
     ff_shell.c). Boot: if the stored name is empty and a self NodeInfo
     already carries one (e.g. set previously via the phone app/CLI),
     it is silently adopted once and persisted (`my_name_from_node`),
     rather than showing an honest-but-useless "(unset)" forever.
  4. **Bench console**: `name` (status: stored/mesh/confirmed) and
     `name <text>` (the exact same commit mechanism the row's DONE
     button uses, bypassing the subview-only guard the same way
     `ff_shell_debug_send_text` already bypasses Compose's modal
     requirement — see `ff_shell_debug_set_name`'s own doc comment) —
     see `docs/hardware/comms-brain.md`.

  See `docs/hardware/comms-brain.md`'s own new section for how the name
  actually reaches the mesh, including the exact firmware citation for
  the no-admin-key local path, and the CLI/phone-app fallback that
  remains available regardless.

- **2026-09-06 — Confirmation fix (same PR, bench finding against a real
  puck + comms brain, Meshtastic 2.7.26): the pill above never turned ✓
  in practice.** The bench proved the push itself works (`meshtastic
  --info` on the comms brain showed the new owner immediately after a
  `name <text>`), but the ORIGINAL confirmation design above — "wait for
  a self NodeInfo reporting a matching `long_name`" — assumed the comms
  brain would re-announce its own identity shortly after a `set_owner`.
  It does not: `AdminModule::handleSetOwner` updates the local
  `nodeDB`/owner in place and never re-broadcasts a fresh self NodeInfo
  on its own; the next one a connected client sees is either the NEXT
  want_config handshake (a reconnect) or the periodic (hours-scale)
  NodeInfo broadcast — neither of which happens promptly after a wearer
  taps DONE. The pill was therefore correct (never dishonestly
  confirming) but useless (never confirming AT ALL within a session).

  **Fix**: a successful `set_owner` push now immediately follows up with
  its own read — `AdminMessage.get_owner_request` to this node's own id
  (`mc_send_get_owner_request`, `meshclient`) — and the reply
  (`get_owner_response`, delivered via a new `mc_events_t.on_owner`
  event) is treated exactly like a self NodeInfo for confirmation
  purposes (`ff_shell.c`'s `shell_ev_owner`). If no reply arrives within
  ~10 s, the request is retried up to 3 times; if the budget is
  exhausted with still no reply, the row/console keep reading the honest
  "..." pending state forever — this fix removes the "confirmation never
  happens" failure, it does not add a NEW way to fabricate one. The
  mesh-level delivery outcome of the `set_owner` write itself
  (`Routing.error_reason`, correlated by outgoing packet id via a new
  `mc_events_t.on_routing_ack` event) is ALSO now surfaced: a NAK shows
  as a distinct "!" (amber) on the pill and `ack=nak` on the bench
  console, rather than looking identical to ordinary pending — this is a
  routing-layer fact, independent of (and no substitute for) the
  `get_owner_response`/self-NodeInfo confirmation itself, per this
  spec's own honest-data rule.

  Bench console gains `pushed=<long>/<short> ack=<none|ok|nak>
  reply=<none|long/short>` on the `name` command's output — see
  `docs/hardware/comms-brain.md`'s "How confirmation works" section for
  the full state machine.

- **2026-09-06 — Confirmation fix round 2 (same PR, bench finding AFTER
  the fix above, commit `51e4ae1`, against a real puck + comms brain,
  Meshtastic 2.7.26): the round-1 fix neither worked nor was fully
  honest.** Two bugs, both found on the SAME bench run:

  1. **`get_owner_request` never got a reply at all.** `name Jake H` ->
     `pushed=Jake H/JAKE ack=ok` within 3 s (the `set_owner` write itself
     really lands — confirmed via the CLI) but `reply=none` after 21 s
     and across every retry. Root cause: Meshtastic's `AdminModule` only
     answers a `get_*_request` when the request's own
     `Data.want_response` flag is set (`AdminModule::handleGetOwner`,
     `meshtastic/firmware` tag `v2.7.26.54e0d8d0`,
     `src/modules/AdminModule.cpp` — the Python CLI sets
     `wantResponse=True` on every admin read for exactly this reason).
     `mc_send_get_owner_request` never set it. **Fixed** by threading a
     `want_response` parameter through `mc_send_data_packet_ex`
     (`meshclient/src/mc_client.c`) and setting it true only for
     `mc_send_get_owner_request` — see that function's own doc comment
     (`mc_client.h`) for the full citation.

  2. **A false positive**, independent of bug 1: `name Jake` (re-pushing
     the puck's CURRENT name — this feature's own documented retry
     mechanism) reported `confirmed=1` **instantly**, with
     `ack=none reply=none` — before any reply could possibly have
     arrived. Root cause: `shell_mesh_name_confirmed` compared the
     stored name against the CACHED `mesh_owner_name` with no notion of
     WHEN that cache was last written relative to the push it was
     supposedly confirming — a value left over from an earlier
     confirmation (or the boot prefill) that happened to already match
     read as "confirmed" for a brand-new push that had not been answered
     yet. **Fixed** by a push-generation counter: `name_pushed_seq`
     increments once per attempted push, `mesh_owner_name_seq` is
     stamped with the CURRENT generation every time `mesh_owner_name` is
     written (both the self-NodeInfo path and the `get_owner_response`
     path), and `shell_mesh_name_confirmed` now additionally requires
     `mesh_owner_name_seq >= name_pushed_seq` — an observation from an
     earlier generation is stale and reads as pending, never confirmed,
     regardless of what string it holds. Both counters start at 0, which
     is what preserves the boot-prefill "(from_node)" ✓ semantics for
     the untouched case (an observation before any push is not stale
     relative to "no push yet"). A NEW state, `mismatch`, was added
     alongside: a FRESH observation (same freshness test) that does NOT
     match the pushed name — distinct from both `confirmed` and from a
     routing NAK, which says nothing about what name the admin module
     actually ended up with.

  Bench console's `name` output gains `seq=<N>` (the push-generation
  counter itself) and `mismatch=<0|1>`, alongside the existing
  `pushed=/ack=/reply=` trio. See `docs/hardware/comms-brain.md`'s "How
  confirmation works" section (its own round-2 subsection) for the full
  mechanism, and `ff_shell_mesh_name_status_t`'s doc comment
  (`ff_shell.h`) for the exact field-by-field rule.

  **Mutation-verified, both bugs, on the pre-fix code**: removing the
  `mesh_owner_name_seq >= name_pushed_seq` clause fails exactly
  `S_name_recommitting_the_same_name_does_not_falsely_confirm_from_stale_mesh_state`
  and
  `S_name_recommit_stale_self_nodeinfo_does_not_falsely_confirm_either`
  (`test_shell.c`) plus
  `dbgconsole_name_recommitting_the_same_name_does_not_falsely_confirm_from_stale_mesh_state`
  (`test_debug_console.c`) — nothing else in either suite; setting
  `want_response` back to `false` on `mc_send_get_owner_request`'s
  encoded packet fails exactly `feat_get_owner_request_encodes_the_request`
  (`test_meshclient.c`) and nothing else. Both reverted after
  confirming.
