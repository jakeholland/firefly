#!/usr/bin/env python3
"""check_no_subtick_sleeps.py — fix/mic-dump-device-path.

## Why this exists
This project's ESP32-S3 build ships `CONFIG_FREERTOS_HZ=100`
(firmware/targets/esp32s3/sdkconfig) — a 10ms tick period.
`pdMS_TO_TICKS(ms)` truncates toward zero, so `pdMS_TO_TICKS(ms)` for
any `ms` under 10 computes to exactly 0 ticks. `vTaskDelay(0)` is a bare
task-yield, not a sleep, and a 0-tick timeout on a blocking driver call
(`usb_serial_jtag_write_bytes`, `xSemaphoreTake`, ...) returns almost
immediately instead of actually waiting — silently turning a "sleep/wait
for N ms" call site into "spin, consuming whatever budget it was meant
to pace over in near-zero wall-clock time instead." That is precisely
what shipped in `dbgconsole_mic_dump`'s FF_MIC_DUMP_POLL_MS=5 poll (bench
evidence: `mic dump 8` reported `frames=0` twice — see
docs/specs/S30-audio-input.md, "mic dump device notes") and is the same
bug class PR #216 already fixed once for the i2c bus-scan's 5ms retry
timeout. Neither the sim build nor any existing unit test can catch
this: the sim never links FreeRTOS at all, so `pdMS_TO_TICKS`'s tick-
rounding has no host-side analogue to test against (see
`firmware/targets/esp32s3/main/tests/test_ticks_at_least_one.c` for the
pure-arithmetic test that covers the HELPER instead). This script is the
static, whole-tree guard: it never lets a bare sub-tick
`pdMS_TO_TICKS(<10)` literal land in this project's own esp32s3 source
again, regardless of which file it's added to.

## What it checks
Greps every tracked `.c`/`.h` file under `firmware/targets/esp32s3/main`
and `firmware/targets/esp32s3/components` (walked directly — this
script has no git dependency) for a `pdMS_TO_TICKS(` call whose argument
is a literal, base-10 integer strictly less than `FF_TICK_PERIOD_MS`
(10 — this project's one committed CONFIG_FREERTOS_HZ, see
`firmware/targets/esp32s3/sdkconfig`'s own `CONFIG_FREERTOS_HZ=100`).
Every such call site found is a build-breaking finding: this project's
convention (fix/mic-dump-device-path) is that a millisecond period
below one tick goes through `ff_ticks_at_least_one()`
(`firmware/targets/esp32s3/main/ff_ticks_at_least_one.h`), never a bare
`pdMS_TO_TICKS(...)`.

`managed_components/` (ESP-IDF-component-manager-fetched, gitignored,
never ours to lint — same "not ours" posture this repo already takes
for vendored/generated code, e.g. `firmware/meshclient/CMakeLists.txt`'s
own nanopb comment) and `build/` (generated) are always excluded.
`_VENDORED_EXCLUDE_DIRS` below additionally excludes vendored-but-
committed components — currently `esp_lcd_touch_spd2010`, a patched
upstream copy (see that file's own "FIREFLY VENDORED COPY" header
comment) whose `reset()` ALREADY guards its own 2ms/22ms resets with an
inline `(delay_tick > 0) ? delay_tick : 1` — the same fix this project's
own `ff_ticks_at_least_one()` gives a name to, just written out by hand
by the original vendor before this project ever touched the file. Not
"ours" to rewrite into the shared helper, and not a bug.

## Known limitation (stated plainly, not hidden)
This is a literal-integer grep, not a preprocessor or dataflow analysis:
`pdMS_TO_TICKS(SOME_MACRO)` where `SOME_MACRO` expands to a value under
10 is NOT caught (`FF_MIC_WATCH_PERIOD_MS`-style named periods in this
codebase are all comfortably >= 20ms today — grep for
`pdMS_TO_TICKS(FF_` across the tree to audit those by hand if a new one
is ever added with a suspiciously small value). What this script DOES
guarantee: nobody can reintroduce the EXACT shipped bug (a bare small
integer literal handed straight to `pdMS_TO_TICKS`) without this check
failing loudly.

## Usage
    firmware/tools/check_no_subtick_sleeps.py

Exits 0 (and prints nothing) if the tree is clean. Exits 1 and prints
one `file:line: pdMS_TO_TICKS(N)` finding per line otherwise.
"""
import re
import sys
from pathlib import Path

