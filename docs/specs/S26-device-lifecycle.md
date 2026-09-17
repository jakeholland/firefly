# S26 — Device lifecycle: power, idle, notifications, home, boot

Status: draft (2026-09-01). Builds on [S25](S25-power-latch.md) (battery
latch, `ff_power`). Field test: Lost Lands, Sep 18–20 2026 — slices (a)–(d)
are the field-test cut; (e)–(g) follow after, informed by real use.

## Why

The puck now runs on battery (S25a) but has no lifecycle: it cannot be turned
off in software, the screen never sleeps (the display is the power hog), a
message that arrives is invisible unless you are already on Signals, and the
five-face swipe carousel is a *line* (Settings is four swipes from Radar) that
scales badly. There is **no** existing inactivity, screen-off, sleep,
notification, launcher, or boot-animation concept in core or app — this spec
is the contract for all of them.

## Memory reality (measured 2026-09-01, `idf.py size`)

Internal DIRAM is at **90.4 % — 32,917 B free**. Two static buffers hold
192 KB of it: `static jsmntok_t toks[8192]` in `fp_pack.c` (**131,072 B**, a
JSON parse scratchpad used only while the festpack parses at boot) and LVGL's
builtin heap pool (`work_mem_int`, 65,536 B). Every slice below costs RAM, so
reclaiming the token buffer is slice (a) and a prerequisite. Note: the nav
carousel already builds only the ACTIVE tile's content (issue #29), so a
launcher does **not** reduce LVGL peak — it is a UX change, not a memory one.

## Lifecycle state machine

```
OFF ──PWR press──▶ BOOT ──▶ ACTIVE ◀──────────── wake: touch · PWR · notification
 ▲                (latch,     │
 │                 splash)    ▼ idle t1        ▼ idle t2          ▼ idle t3 (slice f)
 │                          DIM ─────────▶ SCREEN OFF ─────────▶ LIGHT SLEEP
 │                                                                 (timer-wake)
 └── SYS_EN low ◀── Power off ◀── POWER MENU ◀──PWR long-press── ACTIVE
                                 {Power off · Reboot · Cancel}
```

House rule, same as S25: every decision is a **pure core FSM** (unit-tested,
fed ticks + levels); the esp32s3 target only samples pins and enacts
(backlight, GPIO7, sleep). No `if` about behavior in a screen or in
`app_main`.

## Nav model (slice e)

**AMENDED 2026-09-01 — maintainer decision after on-glass use.** The launcher
+ BOOT button work as originally cut below, but real use on the puck showed
the "Radar is the watchface" model to be wrong: it special-cased Radar in a
way that made the launcher a fragile, transient thing you passed *through*
rather than a place you could actually rest on. The model is now:

**The launcher IS home.** It is what BOOT always returns you to, and — since
this slice's field observation, not the original cut — it is also where the
device simply STAYS if you leave it there; it does not time out or hand
control back to Radar on its own. **Radar gets no special handling**: it is
an ordinary circle in the launcher, ranked no differently from Now/Signals/
Map/Settings, and it is reached the same way every other app is (a launcher
tap), not treated as a default destination. **BOOT (GPIO0) is the home
button**: from any app (Radar included) it returns to the launcher; from the
launcher itself it is a no-op (there is nowhere "home-er" to go). Picking a
circle enters that app full-screen. **The screen wakes to whatever base was
showing** when it went to sleep — the launcher if that's where BOOT last left
it, or an app if you were in one — never to a fixed "watchface" regardless of
where you were.

This retains the original cut's retirement of the horizontal carousel: with
one global nav gesture (a button), vertical drag is unambiguously scroll
everywhere, and horizontal is free for apps. GPIO0 is a normal input once
booted (only special at reset) and is free in the pin map.

**Renamed for the field (2026-09-01, same maintainer pass):** the "Now" face
is called **Lineup** and the "Signals" face is called **Inbox** everywhere a
user reads the name — launcher labels, screen headers, and this document.
This is a user-facing rename only: the code identifiers, files, and the S24
spec's own title (`S24-signals-inbox.md`) are unchanged, and so is "now" the
word (an item's freshness, e.g. `ff_fmt_age`'s "now" for an age under a
minute, or the Rally WHEN chip's "Now" meaning "right now" as opposed to
"+15m") — only the screen NAME changed.

**Code identifiers renamed to inbox/lineup on 2026-09-02** (mechanical
follow-up, no behaviour change): the paragraph above is now the HISTORICAL
record of the on-glass rename — the code identifiers and files it says were
"unchanged" have since caught up (`scr_signals.{c,h}` -> `scr_inbox.{c,h}`,
`scr_now.{c,h}` -> `scr_lineup.{c,h}`, `FF_APP_FACE_SIGNALS`/`FF_APP_FACE_NOW`
-> `FF_APP_FACE_INBOX`/`FF_APP_FACE_LINEUP`, etc.), while the S24 spec's own
title/filename and the time-word "now" remain exactly as described above.

