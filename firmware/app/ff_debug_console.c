/**
 * ff_debug_console.c — see ff_debug_console.h.
 */
#include "ff_debug_console.h"

#if defined(FF_TARGET_SIM) || defined(CONFIG_FF_DEBUG_CONSOLE)

#include <stdio.h>
#include <string.h>

#include "ff_dbgcmd.h"

/* Reply lines are built here and handed to `reply` one at a time. 200
 * chars covers every line this file builds (roster's widest row: id +
 * name[16] + presence + lat/lon) with headroom; a name/status field is
 * always `snprintf`-truncated into its own budget below, never allowed
 * to blow this buffer regardless of what a member's free-text name/
 * status contains. */
#define DBGCONSOLE_LINE_BUF 200u

static void reply_line(ff_dbgconsole_reply_fn reply, void *user, char const *line)
{
    if (reply != NULL) reply(user, line);
}

static char const *link_name(ff_shell_link_t link)
{
    switch (link) {
    case FF_SHELL_LINK_NONE: return "NONE";
    case FF_SHELL_LINK_RECONNECTING: return "RECONNECTING";
    case FF_SHELL_LINK_CONNECTED: return "CONNECTED";
    }
    return "?";
}

static char const *trust_name(ff_wall_trust_t t)
{
    switch (t) {
    case FF_WALL_TRUST_BOOTSTRAP: return "BOOTSTRAP";
    case FF_WALL_TRUST_TRUSTED: return "TRUSTED";
    case FF_WALL_TRUST_CORROBORATED: return "CORROBORATED";
    }
    return "?";
}

static char const *presence_name(ff_freshness_t f)
{
    switch (f) {
    case FF_FRESH_LIVE: return "LIVE";
    case FF_FRESH_STALE: return "STALE";
    case FF_FRESH_LOST: return "LOST";
    case FF_FRESH_NEVER: return "NEVER";
    case FF_FRESH_ASSERTED: return "ASSERTED";
    }
    return "?";
}

/* DIAGNOSTICS — small name tables for `ff_app_diag_t`'s own app-layer
 * enums (`ff_app_link_t`/`ff_app_pos_src_t`/`ff_app_wall_trust_t`/
 * `ff_app_mag_kind_t`/`ff_app_imu_state_t`, ff_app_state.h). Deliberately
 * separate from `link_name`/`trust_name` above, which take the LOWER-
 * layer `ff_shell_link_t`/`ff_wall_trust_t` types other commands read
 * directly — `diag` reads only `ff_app_diag_t` (the same struct the
 * Settings DIAGNOSTICS page renders, via `ff_shell_diag_debug`), so its
 * own name tables key off the APP-layer enums, matching
 * `scr_settings.c`'s own (independent, screen-side) name tables for the
 * same enums — this repo's existing "each layer names its own boundary
 * type" precedent, not a copy-paste to fix. */
static char const *diag_link_name(ff_app_link_t l)
{
    switch (l) {
    case FF_APP_LINK_RECONNECTING: return "RECONNECTING";
    case FF_APP_LINK_CONNECTED: return "CONNECTED";
    case FF_APP_LINK_NONE:
    default: return "NONE";
    }
}

static char const *diag_pos_src_name(ff_app_pos_src_t s)
{
    switch (s) {
    case FF_APP_POS_SRC_MANUAL: return "manual";
    case FF_APP_POS_SRC_INTERNAL: return "internal";
    case FF_APP_POS_SRC_EXTERNAL: return "external";
    case FF_APP_POS_SRC_UNKNOWN:
    default: return "unknown";
    }
}

static char const *diag_wall_trust_name(ff_app_wall_trust_t t)
{
    switch (t) {
    case FF_APP_WALL_TRUST_TRUSTED: return "trusted";
    case FF_APP_WALL_TRUST_CORROBORATED: return "corroborated";
    case FF_APP_WALL_TRUST_BOOTSTRAP:
    default: return "bootstrap";
    }
}

static char const *diag_mag_kind_name(ff_app_mag_kind_t k)
{
    switch (k) {
    case FF_APP_MAG_QMC5883L: return "QMC5883L";
    case FF_APP_MAG_HMC5883L: return "HMC5883L";
    case FF_APP_MAG_QMC5883P: return "QMC5883P";
    case FF_APP_MAG_NONE:
    default: return "none";
    }
}

static char const *diag_imu_state_name(ff_app_imu_state_t s)
{
    switch (s) {
    case FF_APP_IMU_NO_DATA: return "no-data";
    case FF_APP_IMU_OK: return "ok";
    case FF_APP_IMU_ABSENT:
    default: return "absent";
    }
}

/* "?" for an age/value this fact's own has_-flag says was never
 * observed — matches `dbgconsole_wall`'s own `offset_buf`/`assumed_buf`
 * "?" convention above for the same "not applicable, not zero" reason. */
static void diag_u32_or_q(char *buf, size_t n, bool has, uint32_t v)
{
    if (has) {
        snprintf(buf, n, "%u", (unsigned)v);
    } else {
        snprintf(buf, n, "?");
    }
}

static void dbgconsole_help(ff_dbgconsole_reply_fn reply, void *user)
{
    reply_line(reply, user, "dbg: help                    this list");
    reply_line(reply, user, "dbg: me                       my node id / link / position / wall clock");
    reply_line(reply, user, "dbg: roster                   paired crew: id, name, presence, position");
    reply_line(reply, user, "dbg: heard                    heard-but-unpaired node ids");
    reply_line(reply, user, "dbg: send <text>              crew broadcast");
    reply_line(reply, user, "dbg: dm <node_hex> <text>     addressed send (hex, optional ! or 0x prefix)");
    reply_line(reply, user, "dbg: flare                    start a quick flare");
    reply_line(reply, user, "dbg: flare cancel             cancel a flare in progress");
    reply_line(reply, user, "dbg: wall                     wall-clock latch dump");
    reply_line(reply, user, "dbg: i2c                      shared I2C bus scan + compass status + touch health");
    reply_line(reply, user, "dbg: cal                      compass calibration ritual status");
    reply_line(reply, user, "dbg: cal start                begin a calibration session");
    reply_line(reply, user, "dbg: cal finish               end the session, persist if coverage is enough");
    reply_line(reply, user, "dbg: cal cancel               abandon the session, calibration unchanged");
    reply_line(reply, user, "dbg: cal clear                drop the stored calibration back to identity");
    reply_line(reply, user, "dbg: name                     NAME in Settings: stored/mesh/confirmed status");
    reply_line(reply, user, "dbg: name <text>              set + push the Meshtastic owner update");
    reply_line(reply, user, "dbg: crew                     crew code/index/region/precision + START/LEAVE status");
    reply_line(reply, user, "dbg: crew start               mint a code, write the crew channel, verify it");
    reply_line(reply, user, "dbg: crew leave               restore the pre-crew channel, verify it");
    reply_line(reply, user, "dbg: diag                     DIAGNOSTICS: link/position/mesh/time/compass/device");
    reply_line(reply, user, "dbg: perf                     frame/LVGL/flush timing, heap, per-task stack high-water");
    reply_line(reply, user, "dbg: ping <node_hex>          S29: one immediate bench PING, outside FIND");
    reply_line(reply, user, "dbg: find <node_hex>          S29: start a FIND session on that node");
    reply_line(reply, user, "dbg: find off                 S29: cancel the active FIND session");
    reply_line(reply, user, "dbg: mic                      S30: one-shot mic status + level");
    reply_line(reply, user, "dbg: mic on                   S30: start the mic channel + reader task");
    reply_line(reply, user, "dbg: mic off                  S30: stop them");
    reply_line(reply, user, "dbg: mic watch <secs>         S30: print RMS/peak/envelope every 250ms, 1-30s");
    reply_line(reply, user, "dbg: mic dump <secs>          2026-09-09: stream raw 16kHz PCM as base64, 1-10s");
    reply_line(reply, user, "dbg: music                    S31: beat detector source/loudness/bpm-estimate");
    reply_line(reply, user, "dbg: music seed <n>           S31: reseed the swarm (bench determinism)");
    reply_line(reply, user, "dbg: sleep                    S26f: force one light-sleep cycle (default period)");
    reply_line(reply, user, "dbg: sleep <ms>               S26f: same, with an explicit timer-wake period, 50-60000ms");
    reply_line(reply, user, "dbg: tpint                    S26f: poll touch-INT level for 5s (tap the glass to test)");
}