# This project's one committed CONFIG_FREERTOS_HZ (firmware/targets/
# esp32s3/sdkconfig) is 100 -> a 10ms tick period. Any ms literal below
# this is a sub-tick argument.
FF_TICK_PERIOD_MS = 10

_PDMS_LITERAL_RE = re.compile(r"pdMS_TO_TICKS\(\s*([0-9]+)\s*[uU]?\s*\)")

# Vendored-but-committed components this repo does not own the contents
# of — see this file's own top comment, "What it checks", for why.
_VENDORED_EXCLUDE_DIRS = frozenset({"esp_lcd_touch_spd2010"})

_ALWAYS_EXCLUDE_DIR_NAMES = frozenset({"managed_components", "build"})


def _is_excluded(path: Path) -> bool:
    parts = set(path.parts)
    return bool(parts & _ALWAYS_EXCLUDE_DIR_NAMES) or bool(parts & _VENDORED_EXCLUDE_DIRS)


def _strip_comments(lines: list) -> list:
    """Returns `lines` with C `//` and `/* ... */` comment TEXT blanked out
    (line count and non-comment column positions preserved, so line
    numbers in findings still point at the real source line). This
    codebase's own doc comments routinely quote code containing
    `pdMS_TO_TICKS(<N>)` as an example (this very script's docstring
    does, and app_main.c's `ff_ticks_at_least_one` doc comment does) —
    without this, the check would flag its own documentation. A simple
    per-line state machine, not a real C tokenizer: does not understand
    string/char literals containing `/*`/`//` (none of this project's
    own pdMS_TO_TICKS call sites are ever inside a string literal, so
    that gap is never live here)."""
    out = []
    in_block = False
    for line in lines:
        result_chars = []
        i = 0
        n = len(line)
        while i < n:
            if in_block:
                end = line.find("*/", i)
                if end == -1:
                    i = n  # rest of line is inside the block comment
                else:
                    i = end + 2
                    in_block = False
                continue
            two = line[i : i + 2]
            if two == "//":
                break  # rest of line is a line comment
            if two == "/*":
                start_block = line.find("*/", i + 2)
                if start_block == -1:
                    in_block = True
                    i = n
                else:
                    i = start_block + 2
                continue
            result_chars.append(line[i])
            i += 1
        out.append("".join(result_chars))
    return out


def find_subtick_sleeps(root: Path) -> list:
    """Returns a list of "file:line: snippet" strings, one per finding."""
    findings = []
    for path in sorted(root.rglob("*")):
        if path.suffix not in (".c", ".h") or not path.is_file() or _is_excluded(path):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        raw_lines = text.splitlines()
        code_lines = _strip_comments(raw_lines)
        for lineno, (raw_line, code_line) in enumerate(zip(raw_lines, code_lines), start=1):
            for m in _PDMS_LITERAL_RE.finditer(code_line):
                ms = int(m.group(1))
                if ms < FF_TICK_PERIOD_MS:
                    findings.append(f"{path}:{lineno}: {raw_line.strip()}")
    return findings


def main(argv: list) -> int:
    if len(argv) > 1:
        print(f"usage: {argv[0]}", file=sys.stderr)
        return 2

    # Resolved relative to this script's own location (matching this
    # project's other tooling, e.g. check_dram_budget.py's own CWD-
    # independence convention), not the caller's CWD.
    esp32s3_dir = Path(__file__).resolve().parent.parent / "targets" / "esp32s3"
    if not esp32s3_dir.is_dir():
        print(f"error: {esp32s3_dir} not found", file=sys.stderr)
        return 2

    findings = []
    for sub in ("main", "components"):
        d = esp32s3_dir / sub
        if d.is_dir():
            findings.extend(find_subtick_sleeps(d))

    if findings:
        print(f"found {len(findings)} bare pdMS_TO_TICKS(<{FF_TICK_PERIOD_MS}) sub-tick literal(s) — "
              "route these through ff_ticks_at_least_one() instead "
              "(firmware/targets/esp32s3/main/ff_ticks_at_least_one.h):", file=sys.stderr)
        for f in findings:
            print(f"  {f}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
