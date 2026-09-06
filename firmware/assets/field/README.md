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

Loaded at boot by the ESP32-S3 target under `CONFIG_FF_FIELD_PACK` (see
`firmware/targets/esp32s3/main/Kconfig.projbuild`) — mutually exclusive with
`CONFIG_FF_DEMO_MODE`. No crew, positions, or clock are seeded; only the
festpack itself is loaded (S05, honest-data — see `CLAUDE.md`).

The pack has no `utc_offset_min` extension field, so the shell falls back to
the documented -240 min default with `utc_offset_assumed == true` until a
real mesh timestamp (or a future schema update) supplies one explicitly.

**Wall-clock plausibility window** (`ff_wall_window_from_pack`, ±14 days
around the festival's own dates): with `start_doy`/`end_doy` for Sep 18–20
2026, the tightened window is **2026-09-04T00:00:00Z through
2026-10-05T00:00:00Z** (1788480000 .. 1791158400, exclusive ceiling). A bench
test run before **2026-09-04** would have had every incoming mesh timestamp
rejected by the wall-clock plausibility gate (S18) — the window opens exactly
14 days before day-of-year 261 (Sep 18). That is fine for the actual field
test (Sep 18–20) and for bench runs from Sep 4 onward, but keep it in mind if
testing earlier.
