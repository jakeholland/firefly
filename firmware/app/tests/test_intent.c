/**
 * test_intent.c — S16 slice c1: `ff_shell_intent` dispatch and the emit
 * seam.
 *
 * Slice c1 has no numbered acceptance criterion of its own (its row in
 * S16's slice table defines scope, not ACs), so tests are named
 * S16_c1_* after the rules they pin — the same convention test_shell.c's
 * S16_b1_* uses. The three that matter most:
 *
 *  - the S10 Ruling 3 property, mandated by the slice brief:
 *    FF_INTENT_RELEASE_LOCK and FF_INTENT_TAKEOVER_DISMISS are DISTINCT
 *    AND NEVER FOLDED — each is exercised with both a pending takeover
 *    and an established lock present, and each must touch only its own
 *    fact;
 *  - dispatch while a takeover is visible goes to the visible face
 *    (routing rule 4) — AC3b proper is slice c3's (the draft half needs
 *    shell-owned T9 state), but the routing half is pinned here so c3
 *    extends rather than retrofits;
 *  - the payload-ownership contract ("not owned; copied"): the intent
 *    struct and its pointer payloads are clobbered immediately after
 *    dispatch and nothing downstream may have kept them.
 *
 * THE PROXY, stated up front (docs/review/code-review.md, item 6): the
 * Ruling 3 test's proxy is "the other fact still reads true after the
 * call". A dispatch that silently no-ops BOTH intents satisfies that
 * proxy while violating the property ("distinct" includes "each one
 * works"). So every never-folded assertion here is paired with the
 * positive assertion that the intended fact DID change — lock actually
 * released, takeover actually dismissed — on the same shell, in the
 * same test.
 *
 * No transport, no socket, no LVGL: inbound events are injected through
 * `ff_shell_events()` and intents through `ff_shell_intent()` /
 * `ff_intent_emit()` — the exact seams the real target uses.
 */
#include <string.h>

#include "unity.h"

#include "ff_intent.h"
#include "ff_shell.h"

#include "ff_crew.h"
#include "ff_proto.h"

/* ------------------------------------------------------------------- */
/* harness — a slimmed copy of test_shell.c's (fake clock, no transport) */
/* ------------------------------------------------------------------- */

typedef struct {
    uint32_t t;
} fake_clock_t;

static uint32_t fake_now(void *user)
{
    return ((fake_clock_t *)user)->t;
}

typedef struct {
    fake_clock_t clk;
    ff_clock_t clock;
    fp_pack_t pack;
    ff_shell_t shell;
    mc_events_t ev;
} harness_t;

static harness_t H;

#define MY_ID 0x00001000u
#define DANA 0x0000DA1Au
#define KEV_ID 0x0000CEE0u
#define STRANGER 0x0000AAAAu

static void harness_init(uint32_t t0_ms)
{
    memset(&H, 0, sizeof(H));
    H.clk.t = t0_ms;
    H.clock.now_ms = fake_now;
    H.clock.user = &H.clk;

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &H.clock;
    cfg.pack = &H.pack;
    /* store/haptic NULL, transport zeroed: the documented no-transport,
     * no-persistence, no-buzz test bring-up. */

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&H.shell, &cfg));
    H.ev = ff_shell_events(&H.shell);
    H.ev.on_my_info(H.ev.user, MY_ID);
}

/** Give an existing roster member a display name via the same NodeInfo
 *  path the radio uses. `last_heard = 0` is mc_client.h's "unknown", so
 *  this never touches the wall-clock latch — these tests are about
 *  routing, not time. */
static void name_node(uint32_t node, char const *name)
{
    mc_nodeinfo_t n;
    memset(&n, 0, sizeof(n));
    n.node_num = node;
    n.has_short_name = true;
    strncpy(n.short_name, name, sizeof(n.short_name) - 1);
    H.ev.on_node(H.ev.user, &n);
}

static void pair_named(uint32_t node, char const *name)
{
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, node, true));
    name_node(node, name);
}

static void inject_flare(uint32_t from, uint16_t dur_s)
{
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_flare(buf, sizeof(buf), dur_s);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, from, FF_PORTNUM, buf, (size_t)n);
}

/* --- intent shorthands ---------------------------------------------- */

