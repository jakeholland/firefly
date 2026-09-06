/**
 * ff_settings.h — user prefs + durable state (spec S11, slice a).
 *
 * Pure C11: no I/O of its own — persistence goes through the ff_store_t
 * seam (ff_store.h). Zero allocation; callers own all storage.
 *
 * Scope note: this slice implements the store seam, the settings struct,
 * load/save with defaults-on-corruption, quiet-hours math, and the
 * water-nudge tick. It does NOT implement the Settings face (slice b) or
 * GHOST admin-message wiring (slice c) — see docs/specs/S11-settings.md.
 */
#ifndef FF_SETTINGS_H
#define FF_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_crew.h" /* FF_CREW_MAX — the persisted paired-list's cap, S12/S04 [api] */
#include "ff_geo.h"
#include "ff_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* share_mode values (S11 spec: "0 LIVE / 1 ZONES / 2 GHOST"). v1 honors
 * LIVE and GHOST; ZONES behaves as LIVE (tracked as a known issue) —
 * that behavior lives in the app layer / slice c, not here. */
#define FF_SHARE_LIVE   0
#define FF_SHARE_ZONES  1
#define FF_SHARE_GHOST  2

/* my_name capacity, including NUL terminator, per spec (`char my_name[16]`). */
#define FF_SETTINGS_NAME_LEN 16

/* Display brightness (S15/#100). A percentage, clamped to
 * [FF_BRIGHTNESS_MIN_PCT, FF_BRIGHTNESS_MAX_PCT] — the floor is NON-zero on
 * purpose: 0% is a black backlight the user cannot see to recover from (the
 * "never a black screen you can't get out of" note in issue #100), so the
 * lowest the setting can reach is a dim-but-legible floor, not off. The
 * value is a pure stored number here; the physical LEDC PWM apply is a
 * device-HAL concern (targets/esp32s3/components/ff_display), a no-op in the
 * sim — core stays logic-only, honest value everywhere, physical effect
 * device-only (#100's "keep the setting honest").
 *
 * debt/batt-low-core: `targets/esp32s3/components/ff_display/include/
 * ff_display.h`'s `FF_BL_MIN_PCT`/`FF_BL_MAX_PCT` (the same range, applied
 * to the physical LEDC PWM by `ff_display_set_brightness`) are now
 * `#define`d as aliases of these two constants rather than a second pair
 * of literals that had drifted no further apart than "10u/100u" purely
 * by luck — see that header for the alias and the physical-side
 * doc comment. */
#define FF_BRIGHTNESS_MIN_PCT     10u
#define FF_BRIGHTNESS_MAX_PCT     100u
#define FF_BRIGHTNESS_DEFAULT_PCT 70u

/* Persisted-layout budget for the compass-calibration field, bytes. Kept
 * as a named constant (rather than just sizeof(ff_geo_cal_t)) so growth in
 * ff_geo_cal_t is a deliberate, reviewed budget decision — see the
 * _Static_assert below ff_settings_t. */
#define FF_SETTINGS_CAL_BLOB_LEN 32

