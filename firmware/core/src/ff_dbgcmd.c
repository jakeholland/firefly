/**
 * ff_dbgcmd.c — see ff_dbgcmd.h.
 */
#include "ff_dbgcmd.h"

#include <stdbool.h>
#include <string.h>

/* Local scratch buffer sized to the max accepted line, +1 for a NUL this
 * file adds itself (the input `line` is never assumed to carry one). */
#define DBGCMD_BUF_SZ (FF_DBGCMD_LINE_MAX + 1u)

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint8_t hex_val(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return (uint8_t)(c - 'A' + 10);
}

/* Parse a bounded hex node id: optional "!" or "0x"/"0X" prefix, then
 * 1-8 hex digits and nothing else. `tok`/`tok_len` name exactly the
 * token (no surrounding whitespace). Returns false (leaving *out
 * unspecified) on anything else — empty digits, >8 digits, a non-hex
 * character, or a prefix with nothing after it. Never reads past
 * `tok_len`. */
static bool parse_node_hex(char const *tok, size_t tok_len, uint32_t *out)
{
    size_t i = 0;
    if (tok_len >= 1 && tok[0] == '!') {
        i = 1;
    } else if (tok_len >= 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        i = 2;
    }
    size_t const digits_start = i;
    size_t const ndigits = tok_len - digits_start;
    if (ndigits == 0u || ndigits > 8u) return false;
    uint32_t v = 0u;
    for (; i < tok_len; ++i) {
        if (!is_hex_digit(tok[i])) return false;
        v = (v << 4) | hex_val(tok[i]);
    }
    *out = v;
    return true;
}

/* S30 — parse a bounded decimal token (1-3 digits, no sign, no leading
 * '+', nothing else): "mic watch <secs>"'s own argument shape. Returns
 * false (leaving *out unspecified) on an empty token, a non-digit
 * character, or a token longer than 3 digits (comfortably wider than
 * FF_DBGCMD_MIC_WATCH_MAX_S's own 2 digits, so a genuinely-too-large
 * value like "999" still parses to a real number and gets rejected by
 * the caller's own range check below, not silently truncated/overflowed
 * by this helper first). Never reads past `tok_len`. */
static bool parse_u32_dec(char const *tok, size_t tok_len, uint32_t *out)
{
    if (tok_len == 0u || tok_len > 3u) return false;
    uint32_t v = 0u;
    for (size_t i = 0; i < tok_len; ++i) {
        char const c = tok[i];
        if (c < '0' || c > '9') return false;
        v = v * 10u + (uint32_t)(c - '0');
    }
    *out = v;
    return true;
}

/* Find the end of the first whitespace-delimited token starting at
 * `start` (which must already be non-whitespace or == `end`). Returns
 * the index of the first whitespace char at/after `start`, or `end` if
 * the token runs to the end of the trimmed line. */
static size_t token_end(char const *buf, size_t start, size_t end)
{
    size_t i = start;
    while (i < end && !is_space(buf[i])) ++i;
    return i;
}

/* Skip whitespace starting at `start`, bounded by `end`. */
static size_t skip_space(char const *buf, size_t start, size_t end)
{
    size_t i = start;
    while (i < end && is_space(buf[i])) ++i;
    return i;
}

static bool tok_eq(char const *buf, size_t start, size_t end, char const *lit)
{
    size_t const lit_len = strlen(lit);
    if (end - start != lit_len) return false;
    return memcmp(buf + start, lit, lit_len) == 0;
}

