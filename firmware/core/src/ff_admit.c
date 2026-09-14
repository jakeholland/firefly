/**
 * ff_admit.c — the crew-admission rule (docs/specs/S02-core-crew.md's
 * 2026-09-13 amendment §B). See ff_admit.h for the rule, the policy
 * change it embodies, and why each clause is here.
 */
#include "ff_admit.h"

#include <stddef.h> /* NULL */

bool ff_admit_portnum_ok(uint32_t portnum)
{
    /* Matched by RAW VALUE, deliberately — 269 is not a named enumerator
     * in portnums.proto and matching a `PRIVATE_APP` case would admit
     * nobody (ff_admit.h). */
    return portnum == FF_ADMIT_PORTNUM_TEXT || portnum == FF_ADMIT_PORTNUM_POSITION ||
           portnum == FF_ADMIT_PORTNUM_NODEINFO || portnum == FF_ADMIT_PORTNUM_FIREFLY;
}

ff_admit_result_t ff_admit(ff_admit_in_t const *in)
{
    if (in == NULL) return FF_ADMIT_NO_DISABLED;
    if (!in->auto_crew_enabled) return FF_ADMIT_NO_DISABLED;

    /* 1 — decrypted. */
    if (!in->decrypted) return FF_ADMIT_NO_ENCRYPTED;

    /* 2 — on the crew channel's index on THIS radio.
     *
     * Two distinct rejections, kept distinct because they mean different
     * things to the wearer: "your puck isn't on this crew's channel" is
     * a setup problem the CREW page must say out loud, where "that
     * packet was on some other channel" is ordinary mesh noise. Folding
     * them into one reason would let the page report the wrong one. */
    if (!in->crew_index_known) return FF_ADMIT_NO_NO_CREW_CHANNEL;
    if (!in->has_channel_index || in->channel_index != in->crew_index) {
        return FF_ADMIT_NO_OTHER_CHANNEL;
    }

    /* 3 — not nobody, and not us.
     *
     * Note the `!has_my_node_id` arm: without our own id we cannot prove
     * a packet is not our own echo, so we refuse rather than guess. That
     * is a real window (it closes on the handshake's my_info, ~20 ms
     * after connect), and erring toward "don't admit" there costs at
     * most one extra packet's delay. */
    if (in->from == 0u) return FF_ADMIT_NO_SELF;
    if (!in->has_my_node_id || in->from == in->my_node_id) return FF_ADMIT_NO_SELF;

    /* 4 — not hidden. Consulted HERE, not only at render time, so a
     * hidden node is never silently re-admitted by its next packet. */
    if (in->hidden) return FF_ADMIT_NO_HIDDEN;

    /* 5 — not via MQTT. */
    if (in->via_mqtt) return FF_ADMIT_NO_VIA_MQTT;

    /* 6 — a portnum that carries identity or intent.
     *
     * TELEMETRY_APP is deliberately absent: it refreshes an existing
     * member's presence through the shell's unconditional
     * `ff_crew_on_heard` call, but it carries neither identity nor
     * intent, and NodeInfo follows within minutes anyway. */
    if (!in->has_portnum || !ff_admit_portnum_ok(in->portnum)) return FF_ADMIT_NO_PORTNUM;

    return FF_ADMIT_YES;
}

char const *ff_admit_reason(ff_admit_result_t r)
{
    switch (r) {
    case FF_ADMIT_YES:                 return "admit";
    case FF_ADMIT_NO_DISABLED:         return "auto-crew-off";
    case FF_ADMIT_NO_ENCRYPTED:        return "not-decrypted";
    case FF_ADMIT_NO_NO_CREW_CHANNEL:  return "no-crew-channel";
    case FF_ADMIT_NO_OTHER_CHANNEL:    return "other-channel";
    case FF_ADMIT_NO_SELF:             return "self";
    case FF_ADMIT_NO_HIDDEN:           return "hidden";
    case FF_ADMIT_NO_VIA_MQTT:         return "via-mqtt";
    case FF_ADMIT_NO_PORTNUM:          return "portnum";
    }
    /* No default: above, so a new enumerator is a -Wswitch error rather
     * than a silent "unknown" at runtime. This line is only reached for
     * a value cast in from outside the enum. */
    return "unknown";
}
