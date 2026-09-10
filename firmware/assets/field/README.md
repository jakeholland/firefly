# Field festpack

`lost-lands-2026.festpack.json` is a **verbatim copy** of
`packs/lost-lands/2026/festpack.json` from
[fest-almanac](https://github.com/jakeholland/fest-almanac) (schema v0.1) —
the real, non-fictional Lost Lands 2026 pack (Legend Valley, Thornville OH,
Sep 18–20 2026). This is our field-test venue.

**Never hand-edit this file.** To refresh it after fest-almanac updates the
pack:

```
cp /path/to/fest-almanac/packs/lost-lands/2026/festpack.json \
   firmware/assets/field/lost-lands-2026.festpack.json
```

**2026-09-09 — real set times.** fest-almanac's pack now carries Lost
Lands' published 2026 set-time grid: 222 sets, `meta.complete.set_times`
`"full"`. This file is a straight `cp` of that pack (verify with `cmp`),
so the rule above stands unchanged — no hand-editing happened and none
should. Two schedule fields arrived with it, both optional and both
handled by `fp_pack.c`'s `fp_parse_set_daytime()`:

- `night` — the ISO date of the festival NIGHT a set is billed under.
  Equals `day` for an ordinary set; equals `day` minus one day for an
  after-midnight set (55 of the 222). `day`/`start` stay plain calendar
  facts: `start` is always `HH:MM` with `HH` in 00..23.
- `end_day` — the ISO date of `end` when it differs from `day`. Exactly
  one set has it: Excision, 2026-09-18 22:10 → 00:10 on 2026-09-19.

See `docs/specs/S05-festpack.md`'s 2026-09-09 amendment for the model,
and `tools/festpack_lint.py` (wired into ctest) for the invariants a
refreshed pack must still satisfy.

Loaded at boot by the ESP32-S3 target under `CONFIG_FF_FIELD_PACK` (see
`firmware/targets/esp32s3/main/Kconfig.projbuild`) — mutually exclusive with
`CONFIG_FF_DEMO_MODE`. No crew, positions, or clock are seeded; only the
festpack itself is loaded (S05, honest-data — see `CLAUDE.md`).

The pack states `"utc_offset_min": -240` explicitly (it did not before the
2026-09-09 refresh), so the shell reads a KNOWN offset — `utc_offset_assumed
== false` — rather than falling back to the documented default. A pack that
omits the field still gets that -240 fallback, flagged assumed.

**Wall-clock plausibility window** (`ff_wall_window_from_pack`, ±14 days
around the festival's own dates): with `start_doy`/`end_doy` for Sep 18–20
2026, the tightened window is **2026-09-04T00:00:00Z through
2026-10-05T00:00:00Z** (1788480000 .. 1791158400, exclusive ceiling). A bench
test run before **2026-09-04** would have had every incoming mesh timestamp
rejected by the wall-clock plausibility gate (S18) — the window opens exactly
14 days before day-of-year 261 (Sep 18). That is fine for the actual field
test (Sep 18–20) and for bench runs from Sep 4 onward, but keep it in mind if
testing earlier.