**Visual: compass ring (2026-09-01, same maintainer pass — the maintainer's
pick off the design canvas).** The launcher's shipped 2-over-3 grid of five
uniform circles is replaced by a compass ring: Radar becomes a 120px HUB
disc at the puck's own center (not a sixth thing "reached" — it is drawn
inside the same launcher, still an ordinary circle per the nav model above,
just visually the middle one), and the other faces sit as 88px SATELLITE
discs on a 128px orbit around it. Satellite placement is **N-agnostic**: the
first satellite sits at the top (12 o'clock) and the rest step `360/N`
degrees clockwise, for whatever `N` real, routable apps exist — today `N=4`
(Inbox, Lineup, Settings, Map), which lands them on the four cardinal
points; a real fifth app added later (Music, on the design canvas's own
pentagon) becomes a one-line addition to that same computation, not a
redesign. **No dead tiles**: a circle that routes nowhere is not shipped
just to pre-fill a slot the design shows — the honesty rule (CLAUDE.md)
covers controls, not only data. This is a rendering change only:
`launcher_idx`/`FF_INTENT_LAUNCHER_SELECT`'s five values and
`ff_route_launcher_select`'s semantics are unchanged — see
`app/screens/scr_launcher.c`'s own top comment for the full geometry,
press-state, and icon-pipeline detail (drawn with LVGL primitives, not
image assets — no SVG rasterizer was available in the build sandbox).

### Pre-amendment model (superseded, kept for history)
The original cut of this slice made Radar the watchface: what the screen
woke to and rested on, with BOOT opening a transient launcher (a ring of Now
· Signals · Map · Settings — Radar was deliberately excluded, "it is home")
that auto-dismissed back to Radar after a short idle timeout, and that a PWR
long-press could replace with the power menu. None of that survives this
amendment: there is no watchface, no launcher timeout, and no replace-the-
launcher special case (the power menu now opens as a plain modal over
whichever base — launcher or app — is showing, the same as it does over any
other base).

**Download-mode guard:** GPIO0 held LOW during a reset enters the ROM
bootloader. A "Reboot" from the power menu therefore waits until BOOT reads
released before calling `esp_restart()`.

## Notifications (slice d)

`ff_flare_t`'s takeover (active / node / expiry / dismiss / coexists-with-lock)
is the seed. Generalise into core `ff_notify`: a small queue of
`{kind, tier, node, text, at_ms, expiry_ms}` with
**kind** ∈ MESSAGE · FLARE · RALLY · SYSTEM and **tier** ∈
- **BANNER** — transient, non-blocking strip at the top of whatever face is
  showing; auto-expires; tap opens the relevant thread.
- **TAKEOVER** — full-screen, demands a decision (GO / DISMISS). Flare stays
  here; this slice does **not** rewrite flare, it leaves the existing takeover
  untouched and adds BANNER for MESSAGE/RALLY. Folding flare in is later.

A notification **wakes the screen** (DIM/OFF → ACTIVE) — otherwise "come find
me" is useless while idle. Honest data: a banner shows the real `at_ms`
age via `ff_fmt_age`, never a fabricated "now".

**Placement — SUPERSEDED 2026-09-15 (puck-ux-usability-2026-09-15.md finding 4,
fix-plan slice 4).** Maintainer decision B (2026-09-02; refined by orchestrator
review round 2) put the BANNER strip on top of the status bar row (clock ·
MESH · battery — `RADAR_LAYOUT_STATUS_BAR_DY`), on the theory that a
transient banner should hide the LEAST valuable row on whatever face is
showing, and that row was less valuable than Radar's compass/close-range
readout or a thread's first message bubble. The 2026-09-15 usability review
measured the actual result and found that reasoning wrong:
`banner_on_radar.png` showed the clock truncated, `LINKED` entirely gone, and
the battery reduced to a bare `%` — "the two facts a user checks before
trusting the device — can it reach anyone and will it last — are hidden by
the notification that made them look." **The strip now sits BELOW the status
row instead, disjoint from its text** (`BANNER_CY = RADAR_LAYOUT_STATUS_BAR_DY
+ 33`, puck-local y≈84 — see `scr_banner.c`'s own layout comment for the full,
measured derivation, including the two corrections its `+ 40` first draft
needed once checked against the launcher's satellite ring and the thread's
first bubble, neither of which the original review's own worked example
checked). The extra room bought by moving down still pays for a wider strip
(160 → 200 px), which is what fixes the message-preview truncation the
review also named ("The Firefly To…").

The one place this move could not buy a fully clean trade: `scr_inbox.c`'s
thread view leaves only a 44 px band between the status text's real bottom
edge and the first message bubble's real top edge — one pixel short of the
banner's own 48 px hit-target floor with zero margin to spare on either
side. The strip is positioned to keep the status-row clearance genuine (a
real, non-zero gap) and accepts a small, deliberate, documented overlap with
the thread bubble's own decorative background instead — never its actual
text, which sits far enough below the bubble's own top edge to stay clear
(see `test_scr_banner.c`'s own doc comment for the exact numbers and the
"interpretation call" this trade is flagged as, per AGENTS.md).

Width and corner-clearance math (the `FF_THEME_GLASS_R`/`FF_THEME_GLASS_CX/CY`
chord check, `S26d_AC2_banner_corners_clear_glass_by_10px`) is unchanged in
kind from the original round-2 derivation — only the numbers moved, since the
centre is now lower (closer to the puck's own centre), which widens the
available chord rather than narrowing it. The age still sits beside the name
on the same row (not a separate corner chip), and at 200 px both the sender's
full demo name and a longer preview render before DOTS ellipsis has to step
in.

Widening the strip this much also reaches `scr_inbox.c`'s pinned BACK
button (`FF_INBOX_BACK_Y`/`_PX`) on the thread/picker/popup/rally
sub-views. Rather than either shrinking the strip back down (defeating the
readability fix above) or growing `scr_banner.c` face-aware knowledge of
`scr_inbox.c`'s internals, `scr_nav.c` — the one place that already
composes every face's content with the banner overlay — runs a shared
post-pass (`ff_scr_nav_mask_clickables_under_banner`, scr_nav.h) right
after building it.

**The masking rule (review round 3 correction):** an earlier pass masked
`LV_OBJ_FLAG_CLICKABLE` on ANY control the banner merely touched, on the
claim that LVGL's own top-z hit-testing already routes taps there to the
banner — measurably false for a PARTIAL overlap (the thread BACK button
is only 25 of its 44px width under the strip, leaving a real, visible
19px sliver LVGL would route straight to BACK). The rule is now honest:
a control is masked only when its UNCOVERED REMAINDER — the largest
rectangular piece of it left outside the banner
(`ff_scr_nav_rect_best_remainder`) — itself fails `FF_THEME_MIN_HIT_PX`
(44px) in either dimension, the same floor `test_face_hit_targets.c`
already holds every other control to; a control whose remainder still
clears 44px both ways keeps its clickability, since LVGL routes correctly
between it and the banner on its own. Checked against every real overlap
this repo ships: the thread BACK button's remainder (a 19px-wide sliver)
fails the width floor and stays masked; nothing currently produces a
"kept clickable" case.

**Launcher wiring:** the launcher (home) face now composites the banner too
— `ff_scr_launcher_build` calls `ff_scr_banner_build` last, the same
"built after, drawn on top" convention `scr_nav.c` uses for every other
face, then calls the SAME shared `ff_scr_nav_mask_clickables_under_banner`
pass rather than a second, launcher-specific rule. The banner only ever
reaches the top compass satellite (Inbox, `compass_pos == 0`, a 100×100
disc centred at the orbit radius (`LAUNCHER_ORBIT_RADIUS_PX`, 128) directly
above the hub); at the 2026-09-15 slice-4 geometry its uncovered remainder
splits into a 100×27px sliver above the strip and a 100×25px sliver below
it — both well under the 44px HEIGHT floor either way — so it stays masked,
same as before slice 4 moved the strip (only the exact remainder height
changed, not the outcome). Accepted as intentional and semantically
consistent: while a banner shows, that region IS the banner, and tapping it
opens the sender's thread, which is roughly where tapping Inbox would have
led anyway. Slice 4 ALSO had to clear the two neighbouring satellites
(Lineup/Music, `compass_pos` ±1) by the ordinary `FF_HIT_MIN_GAP_PX` (8px)
adjacency floor — a genuine near-miss the wider (200px) strip introduced,
fixed by the same `BANNER_CY` placement described above rather than a
launcher-specific carve-out (`test_face_hit_targets.c`'s whole-device sweep
is what caught it; see `scr_banner.c`'s own comment for the exact numbers).
The launcher's own status row (bottom of the puck, `LAUNCHER_STATUS_ROW_DY`)
is far enough from the banner's position to never compete with it. Launcher
renders WITHOUT an active banner are untouched (goldens byte-identical).

## Slices + acceptance criteria

### (a) Reclaim the festpack token buffer — `[api]`
`fp_pack.c` is pure C11 (no ESP allocators, no `EXT_RAM_BSS_ATTR`), so the
caller supplies the scratch: `fp_pack_parse(..., jsmntok_t *toks, int ntoks)`
(exact signature per the existing parse entry point; keep a thin wrapper only
if the sim/tests need it). The esp32s3 target allocates `FP_MAX_TOKENS * sizeof
(jsmntok_t)` (8192 tokens = 128 KB when this was written; 16384 = 256 KB since
the 2026-09-16 S05 amendment) in **PSRAM** (`MALLOC_CAP_SPIRAM`) around the demo parse and
frees it after (transient — it is parse-time only).
- **AC1** `static jsmntok_t toks[...]` is gone from `fp_pack.c`; `idf.py size`
  shows `.bss.toks` absent and DIRAM free ≥ 150 KB (record the number in the
  PR).
- **AC2** Parse behaviour is byte-identical: all festpack unit tests + goldens
  pass unchanged; a too-small `ntoks` returns the existing "too many tokens"
  error rather than overrunning (unit test).
- **AC3** Pure-core rule intact: `fp_pack.c` gains no platform include.

### (b) Power button → power menu → soft power-off (S25b)
Core `ff_power_fsm`: fed `(now_ms, pwr_level)`; emits SHORT_PRESS,
LONG_PRESS (≥ 1500 ms — well under the ~6 s hardware force-off), and RELEASE.
Debounce 30 ms. Target: poll GPIO6 (PWR, reference-driver sense: the boot-time
held read is LOW; **verify the runtime active level on glass and log it**) on
the existing tick; `ff_power_off()` drives GPIO7 low. Shell: LONG_PRESS opens a
**Power menu** modal (Power off · Reboot · Cancel — big round-glass buttons,
press states, S24 render-key discipline); Power off → `ff_power_off` +
backlight 0; Reboot → wait for BOOT released, then `esp_restart`; Cancel /
BOOT / timeout 10 s → back. SHORT_PRESS while ACTIVE = no-op this slice
(reserved: screen off in (c)).
- **AC1** `ff_power_fsm` unit tests: debounce, short vs long threshold at the
  boundary, a held press emits LONG exactly once, release after long does not
  also emit SHORT.
- **AC2** Power-menu golden (sim) + press-state coverage; menu is opaque in the
  render key.
- **AC3** On glass: hold PWR ~1.5 s → menu; Power off → puck turns off
  (battery) and the log shows SYS_EN low. USB: same menu, board stays up.
- **AC4** Reboot never enters download mode (BOOT-release guard; unit-testable
  in the FSM as a "reboot pending until GPIO0 high" state).

**AMENDED 2026-09-16, field-hardening ahead of Lost Lands** (the printed
case's physical PWR button does not actuate reliably): the power menu is
also reachable from Settings → POWER, at the bottom of the list.

### (c) Inactivity → dim → screen off
Core `ff_idle`: `(now_ms, input_event)` → ACTIVE · DIM · OFF with
`t_dim = 15 s`, `t_off = 30 s` (constants in core, later a setting). ANY
input (touch, PWR, BOOT) resets to ACTIVE; a notification (d) resets to ACTIVE.
Target enacts: DIM = backlight to `FF_BL_MIN_PCT`, OFF = backlight 0 and
**skip rendering** (the LVGL tick still runs; no face rebuilds while OFF — a
dirty view is rebuilt on wake). Wake restores the stored brightness. PWR
SHORT_PRESS while OFF = wake; while ACTIVE = go OFF immediately (this is where
(b)'s reserved short-press lands).
- **AC1** `ff_idle` unit tests: transition times, reset-on-input from each
  state, no transition while an FSM-declared "keep awake" holds (flare
  takeover pending, power menu open, calibration running).
- **AC2** Brightness round-trip: wake restores exactly the pre-dim
  `brightness_pct` (unit + on-glass).
- **AC3** OFF skips face rebuilds (assert the render loop's rebuild count is 0
  across an OFF window in the sim ctl harness).

**AMENDED 2026-09-02 — "a tap on a dim/off screen shouldn't also tap
whatever's behind it"** (maintainer decision, on-glass bug report). ANY
input resetting to ACTIVE (above) was correct but incomplete: a touch or
BOOT press that WOKE the screen was also being delivered to the UI as an
ordinary tap — the wake and the tap were the same physical gesture,
landing on whatever button happened to be under the finger when the
screen was dim/off/asleep. The fix: **a touch or button press that
begins while the screen is not ACTIVE is a wake-only input and is never
delivered to the UI.** The press still wakes the screen (unchanged); the
entire gesture — every sample from press to release — is withheld from
the UI, so no button highlights, no click fires, no navigation happens.
A press that begins while the screen IS already ACTIVE is delivered
normally, including one that continues to be held as the screen later
DIMs under it (state matters only at the instant the press begins, not
for the rest of the gesture — this is what makes a legitimate long-press
or drag survive a mid-gesture dim). Implemented as a pure gate,
`ff_idle_touch_gate` (core/include/ff_idle.h `[api]`), consulted from
both the touch read path (`ff_display_touch_start`/
`ff_display_touch_set_idle`, targets/esp32s3/components/ff_display/
ff_display.c) and the BOOT-as-home debounce (app_main.c) — each input
source gets its own `ff_idle_touch_gate_t` latch instance
(`ff_idle_touch_gate_t`'s own doc comment). PWR SHORT_PRESS already only
ever wakes (or force-offs) with no other side effect
(`ff_idle_short_press`, unchanged by this amendment) — confirmed, not
altered. The sim's ctl pointer indev (`targets/sim/ctl_loop.c`) mirrors
the same gate for its own AC3-style harness test.

**AMENDED 2026-09-04, fix/flare-cancel-taps — "the finger-down rebuild
gate must be atomic with the touch it reads, not just present."**
Maintainer report, on glass, after a 5x HOME quick flare: a CANCEL tap
"seems to pass through" and needs multiple attempts. Three of the four
independent root causes behind that report belong to other specs (the
sender overlay's own render-key churn and dim-catcher clickability are
S10 concerns — see `docs/specs/S10-flare.md`'s own Amendment on this
fix); this device-lifecycle spec owns the fourth, and it is the one
this AC3 rebuild-gate paragraph already half-states: `rebuild_pending &&
!ff_display_touch_is_down() && !screen_blank` (`app_main.c`) IS the
correct gate — but before this fix, `!ff_display_touch_is_down()` was
read OUTSIDE the `ff_display_lock()` the rebuild a few lines later
actually takes, not atomically with it.

`ff_display_lock`/`_unlock` are bare aliases for esp_lvgl_port's own
`lvgl_port_lock`/`_unlock` — the exact mutex esp_lvgl_port's own task
holds for the ENTIRE duration of every `lv_timer_handler()` call it
makes, which is where the touch read callback
(`ff_touch_gate_read_cb`, targets/esp32s3/components/ff_display/
ff_display.c) actually runs (LVGL's indev read timer fires from inside
`lv_timer_handler`). So a real window existed between the check and the
lock: this task reads "no finger down" as true, then — before it reaches
`ff_display_lock()` — the port task runs a `lv_timer_handler()` pass
that processes a NEW physical press (`s_touch_raw_down` -> true,
PRESSED delivered into LVGL), and this task then takes the lock and
rebuilds anyway, destroying the button that had just gone pressed —
`rebuild-mid-tap` happening despite the gate that exists specifically to
prevent it, because the gate's own check-then-act was not atomic with
the fact it was checking.

**The rule, stated for future gates of this shape:** any decision that
reads LVGL/touch state and then acts on the LVGL tree must take
`ff_display_lock()` BEFORE the read and hold it through the act, not
merely before the act — because the ONE thing that can invalidate the
read (the port task's own touch processing) runs only inside a
`lv_timer_handler()` call already serialized against this exact lock.
Applied here: `app_main.c` now takes `ff_display_lock()` first, then
re-checks `!ff_display_touch_is_down()` and does the whole
`lv_obj_clean()`+`ff_face_build()` while still holding it, making the
check and the port task's own touch processing mutually exclusive for
the whole gated section.

Not reproducible in the sim (`targets/sim/ctl_loop.c`/`sim_lifecycle.c`
read `ctx->pointer_state` directly, single-threaded, no cross-task
handoff, per `sim_lifecycle.h`'s own "RAW pointer-down truth" doc
comment on `finger_down` — there is no lock, and no race, to close
there) — verified instead by code reading against esp_lvgl_port's
documented lock contract and a zero-warning ESP-IDF device build
(`idf.py build`, `firmware/targets/esp32s3`).

**AC named to this fix:** the device rebuild gate's finger-down check
and the rebuild it gates are one atomic, lock-held operation
(`app_main.c`, code-reading + device build, per the paragraph above —
no automated test possible on this target without hardware-in-the-loop
touch injection).

**AMENDED 2026-09-07 — "DIM is visible; don't eat the next tap after a
normal reading pause"** (maintainer decision, on-glass bug report,
fix/dim-touch-delivery). Bench trace on Jake's puck: the screen woke to
100% backlight with NO touch delivered to the UI, then dimmed 15 s
later — twice. Root cause: the 2026-09-02 amendment above ("a touch or
button press that begins while the screen is not ACTIVE is a wake-only
input and is never delivered to the UI") swallowed a press-begin at
**DIM** exactly the same way it swallowed one at OFF or SLEEP — but
`FF_IDLE_T_DIM_MS` is only 15 s, and at DIM the screen is still fully
readable (minimum backlight, not dark). A wearer who simply pauses to
read for 15 s — an entirely ordinary interval, not an edge case — had
their very next tap silently eaten, indistinguishable on glass from
"the touchscreen doesn't work." OFF and SLEEP have a real screen (dark)
to protect against an accidental tap on hidden UI; DIM does not — the
wearer can see exactly what they're about to tap.

**The fix:** at **DIM**, a press that begins is now delivered to the UI
normally, AND wakes the screen (restores brightness) — the same
gesture does both, same as it always has at DIM's neighbor states, just
without the withholding. At **OFF** and **SLEEP** (screen dark),
behavior is unchanged: wake-only, the whole gesture withheld until
release. The "decision made once, at press-begin" semantics from the
original amendment are unchanged — a press that begins at ACTIVE and
continues into DIM was already delivered before this amendment (state
matters only at press START) and still is; this amendment only changes
what happens when a press *begins* while already at DIM.

Implemented as a one-line change to the SAME pure gate,
`ff_idle_touch_gate` (`core/include/ff_idle.h`/`.c` `[api]`) — the
begin-time branch now checks for OFF/SLEEP specifically (swallow) rather
than "not ACTIVE" (swallow), with DIM falling through to the same
deliver-and-wake path ACTIVE's own begin-sample already took the "no
wake needed" half of. Both consulting call sites inherit the new
behavior automatically, with no call-site changes needed: the touch
read path (`ff_display_touch_start`/`ff_touch_gate_read_cb`,
`targets/esp32s3/components/ff_display/ff_display.c`) and the
BOOT-as-home debounce (`app_main.c`, `s_boot_gate`) — a BOOT press that
begins at DIM now also acts as HOME and wakes, same rule as touch,
since the amendment's premise ("nothing to protect against on a
readable screen") applies equally to the physical button; no reason
found for BOOT to differ. The sim's ctl pointer indev
(`targets/sim/ctl_loop.c`) mirrors the same gate and needed no logic
change, only comment updates.

**New permanent diagnostic** (`ff_touch_gate_read_cb`,
`targets/esp32s3/components/ff_display/ff_display.c`): a swallowed
press-begin (now only possible at OFF/SLEEP) logs one INFO line, `touch
swallowed (wake-only) @ (x, y) state=<OFF|SLEEP>`, rate-limited to one
per 200 ms — the same rate-limit window `ff_touch_press_log_cb` already
used for delivered presses — so the wake-only leg of this contract stays
observable on the bench console going forward instead of being silent
by design. A second, independently rate-limited line, `touch poll
skipped: i2c bus busy`, covers the callback's pre-existing I2C-bus-lock
failure path (`ff_display_i2c_bus_lock`'s own doc comment has the bus-
contention background): a failed lock reports "no touch" for that poll,
indistinguishable downstream from a genuine release, so this makes lock
contention visible rather than silently masquerading as a spurious
finger lift.

**Tests:** `firmware/core/tests/test_idle.c` — the DIM leg of the
wake-only touch-gate suite now asserts delivery from the first sample
(renamed `S26_wakeonly_AC_press_during_dim_delivers_from_first_sample_
and_wakes`; previously asserted the opposite, swallow-until-release);
confirmed fail-first against the pre-amendment gate before this fix
landed. The OFF and SLEEP legs (unchanged behavior) and the
ACTIVE-begins/continues-into-DIM leg (`..._stays_delivered_through_
dim_transition`) were already covered and continue to pass unmodified.
Sim integration: `targets/sim/tests/test_wakeonly_touch.c` gained its
own dedicated DIM case (`S26_wakeonly_dim_tap_delivers_from_first_
sample_and_wakes` — a real launcher tap through a live `ff_ctl_loop_*`
session, asserting BOTH the LVGL PRESSED style and the resulting
navigation on the first press) split out from the shared OFF/SLEEP
harness (renamed `run_wakeonly_gated_case`), since DIM now asserts the
opposite outcome from that harness's shared logic. No golden changed
(92/92 byte-identical against committed goldens) — this amendment
changes input delivery timing, not any rendered pixel.

**AMENDED 2026-09-07, fix/tap-lost-midpress-rebuild — "most taps take a
few tries" on Inbox/Compose/Signals; the finger-down rebuild gate was
innocent.** Maintainer bench trace, Jake's puck on main `72324ab`,
logging in the gesture glue's indev event callback (`LV_EVENT_ALL` on
the indev, `app/ff_gesture_glue.c`):

```
INBOX   (187,160) dur=184ms move=0px -> RELEASED            (no click)
INBOX   (143,167) dur=189ms move=0px -> RELEASED            (no click)
INBOX   (199,181) dur=119ms move=0px -> RELEASED SHORT_CLICKED CLICKED
COMPOSE (215,168) dur=185ms move=0px -> RELEASED            (no click)
COMPOSE (216,166) dur=191ms move=0px -> RELEASED            (no click)
COMPOSE (210,168) dur= 40ms move=0px -> RELEASED SHORT_CLICKED CLICKED
COMPOSE ( 78,108) dur=221ms move=0px -> RELEASED            (no click)
COMPOSE (101,261) dur=188ms move=0px -> RELEASED            (no click)
COMPOSE (104,261) dur=189ms move=0px -> RELEASED            (no click)
COMPOSE ( 97,262) dur=240ms move=0px -> RELEASED SHORT_CLICKED CLICKED
COMPOSE ( 78,213) dur=390ms move=0px -> RELEASED            (no click)
COMPOSE ( 74,209) dur=429ms move=0px -> RELEASED            (no click)
COMPOSE (224,216) dur=280ms move=0px -> RELEASED SHORT_CLICKED CLICKED
```

Roughly half of motionless taps produced RELEASED with no CLICKED. No
`touch swallowed` and no `i2c bus busy` lines fired, and the gesture
engine's own poll (every 20 ms) saw a continuous PRESSED indev state
throughout every one of these — the touch input itself was fine; LVGL's
internal `pointer.pressed` bit was the thing going false.

**First, what this ISN'T (verified, not assumed):** the coordinator's
initial reading suspected the SAME class of bug the 2026-09-04 amendment
above fixed — something tearing down and rebuilding the widget under a
still-held finger, either via a race in the finger-down rebuild gate or
a second, ungated `lv_obj_clean`/`ff_face_build` call site outside it.
Both were run down and ruled out:

  - Every `lv_obj_clean`/`ff_face_build`(`ff_build_face_screen`) call
    site in both targets was enumerated (grep, not sampling) — the ONE
    per-frame gated call in `app_main.c` (mirrored exactly by
    `ff_sim_lifecycle_pump`, `targets/sim/sim_lifecycle.c`) is the only
    call site reachable during ordinary use; the others are one-shot
    boot/calibration-recovery builds that cannot race a live tap.
  - The gate's own lock discipline was re-verified directly against the
    vendored `esp_lvgl_port` 2.9.0 source
    (`managed_components/espressif__esp_lvgl_port/src/lvgl9/
    esp_lvgl_port.c`): `lvgl_port_task`'s loop calls `lv_indev_read()`
    (where the touch read callback sets `s_touch_raw_down`) and
    `lv_timer_handler()` back to back under the SAME non-blocking
    `lvgl_port_lock(0)` — confirming the 2026-09-04 amendment's own
    claim, not just trusting its comment.
  - `firmware/targets/sim/tests/test_ctl_rebuild_under_finger_screens.c`
    (new) generalizes `test_ctl_rebuild_under_finger.c`'s launcher-only
    proof to the three faces this report actually named — an Inbox
    row, a Compose T9 key, and a Settings row, each held through a
    genuine dirty tick (an inbound message from a freshly-paired
    sender, the SAME producer the existing launcher test uses) —
    asserting object identity and PRESSED survive, and CLICKED still
    lands on release. All three **pass on main, unmodified** — the
    finger-down rebuild gate already generalizes correctly; it was
    never this report's cause.

**The real root cause: `ff_scr_button_create` (`app/screens/scr_nav.c`)
clears `LV_OBJ_FLAG_PRESS_LOCK` on every button in the app, and LVGL's
per-poll re-hit-test has zero tolerance for it.** `indev_proc_press`
(vendored `lvgl__lvgl` 9.5.0, `src/indev/lv_indev.c`) re-runs
`pointer_search_obj()` at the indev's CURRENT point on every ~33ms poll
whenever the pressed object lacks `PRESS_LOCK` — not gated on how far
the point moved, just "what is under this exact pixel right now". If
that search ever returns anything other than the object already tracked
as pressed — even for ONE poll, even if the very next poll finds the
SAME object again — LVGL treats it as "a new object was found" and sets
`indev->pointer.pressed = (indev->prev_state == RELEASED)`. Mid-hold,
`prev_state` is PRESSED, so this evaluates false and **stays** false for
the rest of that touch (it can only become true again on a fresh
RELEASED→PRESSED transition) — so `indev_proc_release`'s `if
(scroll_obj == NULL) { if (pointer.pressed) { deliver CLICKED } }` never
fires, even though the SAME widget is exactly what the finger is resting
on at release. No rebuild, no deletion, no second object even alive:
`act_obj` at release time correctly reads back the original button.

`ff_scr_button_create` clears PRESS_LOCK deliberately, for a real and
separately-necessary reason (#145/#148, the launcher-hub/compose-keypad
slide-off-cancels-a-tap fix, generalized into this one choke point): "a
press that starts on FLARE or Power-off and slides away before lifting
must never commit" (see `scr_nav.h`'s own doc comment, pre-amendment).
LVGL's `PRESS_LOCK` is the only flag governing this, and it is binary —
set, and ANY excursion (a deliberate 150px slide, or a single-poll 2px
wobble) is tolerated and still commits on release; cleared, and NEITHER
is. The #145/#148 fix chose "clear it, tolerate nothing" to correctly
stop the slide; the untested side effect is that it ALSO stops a
control from surviving a touch-controller's own raw coordinate noise —
which matters most exactly where a press is most likely to sit, near a
control's edge rather than dead-center, and exactly on a puck that ships
"touch uncalibrated by default" (S21's own honest-data rule,
`firefly-touch-cal-default.md`) rather than pre-loading a factory
calibration that would otherwise narrow this noise.

**The fix:** `ff_scr_button_create` now leaves `LV_OBJ_FLAG_PRESS_LOCK`
SET (LVGL's own default) — so the zero-tolerance re-search above never
runs at all for an already-pressed button — and enforces slide-off-
cancels-a-tap EXPLICITLY and TOLERANTLY instead: a new PRESSED/PRESSING
event pair on the button itself tracks the touch's down point and calls
`lv_indev_wait_release()` — the SAME function `app/ff_gesture_glue.c`
already uses for the BACK/HOME edge-swipe gestures — the first time
total displacement from that down point exceeds
`FF_SCR_BUTTON_SLIDE_CANCEL_PX` (12px; reuses `ff_gesture_cfg_t.
long_slop_px`'s own precedent value and reasoning — core/include/
ff_gesture.h's G3 doc comment — "how much wobble is still basically a
stationary press", made once already for the exact same class of noise
on the exact same glass). `lv_indev_wait_release` makes every remaining
`indev_proc_press` call for that touch a no-op and fires `PRESS_LOST`
(never `CLICKED`) on release (`lv_indev.c`'s own `wait_until_release`
handling) — the identical observable "never commits" outcome the old
PRESS_LOCK-clearing produced for a real slide, just with a 12px floor
under it instead of a single raw pixel. Full mechanism and history:
`scr_nav.h`'s doc comment on `ff_scr_button_create`.

One collateral finding, caught by `test_face_hit_targets` rather than
assumed away: adding the new PRESSED/PRESSING callbacks BEFORE the
existing tap-sound `CLICKED` callback regressed that sweep from 0 to 54
adjacency-floor violations against real, committed fixtures. Cause:
the sweep's own "are these two pills really ONE composite control"
exclusion (`sweep_same_composite_control`) identifies a button purely by
`lv_obj_get_event_dsc(obj, 0)` — event descriptor INDEX 0's callback
and user_data — and every button in the app happens to share the exact
same tap-sound callback (`ff_scr_button_tap_sound_cb`, always `NULL`
user_data) at that index, which is what lets a toggle row's two pills
(sharing one real `cb`) register as one composite control at all.
Adding the new tracking callbacks FIRST put a per-button-unique
`malloc`'d pointer at index 0 instead, which — correctly, by the
sweep's own rule, but not the intended effect — made no two buttons in
the app look like the same composite control anymore. Fixed by keeping
the tap-sound `CLICKED` registration first (`scr_nav.c`'s own comment
on the ordering has the full derivation); the new tracking callbacks
register after it and are unaffected by their own position, since LVGL
dispatches by event code, not by index.

**Tests** (fail-first, confirmed against a temporarily-reverted
`scr_nav.c`/`scr_nav.h` before landing the fix, then confirmed green
after):
  - `firmware/app/screens/tests/test_scr_intent.c`'s new
    `S26_compose_key_survives_a_tiny_edge_jitter_and_still_commits` —
    presses the Compose DEF key 3px inside its own top edge (near the
    boundary, not dead-center — where the bug needs a press to start),
    jitters 5px total (2px PAST the edge into the inter-key gap, then
    straight back to the down point — comfortably under the 12px
    tolerance above), releases at the SAME point it started. **Fails on
    main** (`s_spy.count` reads 0); **passes** with the fix.
    Unmodified, this file's whole existing `S99_compose_drag_off_*` /
    `S26e_launcher_drag_across_satellites_emits_nothing` /
    `PL_*_drag_off_*` family (every real 150px+ drag-off proof from
    #145/#148's own generalization) still passes — a genuine slide is
    still cancelled, only a few-px in-place jitter now survives.
  - `firmware/targets/sim/tests/test_ctl_rebuild_under_finger_screens.c`
    (new, described above) — not fail-first for THIS bug (the finger-
    down rebuild gate was never broken), but locks in, as permanent
    regression coverage, the property the coordinator's initial
    diagnosis asked to verify: an Inbox row / Compose key / Settings
    row survives a genuine mid-press dirty tick with its object
    identity and PRESSED state intact, and its CLICKED still lands on
    release.

**Gates:** clang and gcc-14 sim builds, zero warnings; `ctest --test-dir
build` 75/75 (74 pre-existing + the 2 new files above; one pre-existing
test, `test_scr_intent`, gained the one new `RUN_TEST` line), on both
compilers; no golden changed (`test_png_diff` green on both builds —
this fix changes input-event handling, not any rendered pixel);
`idf.py build` (`firmware/targets/esp32s3`, sdkconfig copied from the
real device config plus `CONFIG_FF_DEBUG_CONSOLE=y`/
`CONFIG_FF_COMPASS=y`, built to a scratch directory, not flashed) clean.
Does not touch `scr_settings.c`'s DIAGNOSTICS section or its scroll
persistence (a concurrent, unrelated fix on `fix/diag-scroll-persist`
owns that file's diagnostics area) — this fix's only settings-adjacent
test presses the DISPLAY section's CLOCK toggle instead.

**AMENDED 2026-09-09, fix/s31-music-idle-drain — "Music must never
override the idle policy."** Slice (c)'s own keep-awake list above (AC1:
"flare takeover pending, power menu open, calibration running") never
named Music because Music was never supposed to be a keep-awake source
at all — but S31's own implementation (`docs/specs/S31-music-swarm.md`)
quietly added one anyway: a loudness-gated branch on `ff_shell_keep_awake`
that, per bench evidence (Jake's puck, main `51c5d16`, left on Music
overnight on USB), never actually released in an ordinary room — no DIM
at 15s, no OFF at 30s, for 6.6 HOURS straight, screen at 90% the whole
time. Root cause and full writeup: `docs/specs/S31-music-swarm.md`'s own
"Power policy" > "Keep-awake (REMOVED, 2026-09-09 amendment)" section.
Fix: that branch is deleted outright. Music now dims at `t_dim` and
turns off at `t_off` exactly like every other face this slice already
covers — the launcher's own "does NOT keep awake" precedent (this
slice's amendments above) extended to a second face, this time by
REMOVING a special case rather than by never having added one. Slice
(c)'s own keep-awake source list (AC1) needed no correction: it was
right all along; S31's own doc is where the stale claim lived and is
where it has been fixed. Regression coverage: a unit test
(`fix_s31_keep_awake_false_for_music_face_regardless_of_loudness`,
`app/tests/test_shell.c`) and a sim ctl-harness reproduction of the
overnight case itself (`targets/sim/tests/test_ctl_music_idle_drain.c`).

### (d) `ff_notify` + message banner
Core `ff_notify` as above (queue depth 4, FIFO, expiry, `dismiss`, `pop`).
Shell: an incoming MESSAGE / RALLY (paired sender) enqueues a BANNER; the
active face renders the banner strip on top (a new `scr_banner` overlay, not a
face); tap → **the conversation the message belongs to** (S24 — see AC5
below and this section's 2026-09-03 Amendment for the full rule and the
bug it fixes) and marks read; auto-expire 6 s; enqueue wakes the screen via
(c). Flare takeover untouched.
- **AC1** `ff_notify` unit tests: FIFO, overflow drops oldest, expiry, dismiss,
  a duplicate (same node+kind within 2 s) coalesces.
- **AC2** Banner golden on Radar + on a thread; opacity in the render key;
  press state.
- **AC3** Unpaired sender never produces a banner (S22 stranger rule).
- **AC4** Field build carries no demo-only banner content (nm check, S23 AC4).
- **AC5** (2026-09-03 amendment) A banner's tap opens the S24 `ff_inbox`
  CONVERSATION the underlying message was filed under — the CREW thread for
  a broadcast/group message, the sender's own 1:1 thread for a message
  addressed directly to this device — never routed by sender identity
  alone. Mark-read on open hits that same conversation. See this section's
  Amendment below for the full rule statement, the bug, and the tests it
  names.

### (e) Home button + launcher — `[api]`
Target: GPIO0 sampled like GPIO6 (same debounce module); core: `ff_route`
gains HOME semantics per the nav model above; a new `scr_launcher` face (grid
of app circles, ≥ 56 px targets — the shipped build uses 96 px — on round
glass, press states). Remove the carousel swipe from `scr_nav.c` (LEFT/RIGHT
no longer change faces); page dots go. Long-press-anywhere → Settings is
retired (Settings is a launcher circle).

**AMENDED 2026-09-01**, per the nav model's own amendment above — ACs below
are the CURRENT contract, not the original cut's:
- **AC1** `ff_route` tests: init base == launcher; HOME from every base
  (Radar included, no special case) → launcher; HOME on the launcher → no
  change; selecting a launcher circle (Radar included) → that base; a live
  modal (Compose, power menu) or a takeover suppresses HOME; the power menu
  opens as a plain modal over the launcher base and Cancel reveals the
  launcher again (no launcher timeout, no replace-the-launcher special case —
  both retired with the model that needed them).
- **AC2** Launcher golden (five circles, no privileged member); hit-target
  sweep green.
- **AC3** No horizontal swipe changes face (sim indev test).

### (f) Light sleep — timer-based
After OFF + `t_sleep = 120 s`: `esp_light_sleep_start` with a **timer wake**
(1–2 s) plus touch-INT and PWR GPIO wakes. **Not** UART-wake: the RX bytes
that trigger a wake are lost and the XIAO runs stock Meshtastic (no preamble).
Each timer wake services the link and returns to sleep unless input arrived.
- **AC1** Sleep entry/exit leaves LVGL + touch functional (on glass: sleep,
  tap → wakes to Radar with correct brightness).
- **AC2** Not entered while any keep-awake holds (reuses (c)'s predicate).

**AMENDED 2026-09-02 — "don't enter light sleep while USB is connected"**
(maintainer decision). Light sleep is inhibited while USB is connected: the
ESP32-S3's native USB-Serial/JTAG powers down during light sleep, so the
host loses the port the moment the screen sleeps — every dev/flash session
tethered over USB was breaking on the very cadence this slice introduced.
USB-powered operation is also not battery-limited, so there is no cost to
staying awake while connected. Dim/off still apply exactly as before — this
only withholds the OFF → SLEEP transition itself; a USB-tethered puck sitting
idle still dims at `t_dim` and blanks the screen at `t_off`, it just never
stops answering the host. Implemented as a second, independent input to
`ff_idle_tick` (`sleep_inhibit`, core/include/ff_idle.h `[api]`) — distinct
from `keep_awake`: it does not force ACTIVE and does not re-pin the idle
reference, so DIM/OFF timings are unaffected; once USB disconnects, SLEEP is
entered as soon as `t_sleep` has actually elapsed from the same unmoved
reference (immediately, if it already had). The esp32s3 target samples
`usb_serial_jtag_is_connected()` (`driver/usb_serial_jtag.h`) once per frame
and passes it straight through — no behavior `if` outside core. That
connection monitor is backed by the host's USB SOF packets, not merely VBUS
power, and is already linked into this build (this project's sdkconfig sets
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED=y`, which is what
`esp_driver_usb_serial_jtag`'s own CMakeLists.txt force-links the connection
monitor on) — no new Kconfig, no `usb_serial_jtag_driver_install()` call.

**AMENDED 2026-09-16 — "on battery the screen went black and tapping the
screen didn't wake it; had to use PWR"** (owner field report). Root cause,
confirmed by reading `ff_display.c`'s own S15b comment and by bench
evidence below: the SPD2010 touch controller is POLLED, not interrupt-
driven, on this board — the ONLY way a tap can wake the device from light
sleep is the periodic TIMER wake sampling the controller right after each
wake. At the steady-state 1500ms period this slice originally used, an
ordinary short tap (well under a second) can land entirely BETWEEN two
timer wakes and never be sampled — only a HELD press (>= one full period)
was guaranteed to still be down at the next wake's poll. This was
invisible on the bench because light sleep is inhibited while USB is
connected (the amendment above) — every prior bench session was, by
construction, never exercising this path at all.

**The fix:** `ff_idle_light_sleep_timer_ms` (`core/include/ff_idle.h`
`[api]`) — a pure function of "how long has the device been asleep" —
shortens the timer-wake period to `FF_IDLE_LIGHT_SLEEP_FAST_TIMER_MS`
(300ms) for the first `FF_IDLE_LIGHT_SLEEP_FAST_WINDOW_MS` (5 minutes)
after SLEEP is entered — the window a wearer who just set the puck down
is statistically most likely to still be interacting with it — then backs
off to `FF_IDLE_LIGHT_SLEEP_SLOW_TIMER_MS` (1500ms, the original spec
value) for the rest of the sleep. Deterministic and unit-tested (short,
then long — `S26f_fix_timer_ms_*`, `core/tests/test_idle.c`), independent
of whether touch-INT ever fires. `app_main.c` computes "ms since SLEEP was
entered" from `ff_idle_t.ref_ms` (the struct is fully-defined, not opaque)
and reprograms `esp_sleep_enable_timer_wakeup()` fresh before every light-
sleep cycle. This does not change touch delivery or the wake-only rule at
all (the waking touch is still never delivered as a tap) — only how OFTEN
the existing timer-wake-plus-poll mechanism samples the controller.

**Estimated battery cost** (published ESP32-S3 figures, NOT a bench
measurement — no ammeter was available for this fix; state this
explicitly rather than fabricate precision): light-sleep current on this
chip is on the order of several hundred µA to ~1 mA depending on which
domains stay powered (this build forces `VDD_SDIO` ON for PSRAM/flash,
above the datasheet's minimal-config baseline); each wake burst runs one
ordinary render-loop iteration (single-digit milliseconds, per this file's
own TWDT-margin comments) at roughly tens of mA. Over the fast window,
300ms cadence costs ~1000 wake bursts in 5 minutes versus ~200 at the
1500ms steady-state period — ~800 extra brief bursts, ONCE per sleep-entry
event, each on the order of 8e-5 mAh: comfortably under 0.1 mAh total per
occurrence, negligible against any battery capacity this puck could carry.
This is an estimate to be confirmed with a real measurement in the field,
not a claim of a measured number.

**New bench tooling** (`CONFIG_FF_DEBUG_CONSOLE`, device only — see
`docs/hardware/comms-brain.md`'s "Bench console" section for the full
reference): `sleep` / `sleep <ms>` forces ONE light-sleep cycle right now,
ignoring the USB-connected inhibit (every wake source stays armed), and
reports the wake cause, the touch-INT GPIO level sampled immediately
before sleep and immediately after wake, and the elapsed time; `tpint`
polls the touch-INT GPIO level for a fixed 5s window so a bench operator
can tap the glass and see whether the line moves. Both share
`ff_run_light_sleep_cycle` (`app_main.c`) with the ordinary scheduled
sleep path — never two hand-copied implementations. Every cycle (forced
or scheduled) is recorded into a small ring buffer
(`FF_WAKE_LOG_CAPACITY` = 8 entries) that `diag` reads back — this is
what lets the owner put the puck to sleep on battery, tap the glass, plug
back into USB, and read `diag` to see what actually happened, since a
live console session cannot span a sleep (USB drops during it) — reconnect
after, then run `diag`.

**Bench evidence gathered** (2026-09-16, board 3 XIAO `!8f48af24`, crew
FIRE-8MNTT2, USB-tethered — `sleep`/`tpint` exist specifically to make
this possible without inhibiting sleep): with NO finger anywhere near the
glass, repeated `sleep`/`sleep <ms>` calls at periods from 50ms to 10000ms
show two honest, unexpected results, reported here without being
smoothed over:
  1. At a genuinely short period (50ms) the TIMER wake fires with
     `elapsed_ms` matching the configured period EXACTLY, every time —
     direct confirmation the fast-window mechanism itself works correctly
     on real hardware.
  2. At longer periods (1500ms, and even an explicit 10000ms), most
     cycles instead woke on a GPIO cause well BEFORE the configured timer
     — elapsed times of 52-487ms were observed against configured periods
     up to 10s — with the touch-INT level reading HIGH immediately before
     sleep and LOW immediately after. One cycle returned near-instantly
     (`elapsed_ms=1`, wake cause `UNDEFINED`) with touch-INT ALREADY LOW
     at the pre-sleep sample, consistent with a level wake source that was
     already asserted aborting sleep entry outright.
  This is real, repeatable evidence that GPIO4 (touch-INT) does NOT idle
  cleanly HIGH on this bench unit — it is honest to report, and it is
  NOT the same claim as "the SPD2010 asserts INT on a genuine touch": the
  bench evidence was gathered entirely over USB, which could itself be an
  EMI/ground-noise source for this GPIO (the CDC link's own activity) —
  this cannot be ruled out without a battery-only test, which needs the
  owner (an active console session cannot survive a real light sleep to
  observe it directly; the ring-buffer/`diag` readback above is the
  designed workaround). If the noise turns out to be real (not
  USB-induced) it also means an occasional light-sleep call may fail to
  actually sleep at all (the `UNDEFINED`-cause case above) — a possible
  additional battery cost distinct from the tap-wake bug this fix
  targets, flagged for the owner's awareness rather than addressed here,
  given the field deadline.

**Independent review decision (2026-09-16) — touch-INT wake disarmed by
default on the scheduled (field/battery) sleep path.** The bench evidence
above is exactly the hazard this fix's own risk assessment must weigh: if
GPIO4 fires spuriously on battery the way it did over USB here, the puck
never reaches the intended 300ms/1500ms cadence — it wakes on every
spurious edge instead, each wake costing an active-mode render-loop
burst, which is a real and unbounded (not "once per sleep-entry event")
battery cost, unlike the fast-window fix's own bounded estimate above. Against
that risk, touch-INT's actual benefit is now small: with the 300ms
fast-window timer wake already bounding a missed tap to at most one fast
period for the first 5 minutes (the window a wearer is statistically most
likely to still be interacting with the puck), a working touch-INT would
only improve on an already-tight worst case, and there is no confirmed
evidence it works at all on this hardware.

Decision: `CONFIG_FF_TOUCH_INT_WAKE` (`firmware/targets/esp32s3/main/
Kconfig.projbuild`, `[api]`), default **OFF**, gates whether touch-INT
(GPIO4) is armed as a wake source on the ORDINARY SCHEDULED light-sleep
path — TIMER, PWR (GPIO6), and BOOT (GPIO0) remain armed either way, and
timer wake alone already guarantees a wake per its own contract. The
bench `sleep`/`sleep <ms>` debug-console command is deliberately
UNAFFECTED by this default: `ff_run_light_sleep_cycle` (`app_main.c`)
arms touch-INT for every FORCED cycle regardless of the Kconfig setting,
so the owner can keep running the verification steps below on the bench
without rebuilding — the default only governs what a puck in someone's
pocket does. Turn the Kconfig option on once a battery-only test (step 3
below) shows GPIO4 genuinely quiescent off USB; until then, shipping it
armed by default would risk trading a bounded, estimated ~0.1 mAh/sleep-
event cost for an unbounded one on the strength of bench evidence that
points the other way.

**Owner steps to finish verification** (needs a real finger and/or real
battery operation — an agent cannot do either):
  1. **On-glass tap test** (bench, USB fine): `sleep 5000` on the console,
     then tap the glass once within the 5s window. Reconnect after (the
     port drops during the sleep) and read the direct reply if it landed,
     or run `diag` and read the newest `wakes` entry — `cause=GPIO`
     alongside `touch_int_post=0` (assuming an active-LOW controller,
     matching this file's own polarity assumption) is the signature of a
     touch-INT-caused wake; `cause=TIMER` means the fast-window fix (not
     touch-INT) caught it, still correct per the spec's wake-only rule.
  2. **`tpint` with a tap**: `tpint`, then tap the glass once during the
     5s window; `transitions` and `ever_low` should move from whatever the
     no-touch baseline showed.
  3. **Real battery test**: unplug USB, let the puck sit untouched for a
     few minutes past `t_off + t_sleep` (~2.5 min) so it enters SLEEP for
     real, optionally tap once, then reconnect USB and run `diag` — the
     `wakes` line's newest (non-`forced`) entries are genuine battery-
     power evidence, unlike every bench number above (all forced, all
     USB-tethered). This is also the only way to confirm the fast-window
     schedule's own device-side wiring (`app_main.c`'s `ms_since_sleep_
     entered` computation, which `sleep`/`sleep <ms>` deliberately bypass
     by taking an explicit period) actually engages on device — it is
     unit-tested at the pure-function level
     (`ff_idle_light_sleep_timer_ms`) and code-reviewed, but not yet
     exercised end-to-end on hardware, since light sleep cannot be
     naturally entered while USB-tethered (the amendment above) and no
     agent can unplug the cable.

**AMENDED 2026-09-16 — "sleep_inhibit gains a fourth source: a meshclient
handshake in flight" (debt/link-churn-2026-09-16, festival field-test
finding).** `scratchpad/link-churn-2026-09-16.md`'s investigation traced
315 `reconnects` in 4h17m of uptime to, in part, this exact race: light
sleep (this slice) drops in-flight inbound UART bytes outright (the
sentence at the top of this slice, unchanged by this fix), and if that
happens to a `want_config` request or its `config_complete` answer, the
handshake is guaranteed to sit stalled for a full
`MC_HANDSHAKE_TIMEOUT_MS` retry cycle (`mc_client.h`) before it can
recover — a race that is CERTAIN to cost time whenever it fires, not a
rare unlucky coincidence.

**The fix**: `ff_shell_handshake_in_flight(&s_shell)` — true exactly
while `mc_state(&sh->mc) == MC_STATE_HANDSHAKE` (a want_config sent, no
config_complete landed yet) — joins `usb_connected`, `ff_audio_busy()`,
and `ff_mic_status().running` as a fourth OR'd source into the SAME
`sleep_inhibit` parameter this slice's 2026-09-02 amendment introduced
(`app_main.c`, `ff_idle_tick`'s 4th argument) — same composition pattern,
same semantics (withholds only the OFF → SLEEP transition; DIM/OFF still
happen on schedule; `ref_ms` is never re-pinned by it). `mc_state()` is
the existing accessor (`mc_client.h`) — `ff_shell_handshake_in_flight` is
a thin wrapper that never reaches into `mc_client_t` directly, same
discipline `ff_shell_handshake_retries` (debt/S15c-handshake-stall,
above) already follows for the same struct.

**Deliberately narrower than `ff_shell_link() ==
FF_SHELL_LINK_RECONNECTING`**: that display-facing mapping (`ff_shell.h`'s
own "Link state" comment) folds `MC_STATE_HANDSHAKE` and
`MC_STATE_DISCONNECTED` together, but this inhibit source needs exactly
the HANDSHAKE half — inhibiting sleep while a session is genuinely being
negotiated, but NOT while merely sitting in the ~2s DISCONNECTED
reconnect backoff between attempts. Folding the two together would have
inhibited sleep almost continuously during a sustained link failure,
which is exactly the unbounded-inhibit risk addressed below.

**Bounded, not permanent — the part most likely to be got wrong.** A
handshake that never completes cannot hold `sleep_inhibit` true forever:
the existing S15c handshake-stall ladder (`MC_HANDSHAKE_TIMEOUT_MS *
(MC_HANDSHAKE_MAX_RETRIES + 1)` = 40s, then a ~2s reconnect backoff before
a fresh handshake begins — `mc_client.c`, unchanged by this fix) forces a
real drop out of `MC_STATE_HANDSHAKE` into `MC_STATE_DISCONNECTED` every
~42s, and that drop recurs every cycle for as long as the handshake keeps
failing — it is not a one-time grace period that only fires once. A
wedged handshake therefore costs light sleep MOST of the time it stays
wedged (roughly 40 of every 42 seconds), never ALL of it: never a silent,
permanent battery leak. This is intentionally NOT the same claim as "sleep
resumes promptly" — a device stuck in this state is still losing the
large majority of its light-sleep window, which is itself a real, visible
cost (surfaced via `hs_retries`/`reconnects` climbing in `diag`) rather
than a hidden one. Pinned by `test_meshclient.c`'s
`S03_debt_handshake_never_completing_does_not_inhibit_sleep_forever`,
which walks THREE full 42s cycles of a handshake that never completes and
asserts `mc_state()` drops to `MC_STATE_DISCONNECTED` in each cycle's ~2s
window — proving the release recurs, not just fires once — and by
`test_shell.c`'s
`S_link_churn_handshake_in_flight_true_only_between_connect_and_config_complete`,
which drives a REAL `mc_client_t` through the real-transport pipeline
(not the lighter synthetic-event-injection harness most of that file
uses, which never touches the underlying `mc_client_t.state` field and so
could not distinguish this accessor from a stub) to confirm it reads
true immediately after `mc_connect()` and false the instant
`config_complete` lands.

**Explicitly out of scope for this fix** (would trade battery for link
stability without the field data to justify it yet, per the task that
produced this amendment): broadening this into "never sleep while the
link is up" (`scratchpad/link-churn-2026-09-16.md`'s ranked fix #4) — this
amendment inhibits sleep only DURING the negotiation itself, not for the
whole time the link is `READY`; once connected, this source contributes
nothing to the `sleep_inhibit` OR. Also out of scope: any change to the
light-sleep timings themselves (fast/slow window durations, this slice's
own 2026-09-16 touch-INT amendment above), making UART a wake source, or
adding an `esp_pm_lock` (`scratchpad/link-churn-2026-09-16.md`'s ranked
fix #1, the "real fix" — flagged there as needing bench verification this
task's timeline did not allow).

**What could not be verified without hardware**: whether this
measurably reduces `reconnects`/`hs_retries` on a real device over a real
field session — that needs the bench procedure
`scratchpad/link-churn-2026-09-16.md` §7 already lays out (read `diag`
before/after a period of light-sleep cycling), run on real hardware,
which this task's environment cannot do (no serial port opened, per the
task's own constraint).

### (g) Boot animation
A splash (the firefly mark, ~1 s: ramp up, hold at full amber, ramp down —
raised from ≤ 1 s after the first cut read as a blink on glass) drawn as the
FIRST panel content, covering
the reset pulses + LVGL init. **Must not delay `ff_power_latch_on`** (still
line one). Field and demo builds identical.
- **AC1** Latch still precedes everything (assert order in `app_main`, log
  timestamps on glass).
- **AC2** Splash golden; no change to the first-face golden.

**AMENDED 2026-09-02 — "can the boot animation be the flare animation?"**
The first cut of this splash (PR #139) drew a plain breathing amber dot as a
deliberately simplified stand-in for "the firefly mark," reasoning that
plotting rotated ray geometry in the raw pre-LVGL draw path wasn't worth the
complexity for a splash on screen well under 1.1 s. The maintainer's
follow-up ask, after seeing it on glass, was direct: the splash should be
the actual flare mark — the same 8-ray burst + center dot
`app/screens/scr_flare.c`'s flare takeover screen breathes (`ff_theme`
amber, `flare_build_mark`), not an abstracted dot. Implemented in the same
PR that added this amendment: `ff_display_draw_boot_splash`
(`targets/esp32s3/components/ff_display/ff_display.c`) now rasterizes the
mark procedurally (per-pixel capsule test against each ray segment plus the
center dot — no LVGL, no anti-aliasing) from `core/include/ff_flare_mark.h`,
a new core-free, LVGL-free header holding the ray count, the per-ray length
fractions, the dot radius, and the stroke width — the exact values
`scr_flare.c` already drew with, moved to one shared table so `scr_flare.c`
and the splash cannot draw two different shapes. The breathe ramp
(`kFadeSteps`, `FF_SPLASH_STEP_MS`, `FF_SPLASH_HOLD_MS`) and the ~1.1 s
total budget are unchanged by this amendment — only the per-pixel shape
test (mark vs. circle) changed. The mark is centered on the panel rather
than at `scr_flare.c`'s off-center `FLARE_MARK_CY` (this splash has no
headline/buttons competing for space).

**AMENDED 2026-09-02 — SCREEN flip setting (see `docs/specs/S21-settings-
rework.md`'s own amendment for the full mechanism).** This splash's own
orientation is unaffected by `ff_settings_t.screen_flip` — it is drawn
raw, in the framebuffer's own coordinate space, exactly as before. The
FLIPPED case orientation instead renders correctly because `app_main.c`
applies `ff_display_set_flip` (a HARDWARE panel mirror) right after the
panel initializes and BEFORE this splash draws — so the splash is already
the first UPRIGHT content on a flipped puck's glass, the same "first
content, no visible flip" property (g)'s own AC2 goldens verify for
NORMAL orientation. The sim cannot render this distinction (it has no
panel mirror to apply); on-glass verification that the splash reads
upright in the flipped case is the maintainer's.

## Sequencing

(a) and (b) in parallel (a touches `festpack` + one `app_main` call; b
touches `ff_power` + core + shell) → (c) and (d) after both land (both touch
the shell tick) → (e), (f), (g) after the festival cut is on glass.

## Out of scope

Deep sleep (UART framing + wake source unresolved), battery gauge (S25c),
folding flare into `ff_notify` (later), per-user idle timeouts in Settings.

## Amendments

- **2026-09-02, maintainer decision — PULSE retired end to end:**
  `ff_notify_kind_t` never had a PULSE value — this Notifications section's
  **kind** ∈ MESSAGE · FLARE · RALLY · SYSTEM list was accurate before and
  after PULSE's retirement (see `S04-firefly-protocol.md`'s Amendments),
  since only MESSAGE/RALLY were ever banner-eligible and PULSE was never
  one of the four. Recorded here rather than left silent because a
  RESERVED_01 (retired PULSE) inbound frame is now explicitly a
  banner-eligibility non-event too, by the SAME "not MESSAGE/RALLY, no
  banner" rule this section already states — `app/ff_wiring.c`'s drop of a
  RESERVED_01 frame (no feed item) means `ff_shell.c`'s banner push never
  even sees one to consider.

- **2026-09-03, maintainer-reported bug fix — "a banner opens the
  conversation the message belongs to" (slice (d) AC5, above):**
  tapping the banner for a message that went to the GROUP chat was
  opening the sender's 1:1 conversation instead of the CREW thread. The
  rule as shipped before this fix (this section's original prose, "tap →
  the sender's thread") conflated two different facts that only happen
  to coincide for a DIRECT message: WHO sent it (`node_id`) and WHICH
  CONVERSATION it belongs to (S24's `ff_inbox` CREW-vs-member model,
  `docs/specs/S24-signals-inbox.md`'s "Conversation membership" section —
  broadcast/UNKNOWN-direction traffic files under CREW, a specifically-
  addressed-to-me item files under that sender's own thread). `ff_shell.c`
  routed `FF_INTENT_BANNER_OPEN` by `node_id` alone, so a group message's
  banner (whose `node_id` is still the real sender, for display) opened
  that sender's private thread instead of CREW.

  **The fix:** `ff_notify_entry_t` (`core/include/ff_notify.h`) gains a
  `conv` field (`ff_notify_conv_t`: `FF_NOTIFY_CONV_CREW` /
  `FF_NOTIFY_CONV_DIRECT`) recording which conversation the push belongs
  to — a small, dependency-light LOCAL enum (not `ff_inbox.h`'s own
  `ff_conv_kind_t`, to keep `ff_notify` free of the crew/sigview
  dependency `ff_inbox.h` carries), sharing that enum's zero-is-CREW
  convention so the one-line bridge in `ff_shell.c` can never disagree
  with S24's own membership rule. `ff_notify_push` takes `conv` and now
  coalesces on `(kind, node_id, conv)` instead of `(kind, node_id)` — a
  duplicate check widened, not narrowed: the same sender's group message
  and direct message, even seconds apart, are two distinct facts and stay
  two banners (`ff_notify.h`'s own judgment call 5 has the full reasoning
  and the "why AC1's literal wording under-specifies this" note).
  `ff_wiring.c`'s existing S24 AC1 direction classifier is exported as
  `ff_wiring_classify_dir` (`[api]`) so the banner push
  (`shell_notify_push_banner`) classifies a message's destination through
  the EXACT SAME rule the feed item's own `ff_feed_dir_t` already used —
  a banner cannot disagree with where its message actually landed in the
  inbox. `FF_INTENT_BANNER_OPEN` then routes on `h->conv`: CREW opens the
  CREW thread (`inbox_thread_node = 0`, the sentinel this file's
  `FF_INTENT_INBOX_OPEN_THREAD`/`PICK` handling already used), anything
  else opens the sender's own 1:1 thread — and mark-read
  (`ff_inbox_mark_thread_read`) follows the same split.

  **Tests naming the AC:** `firmware/core/tests/test_notify.c`'s
  `S26_conv_field_is_stored_by_literal`,
  `S26_same_sender_group_and_direct_do_not_coalesce`,
  `S26_same_sender_same_conv_still_coalesces` (the `conv` field/coalescing
  half); `firmware/app/tests/test_shell.c`'s
  `S26_banner_open_group_message_opens_crew_thread_not_senders`,
  `S26_banner_open_direct_message_opens_senders_thread`,
  `S26_banner_open_group_rally_opens_crew_thread`,
  `S26_banner_open_picks_the_head_banners_own_conversation` (AC5's routing
  + mark-read rule, at the shell/intent level); and
  `firmware/targets/sim/tests/test_ctl_flare_sequence.c`'s
  `S26_banner_tap_from_the_launcher_lands_on_the_crew_thread_for_a_group_
  message` (the same rule driven end to end through the real ctl/LVGL
  stack, asserting the RENDERED thread header reads "CREW" — not merely
  the view-model field). Mutation-tested: reverting the routing decision
  to "route by `node_id` only" (the pre-fix behavior) fails the group-
  routing tests above while leaving the direct-message test green, which
  is exactly the asymmetry this bug had.

  See `docs/specs/S24-signals-inbox.md`'s `## Model` →
  `### Inbox/thread view-model — new core module ff_inbox` section, and
  that module's own header (`core/include/ff_inbox.h`, "Conversation
  membership" — the fuller rulebook, including the UNKNOWN-direction
  placement decision this fix's CREW-default relies on), for the
  authoritative CREW-vs-member filing rule this fix now agrees with.

- **2026-09-05, S28 (on-glass navigation gestures) — BOOT/launcher HOME
  gets an on-glass sibling.** Slice (e)'s BOOT-button HOME
  (`ff_shell_home_press` -> `FF_INTENT_HOME`) and the launcher's own tap
  are no longer the only way to reach the launcher: a bottom-rim
  edge-swipe-up (G2) now emits the same `FF_INTENT_HOME`, and a
  left-rim edge-swipe (G1) emits `FF_INTENT_BACK`, which itself now
  falls through to HOME from any base face with no open modal/sub-view
  (see `docs/specs/S16-app-shell.md`'s own Amendments for that rule
  change). A third on-glass gesture, long-press-to-flare, is a
  touch-only sibling of the existing 5-tap-HOME quick flare
  (`ff_multitap`/`ff_shell_multitap_edge`) — same `FF_INTENT_QUICK_FLARE`
  destination, gated to the Radar face and to non-interactive glass
  only. See `docs/specs/S28-gestures.md` for the full gesture set,
  geometry, and axis-lock rules that keep these from colliding with an
  ordinary scroll (the PR #130 regression this spec's own history notes).
