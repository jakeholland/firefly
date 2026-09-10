#!/usr/bin/env python3
"""check_dram_budget.py — fix/s31-sprites-psram (2026-09-09).

## Why this exists
`scr_music.c`'s glow-sprite table used to be a plain `static` array —
ordinary internal `.dram0.bss` — and on the esp32s3 target internal RAM
is ALSO where `esp_lvgl_port`'s own (DMA-capable) display buffers must
be allocated from. That one 47.5KB table pushed the internal-RAM budget
below what the LVGL port needed, and the device parked forever on the
boot splash: `lvgl_port_add_disp_priv(389): Not enough memory for LVGL
buffer (buf2) allocation!` (bench evidence, main a853bb5, PR #252's S31
canvas renderer — see `firmware/app/screens/scr_music.c`'s own top
comment, "Renderer", for the full writeup). Neither the sim build nor
any existing unit test could ever have caught this: the sim has no
internal-RAM/PSRAM distinction to exhaust, and no CI job read the
device's own map file. This script is that missing check.

## What it checks
Sums `.dram0.data` + `.dram0.bss` (every plain internal-RAM global —
initialized and zero-initialized) from the ESP32-S3 project's own
linker map file (`idf.py build`'s `build/firefly_esp32s3.map`, from
`project(firefly_esp32s3)`, firmware/targets/esp32s3/CMakeLists.txt)
and fails if that total exceeds FF_DRAM_STATIC_BUDGET_BYTES below.

## Where the budget number comes from (this PR's own measurement)
Built firmware/targets/esp32s3 twice, same sdkconfig (the maintainer's
own committed one, CONFIG_FF_BRINGUP_STAGE_3 — the shipping config that
actually links the display/LVGL/screens code; the "defaults"-only CI
leg, stage 1, links none of it and its own static total is always far
below this budget) — once at main a853bb5 (the regression) and once
with this PR's fix:

    dram0_0_seg (internal DRAM, Memory Configuration in the map)
        = 0x53700 = 341,760 bytes total
    before (a853bb5): .dram0.data 0x3d40 (15,680) + .dram0.bss 0x29020
        (167,968) = 183,648 bytes static -- ~48,608 of that is
        scr_music.c's OLD `s_sprites` array alone (matches the bench
        log's own arithmetic exactly: 8 sprites * 6075 bytes).
    after (this PR):   .dram0.data 0x3d40 (15,680) + .dram0.bss 0x1d240
        (119,360) = 135,040 bytes static -- s_sprites is now a lazily-
        PSRAM-allocated pointer, 4 bytes.

The esp_lvgl_port display buffers themselves (`ff_display.c`'s
`disp_cfg`, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, same pool this budget
protects) are FF_LCD_H_RES(412) * FF_LVGL_STRIP_LINES(40) * 2 bytes/px
= 32,960 bytes EACH, and `double_buffer = true` means TWO of them:
~65,920 bytes that must come out of whatever `dram0_0_seg` has left
after `.dram0.data` + `.dram0.bss` at the moment `ff_display_lvgl_
start()` runs (plus task stacks and other MALLOC_CAP_INTERNAL
allocations from other subsystems, all drawing from the same pool).

FF_DRAM_STATIC_BUDGET_BYTES = 163,840 (160 KiB), chosen so that:
  - the a853bb5 regression (183,648 bytes) FAILS this check -- the
    exact bug this PR fixes would have been caught before merge.
  - this PR's own fix (135,040 bytes) PASSES with ~28.8KB (~17.6%)
    headroom for ordinary future growth.
  - what's left over for the internal heap at this budget's ceiling --
    341,760 - 163,840 = 177,920 bytes -- is comfortably more than
    DOUBLE the ~65.9KB the LVGL port's own double-buffered display
    buffers need, leaving real room for task stacks and every other
    MALLOC_CAP_INTERNAL consumer too.
This is a budget on STATIC footprint, not a guarantee the heap will
never fragment -- see app_main.c's `ff_park_lvgl_failure` (this same
PR) for the runtime-diagnostic half of this fix: if the LVGL port ever
fails to allocate anyway (a different consumer having eaten the
runtime heap, say), that failure is no longer silent.

## Usage
    tools/check_dram_budget.py [path/to/firefly_esp32s3.map]

Defaults to `firmware/targets/esp32s3/build/firefly_esp32s3.map`
(resolved relative to this script's own location, matching this
project's other tooling — e.g. `firmware/tests/run_goldens.sh`'s own
CWD-independence convention) when no path is given, which is where
`idf.py build` leaves it by default. Exits 0 and prints the totals on
success; exits 1 with a budget-exceeded message (naming the exact
overage and pointing at this file's own budget-derivation comment
above) on failure; exits 2 if the map file is missing or unparseable
(a distinct code so CI logs never read a "map file not found" mistake
as "budget exceeded" — the two mean very different things to whoever
is triaging the failure).
"""
import re
import sys
from pathlib import Path