static void send_kind(ff_intent_kind_t kind)
{
    ff_intent_t in = {.kind = kind, .u = {0}};
    ff_shell_intent(&H.shell, &in);
}

static void send_swipe(int8_t dir)
{
    ff_intent_t in = {.kind = FF_INTENT_SWIPE, .u = {0}};
    in.u.swipe_dir = dir;
    ff_shell_intent(&H.shell, &in);
}

static void send_open_compose(uint32_t node_id)
{
    ff_intent_t in = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    in.u.node_id = node_id;
    ff_shell_intent(&H.shell, &in);
}

/** Tick once (clock unmoved) and return the freshly-projected view. */
static ff_app_state_t const *view(void)
{
    (void)ff_shell_tick(&H.shell, H.clk.t);
    return ff_shell_view(&H.shell);
}

/** The composer destination FACT, not its name projection. Every
 *  destination assertion below checks this alongside (or instead of)
 *  `compose.to_name`: the name is a lossy proxy — "" is broadcast AND
 *  any nameless/unknown node — and PR #54's review found a surviving
 *  mutant (`shell_compose_dest` minus its trust guard) living exactly
 *  in that overlap: it stored a stranger's id while every name-based
 *  assertion kept passing. */
static uint32_t compose_to(void)
{
    return ff_shell_compose_to_node(&H.shell);
}

void setUp(void)
{
    /* The emit sink is process-global by design (ff_intent.h); unbind so
     * no test inherits another test's binding. */
    ff_intent_emit_bind(NULL, NULL);
}

void tearDown(void) {}

/* =================================================================== */
/* SWIPE — through the seam, the route's own rules hold                 */
/* =================================================================== */

static void S16_c1_swipe_dispatch_moves_base_and_stays_bounded(void)
{
    harness_init(100000u);

    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face); /* ff_route_init's opening face */

    send_swipe(-1); /* off the RADAR end: bounded, not wrapping (AC1 at the seam) */
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);

    send_swipe(+1);
    TEST_ASSERT_EQUAL(FF_APP_FACE_NOW, view()->active_face);
    send_swipe(+1);
    TEST_ASSERT_EQUAL(FF_APP_FACE_SIGNALS, view()->active_face);
    send_swipe(+1); /* off the SIGNALS end */
    TEST_ASSERT_EQUAL(FF_APP_FACE_SIGNALS, view()->active_face);

    send_swipe(-1);
    TEST_ASSERT_EQUAL(FF_APP_FACE_NOW, view()->active_face);
}

/* =================================================================== */
/* OPEN_COMPOSE / BACK — the modal round trip                           */
/* =================================================================== */

static void S16_c1_open_compose_and_back_round_trip(void)
{
    harness_init(100000u);
    send_swipe(+1);
    send_swipe(+1); /* Signals — where the real "+" lives */
    TEST_ASSERT_EQUAL(FF_APP_FACE_SIGNALS, view()->active_face);

    send_open_compose(0u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);

    /* Any modal suppresses swipe (AC2 at the seam): a horizontal drag
     * must never slide the composer away. */
    send_swipe(-1);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);

    /* A second OPEN_COMPOSE over the modal is rejected, not a replace —
     * one modal slot, never silently swapped. */
    send_open_compose(0u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);

    send_kind(FF_INTENT_BACK);
    TEST_ASSERT_EQUAL(FF_APP_FACE_SIGNALS, view()->active_face); /* base intact underneath */

    /* BACK on a bare face is a no-op, not a face change. */
    send_kind(FF_INTENT_BACK);
    TEST_ASSERT_EQUAL(FF_APP_FACE_SIGNALS, view()->active_face);
}

/* =================================================================== */
/* OPEN_COMPOSE destination — S08's "TO = selected crew member" rule    */
/* =================================================================== */

static void S16_c1_open_compose_with_no_crew_is_broadcast(void)
{
    harness_init(100000u);
    send_open_compose(0u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);
    TEST_ASSERT_EQUAL_UINT32(0u, compose_to());
    TEST_ASSERT_EQUAL_STRING("", view()->compose.to_name); /* scr_compose renders "TO: EVERYONE" */
}

