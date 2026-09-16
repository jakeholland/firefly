/**
 * ff_session_log.h — S25 latch-hold amendment (2026-09-16 field report):
 * a small NVS-persisted heartbeat + `clean_shutdown` flag, so a puck
 * that stops without going through POWER OFF/REBOOT can say so on its
 * NEXT boot, even when no panic fired and no core dump exists (a silent
 * brownout, or any digital-core reset class docs/specs/
 * S25-power-latch.md's pad-hold fix doesn't cover).
 *
 * Pure C11, persisted through the same `ff_store_t` seam `ff_settings.c`
 * uses (`ff_store.h`) — no I/O of its own, testable on the host with a
 * mock or the sim's real file-backed store, same as settings.
 *
 * ## The mechanism
 * The device caller (app_main.c) writes a heartbeat record — current
 * uptime/battery/link/face, `clean_shutdown = false` — roughly every
 * `FF_SESSION_LOG_HEARTBEAT_MS`, and ALSO right before the POWER
 * OFF/REBOOT paths actually act (`clean_shutdown = true`). On the NEXT
 * boot, before the first heartbeat of the new session overwrites it, the
 * caller loads the record: if it says `clean_shutdown == false`, the
 * PREVIOUS session's last heartbeat is the last thing known about it —
 * it stopped sometime after that write, without going through an
 * intentional power-off/reboot. `ff_session_log_format_last_time` turns
 * that into the one canonical line the Diagnostics face and `diag`
 * console both render (docs/specs/S25-power-latch.md's Amendments: "Last
 * time: stopped unexpectedly after 2h13m - battery 3.62 V" — an ASCII
 * hyphen, not the task brief's own "·", a deliberate interpretation call:
 * this project's compiled-in Montserrat Kconfig subset
 * (sdkconfig.defaults' CONFIG_LV_FONT_MONTSERRAT_* block) has not been
 * verified to include U+00B7 MIDDLE DOT, and a missing glyph renders as
 * nothing/a box rather than failing loudly, which would be a silent
 * regression of the very "honest, readable" surface this exists for).
 *
 * Honest by construction: a fresh puck (no prior record — a fresh NVS
 * partition, or the store degraded) renders NOTHING, same as a puck
 * whose last session WAS clean. This module never guesses at what
 * happened before the very first heartbeat it can actually read.
 */
#ifndef FF_SESSION_LOG_H
#define FF_SESSION_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Heartbeat cadence, ms — how often the device caller re-writes the
 * running session's record (uptime/batt/link/face, clean_shutdown =
 * false). See `ff_session_log_heartbeat_due`. */
#define FF_SESSION_LOG_HEARTBEAT_MS ((uint32_t)60000u)

/**
 * ff_session_link_t — a small, closed link-state vocabulary for the
 * persisted record. Deliberately NOT `ff_shell_link_t` (app/include/
 * ff_shell.h) — core stays free of app-layer headers; the device caller
 * maps `ff_shell_link_t` -> this enum one layer up (the same boundary-
 * translation `ff_app_link_t`, app/include/ff_app_state.h, already does
 * for the Diagnostics projection), by NUMBER, so the three stay in
 * lock-step by convention, not by a shared header.
 */
typedef enum {
    FF_SESSION_LINK_NONE = 0,
    FF_SESSION_LINK_RECONNECTING = 1,
    FF_SESSION_LINK_CONNECTED = 2,
} ff_session_link_t;

/**
 * ff_reset_reason_t — the small vocabulary the device caller translates
 * `esp_reset_reason()` into (core stays ESP-free). Only as fine-grained
 * as this module's own callers need (a startup LOG line and the
 * Diagnostics/`diag` "last time" fact do not currently distinguish PANIC
 * from TASK_WDT from BROWNOUT beyond naming them) — NOT a general
 * `esp_reset_reason_t` mirror.
 */
typedef enum {
    FF_RESET_REASON_UNKNOWN = 0,
    FF_RESET_REASON_POWERON,
    FF_RESET_REASON_SW,      /* esp_restart() (includes CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT's panic->restart path) */
    FF_RESET_REASON_TASK_WDT,
    FF_RESET_REASON_BROWNOUT,
    FF_RESET_REASON_OTHER,   /* a real esp_reset_reason_t value this vocabulary doesn't name individually */
} ff_reset_reason_t;

/** ff_session_log_t — the persisted record. `clean_shutdown` is the
 * whole mechanism: true only in the narrow window between the POWER
 * OFF/REBOOT paths deciding to act and the reset that follows; false at
 * every ordinary heartbeat, including the very first one of a session. */
