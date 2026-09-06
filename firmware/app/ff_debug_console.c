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
    reply_line(reply, user, "dbg: i2c                      shared I2C bus scan + one-shot compass status");
    reply_line(reply, user, "dbg: cal                      compass calibration ritual status");
    reply_line(reply, user, "dbg: cal start                begin a calibration session");
    reply_line(reply, user, "dbg: cal finish               end the session, persist if coverage is enough");
    reply_line(reply, user, "dbg: cal cancel               abandon the session, calibration unchanged");
    reply_line(reply, user, "dbg: cal clear                drop the stored calibration back to identity");
    reply_line(reply, user, "dbg: name                     NAME in Settings: stored/mesh/confirmed status");
    reply_line(reply, user, "dbg: name <text>              set + push the Meshtastic owner update");
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

static void dbgconsole_send(ff_shell_t *sh, char const *text, ff_dbgconsole_reply_fn reply, void *user)
{
    int const rc = ff_shell_debug_send_text(sh, 0u, text); /* 0 = crew broadcast */
    char line[DBGCONSOLE_LINE_BUF];
    snprintf(line, sizeof(line), "dbg: send %s dest=broadcast", (rc == 0) ? "ok" : "failed");
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
    snprintf(line, sizeof(line), "dbg: dm %s dest=!%08x", (rc == 0) ? "ok" : "failed", (unsigned)dest);
    reply_line(reply, user, line);
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
     * owner actually ended up being). */
    snprintf(line, sizeof(line), "dbg: name stored=%s mesh=%s confirmed=%d%s seq=%u pushed=%s ack=%s reply=%s mismatch=%d",
             stored, mesh_buf, st.confirmed ? 1 : 0, st.my_name_from_node ? " (from_node)" : "",
             (unsigned)st.pushed_seq, pushed_buf, ack_str, reply_buf, st.mismatch ? 1 : 0);
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
                            void *hook_user, ff_dbgconsole_reply_fn reply, void *user)
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
}

void ff_dbgconsole_handle_line(ff_shell_t *sh, char const *line, size_t line_len, uint32_t now_ms,
                                ff_dbgconsole_reply_fn reply, void *user, ff_dbgconsole_i2c_scan_fn i2c_scan,
                                ff_dbgconsole_compass_status_fn compass_status)
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
    case FF_DBGCMD_I2C: dbgconsole_i2c(i2c_scan, compass_status, user, reply, user); return;
    case FF_DBGCMD_CAL: dbgconsole_cal_status(sh, reply, user); return;
    case FF_DBGCMD_CAL_START: dbgconsole_cal_start(sh, reply, user); return;
    case FF_DBGCMD_CAL_FINISH: dbgconsole_cal_finish(sh, reply, user); return;
    case FF_DBGCMD_CAL_CANCEL: dbgconsole_cal_cancel(sh, reply, user); return;
    case FF_DBGCMD_CAL_CLEAR: dbgconsole_cal_clear(sh, reply, user); return;
    case FF_DBGCMD_NAME: dbgconsole_name_status(sh, reply, user); return;
    case FF_DBGCMD_NAME_SET: dbgconsole_name_set(sh, cmd.u.text, reply, user); return;
    case FF_DBGCMD_NONE: break; /* ff_dbgcmd_parse never returns OK with NONE — unreachable */
    }
    reply_line(reply, user, "dbg: ? try help");
}

#endif /* FF_TARGET_SIM || CONFIG_FF_DEBUG_CONSOLE */