static void S16_c1_open_compose_defaults_to_the_selected_crew_member(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA"); /* first paired member == the self-healing selection */
    pair_named(KEV_ID, "KEV");

    send_open_compose(0u); /* the Signals "+" shape: no explicit destination */
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);
    TEST_ASSERT_EQUAL_UINT32(DANA, compose_to());
    TEST_ASSERT_EQUAL_STRING("DANA", view()->compose.to_name);
}

static void S16_c1_open_compose_honors_an_explicit_paired_destination(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA"); /* the selection — must NOT win over an explicit id */
    pair_named(KEV_ID, "KEV");

    send_open_compose(KEV_ID);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, compose_to());
    TEST_ASSERT_EQUAL_STRING("KEV", view()->compose.to_name);
}

static void S16_c1_open_compose_never_retargets_an_unhonorable_destination(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");

    /* An explicit id the trust policy won't message degrades to
     * BROADCAST — never to the selected member: a message must not be
     * silently retargeted at somebody the caller did not name.
     *
     * Asserted on the FACT (`ff_shell_compose_to_node`), not only the
     * name: PR #54's review showed the name-only version green over a
     * `shell_compose_dest` with its trust guard deleted, because a
     * stranger's stored id still projects to_name == "" — the proxy is
     * lossy at exactly the values under test. The fact assertion is
     * what kills that mutant: guard deleted, this reads STRANGER. */
    send_open_compose(STRANGER); /* never heard of them */
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);
    TEST_ASSERT_EQUAL_UINT32(0u, compose_to());
    TEST_ASSERT_EQUAL_STRING("", view()->compose.to_name);
    send_kind(FF_INTENT_BACK);

    /* Known but unpaired: same rule (the roster can hold unpaired
     * members; pairing is the messaging trust gate). KEV gets a NAME
     * first — reviewer's second half of the same finding: a nameless
     * unpaired member makes even the name assertion vacuous, while a
     * named one turns it into a second, independent mutant-killer
     * (guard deleted -> to_name reads "KEV", not ""). */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, false));
    name_node(KEV_ID, "KEV");
    send_open_compose(KEV_ID);
    TEST_ASSERT_EQUAL_UINT32(0u, compose_to());
    TEST_ASSERT_EQUAL_STRING("", view()->compose.to_name);
}

static void S16_c1_a_rejected_open_compose_does_not_retarget_the_composer(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA"); /* first paired member == the selection */
    pair_named(KEV_ID, "KEV");

    /* Open explicitly to KEV — deliberately NOT the selection (DANA), so
     * both mutant shapes below produce an observable wrong value. */
    send_open_compose(KEV_ID);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, compose_to());

    /* One modal slot: a second OPEN_COMPOSE while the composer is up is
     * rejected by push_modal — and a REJECTED open must not touch the
     * destination either. PR #54 review, second surviving mutant:
     * hoisting the destination write out of the push_modal guard passed
     * every then-existing test while silently retargeting a half-typed
     * draft at whoever the second request named. */
    send_open_compose(DANA); /* explicit: hoisted write would store DANA */
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, compose_to());
    TEST_ASSERT_EQUAL_STRING("KEV", view()->compose.to_name);

    send_open_compose(0u); /* the "+" shape: hoisted write would resolve the selection (DANA) */
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, compose_to());
    TEST_ASSERT_EQUAL_STRING("KEV", view()->compose.to_name);
}

static void S16_c1_back_clears_the_compose_destination(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");

    send_open_compose(DANA);
    TEST_ASSERT_EQUAL_UINT32(DANA, compose_to());
    TEST_ASSERT_EQUAL_STRING("DANA", view()->compose.to_name);
    send_kind(FF_INTENT_BACK);

    /* A later compose session must not inherit the abandoned "who". */
    TEST_ASSERT_EQUAL_UINT32(0u, compose_to());
    TEST_ASSERT_EQUAL_STRING("", view()->compose.to_name);
}

/* =================================================================== */
/* OPEN_SETTINGS — the judgment call: rejected until S11b's renderer    */
/* =================================================================== */

static void S16_c1_open_settings_is_rejected_until_a_renderer_exists(void)
{
    harness_init(100000u);

    send_kind(FF_INTENT_OPEN_SETTINGS);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face); /* no modal pushed */

    /* And crucially NOT wedged: no invisible modal is suppressing swipe
     * (the dead-end this judgment call exists to avoid — see
     * ff_shell.c's k_settings_renderer_exists comment). */
    send_swipe(+1);
    TEST_ASSERT_EQUAL(FF_APP_FACE_NOW, view()->active_face);
}

