# ffsim control socket (S13 slice c)

`ffsim --headless --ctl PORT` opens a line-oriented JSON control socket on
`127.0.0.1:PORT` (loopback only — never binds `0.0.0.0`). It's what
`firmware/tests/e2e/` and any interactive scripting drive ffsim with,
instead of adding a `--script FILE.py` flag (see
`docs/specs/S13-sim-target.md`: "scripting stays external").

Currently requires `--headless` — see `targets/sim/main.c`'s "ctl loop"
section for why window mode isn't wired up too.

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

Response: `{"ok": true}`, or `{"ok": false, "error": "tap requires numeric x,y"}`.

### `swipe`

```json
{"cmd": "swipe", "dir": "left"}
```

`dir` must be exactly `"left"` or `"right"`. Injects a press → several
move steps → release sequence through the same pointer indev as `tap`.

Response: `{"ok": true}`, or `{"ok": false, "error": "swipe dir must be \"left\" or \"right\""}`.

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
{"cmd": "screenshot", "path": "/tmp/out.png"}
```

Renders the current frame (forces a fresh `lv_refr_now()` first, so this
always reflects the latest `tap`/`swipe`/`clock`/live-mesh-driven state,
not a stale cached frame) to a PNG at `path`. `path` may be absolute or
relative to ffsim's working directory; the directory must already exist.

Response: `{"ok": true}`, or `{"ok": false, "error": "screenshot write failed"}`
(bad directory, permissions, etc — see stderr for the underlying reason).

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
