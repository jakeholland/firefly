#include "ff_settings.h"

#include <string.h>

/* Persisted-blob framing: a small versioned header in front of the raw
 * ff_settings_t bytes so ff_settings_load can tell a valid blob from a
 * corrupt one or one written by an incompatible (old/new) struct layout,
 * and fall back to exact defaults rather than trusting garbage. */
#define FF_SETTINGS_STORE_KEY "ff.settings"
#define FF_SETTINGS_MAGIC ((uint32_t)0x46465331u) /* ASCII "FFS1" */
#define FF_SETTINGS_FORMAT_VERSION ((uint16_t)1u)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_size; /* sizeof(ff_settings_t) at write time */
} ff_settings_header_t;

#define FF_SETTINGS_BLOB_LEN (sizeof(ff_settings_header_t) + sizeof(ff_settings_t))

static void ff_settings_apply_defaults(ff_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->imperial = true;
    s->share_mode = FF_SHARE_LIVE;
    s->haptics = true;
    s->night_glow = true;
    s->water_min = 90;
    s->quiet_from_min = 240; /* 4:00a */
    s->quiet_to_min = 600;   /* 10:00a */
    /* my_name: left zeroed -> empty string. */
    /* compass_cal_blob: left zeroed; cal_valid stays false. */
}

void ff_settings_load(ff_settings_t *s, ff_store_t const *st)
{
    if (s == NULL) {
        return;
    }

    ff_settings_apply_defaults(s);

    if (st == NULL || st->get == NULL) {
        return;
    }

    uint8_t buf[FF_SETTINGS_BLOB_LEN];
    int n = st->get(st->io, FF_SETTINGS_STORE_KEY, buf, sizeof(buf));
    if (n < 0 || (size_t)n != sizeof(buf)) {
        /* Missing, short, truncated, or oversized read -> defaults stand. */
        return;
    }

    ff_settings_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.magic != FF_SETTINGS_MAGIC) {
        return;
    }
    if (hdr.version != FF_SETTINGS_FORMAT_VERSION) {
        return;
    }
    if (hdr.payload_size != (uint16_t)sizeof(ff_settings_t)) {
        return;
    }

    memcpy(s, buf + sizeof(hdr), sizeof(*s));
}

void ff_settings_save(ff_settings_t const *s, ff_store_t const *st)
{
    if (s == NULL || st == NULL || st->set == NULL) {
        return;
    }

    uint8_t buf[FF_SETTINGS_BLOB_LEN];
    ff_settings_header_t hdr;
    hdr.magic = FF_SETTINGS_MAGIC;
    hdr.version = FF_SETTINGS_FORMAT_VERSION;
    hdr.payload_size = (uint16_t)sizeof(ff_settings_t);

    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), s, sizeof(*s));

    /* Exactly one write per save — no read-modify-write loop, no retries. */
    st->set(st->io, FF_SETTINGS_STORE_KEY, buf, sizeof(buf));
}

/* Normalizes any int16_t minute-of-day value (including negatives) into
 * [0, 1440). */
static int32_t ff_norm_min(int32_t m)
{
    int32_t r = m % 1440;
    if (r < 0) {
        r += 1440;
    }
    return r;
}

bool ff_quiet_now(ff_settings_t const *s, int16_t now_min)
{
    if (s == NULL) {
        return false;
    }

    int32_t from = ff_norm_min(s->quiet_from_min);
    int32_t to = ff_norm_min(s->quiet_to_min);
    int32_t now = ff_norm_min(now_min);

    if (from == to) {
        /* Empty window: quiet hours off. */
        return false;
    }

    if (from < to) {
        /* Same-day window: [from, to). */
        return now >= from && now < to;
    }

    /* Wraps past midnight: [from, 1440) union [0, to). */
    return now >= from || now < to;
}

void ff_water_state_init(ff_water_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

bool ff_water_tick(ff_water_state_t *state, ff_settings_t const *s, int16_t now_min)
{
    if (state == NULL || s == NULL) {
        return false;
    }

    int32_t nm = ff_norm_min(now_min);

    if (!state->primed || state->configured_water_min != s->water_min) {
        /* First tick ever, or the interval changed underneath us: reset
         * the timer and start counting fresh from this call. */
        state->primed = true;
        state->configured_water_min = s->water_min;
        state->elapsed_min = 0;
        state->last_now_min = (int16_t)nm;
        return false;
    }

    int32_t delta = nm - state->last_now_min;
    if (delta < 0) {
        delta += 1440;
    }
    state->last_now_min = (int16_t)nm;

    if (s->water_min == 0) {
        /* Off. */
        state->elapsed_min = 0;
        return false;
    }

    state->elapsed_min = (uint16_t)(state->elapsed_min + (uint16_t)delta);

    if (state->elapsed_min < s->water_min) {
        return false;
    }

    if (ff_quiet_now(s, (int16_t)nm)) {
        /* Interval elapsed during quiet hours: consume it silently
         * rather than firing the instant quiet hours end. */
        state->elapsed_min = 0;
        return false;
    }

    state->elapsed_min = 0;
    return true;
}
