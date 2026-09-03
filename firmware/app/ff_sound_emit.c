/**
 * ff_sound_emit.c — see ff_sound_emit.h.
 *
 * Deliberately trivial, mirroring ff_intent.c: bind, forward, stay a
 * safe no-op while unbound.
 */
#include "ff_sound_emit.h"

#include <stddef.h>

static ff_sound_emit_fn s_sink;
static void *s_sink_user;

void ff_sound_emit_bind(ff_sound_emit_fn fn, void *user)
{
    s_sink = fn;
    s_sink_user = (fn != NULL) ? user : NULL;
}

void ff_sound_emit(ff_sound_event_t ev)
{
    if (s_sink == NULL) return;
    s_sink(s_sink_user, ev);
}