/* =================================================================== */
/* Routing rule 4 — dispatch targets the VISIBLE face                   */
/* =================================================================== */

static void S16_c1_route_intents_are_rejected_while_a_takeover_is_visible(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");
    pair_named(KEV_ID, "KEV");

    send_open_compose(KEV_ID); /* composing to KEV... */
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);

    inject_flare(DANA, 300u); /* ...when DANA flares: takeover overrides */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    /* AC13 holds even now: active_face never reads FLARE — the takeover
     * reaches the renderer as flare.takeover_active. */
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);
    TEST_ASSERT_TRUE(view()->flare.takeover_active);

    /* Compose receives no intents at all while the takeover is up: a tap
     * landing where "<" was does not close the composer, a swipe does
     * not move the base, and nothing opens over it. */
    send_kind(FF_INTENT_BACK);
    send_swipe(-1);
    send_open_compose(0u);
    send_kind(FF_INTENT_OPEN_SETTINGS);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active); /* nothing consumed it */

    /* Clearing the takeover restores the EXACT prior route: Compose,
     * still addressed to KEV (AC3's shape; the typed-draft half is
     * slice c3's, once the T9 state moves into the shell). */
    send_kind(FF_INTENT_TAKEOVER_DISMISS);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, compose_to());
    TEST_ASSERT_EQUAL_STRING("KEV", view()->compose.to_name);

    /* And dispatch to Compose is restored with it. */
    send_kind(FF_INTENT_BACK);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);
}

static void S16_c1_takeover_decisions_require_a_visible_takeover(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");

    /* With no takeover on screen, GO and DISMISS are not dispatchable —
     * there is no decision to make. */
    send_kind(FF_INTENT_TAKEOVER_GO);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_shell_flare(&H.shell)->locked_node_id);
    send_kind(FF_INTENT_TAKEOVER_DISMISS);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);

    /* Positive control: with one visible, GO consumes it into the lock. */
    inject_flare(DANA, 300u);
    send_kind(FF_INTENT_TAKEOVER_GO);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_flare(&H.shell)->locked_node_id);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
}

/* =================================================================== */
/* S10 Ruling 3 — RELEASE_LOCK and TAKEOVER_DISMISS, never folded       */
/* =================================================================== */

static void S16_c1_release_lock_and_takeover_dismiss_are_never_folded(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");
    pair_named(KEV_ID, "KEV");

    /* Establish the ruling's exact race shape: locked onto DANA (via an
     * earlier GO), then KEV's flare arrives and its takeover is showing. */
    inject_flare(DANA, 300u);
    send_kind(FF_INTENT_TAKEOVER_GO);
    inject_flare(KEV_ID, 300u);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_flare(&H.shell)->locked_node_id);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_flare(&H.shell)->takeover_node_id);

    /* "Stop navigating", tapped as KEV's takeover lands. RELEASE_LOCK
     * must do exactly what the user meant — release the lock — and must
     * NOT be routed into the dismiss branch: KEV's takeover stays
     * pending and shown, nothing is swallowed unseen. Both halves
     * asserted: the lock DID clear (positive), the takeover did NOT
     * (never-folded). */
    send_kind(FF_INTENT_RELEASE_LOCK);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_shell_flare(&H.shell)->locked_node_id);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_flare(&H.shell)->takeover_node_id);

    /* The mirror image: locked onto KEV, DANA's takeover showing —
     * DISMISS must clear only the takeover and leave the lock intact. */
    send_kind(FF_INTENT_TAKEOVER_GO); /* lock KEV */
    inject_flare(DANA, 300u);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_flare(&H.shell)->locked_node_id);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    send_kind(FF_INTENT_TAKEOVER_DISMISS);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active); /* positive half */
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_flare(&H.shell)->locked_node_id); /* never-folded half */
}

/* =================================================================== */
/* S16 slice c2 — FLARE_START / FLARE_END                               */
/* =================================================================== */

static void S16_c2_flare_start_begins_sending(void)
{
    harness_init(100000u);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending);

    send_kind(FF_INTENT_FLARE_START);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending);
}

