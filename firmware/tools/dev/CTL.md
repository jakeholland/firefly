# ffsim control socket (S13 slice c)

`ffsim --headless --ctl PORT` opens a line-oriented JSON control socket on
`127.0.0.1:PORT` (loopback only — never binds `0.0.0.0`). It's what
`firmware/tests/e2e/` and any interactive scripting drive ffsim with,
instead of adding a `--script FILE.py` flag (see
`docs/specs/S13-sim-target.md`: "scripting stays external").

Currently requires `--headless` — see `targets/sim/main.c`'s "ctl loop"
section for why window mode isn't wired up too. `--ctl-out DIR` sets
where the `screenshot` command is allowed to write (see that command's
section below) — optional; ffsim picks a sensible default if omitted.

## Protocol

Newline-delimited JSON, one request per line in, exactly one JSON response
line out, in order, over a single TCP connection. `ffsim` serves one
client connection at a time; connect, send commands, and either close the
connection or send `{"cmd":"quit"}` to shut ffsim down entirely.

A minimal client (Python):

```python
import json, socket

s = socket.create_connection(("127.0.0.1", 9000))
f = s.makefile("rwb", buffering=0)

def cmd(obj):
    f.write((json.dumps(obj) + "\n").encode())
    return json.loads(f.readline())

print(cmd({"cmd": "state"}))
```

Every response is a JSON object with `"ok": true` or `"ok": false` (plus
`"error": "<reason>"` on failure). Malformed input, an unknown `cmd`, or a
command missing required fields never crashes ffsim — you always get an
`{"ok":false,...}` line back.

**Bounded reads:** each line is capped at `FF_CTL_MAX_LINE` (4096 bytes,
including the trailing `\n`) — see `targets/sim/ctl_server.h`. A line
over that bound gets a `{"ok":false,"error":"line too long"}` response and
the connection silently discards bytes up to the next `\n` before
resuming normal parsing (no need to reconnect).

**Idle timeout:** a connected client that goes 30s (`FF_CTL_DEFAULT_
IDLE_TIMEOUT_MS`) without completing a single command is dropped, so a
stalled or misbehaving client can't permanently starve the socket (the
listen backlog is 1 — this is a single-client dev tool, not a
multiplexed server). The very next poll accepts a replacement client
immediately.

## Commands

### `tap`

```json
{"cmd": "tap", "x": 120, "y": 340}
```

Injects a press-then-release at screen coordinates `(x, y)` (pixels, the
456×456 sim window's coordinate space) through a real LVGL pointer input
device — the same mechanism a touch driver would use. Until S06+ lands
real interactive screens, there's nothing on screen to click yet, but the
injection path itself is exercised and unit-tested
(`targets/sim/tests/test_ctl_server.c`).

`x`/`y` must be finite numbers within `[-32768, 32767]`
(`FF_CTL_TAP_COORD_MIN`/`MAX` in `ctl_server.h`) — rejected otherwise,
*before* either value ever reaches a narrowing cast to the sim's internal
coordinate type. (An earlier version of this socket didn't validate this:
`{"cmd":"tap","x":1e300,"y":0}` reached an out-of-range `(lv_coord_t)`
cast, undefined behavior per C11 6.3.1.4, confirmed under
`-fsanitize=undefined`. Fixed; pinned as a regression test.)

Response: `{"ok": true}`, or `{"ok": false, "error": "tap requires numeric x,y"}`
or `{"ok": false, "error": "tap x,y must be finite and within [-32768, 32767]"}`.

### `swipe`

```json
{"cmd": "swipe", "dir": "left"}
```

`dir` must be exactly `"left"` or `"right"`. Injects a press → several
move steps → release sequence through the same pointer indev as `tap`.

Response: `{"ok": true}`, or `{"ok": false, "error": "swipe dir must be left or right"}`.

### `clock`

```json
{"cmd": "clock", "advance_ms": 500}
```

Advances the mock clock by `advance_ms` (must be `>= 0`). **Requires
`ffsim` to have been started with `--mock-clock`** — without it, ffsim's
notion of "now" is real wall-clock time and can't be driven by hand;
you'll get `{"ok": false, "error": "clock control requires --mock-clock"}`.

Response: `{"ok": true}` on success.

### `state`

```json
{"cmd": "state"}
```

Dumps the current `ff_app_state_t` as JSON — **the exact schema
`firmware/tests/fixtures/README.md` documents**, so a `state` dump can be
saved and reloaded as a fixture (`ffsim --fixture <saved>.json`) and
round-trips byte-for-byte through another `state` dump
(`targets/sim/tests/test_fixture.c`'s `dump_then_reload_round_trips_*`
tests pin this).

Response: `{"ok": true, "state": {"fixture": "...", "face": "radar", "radar": {...}, ...}}`.

### `screenshot`

```json
{"cmd": "screenshot", "path": "out.png"}
```

Renders the current frame (forces a fresh `lv_refr_now()` first, so this
always reflects the latest `tap`/`swipe`/`clock`/live-mesh-driven state,
not a stale cached frame) to a PNG at `path`.

**`path` is a RELATIVE name, confined to a single configured output
root** — never an arbitrary filesystem path. The root is `--ctl-out DIR`
if given (created if missing), else the `--screenshot DIR` this same
`ffsim` invocation was started with (must already exist), else a fresh
temp directory ffsim creates itself; ffsim prints the resolved root at
startup (`ffsim: ctl screenshot writes confined to ...`). Rejected:
- an absolute `path`,
- any `..` path component, anywhere in `path`,
- a path whose containing directory doesn't already exist under the
  root, or resolves (symlinks included) outside it,
- a leaf name that already exists as a symlink.

(This used to be unconstrained: `{"cmd":"screenshot","path":"/tmp/
anything.png"}` wrote exactly there, an arbitrary-file-write primitive
for anything the ffsim process can write to — loopback-only binding
limited who could reach it, but that's not the same as it being safe.
Fixed; see `targets/sim/ctl_out_path.h` for the confinement policy and
`targets/sim/tests/test_ctl_out_path.c` for the absolute/`..`/symlink-
escape regression tests, including a symlinked-root-itself case.)

Response: `{"ok": true}`, or `{"ok": false, "error": "screenshot write failed"}`
(bad directory, permissions, etc — see stderr for the underlying reason),
or one of the path-confinement rejection messages above (e.g.
`{"ok": false, "error": "screenshot path escapes the configured output root (--ctl-out)"}`).

### `quit`

```json
{"cmd": "quit"}
```

Response: `{"ok": true}`, then ffsim closes the socket and exits 0. This
is the clean-shutdown path the e2e harness and `crew_sim.py`-driven
sessions use instead of killing the process.

## Example session

```
$ ./build/ffsim --headless --ctl 9000 --mock-clock &
ffsim: ...
ffsim: ctl socket listening on 127.0.0.1:9000

$ printf '{"cmd":"clock","advance_ms":1000}\n{"cmd":"state"}\n{"cmd":"quit"}\n' | nc 127.0.0.1 9000
{"ok":true}
{"ok":true,"state":{"fixture":"","face":"radar","radar":{...},...}}
{"ok":true}
```

## Live mode

Combine with `--connect HOST:PORT` (and optionally `--pack FILE.json`) to
have `state` dumps reflect a real meshtasticd connection instead of a
static fixture — see `targets/sim/live.h`'s top comment for exactly what
that wires up (and its deliberately-scoped-down parts), and
`firmware/tools/dev/README.md` for the full dev-loop walkthrough.