ff_dbgcmd_status_t ff_dbgcmd_parse(char const *line, size_t line_len, ff_dbgcmd_t *out)
{
    if (out == NULL) return FF_DBGCMD_ERR_BAD_ARGS;
    memset(out, 0, sizeof(*out));

    if (line == NULL) {
        return (line_len == 0u) ? FF_DBGCMD_ERR_EMPTY : FF_DBGCMD_ERR_BAD_ARGS;
    }
    if (line_len > FF_DBGCMD_LINE_MAX) return FF_DBGCMD_ERR_TOO_LONG;

    char buf[DBGCMD_BUF_SZ];
    memcpy(buf, line, line_len);
    /* No trailing NUL is written/relied upon below — every scan is
     * bounded by an explicit end index, never a NUL search, so a
     * 300-byte input with no NUL anywhere (rejected above by the
     * length check already, but this keeps the invariant true even if
     * that check is ever loosened) can never be over-read here. */

    /* Trim CRLF, then any other trailing whitespace, then leading
     * whitespace — all against `buf`/`line_len` directly, no strlen. */
    size_t end = line_len;
    while (end > 0u && (buf[end - 1u] == '\n' || buf[end - 1u] == '\r')) --end;
    while (end > 0u && is_space(buf[end - 1u])) --end;
    size_t start = skip_space(buf, 0u, end);

    if (start >= end) return FF_DBGCMD_ERR_EMPTY;

    size_t const cmd_end = token_end(buf, start, end);
    size_t const arg_start = skip_space(buf, cmd_end, end);
    /* `rest` = [arg_start, end): already right-trimmed (see `end` above),
     * so an all-whitespace tail collapses to an empty `rest`, not a
     * whitespace-only one. */

    if (tok_eq(buf, start, cmd_end, "help")) {
        if (arg_start < end) return FF_DBGCMD_ERR_BAD_ARGS;
        out->kind = FF_DBGCMD_HELP;
        return FF_DBGCMD_ERR_OK;
    }
    if (tok_eq(buf, start, cmd_end, "me")) {
        if (arg_start < end) return FF_DBGCMD_ERR_BAD_ARGS;
        out->kind = FF_DBGCMD_ME;
        return FF_DBGCMD_ERR_OK;
    }
    if (tok_eq(buf, start, cmd_end, "roster")) {
        if (arg_start < end) return FF_DBGCMD_ERR_BAD_ARGS;
        out->kind = FF_DBGCMD_ROSTER;
        return FF_DBGCMD_ERR_OK;
    }
    if (tok_eq(buf, start, cmd_end, "heard")) {
        if (arg_start < end) return FF_DBGCMD_ERR_BAD_ARGS;
        out->kind = FF_DBGCMD_HEARD;
        return FF_DBGCMD_ERR_OK;
    }
    if (tok_eq(buf, start, cmd_end, "wall")) {
        if (arg_start < end) return FF_DBGCMD_ERR_BAD_ARGS;
        out->kind = FF_DBGCMD_WALL;
        return FF_DBGCMD_ERR_OK;
    }
    if (tok_eq(buf, start, cmd_end, "i2c")) {
        if (arg_start < end) return FF_DBGCMD_ERR_BAD_ARGS;
        out->kind = FF_DBGCMD_I2C;
        return FF_DBGCMD_ERR_OK;
    }
    if (tok_eq(buf, start, cmd_end, "flare")) {
        if (arg_start >= end) {
            out->kind = FF_DBGCMD_FLARE;
            return FF_DBGCMD_ERR_OK;
        }
        if (tok_eq(buf, arg_start, end, "cancel")) {
            out->kind = FF_DBGCMD_FLARE_CANCEL;
            return FF_DBGCMD_ERR_OK;
        }
        return FF_DBGCMD_ERR_BAD_ARGS;
    }
    if (tok_eq(buf, start, cmd_end, "cal")) {
        if (arg_start >= end) {
            out->kind = FF_DBGCMD_CAL;
            return FF_DBGCMD_ERR_OK;
        }
        if (tok_eq(buf, arg_start, end, "start")) {
            out->kind = FF_DBGCMD_CAL_START;
            return FF_DBGCMD_ERR_OK;
        }
        if (tok_eq(buf, arg_start, end, "finish")) {
            out->kind = FF_DBGCMD_CAL_FINISH;
            return FF_DBGCMD_ERR_OK;
        }
        if (tok_eq(buf, arg_start, end, "cancel")) {
            out->kind = FF_DBGCMD_CAL_CANCEL;
            return FF_DBGCMD_ERR_OK;
        }
        if (tok_eq(buf, arg_start, end, "clear")) {
            out->kind = FF_DBGCMD_CAL_CLEAR;
            return FF_DBGCMD_ERR_OK;
        }
        return FF_DBGCMD_ERR_BAD_ARGS;
    }
    if (tok_eq(buf, start, cmd_end, "send")) {
        size_t const n = end - arg_start;
        if (n == 0u || n > FF_DBGCMD_TEXT_MAX) return FF_DBGCMD_ERR_BAD_ARGS;
        memcpy(out->u.text, buf + arg_start, n);
        out->u.text[n] = '\0';
        out->kind = FF_DBGCMD_SEND;
        return FF_DBGCMD_ERR_OK;
    }
    if (tok_eq(buf, start, cmd_end, "dm")) {
        if (arg_start >= end) return FF_DBGCMD_ERR_BAD_ARGS; /* no node id, no text */
        size_t const node_end = token_end(buf, arg_start, end);
        uint32_t dest = 0u;
        if (!parse_node_hex(buf + arg_start, node_end - arg_start, &dest)) {
            return FF_DBGCMD_ERR_BAD_ARGS;
        }
        size_t const text_start = skip_space(buf, node_end, end);
        size_t const n = end - text_start;
        if (n == 0u || n > FF_DBGCMD_TEXT_MAX) return FF_DBGCMD_ERR_BAD_ARGS;
        out->u.dm.dest_node = dest;
        memcpy(out->u.dm.text, buf + text_start, n);
        out->u.dm.text[n] = '\0';
        out->kind = FF_DBGCMD_DM;
        return FF_DBGCMD_ERR_OK;
    }
    if (tok_eq(buf, start, cmd_end, "diag")) {
        if (arg_start < end) return FF_DBGCMD_ERR_BAD_ARGS;
        out->kind = FF_DBGCMD_DIAG;
        return FF_DBGCMD_ERR_OK;
    }
    if (tok_eq(buf, start, cmd_end, "perf")) {
        if (arg_start < end) return FF_DBGCMD_ERR_BAD_ARGS;
        out->kind = FF_DBGCMD_PERF;
        return FF_DBGCMD_ERR_OK;
    }
    if (tok_eq(buf, start, cmd_end, "name")) {
        if (arg_start >= end) {
            out->kind = FF_DBGCMD_NAME;
            return FF_DBGCMD_ERR_OK;
        }
        size_t const n = end - arg_start;
        if (n > FF_DBGCMD_TEXT_MAX) return FF_DBGCMD_ERR_BAD_ARGS;
        memcpy(out->u.text, buf + arg_start, n);
        out->u.text[n] = '\0';
        out->kind = FF_DBGCMD_NAME_SET;
        return FF_DBGCMD_ERR_OK;
    }
    /* S29 PR2 — "ping <node_hex>": exactly `dm`'s node-id shape, minus
     * the trailing text body (a bench probe has no message). Any
     * trailing token after the node id is rejected, not ignored — same
     * "explicit unknown, never silently plausible" discipline this
     * parser applies everywhere else. */
    if (tok_eq(buf, start, cmd_end, "ping")) {
        if (arg_start >= end) return FF_DBGCMD_ERR_BAD_ARGS; /* no node id */
        size_t const node_end = token_end(buf, arg_start, end);
        uint32_t node = 0u;
        if (!parse_node_hex(buf + arg_start, node_end - arg_start, &node)) {
            return FF_DBGCMD_ERR_BAD_ARGS;
        }
        if (skip_space(buf, node_end, end) < end) return FF_DBGCMD_ERR_BAD_ARGS; /* trailing garbage */
        out->u.node = node;
        out->kind = FF_DBGCMD_PING;
        return FF_DBGCMD_ERR_OK;
    }
    /* S29 PR2 — "find <node_hex>" starts a session (same node-id shape
     * as `ping`); "find off" cancels it. `off` is checked as a literal
     * token match on the SAME slot a node hex would occupy, mirroring
     * `flare`/`flare cancel`'s own bare-verb-plus-fixed-sub-verb shape —
     * "off" is never a valid hex token containing only 0/f digits
     * confusable with a node id here, since parse_node_hex requires the
     * WHOLE token to be hex digits (optionally `!`/`0x`-prefixed) and
     * "off" starts with a non-hex 'o'. */
    if (tok_eq(buf, start, cmd_end, "find")) {
        if (arg_start >= end) return FF_DBGCMD_ERR_BAD_ARGS; /* no node id, no "off" */
        size_t const arg_end = token_end(buf, arg_start, end);
        if (tok_eq(buf, arg_start, arg_end, "off")) {
            if (skip_space(buf, arg_end, end) < end) return FF_DBGCMD_ERR_BAD_ARGS;
            out->kind = FF_DBGCMD_FIND_OFF;
            return FF_DBGCMD_ERR_OK;
        }
        uint32_t node = 0u;
        if (!parse_node_hex(buf + arg_start, arg_end - arg_start, &node)) {
            return FF_DBGCMD_ERR_BAD_ARGS;
        }
        if (skip_space(buf, arg_end, end) < end) return FF_DBGCMD_ERR_BAD_ARGS;
        out->u.node = node;
        out->kind = FF_DBGCMD_FIND;
        return FF_DBGCMD_ERR_OK;
    }
    /* S30 — "mic" bare, "mic on", "mic off", "mic watch <secs>". Same
     * bare-verb-plus-fixed-sub-verb shape as `cal`/`flare` above; "watch"
     * additionally carries one decimal argument this parser itself range-
     * checks into [FF_DBGCMD_MIC_WATCH_MIN_S, FF_DBGCMD_MIC_WATCH_MAX_S]
     * (ff_dbgcmd.h's own doc comment on `mic`) — a caller can never see
     * an in-vocabulary FF_DBGCMD_MIC_WATCH with an out-of-range duration. */
    if (tok_eq(buf, start, cmd_end, "mic")) {
        if (arg_start >= end) {
            out->kind = FF_DBGCMD_MIC;
            return FF_DBGCMD_ERR_OK;
        }
        size_t const sub_end = token_end(buf, arg_start, end);
        if (tok_eq(buf, arg_start, sub_end, "on")) {
            if (skip_space(buf, sub_end, end) < end) return FF_DBGCMD_ERR_BAD_ARGS;
            out->kind = FF_DBGCMD_MIC_ON;
            return FF_DBGCMD_ERR_OK;
        }
        if (tok_eq(buf, arg_start, sub_end, "off")) {
            if (skip_space(buf, sub_end, end) < end) return FF_DBGCMD_ERR_BAD_ARGS;
            out->kind = FF_DBGCMD_MIC_OFF;
            return FF_DBGCMD_ERR_OK;
        }
        if (tok_eq(buf, arg_start, sub_end, "watch")) {
            size_t const secs_start = skip_space(buf, sub_end, end);
            if (secs_start >= end) return FF_DBGCMD_ERR_BAD_ARGS; /* no duration */
            size_t const secs_end = token_end(buf, secs_start, end);
            uint32_t secs = 0u;
            if (!parse_u32_dec(buf + secs_start, secs_end - secs_start, &secs)) {
                return FF_DBGCMD_ERR_BAD_ARGS;
            }
            if (skip_space(buf, secs_end, end) < end) return FF_DBGCMD_ERR_BAD_ARGS; /* trailing garbage */
            if (secs < FF_DBGCMD_MIC_WATCH_MIN_S || secs > FF_DBGCMD_MIC_WATCH_MAX_S) {
                return FF_DBGCMD_ERR_BAD_ARGS;
            }
            out->u.mic_watch_secs = secs;
            out->kind = FF_DBGCMD_MIC_WATCH;
            return FF_DBGCMD_ERR_OK;
        }
        return FF_DBGCMD_ERR_BAD_ARGS;
    }

    /* S31 — "music" bare, "music seed <n>". Same bare-verb-plus-
     * fixed-sub-verb shape as `mic` above; "seed" carries one decimal
     * argument, reusing `parse_u32_dec` verbatim (no extra range check —
     * see ff_dbgcmd.h's own doc comment on `u.music_seed`). */
    if (tok_eq(buf, start, cmd_end, "music")) {
        if (arg_start >= end) {
            out->kind = FF_DBGCMD_MUSIC;
            return FF_DBGCMD_ERR_OK;
        }
        size_t const sub_end = token_end(buf, arg_start, end);
        if (tok_eq(buf, arg_start, sub_end, "seed")) {
            size_t const seed_start = skip_space(buf, sub_end, end);
            if (seed_start >= end) return FF_DBGCMD_ERR_BAD_ARGS; /* no seed value */
            size_t const seed_end = token_end(buf, seed_start, end);
            uint32_t seed = 0u;
            if (!parse_u32_dec(buf + seed_start, seed_end - seed_start, &seed)) {
                return FF_DBGCMD_ERR_BAD_ARGS;
            }
            if (skip_space(buf, seed_end, end) < end) return FF_DBGCMD_ERR_BAD_ARGS; /* trailing garbage */
            out->u.music_seed = seed;
            out->kind = FF_DBGCMD_MUSIC_SEED;
            return FF_DBGCMD_ERR_OK;
        }
        return FF_DBGCMD_ERR_BAD_ARGS;
    }

    return FF_DBGCMD_ERR_UNKNOWN_CMD;
}