static void dbgconsole_me(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    char line[DBGCONSOLE_LINE_BUF];

    uint32_t const node = ff_shell_my_node_id(sh);
    snprintf(line, sizeof(line), "dbg: me node=!%08x link=%s", (unsigned)node, link_name(ff_shell_link(sh)));
    reply_line(reply, user, line);

    ff_shell_my_pos_debug_t const pos = ff_shell_my_pos_debug(sh);
    if (pos.ok) {
        char age_buf[24] = "?";
        if (pos.has_age) snprintf(age_buf, sizeof(age_buf), "%u", (unsigned)pos.age_ms);
        snprintf(line, sizeof(line), "dbg: me pos ok=1 lat=%.6f lon=%.6f age_ms=%s", pos.pos.lat, pos.pos.lon, age_buf);
    } else {
        snprintf(line, sizeof(line), "dbg: me pos ok=0");
    }
    reply_line(reply, user, line);

    ff_shell_wall_debug_t const wall = ff_shell_wall_debug(sh);
    if (wall.latched) {
        int64_t unix_s = 0;
        char unix_buf[24] = "unknown";
        if (ff_shell_wall_unix_now(sh, &unix_s)) {
            snprintf(unix_buf, sizeof(unix_buf), "%lld", (long long)unix_s);
        }
        char trust_buf[16] = "none";
        if (wall.has_last_obs) snprintf(trust_buf, sizeof(trust_buf), "%s", trust_name(wall.last_obs_trust));
        snprintf(line, sizeof(line), "dbg: me wall latched=1 trust=%s unix=%s", trust_buf, unix_buf);
    } else {
        snprintf(line, sizeof(line), "dbg: me wall latched=0");
    }
    reply_line(reply, user, line);
}

static void dbgconsole_roster(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_crew_t const *crew = ff_shell_crew(sh);
    char line[DBGCONSOLE_LINE_BUF];

    uint8_t n_paired = 0;
    if (crew != NULL) {
        for (uint8_t i = 0; i < crew->count; ++i) {
            if (crew->members[i].paired) n_paired++;
        }
    }
    snprintf(line, sizeof(line), "dbg: roster n=%u", (unsigned)n_paired);
    reply_line(reply, user, line);

    if (crew == NULL) return;
    for (uint8_t i = 0; i < crew->count; ++i) {
        ff_crew_member_t const *m = &crew->members[i];
        if (!m->paired) continue;
        char const *presence = presence_name(ff_crew_freshness(m, ff_shell_now_ms(sh)));
        if (m->has_pos) {
            snprintf(line, sizeof(line), "dbg: roster id=!%08x name=%s presence=%s has_pos=1 lat=%.6f lon=%.6f",
                     (unsigned)m->node_id, m->name, presence, m->pos.lat, m->pos.lon);
        } else {
            snprintf(line, sizeof(line), "dbg: roster id=!%08x name=%s presence=%s has_pos=0", (unsigned)m->node_id,
                     m->name, presence);
        }
        reply_line(reply, user, line);
    }
}

static void dbgconsole_heard(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_heard_t const *heard = ff_shell_heard(sh);
    char line[DBGCONSOLE_LINE_BUF];

    uint8_t const n = ff_heard_count(heard);
    snprintf(line, sizeof(line), "dbg: heard n=%u", (unsigned)n);
    reply_line(reply, user, line);

    uint32_t const now_ms = ff_shell_now_ms(sh);
    for (uint8_t i = 0; i < n; ++i) {
        ff_heard_entry_t const *e = ff_heard_at(heard, i);
        if (e == NULL) continue;
        snprintf(line, sizeof(line), "dbg: heard id=!%08x age_ms=%u", (unsigned)e->node_id,
                 (unsigned)(now_ms - e->last_heard_ms));
        reply_line(reply, user, line);
    }
}

/**
 * dbgconsole_send_outcome — outbox delivery status feature (2026-09-07):
 * the console's own honest word for what `ff_shell_debug_send_text` just
 * did. `rc != 0` is still "failed" (nothing was pushed at all — empty
 * text, or no sender wired: see that function's own doc comment). But
 * `rc == 0` no longer means "the mesh has it" — it means "accepted into
 * the send pipeline", which per that same doc comment can be either an
 * immediate SENT or a queued WAITING (link not READY right now). This
 * console reply distinguishes them honestly by reading the item it just
 * pushed straight back off the feed (`ff_shell_feed`, newest-first —
 * see ff_feed.h — so index 0 IS the item this very call just pushed)
 * rather than trusting the return code alone. Never claims DELIVERED/
 * NO_ACK here — those only resolve later, asynchronously, and the
 * console command has already returned by the time they do; the thread
 * view (scr_inbox.c's inbox_send_status_text) is where that later fate
 * is shown. */
static char const *dbgconsole_send_outcome(ff_shell_t *sh, int rc)
{
    if (rc != 0) return "failed";
    ff_feed_t const *feed = ff_shell_feed(sh);
    ff_feed_item_t const *it = (feed != NULL && ff_feed_count(feed) > 0u) ? ff_feed_at(feed, 0) : NULL;
    if (it != NULL && it->send_status == FF_SEND_WAITING) return "queued"; /* link not READY — outbox retry */
    return "ok"; /* handed straight to mc_client (or this feature doesn't apply — e.g. no packet id tracked) */
}

static void dbgconsole_send(ff_shell_t *sh, char const *text, ff_dbgconsole_reply_fn reply, void *user)
{
    int const rc = ff_shell_debug_send_text(sh, 0u, text); /* 0 = crew broadcast */
    char line[DBGCONSOLE_LINE_BUF];
    snprintf(line, sizeof(line), "dbg: send %s dest=broadcast", dbgconsole_send_outcome(sh, rc));
    reply_line(reply, user, line);
}

static void dbgconsole_dm(ff_shell_t *sh, uint32_t dest, char const *text, ff_dbgconsole_reply_fn reply, void *user)
{
    /* `ff_shell_debug_send_text` treats dest_node==0 as "no destination
     * given" and silently collapses it to MC_ADDR_BROADCAST (ff_shell.c) —
     * correct for `send`, wrong for `dm`, which promises an ADDRESSED
     * send. `dm 0 ...` / `dm 00000000 ...` both parse to dest==0 (
     * parse_node_hex accepts any 1-8 hex digits, including all zeros),
     * and letting that through would reply "dbg: dm ok dest=!00000000"
     * for a message that actually went to every paired node — a
     * provenance-mislabeling debug surface, exactly what AGENTS.md's
     * standing brief calls a real finding, not a nit. Reject before
     * ever reaching the sender: never silently broadcast a `dm`. */
    if (dest == 0u) {
        reply_line(reply, user, "dbg: ? dm needs a non-zero node id");
        return;
    }
    int const rc = ff_shell_debug_send_text(sh, dest, text);
    char line[DBGCONSOLE_LINE_BUF];
    snprintf(line, sizeof(line), "dbg: dm %s dest=!%08x", dbgconsole_send_outcome(sh, rc), (unsigned)dest);
    reply_line(reply, user, line);
}

/* S29 PR2 — `ping <node_hex>` / `find <node_hex>` / `find off`. The
 * parser (`ff_dbgcmd.c`) already rejects a hex token of all zeros the
 * same way it accepts one for `dm` — mirrors `dbgconsole_dm`'s own
 * dest==0 guard rather than trusting the parser alone, same
 * belt-and-suspenders reasoning that guard's own comment gives. */