static void S16_c2_flare_start_is_rejected_while_a_takeover_is_visible(void)
{
    /* Routing rule 4: the FLARE button lives on the Radar tile, which is
     * not the visible face while a takeover is up — same principle
     * S16_c1_route_intents_are_rejected_while_a_takeover_is_visible pins
     * for Compose. */
    harness_init(100000u);
    pair_named(DANA, "DANA");

    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    send_kind(FF_INTENT_FLARE_START);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending);

    /* Positive control: with the takeover cleared, the identical intent
     * does begin sending — so the rejection above is the routing gate,
     * not a FLARE_START path that never works. */
    send_kind(FF_INTENT_TAKEOVER_DISMISS);
    send_kind(FF_INTENT_FLARE_START);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending);
}

static void S16_c2_flare_end_cancels_a_send_even_while_a_takeover_is_visible(void)
{
    /* FLARE_END (the sender overlay's CANCEL) is deliberately UNGATED:
     * `sending` and `takeover_active` are independent facts by
     * ff_flare_h's own "Independent state" design (I can be sending my
     * own flare AND have a different crew member's takeover pending at
     * once), and the sender overlay renders on the puck itself regardless
     * of face or takeover — same un-gated precedent as RELEASE_LOCK. */
    harness_init(100000u);
    pair_named(DANA, "DANA");

    send_kind(FF_INTENT_FLARE_START);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending);

    inject_flare(DANA, 300u); /* someone else's takeover arrives mid-send */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    send_kind(FF_INTENT_FLARE_END);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending); /* positive half */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active); /* untouched: FLARE_END touches only sending */
}

/* =================================================================== */
/* S16 slice e — FF_INTENT_SETTING_SET (AC8)                            */
/* =================================================================== */

/* A separate, self-contained store spy rather than reusing the global H
 * harness: these tests need to COUNT writes across repeated dispatches,
 * and a dedicated ff_shell_t keeps that bookkeeping local to this
 * section instead of threading a spy through every other test above. */
typedef struct {
    uint8_t buf[512];
    size_t len;
    bool present;
    int set_calls; /* AC8's "on change, never every tick" proxy-killer */
} setting_store_t;

static int setting_store_get(void *io, char const *key, void *buf, size_t n)
{
    setting_store_t *st = (setting_store_t *)io;
    (void)key;
    if (!st->present || st->len > n) return -1;
    memcpy(buf, st->buf, st->len);
    return (int)st->len;
}

static int setting_store_set(void *io, char const *key, void const *buf, size_t n)
{
    setting_store_t *st = (setting_store_t *)io;
    (void)key;
    if (n > sizeof(st->buf)) return -1;
    memcpy(st->buf, buf, n);
    st->len = n;
    st->present = true;
    st->set_calls++;
    return (int)n;
}

static ff_store_t setting_store(setting_store_t *st)
{
    ff_store_t s;
    s.get = setting_store_get;
    s.set = setting_store_set;
    s.io = st;
    return s;
}

typedef struct {
    fake_clock_t clk;
    ff_clock_t clock;
    fp_pack_t pack;
    setting_store_t store_mem;
    ff_store_t store;
    ff_shell_t shell;
} setting_harness_t;

static void setting_harness_init(setting_harness_t *sh)
{
    memset(sh, 0, sizeof(*sh));
    sh->clk.t = 100000u;
    sh->clock.now_ms = fake_now;
    sh->clock.user = &sh->clk;
    sh->store = setting_store(&sh->store_mem);

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &sh->clock;
    cfg.store = &sh->store;
    cfg.pack = &sh->pack;

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&sh->shell, &cfg));
}

static void setting_send(ff_shell_t *shell, ff_setting_id_t id, int32_t i, char const *s)
{
    ff_intent_t in = {.kind = FF_INTENT_SETTING_SET, .u = {0}};
    in.u.setting.id = id;
    if (s != NULL) {
        in.u.setting.v.s = s;
    } else {
        in.u.setting.v.i = i;
    }
    ff_shell_intent(shell, &in);
}

