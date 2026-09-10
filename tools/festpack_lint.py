#!/usr/bin/env python3
"""festpack_lint.py — static validation of a festpack.json's `schedule`
(and general shape) against the invariants firmware/festpack/src/fp_pack.c
and ff_sched.c assume but do not themselves enforce (the parser is
tolerant by design — see docs/specs/S05-festpack.md).

Checks (spec: docs/specs/S05-festpack.md, this file's own header):
  - every set's `stage` (if not null) names a stage that exists in
    `stages[]`
  - `start`/`end` are plain "HH:MM" wall-clock times with HH in 00..23
    (fp_pack.c's fp_min_from_hhmm cap — the pack NEVER encodes an hour
    past 23; an after-midnight set says so with `night`/`end_day`)
  - `night`, when present, is either the set's own `day` or exactly one
    calendar day before it (the 2026-09-09 amendment's contract: a set
    is billed under the festival night it belongs to)
  - `end_day`, when present, is exactly one calendar day after `day`
    and only appears when the set's `end` really does cross midnight
    (end <= start on the clock). Its absence on a crossing set is fine —
    that is the older `end < start` spelling ff_sched.c already folds.
  - every set with known start/end has start < end, compared in the
    festival-NIGHT minute space fp_pack.c folds them into
  - no two sets on the same (stage, night) overlap in time
  - every set's `day` falls within the festival's wall window
    (festival start/end +/- 14 days — see ff_wall_window_from_pack)
  - every `note` fits fp_set_t.note[24] (<= 23 bytes + NUL, i.e. UTF-8
    byte length <= 23)
  - total set count <= FP_MAX_SETS (256)

Usage:
    tools/festpack_lint.py <festpack.json> [<festpack.json> ...]

Exits non-zero (and prints every problem found, not just the first) if
any pack fails a check. Wired into ctest as `festpack_lint` (see
firmware/festpack/CMakeLists.txt) so a bad pack fails the build, not
just a manual run.
"""
import datetime
import json
import sys

FP_MAX_SETS = 256
FP_SET_NOTE_MAX_BYTES = 23  # fp_set_t.note[24], minus the NUL terminator
FP_HH_MAX = 23  # fp_pack.c's fp_min_from_hhmm cap
FP_NIGHT_FOLD_MIN = 360  # fp_pack.c's fallback fold threshold (06:00)
WALL_WINDOW_DAYS = 14
DAY_MIN = 1440


def parse_hhmm(s):
    """Mirrors fp_pack.c's fp_min_from_hhmm: "HH:MM", HH in 0..23, MM in
    0..59. Returns None (the parser's -1/null) for anything else."""
    if not isinstance(s, str) or len(s) != 5 or s[2] != ":":
        return None
    hh, mm = s[:2], s[3:]
    if not (hh.isdigit() and mm.isdigit()):
        return None
    h, m = int(hh), int(mm)
    if h > FP_HH_MAX or m > 59:
        return None
    return h * 60 + m


def iso_date(s):
    try:
        return datetime.date.fromisoformat(s)
    except (ValueError, TypeError):
        return None