static void dbgconsole_ping(ff_shell_t *sh, uint32_t node, ff_dbgconsole_reply_fn reply, void *user)
{
    if (node == 0u) {
        reply_line(reply, user, "dbg: ? ping needs a non-zero node id");
        return;
    }
    int const rc = ff_shell_debug_ping(sh, node);
    char line[DBGCONSOLE_LINE_BUF];
    snprintf(line, sizeof(line), "dbg: ping %s dest=!%08x", (rc == 0) ? "ok" : "failed", (unsigned)node);
    reply_line(reply, user, line);
}

static void dbgconsole_find(ff_shell_t *sh, uint32_t node, ff_dbgconsole_reply_fn reply, void *user)
{
    if (node == 0u) {
        reply_line(reply, user, "dbg: ? find needs a non-zero node id");
        return;
    }
    ff_shell_debug_find_start(sh, node);
    char line[DBGCONSOLE_LINE_BUF];
    snprintf(line, sizeof(line), "dbg: find started target=!%08x", (unsigned)node);
    reply_line(reply, user, line);
}

static void dbgconsole_find_off(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_shell_debug_find_stop(sh);
    reply_line(reply, user, "dbg: find off");
}

static void dbgconsole_flare(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_flare_t const *before = ff_shell_flare(sh);
    bool const was_sending = (before != NULL) && before->sending;

    ff_intent_t const in = {.kind = FF_INTENT_QUICK_FLARE, .u = {0}};
    ff_shell_intent(sh, &in);

    char line[DBGCONSOLE_LINE_BUF];
    if (was_sending) {
        snprintf(line, sizeof(line), "dbg: flare already sending");
    } else {
        ff_flare_t const *after = ff_shell_flare(sh);
        if (after != NULL && after->sending) {
            snprintf(line, sizeof(line), "dbg: flare started dur_s=%u", (unsigned)after->wire_dur_s);
        } else {
            /* Honest fallback — e.g. no sender wired up at all. Never
             * claim "started" when the flare state disagrees. */
            snprintf(line, sizeof(line), "dbg: flare not sending");
        }
    }
    reply_line(reply, user, line);
}

static void dbgconsole_flare_cancel(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_flare_t const *before = ff_shell_flare(sh);
    bool const was_sending = (before != NULL) && before->sending;

    ff_intent_t const in = {.kind = FF_INTENT_FLARE_END, .u = {0}};
    ff_shell_intent(sh, &in);

    reply_line(reply, user, was_sending ? "dbg: flare cancelled" : "dbg: flare not sending");
}

/* `cal` (S12 step 3, the compass calibration ritual) — bare status plus
 * four sub-verbs, ALL dispatched through `ff_shell_intent`/
 * `ff_shell_compass_cal_status`, the SAME seam the Settings ritual
 * screen uses (ff_dbgcmd.h's own doc comment: "never a second path into
 * shell state"). Every reply reads state back through that getter
 * AFTER the intent runs, the same "compare state before/after, since
 * ff_shell_intent has no return channel" pattern `dbgconsole_flare`
 * above already establishes — not a value this dispatcher invents. */
static void dbgconsole_cal_line(ff_shell_compass_cal_status_t const *st, char *out, size_t cap)
{
    if (st->active) {
        snprintf(out, cap, "dbg: cal active=1 progress_pct=%d samples=%u can_finish=%d cal=%s", st->progress_pct,
                 st->sample_count, st->can_finish ? 1 : 0, st->cal_valid ? "custom" : "identity");
    } else {
        snprintf(out, cap, "dbg: cal active=0 cal=%s", st->cal_valid ? "custom" : "identity");
    }
}

static void dbgconsole_cal_status(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_shell_compass_cal_status_t const st = ff_shell_compass_cal_status(sh);
    char line[DBGCONSOLE_LINE_BUF];
    dbgconsole_cal_line(&st, line, sizeof(line));
    reply_line(reply, user, line);
}

static void dbgconsole_cal_start(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    bool const was_active = ff_shell_compass_cal_status(sh).active;

    ff_intent_t const in = {.kind = FF_INTENT_COMPASS_CAL_START, .u = {0}};
    ff_shell_intent(sh, &in);

    reply_line(reply, user, was_active ? "dbg: cal already active" : "dbg: cal started");
}

static void dbgconsole_cal_finish(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_shell_compass_cal_status_t const before = ff_shell_compass_cal_status(sh);
    char line[DBGCONSOLE_LINE_BUF];

    if (!before.active) {
        reply_line(reply, user, "dbg: cal not active");
        return;
    }

    ff_intent_t const in = {.kind = FF_INTENT_COMPASS_CAL_FINISH, .u = {0}};
    ff_shell_intent(sh, &in);

    ff_shell_compass_cal_status_t const after = ff_shell_compass_cal_status(sh);
    if (!after.active) {
        /* The session closed — FF_INTENT_COMPASS_CAL_FINISH's only way to
         * do that is a successful finish (see its own doc comment). */
        snprintf(line, sizeof(line), "dbg: cal finished ok progress_pct=%d samples=%u", before.progress_pct,
                 before.sample_count);
    } else {
        snprintf(line, sizeof(line),
                 "dbg: cal finish failed (need %d%%, have %d%%) — calibration unchanged, session still active",
                 FF_GEO_CAL_MIN_PROGRESS_PCT, after.progress_pct);
    }
    reply_line(reply, user, line);
}

static void dbgconsole_cal_cancel(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    bool const was_active = ff_shell_compass_cal_status(sh).active;

    ff_intent_t const in = {.kind = FF_INTENT_COMPASS_CAL_CANCEL, .u = {0}};
    ff_shell_intent(sh, &in);

    reply_line(reply, user, was_active ? "dbg: cal cancelled" : "dbg: cal not active");
}

static void dbgconsole_cal_clear(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    bool const was_valid = ff_shell_compass_cal_status(sh).cal_valid;

    ff_intent_t const in = {.kind = FF_INTENT_COMPASS_CAL_CLEAR, .u = {0}};
    ff_shell_intent(sh, &in);

    reply_line(reply, user, was_valid ? "dbg: cal cleared" : "dbg: cal already uncalibrated");
}

/* NAME in Settings — bare status plus "set", both dispatched through
 * `ff_shell_mesh_name_status`/`ff_shell_intent`, the SAME seam the
 * Settings NAME row and its T9 editor use (this file's own top-comment
 * "every command that ACTS goes through ff_shell_intent" rule). "set"
 * runs the EXACT same commit path the row's DONE button does
 * (FF_INTENT_SETTINGS_NAME_COMMIT — sanitize, persist, push), so the
 * coordinator can bench the mesh push against real nodes without the
 * touchscreen.
 *
 * Confirmation-fix follow-up (bench finding, 2026-09-06) added the
 * trailing `pushed=<long>/<short> ack=<none|ok|nak> reply=<none|long/
 * short>` fields: the ORIGINAL `stored=.../mesh=.../confirmed=` trio
 * alone could not distinguish "no push has happened yet" from "pushed,
 * still waiting on a reply" from "pushed, got NAK'd" — all three read
 * identically as `confirmed=0`. These three new fields are the CURRENT
 * push's own record (`ff_shell_mesh_name_status_t`'s own doc comment has
 * the full field-by-field rationale): `pushed=none` before any push this
 * session; `ack=none` until a routing reply for that push arrives (NOT a
 * failure — see `ff_mesh_name_ack_t`); `reply=none` until this push's
 * own `get_owner_request` follow-up gets an answer.
 *
 * Confirmation-fix round 2 (2026-09-06, bench finding AFTER commit
 * 51e4ae1, against a real puck + Meshtastic 2.7.26 comms brain) added
 * `seq=<N> mismatch=<0|1>`: `seq=` is the push-generation counter that
 * closes a stale-equality false positive (`name Jake` used to read
 * `confirmed=1` INSTANTLY whenever the mesh's CACHED name already
 * happened to equal the one just pushed, even with `reply=none` — see
 * `ff_shell_mesh_name_status_t`'s doc comment, ff_shell.h, for the full
 * mechanism); `mismatch=1` is a fresh reply/self-NodeInfo for THIS push
 * naming a DIFFERENT owner than was pushed, distinct from `ack=nak`
 * (a routing-layer delivery failure that says nothing about what name
 * the admin module actually ended up with). The SAME bench run also
 * found `get_owner_request` itself never got a reply on real hardware
 * (`reply=none` forever, even past every retry) — a separate,
 * lower-level fix in `mc_send_get_owner_request`
 * (`meshclient/include/mc_client.h`'s own doc comment has the
 * AdminModule citation); this console's `reply=` field is what exposed
 * it on the bench in the first place. */
