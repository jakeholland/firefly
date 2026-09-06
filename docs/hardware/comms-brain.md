# Comms brain: wiring and setup

The UI puck (Waveshare ESP32-S3-Touch-LCD-1.46) talks to the comms brain
(Seeed XIAO ESP32S3 + Wio-SX1262 running stock Meshtastic) over a 4-wire UART
link at 115200 8N1, 3.3 V logic both sides, no level shifter. Wiring diagram
(same content, drawn): https://claude.ai/code/artifact/3fcda222-93b5-41fb-9b63-eed48aa1336a

Pin facts verified 2026-09-04 from docs.waveshare.com (ESP32-S3-Touch-LCD-1.46),
wiki.seeedstudio.com (XIAO ESP32S3) and meshtastic/firmware
`variants/esp32s3/seeed_xiao_s3/variant.h`.

## Pin map

| Wire   | Puck header      | XIAO pin      | Meshtastic setting |
|--------|------------------|---------------|--------------------|
| data   | TXD · GPIO43     | D3 · GPIO4    | `serial.rxd 4`     |
| data   | RXD · GPIO44     | D1 · GPIO2    | `serial.txd 2`     |
| ground | GND              | GND           |                    |
| power  | 3V3 (or shared battery, see below) | 3V3 (or BAT+) | |

- The puck's console is USB-Serial-JTAG ONLY as of the S15c review round (`firmware/targets/esp32s3/sdkconfig.defaults`/`sdkconfig.ci`: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, secondary NONE) — a fresh checkout's UART header pins GPIO43/44 are free for exactly this reason. ESP-IDF's stock default before that fix kept UART0 (GPIO43/44's own default peripheral) as the PRIMARY console with USB-Serial/JTAG only a secondary mirror, so the boot log was actually sharing these pins; see `docs/specs/S15-esp32s3-target.md`'s Amendments for the full reasoning (shared-FIFO corruption risk) and the maintainer action items below.
- **If your board's `sdkconfig` predates this fix** (drift rule, `docs/specs/S15-esp32s3-target.md`): a fresh `sdkconfig.defaults` entry does not reach an already-generated `sdkconfig`. Delete every `CONFIG_ESP_CONSOLE_*` key from the board's `sdkconfig` (or `idf.py fullclean` + reconfigure) and set `CONFIG_FF_LINK_UART=y` by hand before wiring up the comms brain — otherwise the boot log is still on GPIO43/44 despite this file's own claim above, and `FF_LINK` silently stays `NONE`.
- A residual, un-Kconfig-able boot-garbage window remains regardless: the ROM's own first-stage boot banner is hardware-fixed to UART0's default pins and prints before any Kconfig setting is even read. Brief, one-time per boot, and absorbed by whichever side's framer resync counter sees it — see the S15 Amendment for the full reasoning. This is why the mesh link itself stays on UART1 (`FF_UART_PORT` default), not UART0, even with the console moved off it.
- **Do not use the XIAO's D6/D7 (GPIO43/44)**: Meshtastic's `seeed_xiao_s3` variant
  assigns them to the L76K GPS (`GPS_TX_PIN 43`, `GPS_RX_PIN 44`). This supersedes
  S15 deliverable 3's "D6/D7" wording.
- D2 (GPIO3) is skipped (strapping pin). D8–D10 (GPIO7/8/9) are the radio's SPI;
  NSS 41, RST 42, BUSY 40, DIO1 39, RXEN 38 are on the B2B connector.

## Power

- **Option B (recommended for the field):** one LiPo, Y-split to both boards'
  battery inputs (puck MX1.25; XIAO BAT+/BAT- pads on the back). Each board keeps
  its own regulator. Charge through ONE board's USB at a time (two chargers on
  one cell must not run together). The XIAO stays on when the puck latches off.
- **Option A (bench):** puck `3V3` header → XIAO `3V3` pin. The puck's MP1605 buck
  is rated 2 A; the SX1262 TX burst is ~120 mA. The XIAO's 3V3 pin is its
  regulator OUTPUT; back-feeding it is common on XIAO but not documented by Seeed —
  measure before trusting it in the field.
- Never use the puck's `5V` pin on battery (USB-only). Never power the Wio-SX1262
  without its antenna.

## Configure the XIAO once (USB, Python CLI)

Flash stock Meshtastic "Seeed XIAO S3" with the web flasher, then:

```
meshtastic --set lora.region US
meshtastic --set serial.enabled true --set serial.mode PROTO \
           --set serial.baud BAUD_115200 --set serial.txd 2 --set serial.rxd 4
meshtastic --set bluetooth.enabled false        # optional, saves power
meshtastic --set-owner "Jake"
meshtastic --ch-set name Firefly --ch-set psk random --ch-index 0   # copy the PSK to every crew node
```

PROTO mode exposes the protobuf client API (the same one the phone app uses) on
those pins; the puck's meshclient (S03) speaks it.

## Bring-up order

1. Before the boards arrive: jumper puck TXD↔RXD on its header, flash with
   `CONFIG_FF_UART_LOOPBACK_SELFTEST=y`, confirm the loopback PASS line in the
   boot log (proves the driver + header pins alone).
2. Flash + configure the XIAO over USB (above). Antenna on. Confirm it in the
   Meshtastic app/CLI.
3. Power both off. Wire the four lines. Check the cross: puck TXD → XIAO D3,
   puck RXD ← XIAO D1.
4. Power up. Puck log: `link: UART… TXD=43 RXD=44 115200`, then the meshclient
   handshake and READY within a few seconds. Silence = TX/RX swapped; garbage +
   rising resync counters = wrong baud.
5. A second node (T1000-E / Heltec) sends a position → Radar within 10 s (S15 AC2).

## Pairing (crew roster) — bench/field stopgap until S12 ships

A CONNECTED link is not enough on its own: pairing v1 is "channel membership
+ explicit crew list" (`docs/specs/S04-firefly-protocol.md`), and the shell's
roster-trust policy (S16) refuses to grow the paired crew list from anything
the radio says — only an explicit user pairing action does that
(`ff_shell_pair`). Until the real Crew screen (S12) exists to drive that
action, a puck that is CONNECTED to its comms brain but has never been
paired shows "no crew linked" and silently drops every position/text from
the other node — not a bug, the policy working as specified, just with no UI
yet to use it.

**Bench/field stopgap:** set `CONFIG_FF_DEV_TRUST_CHANNEL=y`
(`idf.py menuconfig` → Firefly bring-up, or by hand in `sdkconfig`) until
S12 ships. Every crew channel is already private (its own PSK, set once when
configuring the XIAO above), so on this bench/field setup channel membership
already IS the crew — this option auto-pairs any node heard on that channel
(NodeInfo only, the same S16 AC6 mechanism the sim's `ffsim --dev-trust-all`
uses in dev). Off by default; it must never ship on by default, and the
Kconfig help text says so. Watch for
`firefly: DEV_TRUST_CHANNEL on — auto-pairing every heard node` in the boot
log to confirm it took. See `firmware/app/include/ff_shell.h`'s
`ff_shell_dev_trust_all` doc comment for exactly what this does and does not
change versus the sim flag it mirrors.

## Field festpack (CONFIG_FF_FIELD_PACK)