# See this file's own top comment, "Where the budget number comes
# from", for the full derivation.
FF_DRAM_STATIC_BUDGET_BYTES = 163_840  # 160 KiB

# GNU ld map file section-header line, e.g.:
#   .dram0.bss      0x3fc992d0     0x5c08
# Section name at column 0, VMA, then the section's TOTAL size in hex --
# NOT one of the per-symbol lines further indented under it (those start
# with whitespace before the leading '.').
_SECTION_RE = re.compile(r"^(\.dram0\.(?:data|bss))\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s*$")


def parse_dram_sections(map_text: str) -> dict:
    """Returns {'.dram0.data': int, '.dram0.bss': int} section sizes (bytes)."""
    sizes = {}
    for line in map_text.splitlines():
        m = _SECTION_RE.match(line)
        if m:
            name, hex_size = m.group(1), m.group(2)
            # A well-formed map lists each top-level section exactly once;
            # if that ever stops holding, fail loudly rather than silently
            # pick one (a duplicate would mean this regex is matching the
            # wrong kind of line, e.g. a per-symbol '.dram0.bss.foo' row --
            # checked against, but a future ld/toolchain format change is
            # exactly the case this guard exists for).
            if name in sizes:
                raise ValueError(f"section {name!r} appears more than once in the map file — " "regex too broad?")
            sizes[name] = int(hex_size, 16)
    return sizes


def main(argv: list) -> int:
    if len(argv) > 2:
        print(f"usage: {argv[0]} [path/to/firefly_esp32s3.map]", file=sys.stderr)
        return 2

    if len(argv) == 2:
        map_path = Path(argv[1])
    else:
        # firmware/tools/check_dram_budget.py -> firmware/ -> targets/esp32s3/build/...
        repo_firmware_dir = Path(__file__).resolve().parent.parent
        map_path = repo_firmware_dir / "targets" / "esp32s3" / "build" / "firefly_esp32s3.map"

    if not map_path.is_file():
        print(f"check_dram_budget.py: no map file at {map_path}", file=sys.stderr)
        print("  build first: idf.py -C firmware/targets/esp32s3 build", file=sys.stderr)
        return 2

    try:
        sizes = parse_dram_sections(map_path.read_text(errors="replace"))
    except ValueError as exc:
        print(f"check_dram_budget.py: {exc}", file=sys.stderr)
        return 2

    missing = [name for name in (".dram0.data", ".dram0.bss") if name not in sizes]
    if missing:
        print(f"check_dram_budget.py: {map_path} is missing section(s) {missing} — " "not a valid esp32s3 map file?",
              file=sys.stderr)
        return 2

    data_bytes = sizes[".dram0.data"]
    bss_bytes = sizes[".dram0.bss"]
    total_bytes = data_bytes + bss_bytes

    print(f"check_dram_budget.py: {map_path}")
    print(f"  .dram0.data = {data_bytes:>7} bytes")
    print(f"  .dram0.bss  = {bss_bytes:>7} bytes")
    print(f"  total       = {total_bytes:>7} bytes  (budget: {FF_DRAM_STATIC_BUDGET_BYTES} bytes)")

    if total_bytes > FF_DRAM_STATIC_BUDGET_BYTES:
        overage = total_bytes - FF_DRAM_STATIC_BUDGET_BYTES
        print(
            f"check_dram_budget.py: FAIL — {overage} bytes over budget. A new/grown internal-RAM "
            "static (.dram0.data or .dram0.bss) is crowding out the internal/DMA RAM esp_lvgl_port "
            "needs for its own display buffers — the exact class of regression fix/s31-sprites-psram "
            "(2026-09-09) fixed (scr_music.c's glow-sprite table). See this file's own top comment, "
            "\"Where the budget number comes from\", for the full derivation, and move the new/grown "
            "static to PSRAM (heap_caps_calloc/malloc(..., MALLOC_CAP_SPIRAM), lazily allocated and "
            "NULL-safe — scr_music.c's music_ensure_sprites is the reference pattern) unless it "
            "genuinely must be internal (DMA-touched, or written before PSRAM is initialized).",
            file=sys.stderr,
        )
        return 1

    print(f"check_dram_budget.py: PASS — {FF_DRAM_STATIC_BUDGET_BYTES - total_bytes} bytes of headroom.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
