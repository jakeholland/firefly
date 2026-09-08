/**
 * ff_find.c — see ff_find.h.
 */
#include "ff_find.h"

#include <string.h>

void ff_find_stop(ff_find_t *f)
{
    if (!f) {
        return;
    }
    memset(f, 0, sizeof(*f));
}

void ff_find_start(ff_find_t *f, uint32_t target_node_id, uint32_t now_ms)
{
    if (!f) {
        return;
    }
    /* Single active session: starting on a new target replaces any prior
     * one outright (ff_flare_t's own precedent) — the memset clears
     * EVERYTHING, including has_their_reading/the sample history/the
     * trend latch, so a prior target's PONG readings never bleed into
     * the new session. */
    memset(f, 0, sizeof(*f));
    f->active = true;
    f->target_node_id = target_node_id;
    f->started_ms = now_ms;
    /* has_last_ping_sent_ms stays false (from the memset) — the very
     * next ff_find_tick fires immediately, no 10s wait for the first
     * ping of a session. */
}

void ff_find_leave_face(ff_find_t *f)
{
    ff_find_stop(f);
}

ff_find_result_t ff_find_tick(ff_find_t *f, uint32_t now_ms)
{
    ff_find_result_t r = {FF_FIND_INTENT_NONE, 0u};
    if (!f || !f->active) {
        return r;
    }

    /* Session cap: BOTH the ping-count and wall-clock limits are
     * checked independently (see ff_find.h's top comment) — reached
     * either way, the session ends and this tick (and every future one,
     * until a new ff_find_start) returns NONE. */
    uint32_t elapsed_ms = now_ms - f->started_ms; /* wraparound-safe unsigned subtraction */
    if (f->ping_count >= FF_FIND_MAX_PINGS || elapsed_ms >= FF_FIND_SESSION_MAX_MS) {
        f->active = false;
        return r;
    }

    /* Rate-limit floor: at most one SEND_PING per FF_FIND_PING_INTERVAL_MS,
     * enforced inside this module regardless of caller tick cadence. */
    if (f->has_last_ping_sent_ms) {
        uint32_t since_last = now_ms - f->last_ping_sent_ms; /* wraparound-safe */
        if (since_last < FF_FIND_PING_INTERVAL_MS) {
            return r;
        }
    }

    uint32_t nonce = f->next_nonce++;
    f->has_last_ping_sent_ms = true;
    f->last_ping_sent_ms = now_ms;
    f->ping_count++;
    f->has_last_sent_nonce = true;
    f->last_sent_nonce = nonce;

    r.intent = FF_FIND_INTENT_SEND_PING;
    r.nonce = nonce;
    return r;
}

ff_find_haptic_t ff_find_on_pong(ff_find_t *f, uint32_t from_node_id, uint32_t nonce, int16_t rssi_dbm,
                                  bool has_snr, float snr_db, uint32_t now_ms)
{
    if (!f || !f->active || from_node_id != f->target_node_id) {
        return FF_FIND_HAPTIC_NONE;
    }
    if (!f->has_last_sent_nonce || nonce != f->last_sent_nonce) {
        return FF_FIND_HAPTIC_NONE; /* stale/duplicate reply to an earlier ping */
    }

    f->has_their_reading = true;
    f->their_rssi_of_us = rssi_dbm;
    f->their_has_snr = has_snr;
    f->their_snr_of_us = has_snr ? snr_db : 0.0f;
    f->their_reading_age_ms = now_ms;

    /* Ring buffer: FF_FIND_TREND_SAMPLES*2 (6) most recent samples,
     * oldest overwritten first. */
    uint8_t const cap = (uint8_t)(2u * FF_FIND_TREND_SAMPLES);
    f->sample_hist[f->sample_head] = rssi_dbm;
    f->sample_head = (uint8_t)((f->sample_head + 1u) % cap);
    if (f->sample_count < cap) {
        f->sample_count++;
    }

    if (f->sample_count < cap) {
        /* Not enough samples for a full 3-vs-3 comparison yet — see
         * ff_find.h's doc comment for why this is the gate (matches
         * ff_crew_rssi_trend's own "both halves must be non-empty"
         * convention) rather than a looser "skip only the first two"
         * reading. */
        return FF_FIND_HAPTIC_NONE;
    }

    /* sample_hist is a ring; sample_head now points at the OLDEST
     * sample (the slot about to be overwritten next) since we just
     * advanced past the newest write. Walk `cap` entries starting there,
     * in chronological order: the first FF_FIND_TREND_SAMPLES are the
     * "older" half, the last FF_FIND_TREND_SAMPLES are the "newer" half. */
    long sum_old = 0, sum_new = 0;
    for (uint8_t k = 0; k < cap; k++) {
        uint8_t idx = (uint8_t)((f->sample_head + k) % cap);
        int16_t v = f->sample_hist[idx];
        if (k < FF_FIND_TREND_SAMPLES) {
            sum_old += v;
        } else {
            sum_new += v;
        }
    }
    double avg_old = (double)sum_old / (double)FF_FIND_TREND_SAMPLES;
    double avg_new = (double)sum_new / (double)FF_FIND_TREND_SAMPLES;
    double delta = avg_new - avg_old;

    int8_t trend = 0;
    if (delta >= (double)FF_FIND_TREND_THRESHOLD_DBM) {
        trend = 1;
    } else if (delta <= -(double)FF_FIND_TREND_THRESHOLD_DBM) {
        trend = -1;
    }

    ff_find_haptic_t result = FF_FIND_HAPTIC_NONE;
    if (trend != 0 && trend != f->last_fired_trend) {
        result = (trend > 0) ? FF_FIND_HAPTIC_WARMER : FF_FIND_HAPTIC_COLDER;
    }
    f->last_fired_trend = trend;
    return result;
}
