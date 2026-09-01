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

**Radar is the watchface.** It is what the screen wakes to and rests on — at
a festival the compass is glanced at constantly, so waking to a menu would be
wrong. **BOOT (GPIO0) is the home button**: from Radar it opens the
**launcher** (a ring of app circles: Now · Signals · Map · Settings — Radar
is not listed, it is home); from any app it returns to Radar; from the
launcher it returns to Radar. Picking a circle enters that app full-screen.
This retires the horizontal carousel: with one global nav gesture (a button),
vertical drag is unambiguously scroll everywhere, and horizontal is free for
apps. GPIO0 is a normal input once booted (only special at reset) and is free
in the pin map.

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
gains HOME semantics per the nav model above; a new `scr_launcher` face (ring
of app circles, ≥ 56 px targets on round glass, press states). Remove the
carousel swipe from `scr_nav.c` (LEFT/RIGHT no longer change faces); page dots
go. Long-press-anywhere → Settings is retired (Settings is a launcher circle).
- **AC1** `ff_route` tests: BOOT from Radar → launcher, from launcher → Radar,
  from app → Radar; modal (Compose, power menu) suppresses.
- **AC2** Launcher golden; hit-target sweep green.
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
A splash (the firefly mark, ≤ 1 s) drawn as the FIRST panel content, covering
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