static void dbgconsole_name_status(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_shell_mesh_name_status_t const st = ff_shell_mesh_name_status(sh);
    char line[DBGCONSOLE_LINE_BUF];

    char const *stored = (st.my_name[0] != '\0') ? st.my_name : "(unset)";
    /* Confirmation-fix follow-up: mesh_buf/pushed_buf/reply_buf are sized
     * tightly (24, not a round "plenty" number like the pre-existing
     * mesh_buf's old 64) because GCC's -Wformat-truncation estimates a
     * %s argument's worst case as "up to the SOURCE buffer's own declared
     * capacity" when it cannot prove a tighter bound flow-sensitively —
     * so an oversized scratch buffer here inflates line[]'s own computed
     * worst case at line's snprintf below, past DBGCONSOLE_LINE_BUF, and
     * fails the GCC gate (clang has no equivalent check — CLAUDE.md's
     * "read every local clean-under-Werror claim as clang's
     * interpretation" note, again). 24 comfortably covers the real
     * content (name <=15 + '/' + short <=4, or the literal fallbacks,
     * all well under 24) with headroom, while keeping line[]'s own
     * worst-case total near 156 of its 200-byte budget. */
    char mesh_buf[24];
    if (!st.has_mesh_owner_name) {
        snprintf(mesh_buf, sizeof(mesh_buf), "unknown");
    } else if (st.mesh_owner_name[0] != '\0') {
        snprintf(mesh_buf, sizeof(mesh_buf), "%s", st.mesh_owner_name);
    } else {
        snprintf(mesh_buf, sizeof(mesh_buf), "(unset)");
    }

    char pushed_buf[24];
    if (st.has_pushed) {
        snprintf(pushed_buf, sizeof(pushed_buf), "%s/%s", st.pushed_long, st.pushed_short);
    } else {
        snprintf(pushed_buf, sizeof(pushed_buf), "none");
    }

    char const *ack_str = (st.ack == FF_MESH_NAME_ACK_OK) ? "ok" : (st.ack == FF_MESH_NAME_ACK_NAK) ? "nak" : "none";

    char reply_buf[24];
    if (st.has_reply) {
        snprintf(reply_buf, sizeof(reply_buf), "%s/%s", st.reply_long, st.reply_short);
    } else {
        snprintf(reply_buf, sizeof(reply_buf), "none");
    }

    /* Confirmation-fix round 2 (bench finding, 2026-09-06, AFTER commit
     * 51e4ae1): `seq=` and `mismatch=` are the bench-visible form of the
     * stale-equality fix (`ff_shell_mesh_name_status_t`'s own doc
     * comment has the full mechanism) — `seq=` is the push-generation
     * counter (0 before any push this session), `mismatch=1` means a
     * FRESH reply/self-NodeInfo for THIS push arrived but named a
     * different owner than was pushed, distinct from `ack=nak` (a
     * routing-layer delivery failure, silent on what the admin module's
     * owner actually ended up being).
     *
     * Reboot-session-loss fix (bench finding, 2026-09-06): `link=` is the
     * SAME `link_name()` this file already uses elsewhere (`ff_shell.h`'s
     * `ff_shell_link_t`), so a bench operator can tell "pending because
     * the comms brain rebooted and this device is re-handshaking" (link=
     * RECONNECTING) apart from "pending on a live link, waiting on a
     * reply" (link=CONNECTED) — see `ff_shell_mesh_name_status_t`'s own
     * doc comment (`ff_shell.h`) for the retry-gate this reflects. */
    snprintf(line, sizeof(line),
             "dbg: name stored=%s mesh=%s confirmed=%d%s seq=%u pushed=%s ack=%s reply=%s mismatch=%d link=%s",
             stored, mesh_buf, st.confirmed ? 1 : 0, st.my_name_from_node ? " (from_node)" : "",
             (unsigned)st.pushed_seq, pushed_buf, ack_str, reply_buf, st.mismatch ? 1 : 0, link_name(st.link));
    reply_line(reply, user, line);
}

static void dbgconsole_name_set(ff_shell_t *sh, char const *text, ff_dbgconsole_reply_fn reply, void *user)
{
    /* ff_shell_debug_set_name runs the EXACT SAME commit mechanism the
     * Settings NAME row's DONE button does (shell_apply_name_commit:
     * sanitize, persist, push) — see that function's own doc comment
     * (ff_shell.h) for why this bypasses FF_INTENT_SETTINGS_NAME_COMMIT's
     * subview-only guard rather than fighting it, the same "reuse the
     * mechanism, not the screen" shape `ff_shell_debug_send_text` already
     * establishes for `send`/`dm`. */
    ff_shell_debug_set_name(sh, text);
    dbgconsole_name_status(sh, reply, user);
}

/* A02 slice D2 — `crew`: the crew-code / crew-operation status, read
 * through `ff_shell_crew_op_status` (ff_shell.h) — the SAME projection
 * the Settings CREW faces render, so the console and the glass can never
 * answer the same question differently, exactly as `diag` and `name`
 * already do for theirs.
 *
 * Every field on this line is honest about absence rather than
 * defaulting:
 *  - `code` is what the RADIO reports (derived from its channel name),
 *    `pending` is what the CURRENT run minted — separate, because
 *    showing a minted code before the radio accepted it is the whole
 *    thing the verify step exists to prevent;
 *  - `index` prints `?` until the channel table resolves one. It is
 *    NEVER 0-by-default (mesh.proto: the channel index is "inherently a
 *    local concept");
 *  - `region` prints `?` until a handshake has reported one, and `unset`
 *    for the real reading 0 — those are different facts and a crew start
 *    stops on the second one (A02 §1.7). */
static char const *dbgconsole_crew_op_name(ff_app_crew_op_t op)
{
    switch (op) {
    case FF_APP_CREW_OP_NONE: return "none";
    case FF_APP_CREW_OP_START: return "start";
    case FF_APP_CREW_OP_LEAVE: return "leave";
    }
    return "?";
}

static char const *dbgconsole_crew_phase_name(ff_app_crew_phase_t p)
{
    switch (p) {
    case FF_APP_CREW_PHASE_IDLE: return "idle";
    case FF_APP_CREW_PHASE_GENERATING: return "generating";
    case FF_APP_CREW_PHASE_WRITING: return "writing";
    case FF_APP_CREW_PHASE_VERIFYING: return "verifying";
    case FF_APP_CREW_PHASE_READY: return "ready";
    case FF_APP_CREW_PHASE_FAILED: return "failed";
    }
    return "?";
}

static char const *dbgconsole_crew_fail_name(ff_app_crew_fail_t f)
{
    switch (f) {
    case FF_APP_CREW_FAIL_NONE: return "none";
    case FF_APP_CREW_FAIL_NO_LINK: return "no_link";
    case FF_APP_CREW_FAIL_REGION_UNSET: return "region_unset";
    case FF_APP_CREW_FAIL_NO_ENTROPY: return "no_entropy";
    case FF_APP_CREW_FAIL_NO_SNAPSHOT: return "no_snapshot";
    case FF_APP_CREW_FAIL_SEND: return "send";
    case FF_APP_CREW_FAIL_NAK: return "nak";
    case FF_APP_CREW_FAIL_TIMEOUT_ACK: return "timeout_ack";
    case FF_APP_CREW_FAIL_TIMEOUT_VERIFY: return "timeout_verify";
    case FF_APP_CREW_FAIL_MISMATCH: return "mismatch";
    }
    return "?";
}

