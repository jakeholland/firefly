# S11 · settings & persistence

## Purpose
User prefs + durable state behind a tiny key-value seam. Mockup "Settings" is layout authority.

## Interface (`core/include/ff_store.h`, `core/include/ff_settings.h`)
```c
typedef struct {           // storage vtable: sim=file, esp32=NVS
  int (*get)(void *io, char const *key, void *buf, size_t n);   // returns len or <0
  int (*set)(void *io, char const *key, void const *buf, size_t n);
  void *io;
} ff_store_t;
typedef struct {
  bool imperial;            // default true (US festival)
  uint8_t share_mode;       // 0 LIVE / 1 ZONES / 2 GHOST (v1: LIVE/GHOST honored; ZONES=LIVE + issue)
  bool haptics, night_glow; // defaults true
  uint16_t water_min;       // 0 off, default 90
  uint16_t quiet_from_min, quiet_to_min; // local minutes, default 240→600 (4a–10a)
  char my_name[16];
  ff_geo_cal_t compass_cal; bool cal_valid;
} ff_settings_t;
void ff_settings_load(ff_settings_t *s, ff_store_t const *st);   // defaults on missing/corrupt
void ff_settings_save(ff_settings_t const *s, ff_store_t const *st);
bool ff_quiet_now(ff_settings_t const *s, int16_t now_min);      // handles wrap (from>to)
```
Also persisted: paired crew list (S02), starred sets (S07), selected pack id.

## Behavior
- GHOST: app stops honoring outbound STATUS + suppresses `mc_send_position`; radio-level position broadcast is the comms brain's — v1 documents that GHOST silences firefly-layer sharing and sets Meshtastic position broadcast off via admin message (slice c; if flaky, GHOST ships firefly-layer-only with README honesty note).
- Settings face renders per mockup: FT/M segmented, share row, two toggles, water/quiet value rows (tap cycles presets v1: water off/45/90/120; quiet off/2a-8a/4a-10a). Long-press-anywhere opens settings; back = swipe.
- Water nudge: haptic + toast every `water_min` while awake, suppressed in quiet hours.

## Acceptance criteria
1. Load with empty store yields exact defaults; corrupt blob (wrong size/magic) yields defaults not garbage.
2. Round-trip save/load equality for full struct incl. calibration.
3. `ff_quiet_now`: table incl. wrap 23:00→02:00 boundaries inclusive-exclusive documented.
4. Water tick fires at interval, resets on settings change, silent in quiet hours.
5. Golden: `settings.json` matches mockup.
6. Store mock records single write per save (no write-amplification loops).

## Slices
a) store seam + settings struct + tests · b) face render + interactions + golden · c) GHOST admin-message wiring (e2e).
