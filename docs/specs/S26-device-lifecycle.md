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

### (g) Boot animation
A splash (the firefly mark, ~1 s: ramp up, hold at full amber, ramp down —
raised from ≤ 1 s after the first cut read as a blink on glass) drawn as the
FIRST panel content, covering
the reset pulses + LVGL init. **Must not delay `ff_power_latch_on`** (still
line one). Field and demo builds identical.
- **AC1** Latch still precedes everything (assert order in `app_main`, log
  timestamps on glass).
- **AC2** Splash golden; no change to the first-face golden.

## Sequencing

(a) and (b) in parallel (a touches `festpack` + one `app_main` call; b
touches `ff_power` + core + shell) → (c) and (d) after both land (both touch
the shell tick) → (e), (f), (g) after the festival cut is on glass.

## Out of scope

Deep sleep (UART framing + wake source unresolved), battery gauge (S25c),
folding flare into `ff_notify` (later), per-user idle timeouts in Settings.
