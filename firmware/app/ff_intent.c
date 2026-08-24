/**
 * ff_intent.c — the emit seam's one moving part (S16 slice c1).
 *
 * See ff_intent.h for the design and the judgment call behind a
 * process-global sink. This file is deliberately trivial: bind, forward,
 * and stay a safe no-op while unbound — headless/golden rendering never
 * binds anything and must keep producing byte-identical frames.
 */
#include "ff_intent.h"

#include <stddef.h>

static ff_intent_emit_fn s_sink;
static void *s_sink_user;

void ff_intent_emit_bind(ff_intent_emit_fn fn, void *user)
{
    s_sink = fn;
    /* A NULL fn is an unbind; drop the stale user pointer with it rather
     * than keeping a dangling context nothing can reach. */
    s_sink_user = (fn != NULL) ? user : NULL;
}

void ff_intent_emit(ff_intent_t const *in)
{
    if (s_sink == NULL || in == NULL) return;
    s_sink(s_sink_user, in);
}
