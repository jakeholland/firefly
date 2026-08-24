#!/usr/bin/env python3
"""wall_crosscheck.py — S16 slice b0: differential test of ff_wall_split_local
against Python's `datetime`.

    python3 firmware/tools/dev/wall_crosscheck.py            # 20k pairs
    python3 firmware/tools/dev/wall_crosscheck.py -n 200000  # more
    python3 firmware/tools/dev/wall_crosscheck.py --seed 42

Exit status 0 on a clean sweep, 1 on any mismatch (so it can be dropped
into a shell chain). Requires a C compiler and Python 3.7+; nothing else.

## Why this is not a ctest

`test_wall.c` covers the boundaries that matter — the 06:00 festival-day
roll, the year rollback, the plausibility window's four edges — with
hand-derived expected values. What it cannot do is check the civil-date
arithmetic against an *independent implementation*: any expectation
written by the same person who wrote the algorithm shares its
misconceptions, and the failure mode of Hinnant's era algorithms is
exactly the kind that hand-picked cases miss (a specific century, a
specific leap-year boundary, a specific offset sign).

So this is a differential test: same inputs into `ff_wall_split_local`
and into `datetime`, compare. `datetime` is the oracle because it is a
genuinely separate implementation maintained by other people. It needs a
compiler and a Python interpreter running together, which is why it lives
here as a dev harness rather than in ctest — CI's unit-test job builds C
and nothing else.

Run it after touching `ff_wall_year_from_days`, `ff_wall_days_from_civil`,
`ff_wall_floor_div`, or the festival-day shift in `ff_wall_split_local`.
The PR that introduced the module (#37) reported 20,000 clean pairs; the
reviewer independently reproduced it at 200,000.

## What is actually compared

For a uniformly random unix timestamp inside the module's plausibility
window and a uniformly random UTC offset across the real-world range
(UTC-12:00 .. UTC+14:00):

  - `day_doy`  — the festival day's day-of-year, where the festival day
                 rolls at 06:00 local (ff_sched.h's contract), NOT at
                 midnight. The oracle reimplements that roll from the
                 spec's wording rather than from ff_wall.c's shift trick,
                 so the two arrive at it by different routes.
  - `now_min`  — minutes from midnight of `day_doy`'s calendar date,
                 which the contract puts in [360, 1800). Asserted to be
                 in range on every single sample, independently of
                 whether it matched the oracle.

Note the oracle deliberately uses naive `datetime` arithmetic with an
explicit integer offset. It must NOT use a tz database: the module models
a fixed per-event offset with no DST rule (see ff_wall.h), and pulling in
zoneinfo would test a different contract and produce spurious failures
across DST transitions.
"""

import argparse
import datetime as dt
import pathlib
import random
import shutil
import subprocess
import sys
import tempfile

REPO_FIRMWARE = pathlib.Path(__file__).resolve().parents[2]
CORE = REPO_FIRMWARE / "core"

# Mirrors ff_wall.h. Read from the header at runtime rather than
# duplicated, so a bump to the plausibility window can't leave this
# harness sweeping the wrong range.
DRIVER_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include "ff_wall.h"

/* Reads "<unix_s> <offset_min>" lines on stdin, writes
 * "<day_doy> <now_min>" or "REJECT" per line on stdout. */
int main(void)
{
    long long u;
    int off;
    while (scanf("%lld %d", &u, &off) == 2) {
        uint16_t doy = 0;
        int16_t now_min = 0;
        if (ff_wall_split_local((int64_t)u, (int16_t)off, &doy, &now_min)) {
            printf("%u %d\n", (unsigned)doy, (int)now_min);
        } else {
            printf("REJECT\n");
        }
    }
    return 0;
}

/* Print the constants the harness needs so they are never duplicated in
 * Python. Invoked via a separate entry point below. */