static void S16_AC8_setting_set_applies_and_persists_only_on_change(void)
{
    setting_harness_t h;
    setting_harness_init(&h);

    TEST_ASSERT_TRUE(ff_shell_settings(&h.shell)->imperial); /* default */
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls);

    setting_send(&h.shell, FF_SETTING_IMPERIAL, 0, NULL); /* false: a real change */
    TEST_ASSERT_FALSE(ff_shell_settings(&h.shell)->imperial);
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls);

    /* Re-sending the SAME value must not write again — "saved on change,
     * never every tick" (S16 Behavior). Ticking the shell repeatedly in
     * between rules out a per-tick save entirely, not just a per-dispatch
     * one. */
    for (int i = 0; i < 50; i++) {
        (void)ff_shell_tick(&h.shell, h.clk.t + (uint32_t)i);
    }
    setting_send(&h.shell, FF_SETTING_IMPERIAL, 0, NULL); /* same value again */
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls);      /* unchanged: no new write */

    setting_send(&h.shell, FF_SETTING_IMPERIAL, 1, NULL); /* back to true: a change again */
    TEST_ASSERT_TRUE(ff_shell_settings(&h.shell)->imperial);
    TEST_ASSERT_EQUAL_INT(2, h.store_mem.set_calls);

    ff_shell_close(&h.shell);
}

static void S16_AC8_setting_set_out_of_range_is_rejected_not_clamped(void)
{
    setting_harness_t h;
    setting_harness_init(&h);

    uint8_t const share_before = ff_shell_settings(&h.shell)->share_mode;
    setting_send(&h.shell, FF_SETTING_SHARE_MODE, 3, NULL); /* FF_SHARE_GHOST is 2 — 3 is out of range */
    TEST_ASSERT_EQUAL_UINT8(share_before, ff_shell_settings(&h.shell)->share_mode);
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls); /* rejected outright: no write either */

    setting_send(&h.shell, FF_SETTING_SHARE_MODE, -1, NULL);
    TEST_ASSERT_EQUAL_UINT8(share_before, ff_shell_settings(&h.shell)->share_mode);

    setting_send(&h.shell, FF_SETTING_QUIET_FROM_MIN, 1440, NULL); /* one past [0,1439] */
    TEST_ASSERT_EQUAL_UINT16(240u, ff_shell_settings(&h.shell)->quiet_from_min); /* default, untouched */

    setting_send(&h.shell, FF_SETTING_QUIET_TO_MIN, -1, NULL);
    TEST_ASSERT_EQUAL_UINT16(600u, ff_shell_settings(&h.shell)->quiet_to_min); /* default, untouched */

    bool const offset_set_before = ff_shell_settings(&h.shell)->utc_offset_set;
    setting_send(&h.shell, FF_SETTING_UTC_OFFSET_MIN, FF_WALL_OFFSET_MIN_LO - 1, NULL);
    TEST_ASSERT_EQUAL(offset_set_before, ff_shell_settings(&h.shell)->utc_offset_set);
    setting_send(&h.shell, FF_SETTING_UTC_OFFSET_MIN, FF_WALL_OFFSET_MIN_HI + 1, NULL);
    TEST_ASSERT_EQUAL(offset_set_before, ff_shell_settings(&h.shell)->utc_offset_set);

    /* Positive control: the same fields accept an in-range value —
     * including each boundary itself — so the rejections above are the
     * validation gate, not a broken setter. */
    setting_send(&h.shell, FF_SETTING_SHARE_MODE, FF_SHARE_GHOST, NULL);
    TEST_ASSERT_EQUAL_UINT8(FF_SHARE_GHOST, ff_shell_settings(&h.shell)->share_mode);

    setting_send(&h.shell, FF_SETTING_UTC_OFFSET_MIN, FF_WALL_OFFSET_MIN_LO, NULL);
    TEST_ASSERT_TRUE(ff_shell_settings(&h.shell)->utc_offset_set);
    TEST_ASSERT_EQUAL_INT16(FF_WALL_OFFSET_MIN_LO, ff_shell_settings(&h.shell)->utc_offset_min);
    setting_send(&h.shell, FF_SETTING_UTC_OFFSET_MIN, FF_WALL_OFFSET_MIN_HI, NULL);
    TEST_ASSERT_EQUAL_INT16(FF_WALL_OFFSET_MIN_HI, ff_shell_settings(&h.shell)->utc_offset_min);

    ff_shell_close(&h.shell);
}

