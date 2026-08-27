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
 * device-only (#100's "keep the setting honest"). */
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
} ff_settings_t;

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
