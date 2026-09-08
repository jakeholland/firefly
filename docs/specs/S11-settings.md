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

- **2026-09-06 — Confirmation fix round 3 (same PR, bench finding AFTER
  the round-2 fix above, commit `eb1cb06`, against a real puck + comms
  brain, Meshtastic 2.7.26): the FIRST push after boot confirmed
  perfectly (`ack=ok`, a reply within 3 s); every push after that, in the
  SAME session, sat on `ack=none reply=none` forever — even past the
  full retry budget.** Bench transcript (one continuous boot session):

  ```
  name Jake H  → seq=1 … after 3 s:  ack=ok  reply=Jake H/JAKE  confirmed=1  (perfect)
  name Jake    → seq=2 … after 5s, 30s: ack=none reply=none confirmed=0
  name Jake Z  → seq=3 … after 4/12/24/36s: ack=none reply=none
  name Jake    → seq=4 … after 4/16/28s: ack=none reply=none
  ```

  The comms brain applied every push (its owner really ended at
  `Jake (JAKE)`, matching the last push) — so `mc_send_set_owner` and
  `mc_send_get_owner_request` both reach the wire every time. Root
  cause, found by instrumenting the sim harness rather than guessing at
  undocumented real-firmware behavior: `ff_shell_tick`'s
  `get_owner_request` retry loop counted a retry attempt against
  `FF_NAME_OWNER_REQ_MAX_RETRIES`'s budget **regardless of whether the
  send actually reached the wire** (`send_get_owner_request`'s return
  code was discarded). A quiet two-node bench (just the puck + the
  comms brain, no other mesh traffic) trips this device's own
  `mc_client` 30 s no-RX-bytes watchdog into a reconnect once nothing
  else arrives for that long — exactly the gap between typed bench
  commands once the mesh goes quiet after a push confirms. Any retry
  attempted while that reconnect is in flight legitimately fails
  (`send_get_owner_request` returns nonzero, mirroring
  `mc_send_get_owner_request`'s own `state != MC_STATE_READY` gate) —
  and the old code spent the WHOLE retry budget on those failed
  attempts, so once the reconnect completed and the transport was READY
  again there was no budget left to ever ask again. The row read
  pending forever despite the comms brain being perfectly reachable.

  **Fixed**: `ff_shell_tick`'s retry block (`ff_shell.c`) now only
  counts a retry / advances the poll deadline when
  `send_get_owner_request` actually returns 0 (reached the wire); a
  failed attempt leaves both untouched, so `ff_time_reached` simply
  re-fires next tick — a cheap same-thread state check, no I/O — until
  the transport recovers, with the FULL retry budget still available
  for the reply that follows.

  **Tests** (`test_shell.c`): `S_name_owner_request_retry_failures_do_not_burn_the_retry_budget`
  reproduces the exact bug — more transport-down retry attempts than
  the whole budget, then a recovery — and MUST FAIL on the pre-fix code
  (`ctest` output: `the poll must still attempt a fresh
  get_owner_request once the transport recovers`, `Expected TRUE Was
  FALSE`; mutation-verified: reverting the fix fails exactly this test
  and none of the other 250 in the suite). Two more tests close the
  task brief's remaining scenarios: `S_name_second_push_in_the_same_session_also_confirms_via_its_own_reply`
  (two different names pushed back to back, each with its own fresh
  packet id, each confirming off its own ack+reply — this one already
  passed pre-fix, proving the app-level push/poll bookkeeping itself
  was never the bug) and `S_name_commit_abandons_a_still_pending_previous_poll`
  (a second push committed while the first push's poll is still
  outstanding — the old poll's late ack/reply must never confirm or
  even acknowledge the NEW push, which must confirm only off its own
  round trip). `test_meshclient.c` gains
  `feat_two_consecutive_set_owner_round_trips_in_one_session_both_confirm`,
  the same two-round-trip scenario through REAL wire encode/decode (not
  the app-layer mock) — also passed pre-fix, confirming the meshclient
  library's decode dispatch has no per-session "already answered once"
  state either. Together these three passing tests are the evidence
  that the bug was specifically in the retry-budget bookkeeping, not in
  event correlation or the push/poll state machine more broadly.

  **Gates**: clang + gcc-14 sim builds green, zero warnings both.
  `ctest`: 74/74 both compilers (251-test `test_shell`, 92-test
  `test_meshclient`). `run_goldens.sh`: 90/90 fixtures, byte-identical —
  no UI changed, no golden regeneration needed. esp32s3 device build
  (`CONFIG_FF_DEBUG_CONSOLE=y`) compiles clean, zero warnings, **not
  flashed**. See `docs/hardware/comms-brain.md`'s "How confirmation
  works, round 3" subsection for the full mechanism.

  **Honesty note on scope**: this fix addresses a concrete, sim-provable
  defect (the retry budget could be silently exhausted while the
  transport was briefly down) that is consistent with the bench
  transcript's timing (the ~30s/~32s gaps line up with `mc_client`'s
  own 30s reconnect watchdog on a quiet mesh) and with push 1 always
  working (its reply arrived in 3s, well inside the 10s window, before
  any reconnect could have fired). It does **not** claim to be
  independently bench-verified as the sole cause of every symptom in
  the transcript above — in particular, a fresh push's OWN immediate
  `set_owner` routing ack failing on a session that (per this theory)
  had ALREADY finished reconnecting before the push was even typed is
  not fully explained by this fix alone, and could not be reproduced in
  sim absent a real radio. The coordinator's next real-hardware bench
  run is what will confirm or rule out further causes; if `ack=none`
  persists on a push's own immediate attempt even after this fix ships,
  the next place to look is the esp32s3 target's own UART driver/RX
  path (untested by the sim harness) or a genuine Meshtastic firmware
  resource limit on concurrent locally-tracked `want_ack` packets — not
  reasoned about further here for lack of a way to verify either
  without real hardware.

  **Correction (2026-09-06, see the round-4/"reboot-session-loss" entry
  below).** The "honesty note" above was right to hedge: a follow-up
  bench run with frame-level instrumentation found the actual mechanism
  behind this round's own transcript — the comms brain reboots a few
  seconds after `set_owner`, and this device's session with it goes
  stale until a fresh handshake. This retry-budget fix is still real and
  still worth keeping (a quiet bench genuinely does also trip the 30 s
  watchdog, independent of any reboot), but it is not what produced the
  `ack=none reply=none forever` pattern in the transcript above — that
  was the reboot the whole time. `FromRadio.rebooted` fires long before
  the 30 s silence timer ever could.