typedef struct {
    bool imperial;                 /* default true (US festival)              */
    uint8_t share_mode;             /* FF_SHARE_LIVE / _ZONES / _GHOST          */
    bool haptics;                   /* default true                            */
    bool night_glow;                /* default true                            */
    uint16_t water_min;             /* water-nudge interval, minutes; 0 = off, default 90 */
    uint16_t quiet_from_min;        /* quiet hours start, local minutes-of-day, default 240 (4:00a) */
    uint16_t quiet_to_min;          /* quiet hours end, local minutes-of-day, default 600 (10:00a)  */

    /* Local UTC offset, minutes east of UTC (S16 wall clock, slice b0 —
     * docs/specs/S16-app-shell.md's "Wall clock" section; recorded there
     * as an [api] amendment to S11).
     *
     * MEANINGLESS unless utc_offset_set is true. The flag is not
     * redundant: int16_t has no free sentinel here because 0 is
     * legitimately UTC, so absence cannot be encoded as a value that
     * already means something — the same ruling as stage_color_valid and
     * FF_FRESH_NEVER. Defaults to unset, which is honest: a puck that
     * has never been told its offset does not know one, and with no pack
     * loaded the wall clock stays FF_WALL_UNKNOWN rather than guessing
     * (see ff_wall_resolve_offset in ff_wall.h for the full resolution
     * order against the pack's offset).
     *
     * Quiet hours is a settings feature with no festival dependency, so
     * without this field there is no path to local time at all when no
     * pack is loaded and ff_quiet_now silently cannot be evaluated. */
    int16_t utc_offset_min;
    bool utc_offset_set;

    /* [api] S17 slice a (docs/specs/S17-usability-hardening.md) — the
     * colorblind toggle. Read by app/theme/ff_theme.h's
     * ff_theme_crew_color at render time (threaded through as an
     * explicit parameter, not a hidden global — see that header's own
     * doc comment for why): true selects the Okabe-Ito-derived
     * colorblind-safe 8-colour crew palette instead of the brand one.
     * Default false — "not colorblind by default, keep the brand
     * colours" (S17's own scoping note). Purely a render-time selector;
     * it changes no other behavior and nothing here depends on it. */
    bool colorblind;

    /* [api] #100 — display brightness percent, clamped to
     * [FF_BRIGHTNESS_MIN_PCT, FF_BRIGHTNESS_MAX_PCT] (see those constants
     * above). Default FF_BRIGHTNESS_DEFAULT_PCT (~70%). Persisted like every
     * other pref; applied on boot and on change by the app forwarding it to
     * the display HAL (ff_display_set_brightness) — core never touches IO.
     * A real stored value in the sim too; only the physical backlight effect
     * is device-only (#100). */
    uint8_t brightness_pct;

    /* Not guaranteed NUL-terminated by this layer — load/save round-trip
     * the raw bytes as-is. Slice b (name-entry UI) must NUL-terminate
     * (or otherwise bound) whatever it writes here before this layer or
     * any consumer treats it as a C string. */
    char my_name[FF_SETTINGS_NAME_LEN];

    /* Compass calibration (hard/soft-iron correction + declination), S01
     * (ff_geo.h). Defaults to a zeroed ff_geo_cal_t (identity: no offset,
     * unit scale, zero declination) until a real calibration ritual
     * (S12) populates it and sets cal_valid. */
    ff_geo_cal_t compass_cal;
    bool cal_valid;

    /* [api] S15 slice d (docs/specs/S15d-touch-calibration.md) — touch
     * calibration: the per-axis affine that corrects the SPD2010's
     * offset+scale error (screen = a*raw + b per axis; see ff_touchcal.h).
     * These are the four fitted params from ff_touchcal_solve, persisted
     * here so calibration rides the existing settings mechanism.
     *
     * MEANINGLESS unless touch_calibrated is true (the device applies
     * IDENTITY — raw passes through — when false). A scale of 0 is not a
     * free sentinel (it is a real, if useless, value), so the flag, not the
     * values, is what gates application.
     *
     * The DEFAULT is identity with touch_calibrated=false — a fresh puck has
     * genuinely not been calibrated, so touch_calibrated=true always and only
     * means "this unit was calibrated by its owner" (via the in-app CALIBRATE
     * TOUCH row, NVS-persisted). We deliberately do NOT bake a specific board's
     * measured affine in as the default; see ff_settings.c's
     * ff_settings_apply_defaults for the honest-data rationale. Read by the
     * device touch path (targets/esp32s3/ff_display) via ff_touchcal_apply;
     * the sim never consults these (its input is already screen-space). */
    float touch_ax;
    float touch_bx;
    float touch_ay;
    float touch_by;
    bool touch_calibrated;

    /* [api] S21 amendment (clock-format setting) — the Settings CLOCK
     * row's 12H|24H toggle. Read by the wall-clock formatter
     * (`ff_fmt_clock`, ff_wall.h) via the app-shell projection (S18's
     * `clock_str`). Default FALSE — 12-hour with a lowercase am/pm
     * suffix (e.g. "9:46 pm"), the design vocabulary's mockup form —
     * true switches to 24-hour ("21:46", no suffix). Purely a
     * render-time format selector; it changes no other behavior (the
     * underlying wall-clock minute-of-day is unaffected either way). */
    bool clock_24h;

    /* [api] format v8 amendment (maintainer ask, 2026-09-02) — the
     * Settings SCREEN row's NORMAL|FLIPPED toggle. The Fusion-designed
     * case mounts the puck upside-down, so this drives a HARDWARE 180°
     * mirror of the panel (targets/esp32s3/ff_display's
     * ff_display_set_flip, `esp_lcd_panel_mirror`), not a software/LVGL
     * rotation — see docs/hardware/glass-offset.md's flipped-case
     * amendment for why the panel mirror is the correct layer. Two other
     * things key off this same flag, deliberately NOT stored separately
     * (one flip decision, three honest consequences of it, not three
     * settings a puck could get out of sync):
     *   - Touch: `ff_touchcal_flip180` runs AFTER `ff_touchcal_apply` in
     *     the device's `process_coordinates` seam, so a previously-solved
     *     calibration stays valid in either orientation (no
     *     re-calibration on flip).
     *   - Glass geometry: the bezel's visible-window offset
     *     (`FF_THEME_GLASS_CX/CY`, ff_theme.h) is measured against the
     *     NORMAL orientation; flipped, the offset is mirrored too — see
     *     `ff_theme_glass_cx`/`_cy` (ff_theme.h) and
     *     `radar_build_rim_tint` (scr_radar.c), the one on-glass element
     *     that hugs the physical bezel.
     * Default FALSE (NORMAL) — a freshly-flashed puck's case orientation
     * is not known until the owner sets it, and NORMAL is what every
     * puck shipped before this setting existed was already rendering as
     * (the honest reading of "this field didn't exist yet", same
     * "reject-not-migrate lands the new field at its honest default"
     * policy as `clock_24h` above and every touch-cal field before it —
     * see ff_settings.c's v8 migration comment). Purely a render/HAL
     * selector; it changes no domain behavior. */
    bool screen_flip;

    /* [api] format v9 amendment (S27 sounds, docs/specs/S27-sounds.md) —
     * the Settings SOUNDS ON|OFF row: the master switch for every sound
     * this puck plays (core/include/ff_sound.h's `ff_sound_should_play`
     * reads this first — sounds off silences everything, no exception,
     * not even FF_SOUND_FLARE_INCOMING). Default TRUE — unlike
     * `screen_flip`/`clock_24h` above (both display-format preferences
     * with no natural "on" bias), sound is an opt-OUT feature: the
     * maintainer's brief frames this as "we should add noises overall",
     * and `haptics` (S11, also default true) is the closest existing
     * precedent — a puck that can make a sound should, until the owner
     * says otherwise. See `ff_settings.c`'s v9 migration comment for how
     * this default is applied to a migrated (pre-v9) blob, which never
     * had this field at all. */
    bool sounds_on;

    /* [api] format v9 amendment (S27 sounds) — the Settings UI TICKS
     * ON|OFF row: a SECOND, independent gate that additionally must be
     * true for `FF_SOUND_TAP` to play (`ff_sound_should_play` itself
     * does not know about this field — see that function's doc comment
     * for why the TAP gate is composed by the caller, not built into the
     * 3-argument policy function). Default FALSE — deliberately the
     * opposite bias from `sounds_on` above: a tick on every single
     * button press is the kind of thing that gets old fast on a device
     * you tap dozens of times a festival, so this ships silent and the
     * maintainer can flip the default later if the field disagrees. */
    bool ui_ticks;

    /* [api] format v10 amendment (S12/S04 — "pairing v1 = channel
     * membership + explicit crew list", docs/specs/S04-firefly-protocol.md;
     * S02's `ff_crew_upsert`/`ff_crew_set_paired`) — the PERSISTED paired
     * roster, so a puck that pairs its crew in the field keeps them across
     * a reboot instead of re-hearing-and-re-pairing every session.
     *
     * `paired_ids[0..paired_count)` are Meshtastic node ids, in the order
     * they were (re-)paired — the shell replays them through
     * `ff_shell_pair` on boot, in this order, which is also the roster
     * slot order `ff_crew_upsert`'s find-or-create assigns (S16's
     * `shell_pair` colors each new slot once, by insertion order). `paired_
     * count` is the authoritative length; bytes past it are leftover and
     * MUST NOT be read — same "the count is truth, not the array's
     * contents past it" convention `ff_heard_t.count`/`ff_crew_t.count`
     * already use.
     *
     * Bounded to `FF_CREW_MAX` (S02) — the roster itself can never hold
     * more, so this list can never legitimately need more either; a
     * `paired_count` above `FF_CREW_MAX` in a loaded blob is corrupt, not
     * a bigger crew, and callers must never read past `FF_CREW_MAX` here
     * regardless of what a hostile/corrupt blob's count claims (see the
     * struct's own _Static_assert below and `ff_settings_load`'s clamp).
     *
     * Default (a fresh puck, or ANY migration into this format from an
     * older one, v9 included) is the EMPTY list (`paired_count == 0`) —
     * a pre-v10 puck genuinely had no persisted crew (pairing lived only
     * in RAM, lost every reboot), so "nobody yet" is the honest reading
     * of "this puck never had the field", not a guess. Never derived from
     * `ff_crew_t` at migration time (core settings code has no crew
     * roster to read from — see ff_settings.c's v10 migration comment). */
    uint32_t paired_ids[FF_CREW_MAX];
    uint8_t  paired_count;
} ff_settings_t;