static void dbgconsole_crew_status(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_shell_crew_op_status_t const st = ff_shell_crew_op_status(sh);

    char index_buf[8];
    if (st.crew_index_known) {
        snprintf(index_buf, sizeof(index_buf), "%u", (unsigned)st.crew_index);
    } else {
        snprintf(index_buf, sizeof(index_buf), "?");
    }

    char region_buf[16];
    if (!st.region_known) {
        snprintf(region_buf, sizeof(region_buf), "?");
    } else if (st.region == 0u) {
        snprintf(region_buf, sizeof(region_buf), "unset");
    } else {
        snprintf(region_buf, sizeof(region_buf), "%u", (unsigned)st.region);
    }

    /* A02 slice D2 amendment (#47) — what the crew channel's own row
     * currently states about position_precision, exactly like
     * `region_buf` above: a value when the radio reported one, else the
     * honest "unreported" (never "0", which is a real, different value
     * a radio can genuinely state). */
    char precision_buf[16];
    if (!st.precision_known) {
        snprintf(precision_buf, sizeof(precision_buf), "unreported");
    } else {
        snprintf(precision_buf, sizeof(precision_buf), "%u", (unsigned)st.precision);
    }

    char line[256];
    snprintf(line, sizeof(line),
             "dbg: crew code=%s index=%s region=%s precision=%s snapshot=%d can_start=%d can_leave=%d op=%s "
             "phase=%s fail=%s attempts=%u pending=%s",
             (st.code[0] != '\0') ? st.code : "(none)", index_buf, region_buf, precision_buf,
             st.has_snapshot ? 1 : 0, st.can_start ? 1 : 0, st.can_leave ? 1 : 0, dbgconsole_crew_op_name(st.op),
             dbgconsole_crew_phase_name(st.phase), dbgconsole_crew_fail_name(st.fail), (unsigned)st.attempts,
             (st.pending_code[0] != '\0') ? st.pending_code : "(none)");
    reply_line(reply, user, line);
}

/* `crew start` / `crew leave` — the SAME shell bodies the Settings
 * CONFIRM face reaches (`ff_shell_crew_start`/`ff_shell_crew_leave`),
 * never a second path into the machine; the exact discipline
 * `dbgconsole_name_set` keeps for `name <text>`.
 *
 * The return value is deliberately NOT reported as "ok"/"failed": a
 * refused precondition already lands in the status line's `phase=failed
 * fail=<reason>`, which says more, and printing a second, coarser
 * verdict beside it would be two answers to one question. The status
 * dump IS the reply. */
static void dbgconsole_crew_start(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    (void)ff_shell_crew_start(sh);
    dbgconsole_crew_status(sh, reply, user);
}

static void dbgconsole_crew_leave(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    (void)ff_shell_crew_leave(sh);
    dbgconsole_crew_status(sh, reply, user);
}

/* DIAGNOSTICS — `diag`: the SAME `ff_app_diag_t` the Settings DIAGNOSTICS
 * page renders (`ff_shell_diag_debug`, ff_shell.h — one projection, two
 * presentations, per that function's own doc comment), printed as one
 * line per section (link/position/mesh/time/compass/device) — Mesh
 * split across TWO lines (roster+RF, then airtime) — and Link likewise
 * (identity, then counters, debt/S15c-handshake-stall) — rather than one
 * each, purely to stay inside DBGCONSOLE_LINE_BUF under GCC's
 * `-Wformat-truncation` (its worst-case estimate for eight `%s` fields
 * in one line exceeded the budget even though no real value ever comes
 * close — see those splits' own comments) — so a bench operator gets the
 * whole page in eight reply lines without opening it on the touchscreen.
 * Every "?" below is this fact's own `has_*`-flag (or enum-UNKNOWN
 * member) reading false — never a fabricated value, same honest-data
 * discipline as every other command in this file. */