"""

CONSTS_C = r"""
#include <stdio.h>
#include "ff_wall.h"
int main(void)
{
    printf("%lld %lld %d %d %d\n", (long long)FF_WALL_EPOCH_FLOOR,
           (long long)FF_WALL_EPOCH_CEILING, (int)FF_WALL_OFFSET_MIN_LO,
           (int)FF_WALL_OFFSET_MIN_HI, (int)FF_WALL_DAY_START_MIN);
    return 0;
}
"""


def compile_c(tmp: pathlib.Path, name: str, source: str) -> pathlib.Path:
    src = tmp / f"{name}.c"
    src.write_text(source)
    exe = tmp / name
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        sys.exit("no C compiler found (looked for cc, gcc, clang)")
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(CORE / "include"), str(src), str(CORE / "src" / "ff_wall.c"),
         "-o", str(exe)],
        check=True,
    )
    return exe


def oracle(unix_s: int, offset_min: int, day_start_min: int):
    """(day_doy, now_min) per ff_sched.h's festival-day contract.

    Derived from the contract's wording — "clock minutes [0, 360) on a
    calendar date map to the PREVIOUS festival day with now_min in
    [1440, 1800)" — rather than from ff_wall.c's shift-and-floor-divide,
    so a bug in the shift cannot hide behind a matching bug here.
    """
    local = dt.datetime(1970, 1, 1) + dt.timedelta(seconds=unix_s + offset_min * 60)
    minute_of_day = local.hour * 60 + local.minute
    if minute_of_day < day_start_min:
        day = local.date() - dt.timedelta(days=1)
        now_min = minute_of_day + 1440
    else:
        day = local.date()
        now_min = minute_of_day
    return day.timetuple().tm_yday, now_min


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-n", "--pairs", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=20260801)
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        consts_exe = compile_c(tmp, "wall_consts", CONSTS_C)
        floor_s, ceiling_s, off_lo, off_hi, day_start = (
            int(x) for x in subprocess.run(
                [str(consts_exe)], capture_output=True, text=True, check=True
            ).stdout.split()
        )
        print(f"window [{floor_s}, {ceiling_s}) "
              f"= [{dt.datetime.fromtimestamp(floor_s, dt.timezone.utc):%Y-%m-%d}, "
              f"{dt.datetime.fromtimestamp(ceiling_s, dt.timezone.utc):%Y-%m-%d}), "
              f"offsets [{off_lo}, {off_hi}], day starts at {day_start}")

        driver_exe = compile_c(tmp, "wall_driver", DRIVER_C)

        rng = random.Random(args.seed)
        cases = [(rng.randrange(floor_s, ceiling_s), rng.randint(off_lo, off_hi))
                 for _ in range(args.pairs)]

        out = subprocess.run(
            [str(driver_exe)],
            input="".join(f"{u} {o}\n" for u, o in cases),
            capture_output=True, text=True, check=True,
        ).stdout.splitlines()

    if len(out) != len(cases):
        sys.exit(f"driver returned {len(out)} lines for {len(cases)} cases")

    mismatches = 0
    lo_min, hi_min = 10**9, -10**9
    for (unix_s, off), line in zip(cases, out):
        if line == "REJECT":
            mismatches += 1
            print(f"REJECTED in-window input: unix={unix_s} off={off}")
            continue
        got_doy, got_now = (int(x) for x in line.split())
        want_doy, want_now = oracle(unix_s, off, day_start)
        lo_min, hi_min = min(lo_min, got_now), max(hi_min, got_now)
        if not (day_start <= got_now < day_start + 1440):
            mismatches += 1
            print(f"WINDOW VIOLATION unix={unix_s} off={off} now_min={got_now}")
        if (got_doy, got_now) != (want_doy, want_now):
            mismatches += 1
            if mismatches <= 10:
                print(f"MISMATCH unix={unix_s} off={off} "
                      f"got=({got_doy},{got_now}) want=({want_doy},{want_now})")

    print(f"checked {len(cases)} pairs, now_min spanned [{lo_min}, {hi_min}], "
          f"{mismatches} mismatches")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