char const *ff_dbgcmd_kind_name(ff_dbgcmd_kind_t kind)
{
    switch (kind) {
    case FF_DBGCMD_NONE: return "NONE";
    case FF_DBGCMD_HELP: return "HELP";
    case FF_DBGCMD_ME: return "ME";
    case FF_DBGCMD_ROSTER: return "ROSTER";
    case FF_DBGCMD_HEARD: return "HEARD";
    case FF_DBGCMD_SEND: return "SEND";
    case FF_DBGCMD_DM: return "DM";
    case FF_DBGCMD_FLARE: return "FLARE";
    case FF_DBGCMD_FLARE_CANCEL: return "FLARE_CANCEL";
    case FF_DBGCMD_WALL: return "WALL";
    case FF_DBGCMD_I2C: return "I2C";
    case FF_DBGCMD_CAL: return "CAL";
    case FF_DBGCMD_CAL_START: return "CAL_START";
    case FF_DBGCMD_CAL_FINISH: return "CAL_FINISH";
    case FF_DBGCMD_CAL_CANCEL: return "CAL_CANCEL";
    case FF_DBGCMD_CAL_CLEAR: return "CAL_CLEAR";
    case FF_DBGCMD_NAME: return "NAME";
    case FF_DBGCMD_NAME_SET: return "NAME_SET";
    case FF_DBGCMD_DIAG: return "DIAG";
    case FF_DBGCMD_PERF: return "PERF";
    case FF_DBGCMD_PING: return "PING";
    case FF_DBGCMD_FIND: return "FIND";
    case FF_DBGCMD_FIND_OFF: return "FIND_OFF";
    case FF_DBGCMD_MIC: return "MIC";
    case FF_DBGCMD_MIC_ON: return "MIC_ON";
    case FF_DBGCMD_MIC_OFF: return "MIC_OFF";
    case FF_DBGCMD_MIC_WATCH: return "MIC_WATCH";
    case FF_DBGCMD_MUSIC: return "MUSIC";
    case FF_DBGCMD_MUSIC_SEED: return "MUSIC_SEED";
    }
    return "?";
}

char const *ff_dbgcmd_status_name(ff_dbgcmd_status_t status)
{
    switch (status) {
    case FF_DBGCMD_ERR_OK: return "OK";
    case FF_DBGCMD_ERR_EMPTY: return "EMPTY";
    case FF_DBGCMD_ERR_TOO_LONG: return "TOO_LONG";
    case FF_DBGCMD_ERR_UNKNOWN_CMD: return "UNKNOWN_CMD";
    case FF_DBGCMD_ERR_BAD_ARGS: return "BAD_ARGS";
    }
    return "?";
}