A live (non-demo) build loads the real field festpack by default: `CONFIG_FF_FIELD_PACK=y`
(default `y`, `depends on !CONFIG_FF_DEMO_MODE` — mutually exclusive with demo
mode, enforced by both the Kconfig `depends on` and a compile-time `#error`
in `app_main.c`) embeds `firmware/assets/field/lost-lands-2026.festpack.json`
(a verbatim copy of fest-almanac's real Lost Lands 2026 pack — see that
directory's own README for how to refresh it; never hand-edit it) and, on
boot, parses it into the shell via `ff_shell_load_pack` at the same point a
demo build's `ff_demo_seed` runs. That is ALL it does: no crew, no
positions, and no clock are seeded (honest data — `CLAUDE.md`) — only the
festpack itself, so the MAP face gets a real origin and Settings gets a real
UTC offset, from the real mesh. Watch for `firefly: S05 field pack loaded: ...`
(or the parse-error log line) in the boot log to confirm it took.

**Wall-clock consequence:** loading the pack also tightens the S18 wall-clock
plausibility window to the festival's own dates via `ff_wall_window_from_pack`
(±14 days either side of `start_doy`/`end_doy`). For Lost Lands 2026 (Sep
18–20) that window is **2026-09-04T00:00:00Z through 2026-10-05T00:00:00Z**
(unix 1788480000 through 1791158400, exclusive ceiling) — every mesh
timestamp outside it is rejected by the plausibility gate at ANY trust tier
(the same gate `CONFIG_FF_DEV_TRUST_CHANNEL` above has no effect on). A bench
run before **2026-09-04** would have had every incoming timestamp rejected
and the wall clock stuck on the fixed bootstrap window — keep that in mind
scheduling any pre-field bench test.

## Bench console

An opt-in line-command console lets an end-to-end test (or a human at a
terminal) trigger sends, flares, and state dumps over the puck's USB
port — no touchscreen tap needed. Off by default (never ship it on for
a field build): set `CONFIG_FF_DEBUG_CONSOLE=y` (`idf.py menuconfig` →
Firefly bring-up → "Bench/debug console", or by hand in `sdkconfig`).

It reads line commands off the USB-Serial-JTAG port — the SAME port
that already carries the boot log — and is polled only while USB is
connected (the S26f amendment's own connection sample), so it changes
nothing about battery/field operation. Connect with any plain serial
terminal at the puck's USB-Serial-JTAG port (e.g. `idf.py monitor`, or
`screen /dev/tty.usbmodem* 115200` — the console doesn't care about
baud, it's USB-CDC), type a command, press Enter.

Commands (replies are `dbg: `-prefixed; an unrecognized or malformed
line replies `dbg: ? try help`):

| Command | Effect |
|---|---|
| `help` | list the commands below |
| `me` | my node id, link state, my position (ok/lat/lon/age), wall clock (latched?/trust/unix now) |
| `roster` | paired crew: id, name, presence, position |
| `heard` | heard-but-unpaired node ids |
| `send <text>` | crew broadcast — the same send mechanism the composer's SEND button uses |
| `dm <node_hex> <text>` | addressed send to one node (hex, optional `!` or `0x` prefix) |
| `flare` | start a quick flare (same path as the physical 5-tap gesture) |
| `flare cancel` | cancel a flare in progress |
| `wall` | wall-clock latch dump: latched, trust tier, UTC offset, last observation's source node |
| `i2c` | shared I2C bus scan (0x08-0x77, known addresses named) plus a one-shot compass status line |
| `cal` | compass calibration ritual status: active?, progress %, sample count, can-finish?, and whether a calibration is currently stored |
| `cal start` | begin a calibration session (S12 step 3, the figure-eight ritual) |
| `cal finish` | attempt to end the session: persists on >=70% octant coverage, otherwise leaves the old calibration untouched and the session active |
| `cal cancel` | abandon the session; the stored calibration (if any) is untouched |
| `cal clear` | drop the STORED calibration back to identity/uncalibrated |
| `name` | NAME in Settings: stored puck name, mesh-reported name (if any), confirmed/pending, and whether the name was silently adopted from the mesh at boot |
| `name <text>` | commit `<text>` through the EXACT SAME path the Settings NAME row's DONE button uses — sanitize (letters/digits/space), persist, push the Meshtastic owner update — so the mesh push can be bench-tested against real nodes over USB |

Every acting command dispatches through `ff_shell_intent` (the SAME
`FF_INTENT_QUICK_FLARE`/`FF_INTENT_FLARE_END` intents the physical
5-tap gesture and the sender overlay's CANCEL button use) or one of the
debug-only seams — `ff_shell_debug_send_text` for `send`/`dm`,
`ff_shell_debug_set_name` for `name <text>` (both `app/include/
ff_shell.h`) — never a new roster-growth path, never a direct mesh send
bypassing the shell. See that header's doc comment and
`app/include/ff_debug_console.h` for the full seam discipline, and
`core/include/ff_dbgcmd.h` for the line parser (table-driven, bounded,
CRLF-tolerant, unit-tested independent of any shell).

Example session (paired with one crew member, `Dana`, `!0000da1a`):

```
me
dbg: me node=!00001000 link=CONNECTED
dbg: me pos ok=1 lat=40.712800 lon=-74.006000 age_ms=1500
dbg: me wall latched=1 trust=TRUSTED unix=1789768900

roster
dbg: roster n=1
dbg: roster id=!0000da1a name=Dana presence=LIVE has_pos=1 lat=40.700000 lon=-74.010000

send crew, meet at the tent
dbg: send ok dest=broadcast

dm da1a on my way
dbg: dm ok dest=!0000da1a

flare
dbg: flare started dur_s=300

flare cancel
dbg: flare cancelled

wall
dbg: wall latched=1 latch_unix=1789768800 trust=TRUSTED offset_min=-240 assumed=0 last_src=!0000da1a rejected=0

i2c
dbg: i2c 0x20 io-expander, 0x2c qmc5883p, 0x51 rtc, 0x53 touch, 0x6b qmi8658
dbg: compass mag=qmc5883p imu=found heading=127 cal=identity

cal start
dbg: cal started

(rotate the puck in a figure eight, then check progress)
cal
dbg: cal active=1 progress_pct=88 samples=340 can_finish=1 cal=identity

cal finish
dbg: cal finished ok progress_pct=100 samples=390

cal
dbg: cal active=0 cal=custom

name
dbg: name stored=(unset) mesh=unknown confirmed=0

name Jake
dbg: name stored=Jake mesh=unknown confirmed=0

(the comms brain's own NodeInfo replay arrives a moment later)

name
dbg: name stored=Jake mesh=Jake confirmed=1

xyzzy
dbg: ? try help
```

(The scan line above is a real bench capture, 2026-09-05, from a puck
with a current-production GY-273 clone wired up — see "Chip
auto-detection" below for why this board answers at `0x2c` rather than
the `0x0d`/`0x1e` an older clone would.)

`i2c` is a bench diagnostic, not a shell command in the seam-discipline
sense above — it reaches no `ff_shell_*` getter at all. Named addresses:
`0x0d qmc5883l` / `0x1e hmc5883l` / `0x2c qmc5883p` (GY-273
magnetometer — whichever chip `ff_compass_init` actually finds; see the
"Compass" section below), `0x20 io-expander` (TCA9554), `0x51 rtc` (an
aftermarket RTC module on the back header), `0x53 touch` (SPD2010),
`0x6b qmi8658` (onboard IMU). An address not in this list prints bare
(just the hex) rather than a guess. The scan is a two-line diagnostic
on purpose: the bus sweep (what is actually wired up and ACKing)
immediately followed by the compass driver's own one-shot status (what
it believes it found and its last heading/calibration state) — so a
bench engineer sees in one command whether an unresponsive
magnetometer is a wiring problem (never shows up in the scan) or a
driver problem (shows up in the scan but the compass line still
reports `mag=absent`). The compass line's `mag=` field names the
actual chip (`qmc5883l`/`hmc5883l`/`qmc5883p`) rather than a bare
"found", so a bench engineer doesn't have to cross-reference the scan
line above it to know which part responded. On a target with no I2C
bus at all (the sim build), the whole command replies with a single
honest line: `dbg: i2c unavailable on this target`.

## NAME → Meshtastic owner (how the puck's name reaches the mesh)

The Settings "NAME" row (and the bench console's `name <text>`) commit
to two places, in order:

1. **Locally** — `ff_settings_t.my_name`, persisted through the same
   `FF_SETTING_MY_NAME` write-through seam every other setting uses (S16
   slice e). This is what the puck itself remembers.
2. **The comms brain** — a Meshtastic `AdminMessage` carrying
   `set_owner: User{ long_name, short_name }`, sent over the SAME UART
   connection this device already uses as a Meshtastic *client* of the
   comms brain (S03's meshclient library — `mc_send_set_owner`,
   `firmware/meshclient/include/mc_client.h`), addressed to the comms
   brain's OWN node id (`ff_shell_my_node_id`). `want_ack` is always
   true and the packet id comes from the same seeded generator every
   other send on this connection uses.

**Why no admin key is needed.** A client attached over UART/Serial/BLE
— which is exactly this device's relationship to the comms brain, the
same one the official phone app has — gets its outgoing packets'
`MeshPacket.from` zeroed by the comms brain's own firmware before
anything else touches them:

```
// meshtastic/firmware, src/mesh/MeshService.cpp:188 (MeshService::handleToRadio)
p.from = 0;                          // We don't let clients assign nodenums to their sent messages
```

`AdminModule::handleReceivedProtobuf` (`src/modules/AdminModule.cpp`)
only requires a session passkey when `mp.from != 0`:

```
} else if (mp.from == 0) {
    if (config.security.is_managed) { ... }        // local admin: passkey NOT required
}
...
if (mp.from != 0 && !messageIsRequest(r) && !messageIsResponse(r)) {
    if (!checkPassKey(r)) { ... }                   // remote admin: passkey required
}
```

So a `set_owner` this device submits over its own UART connection to
the comms brain arrives with `from == 0` and is trusted without a key
exchange — the exact same "local admin" trust the phone app's own
`set_owner` flow relies on. Verified by reading `meshtastic/firmware`
tag `v2.7.26` (commit `54e0d8d0`) — the same firmware version this
repo's own S03 spec amendments hardware-verified other wire behavior
against (`docs/specs/S03-meshclient.md`, precision_bits/rx_snr).
`mc_send_set_owner`'s own doc comment (`mc_client.h`) carries this same
citation next to the code it justifies.

**Short name derivation** (`ff_meshname_derive_short`,
`core/include/ff_meshname.h`): the first 4 alphanumeric characters of
the long name, uppercased, in that order — non-alphanumeric characters
(spaces, punctuation) are dropped BEFORE truncating, not after, so a
name like "Jake H" derives from the full alphanumeric run ("JakeH")
rather than losing the H to an early space. A name with fewer than 4
alphanumeric characters is never padded with anything fabricated.
Examples: "Taylor" → "TAYL", "Jake" → "JAKE", "Jo" → "JO".

**Confirmation is never assumed.** Accepting a `set_owner` for send
proves nothing about whether the comms brain actually applied it —
`ff_shell.c` never marks the row confirmed until a matching reply
actually says so (`ff_shell_mesh_name_status`/`shell_mesh_name_confirmed`
— the ONE derivation both the Settings row's pill and the bench
console's `name` command read, never two independent guesses). See "How
confirmation works" immediately below for what "a matching reply" means
in practice and why the original design needed a fix.

### How confirmation works (bench fix, 2026-09-06)

**The bench finding.** A real puck + comms brain (Meshtastic 2.7.26)
proved the push itself works — `meshtastic --info` against the comms
brain showed the new owner immediately after a `name <text>` — but the
row's pill sat on `...` (pending) indefinitely. Cause: `AdminModule::
handleSetOwner` updates the comms brain's own `nodeDB`/owner in place
and does **not** re-broadcast a fresh self NodeInfo afterward. The next
one a connected client (this puck) would see is either the next
want_config handshake (a reconnect) or the periodic, hours-scale NodeInfo
broadcast — neither of which happens promptly after a wearer taps DONE.
The ORIGINAL "wait for a self NodeInfo" confirmation design (previous
section) was honest — it never confirmed something that hadn't
happened — but it was also useless, because the thing it was waiting for
essentially never arrived within a session.

**The fix.** A successful `set_owner` send now immediately follows up
with its own read: `AdminMessage.get_owner_request` addressed to this
node's own id (`mc_send_get_owner_request`, `meshclient/include/
mc_client.h` — same local-admin, no-key-needed path as `set_owner`
itself, `want_ack` false since the response payload IS the answer). The
reply (`get_owner_response`, a `User`) arrives via a new `mc_events_t.
on_owner` event, and `ff_shell.c`'s `shell_ev_owner` treats it EXACTLY
like a self NodeInfo for confirmation purposes — same cache
(`mesh_owner_name`), same comparison (`shell_mesh_name_confirmed`). The
original self-NodeInfo path (previous section — the want_config replay
on reconnect, or a live update following `MeshService::reloadOwner`'s
own `nodeDB->updateUser` push back to this connected client) is kept as
a SECOND, independent confirmation source — whichever arrives first (or
either one, on a later reconnect) can confirm.

**Retry/timeout.** If no `get_owner_response` arrives within ~10 s
(`FF_NAME_OWNER_REQ_TIMEOUT_MS`, `ff_shell.c`), the request is retried,
up to 3 retries (`FF_NAME_OWNER_REQ_MAX_RETRIES`) — 4 requests total. If
the budget is exhausted with still no reply, the row/console keep
reading the honest `...` pending state forever: this fix closes the
"confirmation never happens" gap, it does not open a new way to
FABRICATE one. A wearer/coordinator can always retry manually by
re-committing the same name (DONE, or the bench console's `name
<text>`), which starts a fresh push-and-poll cycle.

**Mesh-delivery ACK/NAK.** Independently of the `get_owner_response`
round trip, the `set_owner` WRITE's own mesh-level delivery outcome
(Meshtastic `Routing.error_reason`, correlated to the outgoing packet id
via a new `mc_events_t.on_routing_ack` event) is now surfaced too: a NAK
renders as a distinct `!` (amber) on the pill and `ack=nak` on the bench
console — honestly different from the ordinary `...` pending state,
which would otherwise look identical to "still waiting, give it a
moment". This is a ROUTING-layer fact (did the write reach the comms
brain's admin module at all), separate from and not a substitute for the
`get_owner_response`/self-NodeInfo confirmation above — a NAK'd write is
never treated as pending-but-fine, and an ACK'd write is never, by
itself, treated as confirmed (`ff_mesh_name_ack_t`'s own doc comment,
`ff_shell.h`, has the full three-state rationale: NONE/OK/NAK, where
NONE means "no routing reply yet", not "failed").

**Bench console.** `name`'s output line grows three fields:

```
dbg: name stored=<my_name> mesh=<mesh_owner_name|unknown> confirmed=<0|1>[ (from_node)] pushed=<none|long/short> ack=<none|ok|nak> reply=<none|long/short>
```

- `pushed=` — what the CURRENT push cycle actually sent (reset by the
  NEXT `name <text>`/DONE, never accumulated across pushes); `none`
  before any push this session.
- `ack=` — the routing outcome of that push's `set_owner` write; `none`
  until a routing reply for it arrives.
- `reply=` — the most recent `get_owner_response` this push's own
  follow-up request received, if any; `none` until it does. May differ
  from `mesh=`/`confirmed=` if something else changed the owner in
  between (that mismatch is itself honest bench info, not an error).

**Fallback.** The Meshtastic phone app and CLI (`meshtastic
--set-owner "<name>" --set-owner-short "<short>"`) remain a fully
independent way to set the SAME owner identity directly against the
comms brain — this feature is additive, not a replacement path, and
either one can correct the other's mistake.

### How confirmation works, round 2 (bench fix, 2026-09-06, after commit `51e4ae1`)

The round-1 fix above shipped, then a real bench run (real puck, real
comms brain, Meshtastic 2.7.26) found it neither worked nor was fully
honest. Two independent bugs:

**Bug 1 — `get_owner_request` never got a reply.** `name Jake H` showed
`pushed=Jake H/JAKE ack=ok` within 3 s (the `set_owner` write really
lands — `meshtastic --info` confirmed it) but `reply=none` after 21 s
and across every retry: this device's own follow-up question was never
answered. Cause, read straight from `meshtastic/firmware` tag
`v2.7.26.54e0d8d0`, `src/modules/AdminModule.cpp`:

```
void AdminModule::handleGetOwner(const meshtastic_MeshPacket &req)
{
    if (req.decoded.want_response) {
        meshtastic_AdminMessage res = meshtastic_AdminMessage_init_default;
        res.get_owner_response = owner;
        res.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
        setPassKey(&res);
        myReply = allocDataProtobuf(res);
        ...
    }
}
```

`AdminModule` only builds and queues a `get_owner_response` when the
INCOMING request's `Data.want_response` bit is set — the same bit the
Python CLI sets via `wantResponse=True` on every admin read. This
library's `mc_send_get_owner_request` never set it on any send, so a
real `AdminModule` silently declined to reply every time; the
mock-only test harness that originally validated round 1 doesn't
enforce this check the way real firmware does, so nothing caught it
before the bench.

**Fix**: `mc_send_data_packet_ex` (`meshclient/src/mc_client.c`) gained
a `want_response` parameter, threaded onto `meshtastic_Data.
want_response` on the encoded packet; `mc_send_get_owner_request` is
the ONE caller that passes `true`. Verified by decoding the actual
outgoing packet back off the wire in
`feat_get_owner_request_encodes_the_request`
(`meshclient/tests/test_meshclient.c`), not just asserting the call
returned 0.

**Bug 2 — a false positive**, found on the same bench run, independent
of bug 1: `name Jake` (re-pushing the puck's OWN CURRENT name — this
feature's documented retry mechanism for a push that may have silently
failed) reported `confirmed=1` **instantly**, with `ack=none
reply=none` — before any reply could possibly have arrived yet. Cause:
`shell_mesh_name_confirmed` compared the stored name against the
CACHED `mesh_owner_name` with no notion of *when* that cache was last
written relative to the push it was supposedly confirming. A value left
over from an EARLIER confirmation (or the boot prefill) that happened
to already equal the newly-pushed text read as "confirmed" for a
brand-new push nothing had actually answered yet.

**Fix**: a push-generation counter. `name_pushed_seq` (`ff_shell.c`)
increments once per attempted push; `mesh_owner_name_seq` is stamped
with the CURRENT generation every time `mesh_owner_name` is written —
both by the self-NodeInfo path (`shell_ev_node`) and the
`get_owner_response` path (`shell_ev_owner`). `shell_mesh_name_confirmed`
now additionally requires `mesh_owner_name_seq >= name_pushed_seq`: an
observation stamped from an EARLIER generation is stale and reads as
pending, never confirmed, regardless of what string it holds. Both
counters start at 0 and neither resets independently of the other, so
the pre-any-push case (an observation stamped 0 vs. generation 0) still
compares equal — this is exactly what keeps the boot-prefill
"(from_node)" ✓ semantics working: an observation arriving before the
wearer has ever pushed anything is not stale relative to "no push yet".

A new state, `mismatch`, was added alongside `confirmed`/pending/NAK: a
FRESH observation (same freshness test as `confirmed`) that does NOT
match the pushed name — e.g. someone else re-set the owner in between.
This is distinct from a routing NAK, which is silent on what the admin
module's owner actually ended up being; it is also distinct from plain
pending, which means no fresh observation has arrived at all yet.

**Bench console.** `name`'s output line gains two more fields:

```
dbg: name stored=<my_name> mesh=<mesh_owner_name|unknown> confirmed=<0|1>[ (from_node)] seq=<N> pushed=<none|long/short> ack=<none|ok|nak> reply=<none|long/short> mismatch=<0|1>
```

- `seq=` — the push-generation counter itself (0 before any push this
  session), the bench-visible form of the stale-equality fix above.
- `mismatch=` — see the state's own description just above.

**Mutation-verified, both bugs, against the pre-fix code**: removing
the `mesh_owner_name_seq >= name_pushed_seq` clause fails exactly
`S_name_recommitting_the_same_name_does_not_falsely_confirm_from_stale_mesh_state`
and `S_name_recommit_stale_self_nodeinfo_does_not_falsely_confirm_either`
(`test_shell.c`) plus
`dbgconsole_name_recommitting_the_same_name_does_not_falsely_confirm_from_stale_mesh_state`
(`test_debug_console.c`) — nothing else in either 248/34-test suite;
reverting `mc_send_get_owner_request`'s `want_response` back to `false`
fails exactly `feat_get_owner_request_encodes_the_request`
(`test_meshclient.c`) and nothing else in the 91-test meshclient suite.
Both mutations reverted after confirming.

## The puck's back header (photo, 2026-09-04)

2×10 at 1.27 mm pitch. Left column top→bottom: `13 · 12 · RXD · TXD · G · 3V3 · SDA · SCL · G · BAT`.
Right column: `17 · 16 · 15 · 14 · 1 · 0 · DP · DN · G · 5V`. The four link wires sit
together on left rows 3–6 (RXD 44, TXD 43, G, 3V3). Of the extras: 14/16/17 are the
SD card, 15 is the mic clock; 12/13/0/1 are free GPIO.

**Power option C (via the header):** `BAT` → XIAO `BAT+` pad, `G` → `BAT-`. Measure `BAT`
with the puck OFF first: ~3.7–4.2 V = raw cell (XIAO stays on when the puck latches off);
0 V = behind the latch (XIAO powers down with the puck — preferable). One charger at a time.

**Soldering an IDC ribbon directly:** on a 2×10 IDC, conductors alternate rows
(conductor 1 → pin 1, 2 → the opposite pin, 3 → next down the first row, …); pin 1 is the
red-stripe edge. Confirm each stripped conductor against the silkscreen with a continuity
beep before soldering; only 4–5 conductors are needed (RXD, TXD, G, 3V3, optionally BAT).
Heat-shrink each joint and strain-relieve the ribbon to the case.

## Compass (magnetometer)

S15's heading driver (`firmware/targets/esp32s3/components/ff_compass/`) reads
a GY-273 magnetometer off the SAME back header, plus the board's own onboard
QMI8658 6-axis IMU (soldered to the main PCB, no wiring needed) for tilt
compensation. Before this driver landed, `heading_deg` read -1 forever
(docs/specs/S12-first-run.md's 2026-09-03 amendment) — see
`firmware/targets/esp32s3/components/ff_compass/include/ff_compass.h` for the
full contract and register-level citations; this section is the wiring +
bench-facing summary.

**Wiring:** the GY-273's four pins go on the same back header used for the
comms-brain link above — `VCC`→`3V3`, `GND`→`G`, `SDA`→the header's `SDA`
(GPIO11), `SCL`→the header's `SCL` (GPIO10). `DRDY` is unconnected (this
driver polls, it does not use the data-ready interrupt). This is the SAME
physical I2C bus the SPD2010 touch controller (0x53) and TCA9554 IO expander
(0x20) already share — the compass driver adds its own device(s) onto that
existing bus (`ff_display_i2c_bus()`), it never opens a second I2C master on
these pins.

**Chip auto-detection:** older GY-273 boards actually carry a **QMC5883L**
(I2C address `0x0D`) even when silkscreened "HMC5883L"; a genuine
**HMC5883L** (`0x1E`) does turn up on some. Current-production GY-273
clones increasingly ship a **QMC5883P** instead — QST's successor part, I2C
address `0x2C`, CHIP_ID register (0x00) reading `0x80` — confirmed on the
coordinator's own bench 2026-09-05 (a real puck with a QMC5883P-equipped
GY-273 wired and powered; the `i2c` scan above is that capture). `ff_compass_init`
probes all three at boot via each chip's own identification registers and
logs which it found (or "no magnetometer" if none ACKs/identifies — the
puck still boots and runs; the Radar arrow just cannot point). If something
answers at `0x2C` but its chip id isn't `0x80`, the driver logs the id it
actually got and treats the device as unidentified rather than guessing —
same honesty contract as every other "found but didn't check out" case in
this file. The onboard **QMI8658** IMU is at `0x6B` (alt strap `0x6A`); if
it fails to identify, the driver falls back to an assumed-level accel and
logs `compass: no IMU — assuming level` — tilt rejection is unavailable on
that path (a synthesized always-level reading can never indicate tilt).

**Orientation — verify on the bench.** The magnetometer's mounting
orientation (which physical axis is which) is an ASSUMPTION, documented and
isolated to one small `#define` table in `ff_compass.c` ("Axis mapping:
sensor frame -> board frame"), not verified against a real GY-273 glued into
a case yet. The QMC5883P gets its OWN row in that table
(`FF_MAG_QMC5883P_BOARD_*`, currently seeded with the same values as the
QMC5883L/HMC5883L row since it's the same physical module footprint) —
independently correctable without touching the other chips' mapping, since
its internal die orientation is not assumed identical to theirs. To
check/correct either row:

1. Flash a build with `CONFIG_FF_COMPASS=y` (default) and get to the Radar
   face with at least one paired friend so the arrow renders.
2. Rotate the puck flat (screen up) through a full circle by hand, slowly.
   The arrow should track — pointing at the same real-world friend direction
   regardless of which way the puck is held.
3. If the arrow doesn't move at all: check `ff_compass: no magnetometer` /
   `no IMU` in the boot log first — a wiring or address problem, not an axis
   problem.
4. If the arrow moves but in the wrong direction: 90°-off (arrow leads or
   lags the true rotation) means the X/Y source axes are swapped in the
   `FF_MAG_BOARD_*_SRC` (or, on a QMC5883P board, `FF_MAG_QMC5883P_BOARD_*_SRC`)
   defines; mirrored (arrow turns the opposite way from the puck) means a
   sign needs flipping (the matching `*_SIGN` define). Both are isolated to
   that chip's own row — no other file encodes the mapping, and correcting
   one chip's row never touches another's.
5. Tilting the puck should not make the arrow swing wildly (that's what the
   QMI8658 tilt compensation is for); if it does with the IMU confirmed
   present, check the `FF_IMU_BOARD_*` half of the same table.

**IMU mounting — BENCH-VERIFIED 2026-09-05 (z-sense only).** The QMI8658's
identity mapping was wrong: lying flat, face-up, on the real puck, raw accel
read `(-1721, -260, -7877)` — under the old identity map that lands on board
`-z` (~-1g), which `ff_geo_heading_deg` reads as a ~166° tilt and rejects
every sample (`heading_deg` stuck at -1, always). `FF_IMU_BOARD_X_SIGN` and
`FF_IMU_BOARD_Z_SIGN` are now `-1` (`Y` stays `+1`) — flipping X together
with Z keeps the frame right-handed (a 180° rotation about the board's own
`+y`, i.e. the chip is effectively mounted upside-down relative to the board
frame). With the fix, the same bench pose gives accel `(1714, -257, 7862)` →
board `+z` ≈ `+1g` (level) and a steady heading of 104°. **Still pending:**
this only checks the face-up/face-down (z) sense — the X/Y sense under an
actual physical tilt (nose up/down, roll left/right) has NOT been separately
bench-verified. Do the "tilt without wild swinging" check in step 5 above on
a real puck before trusting tilt compensation; if the arrow rotates the wrong
way (or 90° off) specifically while tilted (not while flat), check
`FF_IMU_BOARD_X_SRC`/`FF_IMU_BOARD_Y_SRC` (axis swap) before assuming another
sign flip is needed.

**Calibration status:** `ff_settings_t.compass_cal` / `.cal_valid`
(`core/include/ff_settings.h`) is the persisted calibration slot; this driver
loads it at boot via `ff_shell_settings()` and applies it if `cal_valid` is
set, else runs uncalibrated (identity — no offset, unit scale, zero
declination). No calibration ritual UI exists yet to ever set `cal_valid`
true (S12's figure-eight ritual, still unimplemented per that spec's own
2026-09-03 amendment) — `ff_compass_set_cal()` is the runtime seam that
ritual will call once it exists. Expect headings to be off by whatever the
local hard/soft-iron environment (case magnets, nearby electronics) imposes
until that ritual ships.
