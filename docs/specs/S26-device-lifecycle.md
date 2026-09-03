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

**Placement (maintainer decision B, 2026-09-02; refined 2026-09-02 orchestrator
review round 2):** the BANNER strip covers the status bar row (clock · MESH ·
battery — `RADAR_LAYOUT_STATUS_BAR_DY`), not the row below it. A transient
banner should hide the LEAST valuable row on whatever face is showing; the
strip's original position (just below the status bar) instead covered the
top of Radar's compass/close-range readout, or a thread's first message
bubble — both more valuable than the clock/mesh/battery row a 6 s banner can
safely eclipse.

Round 1 centered the strip exactly on `RADAR_LAYOUT_STATUS_BAR_DY` (-160)
and shrank its width to ~90 px to fit the round glass there — technically
correct but too narrow to read as a banner (only the MESH label was ever
covered; sender name and preview text were crushed to one or two
characters). Round 2 instead finds the LOWEST (least-negative) centre whose
top edge still clears the status text's own measured top (y=38 on a 412 px
puck): `BANNER_CY = RADAR_LAYOUT_STATUS_BAR_DY + 14` (dy -146, puck-local
y=60) — still derived from the status-row constant, not a second
independent number, just offset by the amount that trade needs. At that
height the strip widens back out to 160 px (up from round 1's 90, still well
short of the original 240) while keeping a real ≥10 px margin off
`FF_THEME_GLASS_R`/`FF_THEME_GLASS_CX/CY` at every corner (see
`scr_banner.c`'s own layout comment for the exact chord math, including a
correction to which radius that check runs against — `FF_THEME_GLASS_R`
200, the real measured glass, not the framebuffer's 206). At this width the
strip now reaches (and partially covers) the clock and battery labels too,
not just MESH — accepted deliberately: the strip's own rectangle has one
constant y-range across its whole width, so wherever it does reach it
covers the text FULLY top-to-bottom, never a half-height sliver poking out.
The age no longer fits as a separate top-right corner chip; it sits beside
the name on the same row instead, and at 160 px both the sender's full demo
name and a preview past 10 characters render before DOTS ellipsis has to
step in.

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
reaches the top compass satellite (Inbox, `compass_pos == 0`); its
remainder there (~88x37px) fails the 44px HEIGHT floor, so it is masked —
accepted as intentional and semantically consistent: while a banner shows,
that region IS the banner, and tapping it opens the sender's thread, which
is roughly where tapping Inbox would have led anyway. The launcher's own
status row (bottom of the puck, `LAUNCHER_STATUS_ROW_DY`) is far enough
from the banner's position to never compete with it. Launcher renders
WITHOUT an active banner are untouched (goldens byte-identical).

## Slices + acceptance criteria

### (a) Reclaim the festpack token buffer — `[api]`
`fp_pack.c` is pure C11 (no ESP allocators, no `EXT_RAM_BSS_ATTR`), so the
caller supplies the scratch: `fp_pack_parse(..., jsmntok_t *toks, int ntoks)`
(exact signature per the existing parse entry point; keep a thin wrapper only
if the sim/tests need it). The esp32s3 target allocates `8192 * sizeof
(jsmntok_t)` in **PSRAM** (`MALLOC_CAP_SPIRAM`) around the demo parse and
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

### (d) `ff_notify` + message banner
Core `ff_notify` as above (queue depth 4, FIFO, expiry, `dismiss`, `pop`).
Shell: an incoming MESSAGE / RALLY (paired sender) enqueues a BANNER; the
active face renders the banner strip on top (a new `scr_banner` overlay, not a
face); tap → the sender's thread (S24) and marks read; auto-expire 6 s;
enqueue wakes the screen via (c). Flare takeover untouched.
- **AC1** `ff_notify` unit tests: FIFO, overflow drops oldest, expiry, dismiss,
  a duplicate (same node+kind within 2 s) coalesces.
- **AC2** Banner golden on Radar + on a thread; opacity in the render key;
  press state.
- **AC3** Unpaired sender never produces a banner (S22 stranger rule).
- **AC4** Field build carries no demo-only banner content (nm check, S23 AC4).

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