static void dbgconsole_diag(ff_shell_t *sh, ff_dbgconsole_wake_log_fn wake_log, void *hook_user,
                             ff_dbgconsole_reply_fn reply, void *user)
{
    ff_app_diag_t const d = ff_shell_diag_debug(sh);
    char line[DBGCONSOLE_LINE_BUF];
    char buf1[24];

    /* 1. Link */
    diag_u32_or_q(buf1, sizeof(buf1), d.has_last_frame_age, d.last_frame_age_ms);
    snprintf(line, sizeof(line), "dbg: diag link=%s node=!%08x name=%s/%s last_frame_ms=%s",
             diag_link_name(d.link), (unsigned)d.my_node_id, d.has_short_name ? d.short_name : "?",
             d.has_long_name ? d.long_name : "?", buf1);
    reply_line(reply, user, line);

    /* Link counters on their OWN line — the same -Wformat-truncation
     * budget reason the Mesh section below is split in two: the identity
     * half above already carries four %s fields whose GCC worst case
     * nearly fills DBGCONSOLE_LINE_BUF on its own, and
     * debt/S15c-handshake-stall's `hs_retries` is the counter that pushed
     * it over. Splitting is also the more readable presentation: one line
     * of "who are we talking to", one line of "how has that gone". */
    snprintf(line, sizeof(line), "dbg: diag frames_ok=%u decode_err=%u reconnects=%u hs_retries=%u",
             (unsigned)d.frames_ok, (unsigned)d.decode_errors, (unsigned)d.reconnects,
             (unsigned)d.handshake_retries);
    reply_line(reply, user, line);

    /* 2. Position (mine) */
    diag_u32_or_q(buf1, sizeof(buf1), d.pos_has_age, d.pos_age_ms);
    if (d.pos_ok) {
        char alt_buf[16] = "?";
        char sats_buf[16] = "?";
        char prec_buf[16] = "?";
        if (d.pos_has_altitude) snprintf(alt_buf, sizeof(alt_buf), "%d", (int)d.pos_altitude_m);
        if (d.pos_has_sats) snprintf(sats_buf, sizeof(sats_buf), "%u", (unsigned)d.pos_sats_in_view);
        if (d.pos_has_precision_bits) snprintf(prec_buf, sizeof(prec_buf), "%u", (unsigned)d.pos_precision_bits);
        snprintf(line, sizeof(line),
                 "dbg: diag pos src=%s ok=1 lat=%.6f lon=%.6f alt_m=%s sats=%s precision_bits=%s age_ms=%s",
                 diag_pos_src_name(d.pos_src), d.pos_lat, d.pos_lon, alt_buf, sats_buf, prec_buf, buf1);
    } else {
        snprintf(line, sizeof(line), "dbg: diag pos src=%s ok=0", diag_pos_src_name(d.pos_src));
    }
    reply_line(reply, user, line);

    /* 3. Mesh — split into three reply lines (roster / RF / airtime)
     * rather than one long one: cramming all eight optional %s fields
     * into a single DBGCONSOLE_LINE_BUF(200)-byte line left GCC's
     * `-Wformat-truncation` unable to prove no truncation (CLAUDE.md:
     * GCC is the build authority, not clang, which has no equivalent
     * check) — it estimates each `%s`'s worst case as its SOURCE
     * buffer's own declared capacity, and eight scratch buffers plus the
     * literal text comfortably exceeds 200 even though no REAL value
     * ever gets close. Splitting removes the arithmetic entirely rather
     * than fighting it with ever-tighter buffer sizes. */
    {
        char rssi_buf[8] = "?";
        char snr_buf[8] = "?";
        char direct_buf[2] = "?";
        char rf_age_buf[16];
        if (d.has_last_rssi) snprintf(rssi_buf, sizeof(rssi_buf), "%d", (int)d.last_rssi_dbm);
        if (d.has_last_snr) snprintf(snr_buf, sizeof(snr_buf), "%.1f", (double)d.last_snr_db);
        if (d.has_last_rssi || d.has_last_snr) snprintf(direct_buf, sizeof(direct_buf), "%d", d.last_rf_direct ? 1 : 0);
        diag_u32_or_q(rf_age_buf, sizeof(rf_age_buf), d.has_last_rf_age, d.last_rf_age_ms);
        snprintf(line, sizeof(line), "dbg: diag mesh crew=%u heard=%u rssi_dbm=%s snr_db=%s direct=%s rf_age_ms=%s",
                 (unsigned)d.crew_count, (unsigned)d.heard_count, rssi_buf, snr_buf, direct_buf, rf_age_buf);
    }
    reply_line(reply, user, line);
    {
        char cu_buf[8] = "?";
        char au_buf[8] = "?";
        char telem_age_buf[16];
        char bcast_age_buf[16];
        if (d.has_chan_util) snprintf(cu_buf, sizeof(cu_buf), "%.0f", (double)d.chan_util_pct);
        if (d.has_air_util_tx) snprintf(au_buf, sizeof(au_buf), "%.0f", (double)d.air_util_tx_pct);
        diag_u32_or_q(telem_age_buf, sizeof(telem_age_buf), d.has_telemetry_age, d.telemetry_age_ms);
        diag_u32_or_q(bcast_age_buf, sizeof(bcast_age_buf), d.has_pos_broadcast_age, d.pos_broadcast_age_ms);
        snprintf(line, sizeof(line), "dbg: diag mesh chan_util_pct=%s air_util_tx_pct=%s telemetry_age_ms=%s pos_bcast_age_ms=%s",
                 cu_buf, au_buf, telem_age_buf, bcast_age_buf);
    }
    reply_line(reply, user, line);

    /* 4. Time */
    {
        char src_buf[16] = "?";
        char offset_buf[16] = "?";
        char assumed_buf[4] = "?";
        if (d.wall_has_src_node) snprintf(src_buf, sizeof(src_buf), "!%08x", (unsigned)d.wall_src_node);
        if (d.wall_has_offset) {
            snprintf(offset_buf, sizeof(offset_buf), "%d", (int)d.wall_offset_min);
            snprintf(assumed_buf, sizeof(assumed_buf), "%d", d.wall_offset_assumed ? 1 : 0);
        }
        snprintf(line, sizeof(line), "dbg: diag time latched=%d trust=%s src_node=%s offset_min=%s assumed=%s local=%s",
                 d.wall_latched ? 1 : 0, d.wall_has_trust ? diag_wall_trust_name(d.wall_trust) : "?", src_buf,
                 offset_buf, assumed_buf, d.has_local_time ? d.local_time_str : "?");
    }
    reply_line(reply, user, line);

    /* 5. Compass */
    {
        char heading_buf[16] = "?";
        if (d.heading_valid) snprintf(heading_buf, sizeof(heading_buf), "%.0f", (double)d.heading_deg);
        snprintf(line, sizeof(line), "dbg: diag compass mag=%s present=%d imu=%s heading_deg=%s cal=%s",
                 diag_mag_kind_name(d.mag_kind), d.mag_present ? 1 : 0, diag_imu_state_name(d.imu_state), heading_buf,
                 d.compass_cal_set ? "set" : "identity");
    }
    reply_line(reply, user, line);

    /* 6. Device */
    {
        char batt_mv_buf[16] = "?";
        char batt_pct_buf[8] = "?";
        char heap_buf[16] = "?";
        if (d.has_batt_mv) snprintf(batt_mv_buf, sizeof(batt_mv_buf), "%u", (unsigned)d.batt_mv);
        if (d.batt_pct >= 0) snprintf(batt_pct_buf, sizeof(batt_pct_buf), "%d", (int)d.batt_pct);
        if (d.has_free_heap) snprintf(heap_buf, sizeof(heap_buf), "%u", (unsigned)d.free_heap_bytes);
        snprintf(line, sizeof(line), "dbg: diag device batt_mv=%s batt_pct=%s uptime_s=%u fw=%s/%s free_heap=%s",
                 batt_mv_buf, batt_pct_buf, (unsigned)d.uptime_s, d.fw_git_sha[0] != '\0' ? d.fw_git_sha : "unknown",
                 d.fw_build_date[0] != '\0' ? d.fw_build_date : "unknown", heap_buf);
    }
    reply_line(reply, user, line);

    /* 7. Wakes (2026-09-16 S26f field fix) — the last few light-sleep
     * wake causes, honestly omitted (not a fabricated "no wakes yet")
     * when the target has no hook (the sim, which has no light sleep at
     * all), same NULL convention as `i2c_health` above. This is what
     * lets the owner put the puck to sleep on battery, tap the glass,
     * plug back into USB, and read `diag` to see what happened, without
     * needing a live console session spanning the sleep itself (the
     * S26f amendment's own USB power-down during light sleep already
     * makes holding one open across a sleep impossible).
     *
     * A DEDICATED (wider than DBGCONSOLE_LINE_BUF) buffer here, unlike
     * every other `diag` fragment above: `ff_wake_log_format`'s own
     * caller (app_main.c) can hold several ring-buffer entries (~70 bytes
     * each), which the shared 200-byte `line` would truncate to 2-3 of
     * them. 768 comfortably covers a header plus 8 entries with headroom
     * (the target's own ring-buffer capacity, small and fixed, but not a
     * fact this app-layer file depends on by name). */
    if (wake_log != NULL) {
        char wake_body[768];
        if (wake_log(hook_user, wake_body, sizeof(wake_body)) >= 0) {
            char wake_line[sizeof(wake_body) + 16u];
            snprintf(wake_line, sizeof(wake_line), "dbg: diag %s", wake_body);
            reply_line(reply, user, wake_line);
        }
    }

    /* 8. Boot evidence (S25 latch-hold amendment) — THREE separate reply
     * lines, not one: `last_session` alone (128-byte source buffer) is
     * already GCC's own worst case close to DBGCONSOLE_LINE_BUF's 200
     * bytes once the "dbg: diag boot last_time=" prefix is added, and
     * `last_crash` (96 bytes) stacked onto the SAME line the way the
     * Mesh/Link sections above combine several small fields would blow
     * past it entirely — same -Wformat-truncation budget reasoning
     * those splits document, just against one wide field instead of
     * several narrow ones. reset_reason always prints (this boot's own
     * fact, always known); last_crash/last_time print "none" when there
     * is nothing honest to say — "?" (every OPTIONAL fact above's
     * absent-marker) would misleadingly read as "unknown" here. */
    snprintf(line, sizeof(line), "dbg: diag boot reset_reason=%s",
             d.boot_reset_reason[0] != '\0' ? d.boot_reset_reason : "unknown");
    reply_line(reply, user, line);
    snprintf(line, sizeof(line), "dbg: diag boot last_crash=%s", d.has_last_crash ? d.last_crash : "none");
    reply_line(reply, user, line);
    snprintf(line, sizeof(line), "dbg: diag boot last_time=%s", d.has_last_session ? d.last_session : "none");
    reply_line(reply, user, line);
}

/* "diag clear" (S25 latch-hold amendment) — erase the crash evidence
 * (device: the flash core dump; both targets: the live `last_crash`
 * line) via `ff_shell_diag_clear_crash`. Always succeeds from the
 * console's point of view (matches `cal clear`'s own unconditional
 * reply) — the device hook logs its own error, if any, to the serial
 * log rather than failing this command. */
static void dbgconsole_diag_clear(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_shell_diag_clear_crash(sh);
    reply_line(reply, user, "dbg: diag cleared");
}