/* The persisted paired-list can never legitimately exceed the roster it
 * mirrors — a format change to FF_CREW_MAX must be a deliberate, reviewed
 * settings-format bump (it changes this struct's layout), not a silent
 * array-size drift. */
_Static_assert(FF_CREW_MAX <= UINT8_MAX, "paired_count (uint8_t) can't index a larger FF_CREW_MAX");

/* ff_geo_cal_t must fit the persisted-layout budget above — a layout
 * change to it (or a bump to FF_SETTINGS_CAL_BLOB_LEN) is a settings
 * format change and must bump FF_SETTINGS_FORMAT_VERSION (ff_settings.c). */
_Static_assert(sizeof(ff_geo_cal_t) <= FF_SETTINGS_CAL_BLOB_LEN,
               "ff_geo_cal_t no longer fits the settings' compass-cal budget");

/**
 * ff_settings_load — populate `s` from the store, or with exact defaults
 * if the store has no settings, or the persisted blob is corrupt/from an
 * incompatible version (wrong magic, version, or size). Never partially
 * applies a bad blob — on any validation failure `s` is set to the full
 * default struct.
 */
void ff_settings_load(ff_settings_t *s, ff_store_t const *st);

/**
 * ff_settings_save — persist `s` to the store under a versioned
 * magic+size header. Issues exactly one `st->set` call.
 */