static void S16_AC8_setting_set_my_name_is_bounded_and_terminated(void)
{
    setting_harness_t h;
    setting_harness_init(&h);

    setting_send(&h.shell, FF_SETTING_MY_NAME, 0, "this name is definitely longer than fifteen characters");
    /* FF_SETTINGS_NAME_LEN is 16, including the NUL — 15 chars survive. */
    TEST_ASSERT_EQUAL_INT(15, (int)strlen(ff_shell_settings(&h.shell)->my_name));
    TEST_ASSERT_EQUAL_STRING("this name is de", ff_shell_settings(&h.shell)->my_name);

    setting_send(&h.shell, FF_SETTING_MY_NAME, 0, NULL); /* NULL payload: a no-op, not a crash or a blank name */
    TEST_ASSERT_EQUAL_STRING("this name is de", ff_shell_settings(&h.shell)->my_name);

    ff_shell_close(&h.shell);
}

static void S16_AC8_setting_set_is_rejected_while_a_takeover_is_visible(void)
{
    setting_harness_t h;
    setting_harness_init(&h);
    TEST_ASSERT_TRUE(ff_shell_pair(&h.shell, DANA, true));

    mc_events_t const ev = ff_shell_events(&h.shell);
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_flare(buf, sizeof(buf), 300u);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    ev.on_private(ev.user, DANA, FF_PORTNUM, buf, (size_t)n);
    TEST_ASSERT_TRUE(ff_shell_flare(&h.shell)->takeover_active);

    bool const before = ff_shell_settings(&h.shell)->imperial;
    setting_send(&h.shell, FF_SETTING_IMPERIAL, before ? 0 : 1, NULL);
    TEST_ASSERT_EQUAL(before, ff_shell_settings(&h.shell)->imperial); /* rejected: routing rule 4 */
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls);

    ff_shell_close(&h.shell);
}

/* =================================================================== */
/* Payload ownership — NOT owned; copied                                */
/* =================================================================== */

static void S16_c1_intent_struct_and_pointer_payloads_are_borrowed_only(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");

    /* The struct itself: clobbered the instant dispatch returns. */
    ff_intent_t in = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    in.u.node_id = DANA;
    ff_shell_intent(&H.shell, &in);
    memset(&in, 0xA5, sizeof(in));
    TEST_ASSERT_EQUAL_STRING("DANA", view()->compose.to_name);

    /* The pointer payloads. T9_INSERT went from a no-op (c1/c2) to a real
     * mutation of the shell-owned draft (S16 slice c3) — the ownership
     * contract ("not owned; copied" — ff_intent.h) is proven MORE
     * strongly now than the old "nothing changed" shape could: clobber
     * the source buffer the INSTANT dispatch returns, and the projection
     * still shows the right text, which is only possible if the shell
     * copied every byte it needed before returning. */
    char text_buf[16];
    strcpy(text_buf, ":)");
    ff_intent_t t9 = {.kind = FF_INTENT_T9_INSERT, .u = {0}};
    t9.u.text = text_buf;
    ff_shell_intent(&H.shell, &t9);
    memset(text_buf, 0xA5, sizeof(text_buf));
    memset(&t9, 0xA5, sizeof(t9));
    TEST_ASSERT_EQUAL_STRING(":)", view()->compose.text);

    /* SETTING_SET (S16 slice e): the same ownership contract, now proven
     * with a REAL mutation the same way T9_INSERT's half just did above
     * — clobber the source buffer the instant dispatch returns, and the
     * projection must still show the applied name, which is only
     * possible if the shell copied every byte it needed before
     * returning. */
    char name_buf[16];
    strcpy(name_buf, "JAKE");
    ff_intent_t set = {.kind = FF_INTENT_SETTING_SET, .u = {0}};
    set.u.setting.id = FF_SETTING_MY_NAME;
    set.u.setting.v.s = name_buf;
    ff_shell_intent(&H.shell, &set);
    memset(name_buf, 0xA5, sizeof(name_buf));
    memset(&set, 0xA5, sizeof(set));

    TEST_ASSERT_EQUAL_STRING("JAKE", view()->settings.my_name);
}

/* =================================================================== */
/* The emit seam — screens' side of the contract                        */
/* =================================================================== */

typedef struct {
    int count;
    ff_intent_t last; /* copied, per the sink contract */
} spy_sink_t;