static void dbgconsole_wall(ff_shell_t *sh, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_shell_wall_debug_t const w = ff_shell_wall_debug(sh);
    char line[DBGCONSOLE_LINE_BUF];

    if (!w.latched) {
        snprintf(line, sizeof(line), "dbg: wall latched=0 rejected=%u", (unsigned)w.rejected_relatches);
        reply_line(reply, user, line);
        return;
    }

    char offset_buf[16] = "?";
    if (w.has_offset) snprintf(offset_buf, sizeof(offset_buf), "%d", (int)w.offset_min);

    /* `assumed` is only meaningful once an offset exists at all — with
     * `!w.has_offset` (offset_buf above is already "?") there is no
     * assumed-vs-stated distinction to report, so print "?" here too
     * rather than "0", which would read as "a definite, non-assumed
     * offset" instead of "not applicable / unknown" (same reasoning as
     * offset_buf itself, just applied to the flag next to it). */
    char assumed_buf[4] = "?";
    if (w.has_offset) snprintf(assumed_buf, sizeof(assumed_buf), "%d", (int)w.offset_assumed);

    char src_buf[16] = "none";
    char trust_buf[16] = "none";
    if (w.has_last_obs) {
        snprintf(src_buf, sizeof(src_buf), "!%08x", (unsigned)w.last_obs_node);
        snprintf(trust_buf, sizeof(trust_buf), "%s", trust_name(w.last_obs_trust));
    }

    snprintf(line, sizeof(line),
             "dbg: wall latched=1 latch_unix=%lld trust=%s offset_min=%s assumed=%s last_src=%s rejected=%u",
             (long long)w.latch_unix_s, trust_buf, offset_buf, assumed_buf, src_buf,
             (unsigned)w.rejected_relatches);
    reply_line(reply, user, line);
}

/* `i2c` — two independent hooks (ff_debug_console.h), each optional.
 * `i2c_scan == NULL` means the WHOLE command is unavailable (nothing to
 * scan, so nothing to follow up on either) — the single honest reply
 * `"dbg: i2c unavailable on this target"`, matching the sim build's own
 * "no I2C bus at all" reality. Otherwise the scan line prints first
 * (or `"dbg: i2c scan failed"` if the hook itself reports it could not
 * run — e.g. the bus was never brought up), then the compass line, IF
 * `compass_status` is non-NULL, independent of whether the scan itself
 * succeeded: the compass driver's own state doesn't depend on this
 * particular bus sweep having worked. */
static void dbgconsole_i2c(ff_dbgconsole_i2c_scan_fn i2c_scan, ff_dbgconsole_compass_status_fn compass_status,
                            ff_dbgconsole_i2c_health_fn i2c_health, void *hook_user, ff_dbgconsole_reply_fn reply,
                            void *user)
{
    /* `line` must fit the longest prefix ("dbg: compass ", 13 bytes)
     * plus a full `body` (up to DBGCONSOLE_LINE_BUF-1 non-NUL bytes)
     * plus the NUL: 13 + 199 + 1 = 213. Sized with headroom so GCC's
     * `-Wformat-truncation` (CLAUDE.md: GCC is the build authority, not
     * clang) can prove the snprintf below never truncates, rather than
     * just happening not to at today's buffer sizes. */
    char body[DBGCONSOLE_LINE_BUF];
    char line[DBGCONSOLE_LINE_BUF + 16u];

    if (i2c_scan == NULL) {
        reply_line(reply, user, "dbg: i2c unavailable on this target");
        return;
    }

    if (i2c_scan(hook_user, body, sizeof(body)) < 0) {
        reply_line(reply, user, "dbg: i2c scan failed");
    } else {
        snprintf(line, sizeof(line), "dbg: i2c %s", body);
        reply_line(reply, user, line);
    }

    if (compass_status != NULL && compass_status(hook_user, body, sizeof(body)) >= 0) {
        snprintf(line, sizeof(line), "dbg: compass %s", body);
        reply_line(reply, user, line);
    }

    /* 2026-09-08 QA hardening — touch read-failure health, honestly
     * omitted (not a fabricated zero) when the target has no hook (the
     * sim), same NULL convention as compass_status just above. */
    if (i2c_health != NULL && i2c_health(hook_user, body, sizeof(body)) >= 0) {
        snprintf(line, sizeof(line), "dbg: touch %s", body);
        reply_line(reply, user, line);
    }
}

/* `perf` — see ff_dbgconsole_perf_fn's own doc comment for why this is a
 * straight forward, unlike the single-line i2c/compass/i2c_health hooks:
 * the hook is handed the reply sink directly and prints its own already-
 * prefixed lines. NULL is the one case this function itself handles. */
static void dbgconsole_perf(ff_dbgconsole_perf_fn perf, void *hook_user, ff_dbgconsole_reply_fn reply, void *user)
{
    if (perf == NULL) {
        reply_line(reply, user, "dbg: perf unavailable on this target");
        return;
    }
    perf(hook_user, reply, user);
}

/* `mic`/`mic on`/`mic off`/`mic watch <secs>` — see ff_dbgconsole_mic_fn's
 * own doc comment (ff_debug_console.h) for the single-hook-four-action
 * shape and the `mic watch` blocking-duration contract. `mic == NULL`
 * (the sim) is the one case this function itself handles, mirroring
 * dbgconsole_perf's identical NULL contract just above. */
static void dbgconsole_mic(ff_dbgconsole_mic_fn mic, ff_dbgconsole_mic_action_t action, uint32_t watch_secs,
                            void *hook_user, ff_dbgconsole_reply_fn reply, void *user)
{
    if (mic == NULL) {
        reply_line(reply, user, "dbg: mic unavailable on this target");
        return;
    }
    mic(hook_user, action, watch_secs, reply, user);
}

/* S31 — `music` / `music seed <n>`. The `source`/`loudness`/`bpm`
 * fields need no platform hook at all: `ff_beat_t` is core state the
 * SHELL already owns regardless of target (`ff_shell_music_debug`/
 * `ff_shell_set_music_seed`, app/include/ff_shell.h) — the mic/IMU
 * bytes feeding it are esp32s3-only, but this command only ever reads
 * the shell's already-projected RESULT, the same "public getter, never
 * reach into shell_t" rule this file's own top comment states for
 * every read-only command. Real (non-NULL-hook, in the sense that there
 * is no hook to be NULL) on BOTH targets — the sim can genuinely answer
 * "what does the beat detector currently think", it just never has real
 * mic/IMU bytes feeding it, so `source` honestly reads `none` there.
 *
 * 2026-09-09 (S31 canvas renderer) added ONE fragment that DOES need a
 * hook — `music_frame` (`ff_dbgconsole_music_frame_fn`, own doc comment,
 * ff_debug_console.h) — because it is sourced from `scr_music.c`'s own
 * per-frame timer, and this file's link target (`ff-debug-console`)
 * deliberately excludes LVGL/`ff-app-ui` (see this file's top comment).
 * `music_frame == NULL` or a negative return (no window closed yet)
 * both append the same honest `frame_ms=n/a canvas_us=n/a` — this
 * function's own job, mirroring `dbgconsole_i2c`'s identical
 * NULL-is-honestly-omitted convention for its own optional fragments. */
static void dbgconsole_music(ff_shell_t *sh, ff_dbgconsole_music_frame_fn music_frame, void *hook_user,
                              ff_dbgconsole_reply_fn reply, void *user)
{
    ff_shell_music_debug_t const d = ff_shell_music_debug(sh);
    char const *src_text =
        (d.source == FF_APP_MUSIC_SRC_MIC) ? "mic" : (d.source == FF_APP_MUSIC_SRC_IMU) ? "imu" : "none";

    /* fix/mic-dump-device-path — widened from 64: `music_frame` now also
     * folds the `perf` command's own `lvgl_refresh_avg_us`/`_max_us`
     * fields in (app_main.c's `dbgconsole_music_frame`, "one line tells
     * the whole story"), on top of the original `frame_ms`/`canvas_us`
     * pair. 128 clears GCC's -Wformat-truncation worst-case digit width
     * for four %u/%.2f fields with headroom (this repo's build
     * authority, CLAUDE.md — see `dbgconsole_mic_status_line`'s own
     * doc comment for the identical sizing reasoning). */
    char frame_body[128];
    bool const have_frame = (music_frame != NULL) && (music_frame(hook_user, frame_body, sizeof(frame_body)) >= 0);

    /* Widened past DBGCONSOLE_LINE_BUF (not raised globally: every other
     * command's line comfortably fits the shared 200-byte size, and this
     * is the one place `frame_body`'s own worst case needs the extra
     * room) — same "+N for one wider fragment" shape `ff_dbgcmd_line`'s
     * own doc comment already uses elsewhere in this file. */
    char line[DBGCONSOLE_LINE_BUF + sizeof(frame_body)];
    if (have_frame) {
        snprintf(line, sizeof(line), "dbg: music source=%s loudness=%.2f bpm=%.1f %s", src_text,
                  (double)d.loudness, (double)d.bpm_estimate, frame_body);
    } else {
        snprintf(line, sizeof(line),
                  "dbg: music source=%s loudness=%.2f bpm=%.1f frame_ms=n/a canvas_us=n/a lvgl_refresh=n/a", src_text,
                  (double)d.loudness, (double)d.bpm_estimate);
    }
    reply_line(reply, user, line);
}