typedef struct {
    uint32_t uptime_s;
    uint16_t batt_mv;      /* 0 = unknown, same sentinel as ff_power_batt_mv() */
    uint8_t  link;         /* ff_session_link_t */
    uint8_t  face;         /* caller's own small face-id encoding; opaque here */
    bool     clean_shutdown;
} ff_session_log_t;

/** ff_session_log_heartbeat — stamp `rec` with a running-session sample.
 * Always sets clean_shutdown = false — this is the "still running, as of
 * this write" fact, never the "about to stop on purpose" one. */
void ff_session_log_heartbeat(ff_session_log_t *rec, uint32_t uptime_s, uint16_t batt_mv, ff_session_link_t link,
                               uint8_t face);

/** ff_session_log_mark_clean — flip `clean_shutdown` true on an existing
 * record, keeping every other field as last heartbeated. Call this,
 * THEN `ff_session_log_save`, from the POWER OFF/REBOOT paths — after
 * this save, the puck stopping for any reason before the next heartbeat
 * reads as "clean" (which is what actually happened: an intentional
 * power-off/reboot was in flight). */
void ff_session_log_mark_clean(ff_session_log_t *rec);

/**
 * ff_session_log_load — populate `rec` from the store and report whether
 * a valid prior record was found. On any miss (absent key, store
 * degraded, corrupt/foreign blob) `rec` is zeroed
 * (`clean_shutdown = false`, every field 0/NONE) and this returns false
 * — callers must gate on the RETURN VALUE, not on `rec`'s own fields,
 * to tell "no prior record" apart from "a genuinely unclean one with
 * every stat honestly reading zero" (an extreme-but-real state: a fresh
 * heartbeat's first write, immediately followed by an unclean stop,
 * would legitimately have uptime_s == 0).
 */
bool ff_session_log_load(ff_session_log_t *rec, ff_store_t const *st);

/** ff_session_log_save — persist `rec`. One `st->set` call, same
 * "exactly one write per save" contract as `ff_settings_save`. */
void ff_session_log_save(ff_session_log_t const *rec, ff_store_t const *st);

/**
 * ff_session_log_heartbeat_due — true once at least
 * `FF_SESSION_LOG_HEARTBEAT_MS` have elapsed since `last_write_ms`.
 * Plain unsigned subtraction, wraparound-safe the same way every other
 * `now_ms`-driven deadline in this codebase is (e.g. `ff_time_reached`,
 * core/include/ff_time_reached.h) — `uint32_t` ms wraps every ~49.7
 * days, and `now_ms - last_write_ms` reads correctly across that wrap
 * for any interval shorter than the wrap period itself, which
 * `FF_SESSION_LOG_HEARTBEAT_MS` (60 s) trivially is.
 */
bool ff_session_log_heartbeat_due(uint32_t last_write_ms, uint32_t now_ms);

/**
 * ff_session_log_format_last_time — the ONE canonical "last time" line
 * (docs/specs/S25-power-latch.md's Amendments) for the Diagnostics face
 * and `diag` console. Writes nothing (`buf[0] = '\0'` when `n > 0`) and
 * returns false when there is nothing honest to say — no prior record
 * (`prev_valid == false`) or the prior session WAS clean
 * (`prev->clean_shutdown == true`); both render as an absent row, not a
 * fabricated "everything's fine" sentence. Returns true and formats
 * `"Last time: stopped unexpectedly after <H>h<MM>m - battery <V> V"` (or
 * "- battery unknown" when `prev->batt_mv == 0`) only for a genuinely
 * unclean prior record. `buf`/`n` follow the usual `snprintf` contract;
 * `n == 0` or `buf == NULL` is treated as "nothing to write", returns
 * false.
 */
bool ff_session_log_format_last_time(char *buf, size_t n, ff_session_log_t const *prev, bool prev_valid);

/**
 * ff_reset_reason_name — a short, lowercase, human-readable name for the
 * boot-time startup LOG line (`app_main.c`) and nowhere else — the
 * Diagnostics/`diag` "last time" fact (above) intentionally does NOT
 * include this-boot's reset reason (it describes the PREVIOUS session's
 * stop, not this boot's own start), so this helper is not part of that
 * formatted line. Never returns NULL.
 */
char const *ff_reset_reason_name(ff_reset_reason_t r);

#ifdef __cplusplus
}
#endif

#endif /* FF_SESSION_LOG_H */