static void spy_sink(void *user, ff_intent_t const *in)
{
    spy_sink_t *s = (spy_sink_t *)user;
    s->count++;
    s->last = *in;
}

static void S16_c1_emit_seam_forwards_when_bound_and_noops_unbound(void)
{
    harness_init(100000u);

    ff_intent_t in = {.kind = FF_INTENT_SWIPE, .u = {0}};
    in.u.swipe_dir = +1;

    /* Unbound (the goldens/headless state): a safe no-op. */
    ff_intent_emit(&in);

    spy_sink_t spy;
    memset(&spy, 0, sizeof(spy));
    ff_intent_emit_bind(spy_sink, &spy);
    ff_intent_emit(&in);
    TEST_ASSERT_EQUAL_INT(1, spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SWIPE, spy.last.kind);
    TEST_ASSERT_EQUAL_INT8(+1, spy.last.u.swipe_dir);

    /* The real production binding: ff_shell_intent_sink adapts the seam
     * onto a shell without a function-pointer cast. */
    ff_intent_emit_bind(ff_shell_intent_sink, &H.shell);
    ff_intent_emit(&in);
    TEST_ASSERT_EQUAL(FF_APP_FACE_NOW, view()->active_face);

    /* Unbind drops the sink AND its user pointer. */
    ff_intent_emit_bind(NULL, &spy); /* user with a NULL fn is ignored */
    ff_intent_emit(&in);
    TEST_ASSERT_EQUAL_INT(1, spy.count); /* the spy did not come back */
    TEST_ASSERT_EQUAL(FF_APP_FACE_NOW, view()->active_face); /* nor did the shell move */
}

/* =================================================================== */
/* NULL / garbage safety                                                */
/* =================================================================== */

static void S16_c1_null_and_garbage_dispatch_is_safe(void)
{
    harness_init(100000u);

    ff_intent_t in = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_shell_intent(NULL, &in);
    ff_shell_intent(&H.shell, NULL);
    ff_shell_intent_sink(NULL, &in);
    ff_intent_emit(NULL);

    /* An out-of-enum kind (a corrupted event) falls off the dispatch
     * switch as a no-op — never routes, never crashes. */
    in.kind = (ff_intent_kind_t)0x7FFF;
    ff_shell_intent(&H.shell, &in);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S16_c1_swipe_dispatch_moves_base_and_stays_bounded);
    RUN_TEST(S16_c1_open_compose_and_back_round_trip);
    RUN_TEST(S16_c1_open_compose_with_no_crew_is_broadcast);
    RUN_TEST(S16_c1_open_compose_defaults_to_the_selected_crew_member);
    RUN_TEST(S16_c1_open_compose_honors_an_explicit_paired_destination);
    RUN_TEST(S16_c1_open_compose_never_retargets_an_unhonorable_destination);
    RUN_TEST(S16_c1_a_rejected_open_compose_does_not_retarget_the_composer);
    RUN_TEST(S16_c1_back_clears_the_compose_destination);
    RUN_TEST(S16_c1_open_settings_is_rejected_until_a_renderer_exists);
    RUN_TEST(S16_c1_route_intents_are_rejected_while_a_takeover_is_visible);
    RUN_TEST(S16_c1_takeover_decisions_require_a_visible_takeover);
    RUN_TEST(S16_c1_release_lock_and_takeover_dismiss_are_never_folded);
    RUN_TEST(S16_c2_flare_start_begins_sending);
    RUN_TEST(S16_c2_flare_start_is_rejected_while_a_takeover_is_visible);
    RUN_TEST(S16_c2_flare_end_cancels_a_send_even_while_a_takeover_is_visible);
    RUN_TEST(S16_AC8_setting_set_applies_and_persists_only_on_change);
    RUN_TEST(S16_AC8_setting_set_out_of_range_is_rejected_not_clamped);
    RUN_TEST(S16_AC8_setting_set_my_name_is_bounded_and_terminated);
    RUN_TEST(S16_AC8_setting_set_is_rejected_while_a_takeover_is_visible);
    RUN_TEST(S16_c1_intent_struct_and_pointer_payloads_are_borrowed_only);
    RUN_TEST(S16_c1_emit_seam_forwards_when_bound_and_noops_unbound);
    RUN_TEST(S16_c1_null_and_garbage_dispatch_is_safe);

    return UNITY_END();
}