- **2026-09-06 — Confirmation fix round 4 / "reboot-session-loss" (same
  PR, bench finding AFTER the round-3 fix above, commit `b828c84`,
  against the same real puck + comms brain, Meshtastic 2.7.26, this time
  with frame-level TX/FromRadio/RX instrumentation).** The real root
  cause behind every round above's `ack=none reply=none forever` pattern
  on a push after the first one in a session:

  ```
  push1  TX set_owner id=…14 (ADMIN, want_ack)   TX get_owner_request id=…15 (want_response)
         FRAMELOG variant=11 ×3 (queueStatus)  FRAMELOG variant=2 → routing ACK for …14
         FRAMELOG variant=2 → ADMIN get_owner_response for …15  → confirmed ✓ (within 3 s)
  push2  (8 s later) TX set_owner …16, TX get_owner_request …17
         FRAMELOG len=2 variant=8            ← FromRadio.rebooted = true  : THE NODE REBOOTED
         …then only variant=11 (queueStatus) frames for every later TX; no ACK, no reply; the
         shell's link state never changes (bytes keep arriving so the no-RX watchdog never
         fires), so the puck never re-handshakes.
  ```

  Meshtastic's `AdminModule` saves the owner change and schedules a
  device reboot a few seconds later (`saveChanges()` → `rebootAtMsec()`,
  when no edit transaction is open). Push 1 is answered just before the
  reboot; push 2 lands in the reboot window and is lost. After the
  reboot the far side's `PhoneAPI` session is fresh and ignores this
  device's packets until a new `want_config` handshake — but this
  device's own `mc_client` kept treating the link as CONNECTED, because
  OTHER `FromRadio` traffic (`queueStatus` frames) kept arriving right
  through the reboot and kept resetting the 30 s no-RX-bytes watchdog.
  The comms brain applies every push correctly (confirmed via the CLI)
  — the owner data was never wrong; the puck's own session handling was.

  **Fixed**:
  1. `mc_client.c` (`meshclient`) now handles `FromRadio.rebooted`
     (tag 8) as an immediate session loss: drop straight into a fresh
     `want_config` handshake (a brand-new random nonce, never the
     pre-reboot one) rather than waiting on the 30 s silence timer that
     this exact scenario defeats. Observed downstream as the ordinary
     `on_state(HANDSHAKE)` → `on_state(READY)` sequence any other link
     drop produces — no second, reboot-specific event needed.
  2. `ff_shell.c`'s `get_owner_request` retry poll (`ff_shell_tick`) now
     only fires while the link reads CONNECTED — sending while it is
     down (mid-reboot) is guaranteed to fail, so the poll stays armed
     rather than attempting it. A self `NodeInfo` replay arriving as
     part of the reconnect's own `want_config` dump (before the link
     reaches CONNECTED) is a first-class confirmation on its own and now
     stops the poll immediately, the same way a `get_owner_response`
     already does — so a push commonly confirms via the replay with no
     extra request needed at all. The bench console's `name` output
     gains `link=<NONE|RECONNECTING|CONNECTED>` so an operator can tell
     "pending because the comms brain rebooted" apart from ordinary
     pending on a live link.
  3. Round 3's retry-budget fix is unchanged and still correct for its
     own (different, real) scenario — see the correction note above.

  **Tests**: `test_meshclient.c` gains
  `S03_debt_reboot_frame_after_ready_drops_state_and_reissues_want_config`,
  `S03_debt_reboot_stale_config_complete_is_ignored_per_handshake_rules`,
  `S03_debt_reboot_then_matching_config_complete_reaches_ready_again`
  (real wire encode/decode throughout — a `rebooted` frame drops READY
  to HANDSHAKE with a fresh nonce, a config_complete naming the STALE
  pre-reboot nonce is ignored per the same rule
  `S03_AC2_handshake_wrong_nonce_stays_in_handshake` already pins, and
  the full round trip reaches READY again). `test_shell.c` gains
  `S_name_push_during_reboot_window_polls_only_after_reconnect` and
  `S_name_two_pushes_eight_seconds_apart_both_confirm_across_a_mid_session_reboot`
  (the exact bench transcript's own timing). The first of the two
  `test_shell.c` tests **MUST FAIL** on the pre-fix code — mutation-
  verified: removing the link-CONNECTED clause from `ff_shell_tick`'s
  retry condition fails exactly it and the two-pushes test (2 failures)
  and nothing else in the 253-test suite; reverted after confirming (a
  stale incremental binary gave a false pass on the first attempt —
  rebuilding after touching the source reproduced the failure, per
  AGENTS.md's own stale-binary caveat).

  **Gates**: clang + gcc-14 sim builds green, zero warnings both.
  `ctest`: 74/74 both compilers (253-test `test_shell`, 95-test
  `test_meshclient`). `run_goldens.sh`: 90/90 fixtures, byte-identical —
  no UI changed. esp32s3 device build (`CONFIG_FF_DEBUG_CONSOLE=y`)
  compiles clean, zero warnings, **not flashed**. See
  `docs/hardware/comms-brain.md`'s "Root cause found: the comms brain
  reboots after every `set_owner`" section for the full mechanism and
  citation.

  **Honesty note on scope**: the reboot mechanism itself is bench-proven
  against real frame-level instrumentation (the transcript above is a
  real capture, not a reconstruction), and the meshclient/shell fixes
  are sim-provable by the tests listed. Not yet re-verified on a fresh
  bench run: that a push made during the reboot's own handshake window
  reliably confirms on REAL hardware (as opposed to the sim
  reproduction here) — the coordinator's next bench session should
  re-run the two-push, 8-seconds-apart transcript this fix targets.
  **Do not merge** — same bench-only caveat as the rest of this PR.

- **2026-09-07 — DIAGNOSTICS page (`feat/settings-diagnostics-page`):
  Settings gains a full-screen, read-only status dump, plus a matching
  `diag` bench-console command.** Jake's ask ("a debug type screen
  showing gps stats and meshtastic stats and such") lands as a new
  **DIAGNOSTICS** row (bottom of the DEVICE section, after CALIBRATE
  COMPASS) that opens a full-screen scrollable page with six sections —
  Link, Position (mine), Mesh, Time, Compass, Device — every fact
  showing "--"/"unknown" whenever its own `ff_app_diag_t` has_-flag (or
  enum-UNKNOWN member) says it was never observed, per this project's
  honest-data rule. BACK returns to the plain Settings list via the
  existing generic subview rule — no new intent case needed for it.

  **New/changed surfaces** (all `[api]`):
  - `ff_app_diag_t` (`app/include/ff_app_state.h`) — the page's own
    view-model struct, built ONLY while `subview ==
    FF_SETTINGS_SUB_DIAGNOSTICS` (the NAME-editor precedent, not CREW's
    "always built" one). Five small enums mirror lower-layer types
    field-for-field at the app/shell boundary (`ff_shell_link_t`,
    `mc_loc_source_t`, `ff_wall_trust_t`, and the esp32s3-only
    `ff_compass_mag_kind_t`/`ff_compass_imu_state_t`) rather than
    including those headers directly.
  - `FF_INTENT_SETTINGS_OPEN_DIAGNOSTICS` (`ff_intent.h`) — the row's
    bare intent, same "a Settings row, a bare intent, the shell decides"
    shape as CREW/NAME.
  - `mc_telemetry_t` / `mc_events_t.on_telemetry` (`meshclient`) — this
    library did not decode `TELEMETRY_APP` (portnum 67) at all before
    this; it now decodes the `device_metrics` variant only (channel
    utilization, air-util TX, battery level, uptime), silently skipping
    every other `Telemetry` variant, matching the existing "well-formed,
    nothing this library understands yet" precedent on ADMIN_APP.
  - `mc_position_t.sats_in_view`/`has_sats_in_view` (`meshclient`) —
    `Position.sats_in_view` was decoded nowhere before; same implicit-
    presence-folds-to-absent treatment as `precision_bits`.
  - `ff_shell_set_device_stats` (`ff_shell.h`) — a push API (mirrors
    `ff_shell_set_batt_mv`/`_set_heading`'s shape) for the two facts with
    genuinely no reading on the sim: free heap and compass chip/IMU
    identification. Wired into `app_main.c`'s render loop at a 2 s
    cadence (`FF_DEVICE_STATS_SAMPLE_PERIOD_MS`) — `heap_caps_get_free_
    size(MALLOC_CAP_DEFAULT)` plus the LAST periodic `ff_compass_status()`
    sample (no extra I2C transaction); honestly reports "no compass"
    when `CONFIG_FF_COMPASS` isn't compiled in at all.
  - `ff_shell_diag_debug` (`ff_shell.h`, debug-only) — the bench
    console's read: computes the SAME `ff_app_diag_t` the page renders
    (`shell_compute_diag`, shared by both callers) regardless of whether
    the DIAGNOSTICS sub-view is actually open, since a bench operator
    should not have to navigate the touchscreen first.
  - `diag` (bench console, `FF_DBGCMD_DIAG`) — zero-arg, prints the same
    facts as seven `dbg: diag ...` reply lines (Mesh splits across two,
    purely to stay inside `DBGCONSOLE_LINE_BUF` under GCC's
    `-Wformat-truncation` worst-case estimate — see that split's own
    comment, `ff_debug_console.c`). See `docs/hardware/comms-brain.md`'s
    Bench console section for the full line format and an example
    transcript.
  - `ff_build_info.h` (new) — `FF_BUILD_GIT_SHA`/`FF_BUILD_DATE`
    compile-time defines for the Device section's "firmware build id".
    No such identifier existed anywhere in this codebase before. Both
    `#define`s fall back to `"unknown"`/`__DATE__` when nothing overrides
    them; `firmware/app/CMakeLists.txt` (sim) and
    `targets/esp32s3/components/ff_app/CMakeLists.txt` (device) both
    supply the real values via `git rev-parse`/`git show` against
    whatever commit the checkout's `HEAD` points at, when a working git
    + repository is available — same value on both targets, since both
    build from the same checkout.

  **Interpretation call, recorded per CLAUDE.md/AGENTS.md**:
  `pos_broadcast_age_ms` (Mesh section) is the SAME timestamp as
  `pos_age_ms` (Position section), surfaced a second time — this puck
  never originates its own Position broadcast (that is the comms
  brain's job), so "how fresh is my fix" and "is my position still
  going out" collapse to the one observation this puck actually has.

  **Interpretation call #2**: the task brief's own parenthetical read
  as "Device (battery mV/%, uptime, firmware build id, free heap —
  device only; '--' on sim)" — taken literally, that would mean the
  WHOLE Device section reads unknown on the sim. This lands narrower,
  matching what is actually true fact-by-fact: `free_heap_bytes` and
  the Compass section's `mag_kind`/`imu_state` are genuinely device-only
  (nothing on the sim ever calls `ff_shell_set_device_stats`), but
  `uptime_s` is the shell's own clock and is equally real on both
  targets (`ff_shell_now_ms` since `ff_shell_init`), and battery/
  firmware-build-id are structurally available on the sim too (the ctl
  socket's `batt_pack_mv` command already pushes a real reading through
  `ff_shell_set_batt_mv` for exactly this kind of test; the git-derived
  build id is computed once, identically, for whichever target the
  checkout is built for). Reporting a genuinely-known uptime/battery
  value as "unknown" on the sim to match the parenthetical literally
  would itself be the dishonest option this project's own "honest data
  over pretty data" rule warns against — so only the two facts with NO
  possible reading on the sim are gated that way.

  **Tests**: `S_diag_all_unknown_when_nothing_observed` and
  `S_diag_reports_observed_facts` (`app/tests/test_shell.c`) — unknowns
  stay unknown, then every fact populates once genuinely observed,
  checked against BOTH the page's own projection and
  `ff_shell_diag_debug` (proving the two presentations agree).
  `S_diag_pos_age_keys_rendered_bucket_only` pins the render-key
  coarsened-age discipline (sub-bucket tick clean, bucket-crossing tick
  dirty) the spec calls for. **Fail-first proof**: temporarily reverting
  `shell_project_diag_page`/`ff_shell_diag_debug` to a no-op (not
  calling `shell_compute_diag` at all) was confirmed to fail all three
  tests before the fix, restored after confirming. `core/tests/
  test_dbgcmd.c` gains `dbgcmd_diag_parses`/`_with_extra_arg_rejected`;
  `app/tests/test_debug_console.c` gains
  `dbgconsole_diag_reports_unknowns_when_nothing_known`/
  `_reports_observed_facts` (seam-level: the command reaches
  `ff_shell_diag_debug` and prints what it says).

  **Goldens**: two new fixtures, `settings_diag_full`/
  `settings_diag_unknown` (`firmware/tests/fixtures/`) — fully populated
  and all-unknown, per this feature's own fail-first honesty proof.
  Adding the DIAGNOSTICS row to DEVICE shifted every row below it down
  by one `FF_SETTINGS_ROW_STEP` — see `docs/specs/S21-settings-rework.md`'s
  own entry for the affected `settings_*` goldens.

  **Layout finding, fixed same PR**: `settings_diag_full.json`'s first
  render showed the page's own back-circle glyph overlapping the "D" of
  "DIAGNOSTICS" — CREW/NAME's shared header geometry
  (`FF_CREW_BACK_Y`/`_HDR_Y`, puck-wide-centered title) has room for
  their own 4-char titles beside the back circle at that height, but not
  an 11-character one. Fixed with DIAGNOSTICS' own, slightly lower
  header constants (`FF_DIAG_BACK_Y`/`_HDR_Y`/`_LIST_Y`/`_LIST_H`) where
  the round glass is measurably wider, plus a toolbar-style title (fixed
  box starting right of the back button, internally centered) that stays
  clear of the back button by construction regardless of title length —
  not just at today's font/word.