void ff_settings_save(ff_settings_t const *s, ff_store_t const *st);

/**
 * ff_quiet_now — true if `now_min` (local minutes-of-day, 0..1439,
 * normalized modulo 1440) falls within the quiet-hours window
 * [quiet_from_min, quiet_to_min), inclusive of the start minute and
 * exclusive of the end minute.
 *
 * Handles wraparound: when quiet_from_min > quiet_to_min the window
 * spans midnight (e.g. 23:00 -> 02:00 is minute 1380 until minute 120,
 * covering [1380, 1440) union [0, 120)). When quiet_from_min ==
 * quiet_to_min the window is empty (quiet hours off) and this always
 * returns false.
 */
bool ff_quiet_now(ff_settings_t const *s, int16_t now_min);

/**
 * ff_water_state_t — caller-owned tick state for ff_water_tick. Core
 * holds no globals (per ARCHITECTURE.md), so the water-nudge's elapsed
 * timer lives here, not inside ff_settings.c.
 *
 * Zero-initialize (or call ff_water_state_init) before the first tick.
 */
typedef struct {
    uint16_t configured_water_min; /* water_min as of the last tick, to detect settings changes */
    uint16_t elapsed_min;          /* minutes accumulated since the last fire/reset */
    int16_t last_now_min;          /* now_min as of the last tick */
    bool primed;                    /* false until the first tick has run */
} ff_water_state_t;

/** ff_water_state_init — zero a water-nudge tick state. */
void ff_water_state_init(ff_water_state_t *state);

/**
 * ff_water_tick — advance the water-nudge timer by the AWAKE minutes
 * elapsed between this call's `now_min` and the previous call's, and
 * report whether the nudge should fire now.
 *
 * - Fires (returns true) once `water_min` minutes have elapsed AWAKE
 *   (outside quiet hours) since the last fire/reset — per spec, "every
 *   water_min while awake". Minutes spent inside quiet hours are never
 *   counted toward the interval, not just gated at the fire instant: a
 *   tick spanning a quiet/awake boundary only banks the awake portion.
 * - `water_min == 0` means off: never fires.
 * - A change in `s->water_min` since the last tick resets the elapsed
 *   timer (that tick reports false; the new interval starts counting
 *   from this call).
 * - Silent during quiet hours: since only awake minutes accrue, the
 *   interval essentially can't complete while quiet — the residual
 *   check against `ff_quiet_now` at the tick instant is a defensive
 *   boundary case (`now_min` landing exactly on a quiet transition),
 *   consumed (reset to 0) rather than firing into quiet hours.
 * - `now_min` is local minutes-of-day (0..1439, normalized modulo 1440,
 *   same units as ff_quiet_now); the first call after init/reset primes
 *   the state and never fires.
 */
bool ff_water_tick(ff_water_state_t *state, ff_settings_t const *s, int16_t now_min);

#ifdef __cplusplus
}
#endif

#endif /* FF_SETTINGS_H */