def lint_pack(path):
    problems = []
    with open(path, encoding="utf-8") as f:
        pack = json.load(f)

    stage_ids = {s["id"] for s in pack.get("stages", [])}
    schedule = pack.get("schedule", [])

    if len(schedule) > FP_MAX_SETS:
        problems.append(f"{len(schedule)} sets exceeds FP_MAX_SETS ({FP_MAX_SETS})")

    festival = pack.get("festival", {})
    fest_start = festival.get("start")
    fest_end = festival.get("end")
    window_lo = window_hi = None
    if fest_start and fest_end:
        d0, d1 = iso_date(fest_start), iso_date(fest_end)
        if d0 is None or d1 is None:
            problems.append(f"festival.start/end not valid ISO dates: {fest_start!r}/{fest_end!r}")
        else:
            window_lo = d0 - datetime.timedelta(days=WALL_WINDOW_DAYS)
            window_hi = d1 + datetime.timedelta(days=WALL_WINDOW_DAYS)

    # Group entries by (stage, night) for overlap checking, using each
    # entry's own index for a stable, readable error message.
    groups = {}

    for i, entry in enumerate(schedule):
        artist = entry.get("artist", f"<no artist, index {i}>")
        loc = f"schedule[{i}] ({artist!r})"

        stage = entry.get("stage")
        if stage is not None and stage not in stage_ids:
            problems.append(f"{loc}: stage {stage!r} not in stages[] ({sorted(stage_ids)})")

        day = entry.get("day")
        day_date = None
        if day is not None:
            day_date = iso_date(day)
            if day_date is None:
                problems.append(f"{loc}: day {day!r} is not a valid ISO date")
            elif window_lo and (day_date < window_lo or day_date > window_hi):
                problems.append(
                    f"{loc}: day {day!r} outside the festival's wall window "
                    f"({window_lo} .. {window_hi})"
                )

        start_raw = entry.get("start")
        end_raw = entry.get("end")
        start_min = parse_hhmm(start_raw) if start_raw is not None else None
        end_min = parse_hhmm(end_raw) if end_raw is not None else None
        if start_raw is not None and start_min is None:
            problems.append(f"{loc}: start {start_raw!r} is not a valid HH:MM (00..{FP_HH_MAX}:00..59)")
        if end_raw is not None and end_min is None:
            problems.append(f"{loc}: end {end_raw!r} is not a valid HH:MM (00..{FP_HH_MAX}:00..59)")

        # `night`: the festival night the set is billed under. Either the
        # set's own calendar `day` (ordinary set) or exactly one day
        # earlier (after-midnight set). Anything else is a fold
        # fp_pack.c's fp_parse_set_daytime deliberately refuses to guess
        # at — see its comment.
        night = entry.get("night")
        fold_days = None
        if night is not None:
            night_date = iso_date(night)
            if night_date is None:
                problems.append(f"{loc}: night {night!r} is not a valid ISO date")
            elif day_date is None:
                problems.append(f"{loc}: night {night!r} present but day {day!r} is missing/invalid")
            else:
                delta = (day_date - night_date).days
                if delta not in (0, 1):
                    problems.append(
                        f"{loc}: night {night!r} must be day {day!r} or the day before it, "
                        f"but is {delta} day(s) before"
                    )
                else:
                    fold_days = delta

        if fold_days is None and day_date is not None:
            # fp_parse_set_daytime's documented fallback.
            fold_days = 1 if (start_min is not None and start_min < FP_NIGHT_FOLD_MIN) else 0

        # `end_day`: present ONLY when the end lands on a later calendar
        # date than the start, and then exactly one day later.
        end_day = entry.get("end_day")
        end_extra_days = 0
        crosses = start_min is not None and end_min is not None and end_min <= start_min
        if end_day is not None:
            end_day_date = iso_date(end_day)
            if end_day_date is None:
                problems.append(f"{loc}: end_day {end_day!r} is not a valid ISO date")
            elif day_date is None:
                problems.append(f"{loc}: end_day {end_day!r} present but day {day!r} is missing/invalid")
            else:
                delta = (end_day_date - day_date).days
                if delta != 1:
                    problems.append(
                        f"{loc}: end_day {end_day!r} must be exactly one day after day {day!r}, "
                        f"but is {delta} day(s) after"
                    )
                else:
                    end_extra_days = 1
                if end_min is None:
                    problems.append(f"{loc}: end_day {end_day!r} present but end is null/invalid")
                elif not crosses:
                    problems.append(
                        f"{loc}: end_day {end_day!r} present but end {end_raw!r} does not cross "
                        f"midnight after start {start_raw!r} — end_day is only for a set whose "
                        f"end falls on a later calendar date"
                    )
        # A bare `end` at or before `start` with no `end_day` is NOT a
        # problem: it is the older, still-legal spelling of "this set
        # crosses midnight" that ff_sched.c's sched_effective_end folds
        # forward by a day (see ff_sched.h's "Midnight-crossing sets"),
        # and the demo pack uses it. `end_day` is the explicit form
        # fest-almanac emits; both fold to the same minute below.

        # Fold both ends into the festival-NIGHT minute space, exactly as
        # fp_parse_set_daytime does, so comparisons below are the ones
        # ff_sched.c will actually make.
        eff_start = eff_end = None
        if fold_days is not None:
            if start_min is not None:
                eff_start = start_min + fold_days * DAY_MIN
            if end_min is not None:
                extra = end_extra_days if end_day is not None else (1 if crosses else 0)
                eff_end = end_min + (fold_days + extra) * DAY_MIN
        else:
            eff_start, eff_end = start_min, end_min

        if eff_start is not None and eff_end is not None and eff_start >= eff_end:
            problems.append(
                f"{loc}: start {start_raw!r} ({eff_start}) >= end {end_raw!r} ({eff_end}) "
                f"in the festival-night minute space"
            )

        note = entry.get("note")
        if note is not None:
            n = len(note.encode("utf-8"))
            if n > FP_SET_NOTE_MAX_BYTES:
                problems.append(f"{loc}: note {note!r} is {n} bytes, over the {FP_SET_NOTE_MAX_BYTES}-byte fp_set_t.note[24] budget")

        artist_field = entry.get("artist")
        if artist_field is not None and len(artist_field.encode("utf-8")) > 31:
            problems.append(f"{loc}: artist {artist_field!r} is over the 31-byte fp_set_t.artist[32] budget")

        if stage is not None and day_date is not None and eff_start is not None:
            night_key = night if night is not None else str(
                day_date - datetime.timedelta(days=fold_days or 0)
            )
            groups.setdefault((stage, night_key), []).append((i, artist, eff_start, eff_end))

    for (stage, night_key), entries in groups.items():
        entries.sort(key=lambda e: e[2])
        prev = None
        for idx, artist, start_min, end_min in entries:
            if prev is not None:
                prev_idx, prev_artist, prev_start, prev_end = prev
                if prev_end is not None and start_min < prev_end:
                    problems.append(
                        f"schedule[{idx}] ({artist!r}) overlaps schedule[{prev_idx}] ({prev_artist!r}) "
                        f"on stage {stage!r} / night {night_key!r}: starts {start_min} before prev ends {prev_end}"
                    )
            prev = (idx, artist, start_min, end_min)

    return problems, len(schedule)


def main(argv):
    if len(argv) < 2:
        print(f"usage: {argv[0]} <festpack.json> [<festpack.json> ...]", file=sys.stderr)
        return 2

    exit_code = 0
    for path in argv[1:]:
        problems, n_sets = lint_pack(path)
        if problems:
            exit_code = 1
            print(f"{path}: {n_sets} sets, {len(problems)} problem(s):")
            for p in problems:
                print(f"  - {p}")
        else:
            print(f"{path}: OK ({n_sets} sets, 0 problems)")
    return exit_code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