static void dbgconsole_music_seed(ff_shell_t *sh, uint32_t seed, ff_dbgconsole_reply_fn reply, void *user)
{
    ff_shell_set_music_seed(sh, seed);
    char line[DBGCONSOLE_LINE_BUF];
    snprintf(line, sizeof(line), "dbg: music seed=%u", (unsigned)seed);
    reply_line(reply, user, line);
}

/* 2026-09-16 S26f field fix — `sleep`/`tpint`, own-lines shape identical
 * to `dbgconsole_perf`/`dbgconsole_mic` above: the hook is handed the
 * reply sink directly and prints its own already-`"dbg: "`-prefixed
 * lines. NULL is the one case this function itself handles. */
static void dbgconsole_sleep(ff_dbgconsole_sleep_fn sleep_fn, bool has_ms, uint32_t ms, void *hook_user,
                              ff_dbgconsole_reply_fn reply, void *user)
{
    if (sleep_fn == NULL) {
        reply_line(reply, user, "dbg: sleep unavailable on this target");
        return;
    }
    sleep_fn(hook_user, has_ms, ms, reply, user);
}

static void dbgconsole_tpint(ff_dbgconsole_tpint_fn tpint, void *hook_user, ff_dbgconsole_reply_fn reply, void *user)
{
    if (tpint == NULL) {
        reply_line(reply, user, "dbg: tpint unavailable on this target");
        return;
    }
    tpint(hook_user, reply, user);
}

void ff_dbgconsole_handle_line(ff_shell_t *sh, char const *line, size_t line_len, uint32_t now_ms,
                                ff_dbgconsole_reply_fn reply, void *user, ff_dbgconsole_i2c_scan_fn i2c_scan,
                                ff_dbgconsole_compass_status_fn compass_status,
                                ff_dbgconsole_i2c_health_fn i2c_health, ff_dbgconsole_perf_fn perf,
                                ff_dbgconsole_mic_fn mic, ff_dbgconsole_music_frame_fn music_frame,
                                ff_dbgconsole_sleep_fn sleep_fn, ff_dbgconsole_tpint_fn tpint,
                                ff_dbgconsole_wake_log_fn wake_log)
{
    (void)now_ms; /* every command below reaches "now" via a shell getter, not this parameter */
    if (sh == NULL || reply == NULL) return;

    ff_dbgcmd_t cmd;
    ff_dbgcmd_status_t const status = ff_dbgcmd_parse(line, line_len, &cmd);

    if (status == FF_DBGCMD_ERR_EMPTY) return; /* Enter on a blank line: no reply, not an error */
    if (status != FF_DBGCMD_ERR_OK) {
        reply_line(reply, user, "dbg: ? try help");
        return;
    }

    switch (cmd.kind) {
    case FF_DBGCMD_HELP: dbgconsole_help(reply, user); return;
    case FF_DBGCMD_ME: dbgconsole_me(sh, reply, user); return;
    case FF_DBGCMD_ROSTER: dbgconsole_roster(sh, reply, user); return;
    case FF_DBGCMD_HEARD: dbgconsole_heard(sh, reply, user); return;
    case FF_DBGCMD_SEND: dbgconsole_send(sh, cmd.u.text, reply, user); return;
    case FF_DBGCMD_DM: dbgconsole_dm(sh, cmd.u.dm.dest_node, cmd.u.dm.text, reply, user); return;
    case FF_DBGCMD_FLARE: dbgconsole_flare(sh, reply, user); return;
    case FF_DBGCMD_FLARE_CANCEL: dbgconsole_flare_cancel(sh, reply, user); return;
    case FF_DBGCMD_WALL: dbgconsole_wall(sh, reply, user); return;
    case FF_DBGCMD_I2C: dbgconsole_i2c(i2c_scan, compass_status, i2c_health, user, reply, user); return;
    case FF_DBGCMD_CAL: dbgconsole_cal_status(sh, reply, user); return;
    case FF_DBGCMD_CAL_START: dbgconsole_cal_start(sh, reply, user); return;
    case FF_DBGCMD_CAL_FINISH: dbgconsole_cal_finish(sh, reply, user); return;
    case FF_DBGCMD_CAL_CANCEL: dbgconsole_cal_cancel(sh, reply, user); return;
    case FF_DBGCMD_CAL_CLEAR: dbgconsole_cal_clear(sh, reply, user); return;
    case FF_DBGCMD_NAME: dbgconsole_name_status(sh, reply, user); return;
    case FF_DBGCMD_NAME_SET: dbgconsole_name_set(sh, cmd.u.text, reply, user); return;
    case FF_DBGCMD_CREW: dbgconsole_crew_status(sh, reply, user); return;
    case FF_DBGCMD_CREW_START: dbgconsole_crew_start(sh, reply, user); return;
    case FF_DBGCMD_CREW_LEAVE: dbgconsole_crew_leave(sh, reply, user); return;
    case FF_DBGCMD_DIAG: dbgconsole_diag(sh, wake_log, user, reply, user); return;
    case FF_DBGCMD_DIAG_CLEAR: dbgconsole_diag_clear(sh, reply, user); return;
    case FF_DBGCMD_PERF: dbgconsole_perf(perf, user, reply, user); return;
    case FF_DBGCMD_PING: dbgconsole_ping(sh, cmd.u.node, reply, user); return;
    case FF_DBGCMD_FIND: dbgconsole_find(sh, cmd.u.node, reply, user); return;
    case FF_DBGCMD_FIND_OFF: dbgconsole_find_off(sh, reply, user); return;
    case FF_DBGCMD_MIC: dbgconsole_mic(mic, FF_DBGCONSOLE_MIC_STATUS, 0u, user, reply, user); return;
    case FF_DBGCMD_MIC_ON: dbgconsole_mic(mic, FF_DBGCONSOLE_MIC_ON, 0u, user, reply, user); return;
    case FF_DBGCMD_MIC_OFF: dbgconsole_mic(mic, FF_DBGCONSOLE_MIC_OFF, 0u, user, reply, user); return;
    case FF_DBGCMD_MIC_WATCH:
        dbgconsole_mic(mic, FF_DBGCONSOLE_MIC_WATCH, cmd.u.mic_watch_secs, user, reply, user);
        return;
    case FF_DBGCMD_MIC_DUMP:
        dbgconsole_mic(mic, FF_DBGCONSOLE_MIC_DUMP, cmd.u.mic_dump_secs, user, reply, user);
        return;
    case FF_DBGCMD_MUSIC: dbgconsole_music(sh, music_frame, user, reply, user); return;
    case FF_DBGCMD_MUSIC_SEED: dbgconsole_music_seed(sh, cmd.u.music_seed, reply, user); return;
    case FF_DBGCMD_SLEEP: dbgconsole_sleep(sleep_fn, cmd.u.sleep.has_ms, cmd.u.sleep.ms, user, reply, user); return;
    case FF_DBGCMD_TPINT: dbgconsole_tpint(tpint, user, reply, user); return;
    case FF_DBGCMD_NONE: break; /* ff_dbgcmd_parse never returns OK with NONE — unreachable */
    }
    reply_line(reply, user, "dbg: ? try help");
}

#endif /* FF_TARGET_SIM || CONFIG_FF_DEBUG_CONSOLE */
