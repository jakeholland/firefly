/**
 * ff_session_log.c — see ff_session_log.h. S25 latch-hold amendment
 * (2026-09-16).
 */
#include "ff_session_log.h"

#include <string.h>
#include <stdio.h>

#define FF_SESSION_LOG_STORE_KEY "ff.sesslog"
#define FF_SESSION_LOG_MAGIC ((uint32_t)0x46534c31u) /* ASCII "FSL1" */
#define FF_SESSION_LOG_FORMAT_VERSION ((uint16_t)1u)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_size;
} ff_session_log_header_t;

#define FF_SESSION_LOG_BLOB_LEN (sizeof(ff_session_log_header_t) + sizeof(ff_session_log_t))

void ff_session_log_heartbeat(ff_session_log_t *rec, uint32_t uptime_s, uint16_t batt_mv, ff_session_link_t link,
                               uint8_t face)
{
    if (rec == NULL) {
        return;
    }
    rec->uptime_s = uptime_s;
    rec->batt_mv = batt_mv;
    rec->link = (uint8_t)link;
    rec->face = face;
    rec->clean_shutdown = false;
}

void ff_session_log_mark_clean(ff_session_log_t *rec)
{
    if (rec == NULL) {
        return;
    }
    rec->clean_shutdown = true;
}

bool ff_session_log_load(ff_session_log_t *rec, ff_store_t const *st)
{
    if (rec == NULL) {
        return false;
    }
    memset(rec, 0, sizeof(*rec));

    if (st == NULL || st->get == NULL) {
        return false;
    }

    uint8_t buf[FF_SESSION_LOG_BLOB_LEN];
    int n = st->get(st->io, FF_SESSION_LOG_STORE_KEY, buf, sizeof(buf));
    if (n < 0 || (size_t)n != FF_SESSION_LOG_BLOB_LEN) {
        /* Missing, failed, or the wrong size for the one version this
         * build knows how to read -> "no prior record", not a guess.
         * (No migration table yet, unlike ff_settings.c — this is the
         * first format version; a future field addition adds one the
         * same way ff_settings.c's own vN shadows do.) */
        return false;
    }

    ff_session_log_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != FF_SESSION_LOG_MAGIC || hdr.version != FF_SESSION_LOG_FORMAT_VERSION ||
        hdr.payload_size != (uint16_t)sizeof(ff_session_log_t)) {
        return false;
    }

    memcpy(rec, buf + sizeof(hdr), sizeof(*rec));
    return true;
}

void ff_session_log_save(ff_session_log_t const *rec, ff_store_t const *st)
{
    if (rec == NULL || st == NULL || st->set == NULL) {
        return;
    }

    uint8_t buf[FF_SESSION_LOG_BLOB_LEN];
    ff_session_log_header_t hdr;
    hdr.magic = FF_SESSION_LOG_MAGIC;
    hdr.version = FF_SESSION_LOG_FORMAT_VERSION;
    hdr.payload_size = (uint16_t)sizeof(ff_session_log_t);

    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), rec, sizeof(*rec));

    st->set(st->io, FF_SESSION_LOG_STORE_KEY, buf, sizeof(buf));
}

bool ff_session_log_heartbeat_due(uint32_t last_write_ms, uint32_t now_ms)
{
    return (now_ms - last_write_ms) >= FF_SESSION_LOG_HEARTBEAT_MS;
}

bool ff_session_log_format_last_time(char *buf, size_t n, ff_session_log_t const *prev, bool prev_valid)
{
    if (buf == NULL || n == 0) {
        return false;
    }
    buf[0] = '\0';

    if (!prev_valid || prev == NULL || prev->clean_shutdown) {
        return false;
    }

    unsigned const h = prev->uptime_s / 3600u;
    unsigned const m = (prev->uptime_s % 3600u) / 60u;

    if (prev->batt_mv > 0u) {
        unsigned const volts = (unsigned)(prev->batt_mv / 1000u);
        unsigned const centivolts = (unsigned)((prev->batt_mv % 1000u) / 10u);
        snprintf(buf, n, "Last time: stopped unexpectedly after %uh%02um - battery %u.%02u V", h, m, volts,
                 centivolts);
    } else {
        snprintf(buf, n, "Last time: stopped unexpectedly after %uh%02um - battery unknown", h, m);
    }
    return true;
}

char const *ff_reset_reason_name(ff_reset_reason_t r)
{
    switch (r) {
    case FF_RESET_REASON_POWERON: return "power-on";
    case FF_RESET_REASON_SW: return "software restart";
    case FF_RESET_REASON_TASK_WDT: return "task watchdog";
    case FF_RESET_REASON_BROWNOUT: return "brownout";
    case FF_RESET_REASON_OTHER: return "other";
    case FF_RESET_REASON_UNKNOWN:
    default: return "unknown";
    }
}
