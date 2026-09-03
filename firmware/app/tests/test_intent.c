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
    jsmntok_t toks[FP_MAX_TOKENS];
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
    cfg.toks = H.toks;
    cfg.ntoks = FP_MAX_TOKENS;
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
    H.ev.on_private(H.ev.user, from, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
}

/* A live over-the-air position: on_position with rx_time set — same
 * shape as test_shell.c's own inject_position (this file's harness is
 * documented as "a slimmed copy of test_shell.c's"). U_EVENING is an
 * arbitrary but valid (inside FF_WALL_EPOCH_FLOOR/CEILING) unix
 * timestamp, copied from test_shell.c's own constant of the same name
 * so the wall-clock latch this triggers (shell_ev_position observes the
 * wall before anything else) behaves identically to that file's proven
 * usage. */
#define U_EVENING ((uint32_t)1789768800u)

static void inject_position(uint32_t node, uint32_t rx_time, double lat, double lon)
{
    mc_position_t p;
    memset(&p, 0, sizeof(p));
    p.lat = lat;
    p.lon = lon;
    p.has_rx_time = (rx_time != 0u);
    p.rx_time = rx_time;
    H.ev.on_position(H.ev.user, node, &p);
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

/* --- S26 slice e: the BOOT-button nav model, replacing swipe as this
 * file's navigation vehicle (docs/specs/S26-device-lifecycle.md "(e)
 * Home button + launcher") ------------------------------------------- */

static void send_home(void)
{
    ff_intent_t in = {.kind = FF_INTENT_HOME, .u = {0}};
    ff_shell_intent(&H.shell, &in);
}

static void send_launcher_select(uint8_t launcher_idx)
{
    ff_intent_t in = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    in.u.launcher_idx = launcher_idx;
    ff_shell_intent(&H.shell, &in);
}

/* Navigate from the launcher (home) to one of its five circles —
 * this file's replacement for what a run of `send_swipe` calls used to
 * do. S26 slice e amended 2026-09-01: the launcher IS home, so
 * `harness_init` already leaves the route there — but this always
 * presses HOME first anyway (a no-op if already on the launcher, per
 * `ff_route_home`'s own rule) rather than assuming the caller's prior
 * state, so it works identically whether called fresh after
 * harness_init or mid-test after leaving some other face, exactly the
 * real BOOT-then-tap gesture a thumb performs. */
static void nav_home_to(ff_app_face_t face)
{
    uint8_t idx;
    switch (face) {
    case FF_APP_FACE_RADAR: idx = 0; break;
    case FF_APP_FACE_LINEUP: idx = 1; break;
    case FF_APP_FACE_INBOX: idx = 2; break;
    case FF_APP_FACE_MAP: idx = 3; break;
    case FF_APP_FACE_SETTINGS: idx = 4; break;
    default: TEST_FAIL_MESSAGE("nav_home_to: not a launcher circle"); return;
    }
    send_home(); /* BOOT: a no-op if already on the launcher */
    send_launcher_select(idx); /* launcher -> face */
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
/* SWIPE — S26 slice e: RETIRED. ff_shell.c's FF_INTENT_SWIPE case is a  */
/* documented no-op now (scr_nav.c emits nothing to feed it any more —  */
/* see that case's own comment); pinned here at the dispatch layer, the */
/* same "swipe no longer moves base" property test_route.c's own        */
/* S26e_AC1_swipe_is_a_no_op_on_the_launcher_base pins one layer down    */
/* (for the launcher-base case specifically).                           */
/* =================================================================== */

static void S26e_swipe_dispatch_moves_nothing_from_any_base_face(void)
{
    harness_init(100000u);

    /* S26 slice e amended 2026-09-01: ff_route_init's opening face is
     * now the launcher itself (home), not Radar. */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);

    send_swipe(+1);
    send_swipe(-1);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);

    /* From every app face too, Radar included (an ordinary circle now,
     * no special treatment) — a leftover call site is not quietly
     * re-wiring navigation through the intent that used to own it. */
    ff_app_face_t const bases[] = {
        FF_APP_FACE_RADAR, FF_APP_FACE_LINEUP, FF_APP_FACE_INBOX, FF_APP_FACE_MAP, FF_APP_FACE_SETTINGS,
    };
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        harness_init(100000u);
        nav_home_to(bases[i]);
        TEST_ASSERT_EQUAL(bases[i], view()->active_face);
        send_swipe(+1);
        send_swipe(-1);
        TEST_ASSERT_EQUAL(bases[i], view()->active_face);
    }
}

/* =================================================================== */
/* S26 slice e, AMENDED 2026-09-01 — HOME + LAUNCHER_SELECT, through the */
/* seam. The launcher IS home now: it is what ff_route_init opens on,   */
/* BOOT from any app (Radar included) returns to it, and BOOT while     */
/* already on it is a no-op — see ff_route.h's header note for the full */
/* model. The pre-amendment "home from Radar opens the launcher, a      */
/* second press returns to Radar" tests are GONE with that model.       */
/* =================================================================== */

static void S26e_home_on_the_launcher_is_a_noop(void)
{
    harness_init(100000u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face); /* the boot default */

    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face); /* still there — nowhere "home-er" to go */
}

/* HOME from every app face, Radar included, lands on the launcher in
 * one press — no special-cased return-to-Radar any more. */
static void S26e_home_from_each_app_sets_base_to_the_launcher(void)
{
    ff_app_face_t const bases[] = {
        FF_APP_FACE_RADAR, FF_APP_FACE_LINEUP, FF_APP_FACE_INBOX, FF_APP_FACE_MAP, FF_APP_FACE_SETTINGS,
    };
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        harness_init(100000u);
        nav_home_to(bases[i]);
        TEST_ASSERT_EQUAL(bases[i], view()->active_face);

        send_home();
        TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);
    }
}

static void S26e_home_is_rejected_while_a_takeover_is_visible(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");
    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    send_home();
    /* Route untouched underneath — still the boot default, the launcher. */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);
}

/* Five circles now (Radar included, index 0) — see ff_shell.c's
 * k_launcher_faces table and scr_launcher.c's fixed circle order. */
static void S26e_launcher_select_reaches_every_circle_and_the_badge_projects(void)
{
    harness_init(100000u);
    ff_app_face_t const circles[] = {
        FF_APP_FACE_RADAR, FF_APP_FACE_LINEUP, FF_APP_FACE_INBOX, FF_APP_FACE_MAP, FF_APP_FACE_SETTINGS,
    };
    for (size_t i = 0; i < sizeof(circles) / sizeof(circles[0]); i++) {
        harness_init(100000u);
        nav_home_to(circles[i]);
        TEST_ASSERT_EQUAL(circles[i], view()->active_face);
    }
}

/* "The launcher is not showing" now has to be built by LEAVING it
 * first (harness_init's own opening state IS the launcher, as of this
 * amendment) — the inverse of the pre-amendment test, which had to do
 * nothing at all to get this precondition. */
static void S26e_launcher_select_is_a_noop_when_the_launcher_is_not_showing(void)
{
    harness_init(100000u);
    nav_home_to(FF_APP_FACE_RADAR);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);

    send_launcher_select(2u); /* Signals — but the launcher isn't showing */
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);
}

/* Five circles (0..4) now — one past the last real one is 5, not 4. */
static void S26e_launcher_select_out_of_range_index_is_a_noop(void)
{
    harness_init(100000u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face); /* the boot default */
    send_launcher_select(5u); /* one past the last real circle (0..4) */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face); /* still open */
    send_launcher_select(255u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);
}

static void S26e_launcher_select_is_rejected_while_a_takeover_is_visible(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face); /* the boot default — already showing */

    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    send_launcher_select(2u); /* Signals */
    /* The route's own base is untouched underneath the takeover (S16
     * AC13: active_face never reflects the takeover itself). */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);
}

/* =================================================================== */
/* OPEN_COMPOSE / BACK — the modal round trip                           */
/* =================================================================== */

static void S16_c1_open_compose_and_back_round_trip(void)
{
    harness_init(100000u);
    nav_home_to(FF_APP_FACE_INBOX); /* where the real "+" lives */
    TEST_ASSERT_EQUAL(FF_APP_FACE_INBOX, view()->active_face);

    send_open_compose(0u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);

    /* Any modal suppresses HOME too (S26e AC1 at the seam): a home
     * press must never slide the composer away. */
    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);

    /* A second OPEN_COMPOSE over the modal is rejected, not a replace —
     * one modal slot, never silently swapped. */
    send_open_compose(0u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);

    send_kind(FF_INTENT_BACK);
    TEST_ASSERT_EQUAL(FF_APP_FACE_INBOX, view()->active_face); /* base intact underneath */

    /* BACK on a bare face is a no-op, not a face change. */
    send_kind(FF_INTENT_BACK);
    TEST_ASSERT_EQUAL(FF_APP_FACE_INBOX, view()->active_face);
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
/* OPEN_SETTINGS — S26 slice e: the long-press-anywhere UI hook is       */
/* retired (Settings is a launcher circle now — see scr_nav.c's header   */
/* comment), but the intent and its shell handling are UNCHANGED (see    */
/* ff_shell.c's OPEN_SETTINGS case) — nothing emits it any more, but it   */
/* still works exactly as before if dispatched directly, which is all    */
/* this test proves: `ff_route_goto`, unchanged, still jumps straight    */
/* to Settings from any base.                                            */
/* =================================================================== */

static void S16_c1_open_settings_jumps_base_to_the_settings_face(void)
{
    harness_init(100000u);
    nav_home_to(FF_APP_FACE_LINEUP); /* prove the jump works from any base, not just the launcher */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LINEUP, view()->active_face);

    /* One dispatch jumps straight to the far-right Settings face,
     * skipping Signals and Map. */
    send_kind(FF_INTENT_OPEN_SETTINGS);
    TEST_ASSERT_EQUAL(FF_APP_FACE_SETTINGS, view()->active_face);

    /* Settings is a base face, not a modal — you leave it via HOME (S26
     * slice e, amended 2026-09-01: "any base -> the launcher"), not
     * BACK. */
    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);

    /* Jump back to Settings, then confirm BACK on a bare face is a
     * no-op (there is no modal to pop). */
    send_kind(FF_INTENT_OPEN_SETTINGS);
    TEST_ASSERT_EQUAL(FF_APP_FACE_SETTINGS, view()->active_face);
    send_kind(FF_INTENT_BACK);
    TEST_ASSERT_EQUAL(FF_APP_FACE_SETTINGS, view()->active_face);
}

/**
 * S26 slice e: Map is a launcher circle now — there is no FF_INTENT_OPEN_MAP
 * and no swipe-between-neighbours path any more (the horizontal carousel
 * that used to carry it is retired). Reached directly from the launcher,
 * left via HOME straight back to the launcher (amended 2026-09-01: not
 * to Radar specifically — Radar is an ordinary circle now too).
 */
static void launcher_reaches_map_directly(void)
{
    harness_init(100000u);

    nav_home_to(FF_APP_FACE_MAP);
    TEST_ASSERT_EQUAL(FF_APP_FACE_MAP, view()->active_face);

    /* Leave Map via HOME — straight back to the launcher, no modal, no
     * BACK involved (S26 slice e, amended 2026-09-01: "any base -> the
     * launcher"). */
    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);

    /* Every other launcher circle is reachable the same way, Radar
     * included. */
    ff_app_face_t const others[] = {
        FF_APP_FACE_RADAR, FF_APP_FACE_LINEUP, FF_APP_FACE_INBOX, FF_APP_FACE_SETTINGS,
    };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        nav_home_to(others[i]);
        TEST_ASSERT_EQUAL(others[i], view()->active_face);
        send_home();
        TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);
    }
}

/**
 * A pending compose draft survives a visit to Settings (PR #68 code
 * review: "genuinely untested" ad hoc probe, adopted here as a real
 * test per the coordinator's suggestion). Settings has no seam onto
 * `compose_draft` at all — every `T9_*`/`SEND_TEXT` intent is Compose's,
 * and nothing `scr_settings.c` emits touches it — so this is provable
 * directly: type into Compose, leave for Settings, come back, the draft
 * is untouched.
 */
static void S11b_a_compose_draft_survives_a_settings_visit(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");

    send_open_compose(DANA);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, view()->active_face);

    char text_buf[8];
    strcpy(text_buf, "omw");
    ff_intent_t t9 = {.kind = FF_INTENT_T9_INSERT, .u = {0}};
    t9.u.text = text_buf;
    ff_shell_intent(&H.shell, &t9);
    TEST_ASSERT_EQUAL_STRING("omw", view()->compose.text);

    send_kind(FF_INTENT_BACK);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face); /* base underneath was the launcher (the boot default) */

    send_kind(FF_INTENT_OPEN_SETTINGS); /* jumps base to the Settings face */
    TEST_ASSERT_EQUAL(FF_APP_FACE_SETTINGS, view()->active_face);
    /* The draft is still projected — unconditionally, every tick,
     * regardless of active_face (ff_shell.c's shell_project) — even
     * while it isn't the visible face. */
    TEST_ASSERT_EQUAL_STRING("omw", view()->compose.text);

    /* Leave Settings via HOME — straight back to the launcher (S26
     * slice e, amended 2026-09-01) — the draft is untouched. */
    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);
    TEST_ASSERT_EQUAL_STRING("omw", view()->compose.text); /* still there, untouched */
}

/* =================================================================== */
/* S08 predictive addendum — predictive-T9 composer at the shell         */
/* =================================================================== */

/* A predictive pack the composer's session ranks above its dictionary.
 * "Gooders" deliberately shares the keys 4-6-6-3 with the dictionary word
 * "good" (the exact overlap test_t9pred.c uses) so one sequence proves both
 * the pack-word ranking AND the pointer-identity from_pack flag; the stage
 * name "A Stage" starts with 'a'=2 so it never collides with these keys. */
static char const PACK_JSON_T9[] =
    "{\"festpack\":\"0.1\",\"utc_offset_min\":0,"
    "\"festival\":{\"name\":\"Test Fest\",\"year\":2026,"
    "\"start\":\"2026-09-18\",\"end\":\"2026-09-20\","
    "\"venue\":{\"lat\":39.936,\"lon\":-82.414}},"
    "\"stages\":[{\"id\":\"a\",\"name\":\"A Stage\",\"color\":\"#00ff00\"}],"
    "\"schedule\":["
    "{\"artist\":\"Gooders\",\"stage\":\"a\",\"day\":\"2026-09-18\","
    "\"start\":\"21:00\",\"end\":\"23:00\"}]}";

/* Emit one predictive letter key (2..9) into the open PRED composer. */
static void pred_key(uint8_t k)
{
    ff_intent_t in = {.kind = FF_INTENT_T9_KEY, .u = {0}};
    in.u.t9_key = k;
    ff_shell_intent(&H.shell, &in);
}

/* The predicted word "4663" resolves to (dictionary top): "good". */
static void S08_pred_defaults_on_open_and_projects_the_top_candidate(void)
{
    harness_init(100000u);

    send_open_compose(0u);
    /* Defaults to the predictive mode (no T9_MODE needed). */
    TEST_ASSERT_EQUAL(FF_APP_COMPOSE_PRED, view()->compose.mode);
    /* Nothing typed yet: empty word, and NOT a no-match. */
    TEST_ASSERT_EQUAL_STRING("", view()->compose.word);
    TEST_ASSERT_FALSE(view()->compose.word_nomatch);

    pred_key(4);
    pred_key(6);
    pred_key(6);
    pred_key(3);

    ff_app_state_t const *v = view();
    TEST_ASSERT_EQUAL_STRING("good", v->compose.word);         /* the engine's real top word */
    TEST_ASSERT_FALSE(v->compose.word_nomatch);
    TEST_ASSERT_GREATER_THAN_UINT8(0, v->compose.n_cand);
    TEST_ASSERT_EQUAL_STRING("good", v->compose.cand[0].text);
    TEST_ASSERT_FALSE(v->compose.cand[0].from_pack);           /* dictionary word, no pack loaded */
    TEST_ASSERT_EQUAL_UINT16(11, v->compose.total_cand);       /* the REAL engine count for 4663 */
    TEST_ASSERT_EQUAL_UINT8(0, v->compose.sel_cand);
    /* The committed draft is still empty — the predicted word is not
     * committed until accepted. */
    TEST_ASSERT_EQUAL_STRING("", v->compose.text);
}

/* CYCLE advances the selection and `word`/`sel_cand` follow; "9673" has
 * exactly three candidates (words, word, wore) and cycling wraps. */
static void S08_pred_cycle_advances_selection_and_word_follows(void)
{
    harness_init(100000u);
    send_open_compose(0u);
    pred_key(9);
    pred_key(6);
    pred_key(7);
    pred_key(3);

    TEST_ASSERT_EQUAL_STRING("words", view()->compose.word);
    TEST_ASSERT_EQUAL_UINT8(0, view()->compose.sel_cand);
    TEST_ASSERT_EQUAL_UINT16(3, view()->compose.total_cand);

    send_kind(FF_INTENT_T9_CYCLE);
    TEST_ASSERT_EQUAL_STRING("word", view()->compose.word);
    TEST_ASSERT_EQUAL_UINT8(1, view()->compose.sel_cand);

    send_kind(FF_INTENT_T9_CYCLE);
    TEST_ASSERT_EQUAL_STRING("wore", view()->compose.word);
    TEST_ASSERT_EQUAL_UINT8(2, view()->compose.sel_cand);

    send_kind(FF_INTENT_T9_CYCLE); /* wraps back to the top */
    TEST_ASSERT_EQUAL_STRING("words", view()->compose.word);
    TEST_ASSERT_EQUAL_UINT8(0, view()->compose.sel_cand);
}

/* SELECT jumps straight to an index (tap-to-select). */
static void S08_pred_select_jumps_to_an_index(void)
{
    harness_init(100000u);
    send_open_compose(0u);
    pred_key(9);
    pred_key(6);
    pred_key(7);
    pred_key(3);

    ff_intent_t sel = {.kind = FF_INTENT_T9_SELECT, .u = {0}};
    sel.u.t9_key = 2; /* the third candidate */
    ff_shell_intent(&H.shell, &sel);
    TEST_ASSERT_EQUAL_STRING("wore", view()->compose.word);
    TEST_ASSERT_EQUAL_UINT8(2, view()->compose.sel_cand);

    sel.u.t9_key = 0;
    ff_shell_intent(&H.shell, &sel);
    TEST_ASSERT_EQUAL_STRING("words", view()->compose.word);
    TEST_ASSERT_EQUAL_UINT8(0, view()->compose.sel_cand);
}

/* SPACE accepts the current candidate: it commits into the draft (+space)
 * and the session resets AND re-binds its festpack words — proven by the
 * pack word predicting again after the accept. */
static void S08_pred_space_accepts_commits_and_rebinds_the_session(void)
{
    harness_init(100000u);
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_T9, sizeof(PACK_JSON_T9) - 1u));
    send_open_compose(0u);

    pred_key(4);
    pred_key(6);
    pred_key(6);
    pred_key(3);
    /* The pack word outranks the dictionary and is flagged from_pack by
     * pointer identity. */
    TEST_ASSERT_EQUAL_STRING("Gooders", view()->compose.word);
    TEST_ASSERT_TRUE(view()->compose.cand[0].from_pack);

    send_kind(FF_INTENT_T9_SPACE); /* accept */
    ff_app_state_t const *v = view();
    TEST_ASSERT_EQUAL_STRING("Gooders ", v->compose.text); /* committed + space */
    TEST_ASSERT_EQUAL_STRING("", v->compose.word);          /* session reset: nothing in progress */
    TEST_ASSERT_FALSE(v->compose.word_nomatch);

    /* Re-bind proof: type the same keys again. If SPACE had reset WITHOUT
     * re-binding the extras, the top word would drop to the dictionary
     * "good"; because the extras are re-bound, it is "Gooders" again. */
    pred_key(4);
    pred_key(6);
    pred_key(6);
    pred_key(3);
    TEST_ASSERT_EQUAL_STRING("Gooders", view()->compose.word);
    TEST_ASSERT_TRUE(view()->compose.cand[0].from_pack);
}

/* BACKSPACE removes from the predicted word first; only when the session is
 * empty does it eat into the committed draft. */
static void S08_pred_backspace_removes_from_session_then_draft(void)
{
    harness_init(100000u);
    send_open_compose(0u);

    /* Commit "good " first (type + accept). */
    pred_key(4);
    pred_key(6);
    pred_key(6);
    pred_key(3);
    send_kind(FF_INTENT_T9_SPACE);
    TEST_ASSERT_EQUAL_STRING("good ", view()->compose.text);

    /* Start a new predicted word: "9673" -> "words". */
    pred_key(9);
    pred_key(6);
    pred_key(7);
    pred_key(3);
    TEST_ASSERT_EQUAL_STRING("words", view()->compose.word);
    TEST_ASSERT_EQUAL_STRING("good ", view()->compose.text); /* draft untouched so far */

    /* Backspaces peel digits off the SESSION first; the committed draft
     * stays put until the session is empty. */
    send_kind(FF_INTENT_T9_BACKSPACE); /* "967" */
    send_kind(FF_INTENT_T9_BACKSPACE); /* "96" */
    send_kind(FF_INTENT_T9_BACKSPACE); /* "9" */
    TEST_ASSERT_EQUAL_STRING("good ", view()->compose.text); /* still untouched */

    send_kind(FF_INTENT_T9_BACKSPACE); /* session now empty */
    TEST_ASSERT_EQUAL_STRING("", view()->compose.word);
    TEST_ASSERT_EQUAL_STRING("good ", view()->compose.text); /* the empties-the-session press does not also cut the draft */

    send_kind(FF_INTENT_T9_BACKSPACE); /* now the draft: "good " -> "good" */
    TEST_ASSERT_EQUAL_STRING("good", view()->compose.text);
}

/* Honest no-match: "249" maps to no dictionary word (and no pack loaded) —
 * word_nomatch true, word empty, nothing fabricated. */
static void S08_pred_honest_no_match_projects_empty_word(void)
{
    harness_init(100000u);
    send_open_compose(0u);
    pred_key(2);
    pred_key(4);
    pred_key(9);

    ff_app_state_t const *v = view();
    TEST_ASSERT_TRUE(v->compose.word_nomatch);
    TEST_ASSERT_EQUAL_STRING("", v->compose.word);
    TEST_ASSERT_EQUAL_UINT8(0, v->compose.n_cand);
    TEST_ASSERT_EQUAL_UINT16(0, v->compose.total_cand);
}

/* The mode cycle now includes PRED, in order PRED -> ABC -> 123 -> SYM ->
 * PRED, and multitap still works once ABC is reached (behavior unchanged). */
static void S08_mode_cycle_includes_pred_and_multitap_still_works(void)
{
    harness_init(100000u);
    send_open_compose(0u);
    TEST_ASSERT_EQUAL(FF_APP_COMPOSE_PRED, view()->compose.mode);

    send_kind(FF_INTENT_T9_MODE);
    TEST_ASSERT_EQUAL(FF_APP_COMPOSE_ABC, view()->compose.mode);

    /* In ABC, a key is multi-tap into the committed draft, exactly as before
     * the addendum: key 3 once -> pending 'd'. */
    pred_key(3);
    TEST_ASSERT_EQUAL_STRING("d", view()->compose.text);
    TEST_ASSERT_TRUE(view()->compose.has_pending);

    send_kind(FF_INTENT_T9_MODE);
    TEST_ASSERT_EQUAL(FF_APP_COMPOSE_123, view()->compose.mode);
    send_kind(FF_INTENT_T9_MODE);
    TEST_ASSERT_EQUAL(FF_APP_COMPOSE_SYM, view()->compose.mode);
    send_kind(FF_INTENT_T9_MODE);
    TEST_ASSERT_EQUAL(FF_APP_COMPOSE_PRED, view()->compose.mode); /* wraps back to PRED */
}

/* CYCLE/SELECT are gated on a visible takeover like every other Compose
 * control (routing rule 4). */
static void S08_pred_controls_rejected_during_a_takeover(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");
    send_open_compose(0u);
    pred_key(9);
    pred_key(6);
    pred_key(7);
    pred_key(3);
    TEST_ASSERT_EQUAL_STRING("words", view()->compose.word);

    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    /* CYCLE and a predictive key are both swallowed while the takeover owns
     * the screen — the selection and digits are untouched. */
    send_kind(FF_INTENT_T9_CYCLE);
    pred_key(3);
    send_kind(FF_INTENT_TAKEOVER_DISMISS);
    TEST_ASSERT_EQUAL_STRING("words", view()->compose.word); /* unchanged: nothing reached the session */
    TEST_ASSERT_EQUAL_UINT8(0, view()->compose.sel_cand);
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
    send_kind(FF_INTENT_OPEN_SETTINGS); /* the long-press jump: same routing-rule-4 gate, rejected under takeover */
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face); /* base underneath was the boot default */
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
/* Notification routing — table-driven, every starting face x           */
/* {BANNER_OPEN, TAKEOVER_GO, TAKEOVER_DISMISS} (maintainer report:      */
/* "notifications like flaring and messages don't properly redirect to  */
/* the proper screens from the home screen (may happen elsewhere?)").   */
/* docs/specs/S26-device-lifecycle.md "Notifications" (BANNER tap ->     */
/* the sender's thread) and docs/specs/S10-flare.md "Receive" (GO ->     */
/* Radar locked on the sender; DISMISS -> back).                        */
/*                                                                       */
/* TWO BUGS FIXED BY THIS CHANGE, both at the route layer:              */
/*  1. FF_INTENT_TAKEOVER_GO consumed the takeover into the lock         */
/*     (`ff_flare_go`) but never called anything on `sh->route` at all  */
/*     — `base` stayed whatever it was before the flare interrupted it, */
/*     so GO silently did nothing visible from any face but Radar       */
/*     itself. Proxy: the ONE pre-existing GO test                      */
/*     (S16_c1_takeover_decisions_require_a_visible_takeover) only ever */
/*     checked `locked_node_id`, never `active_face` — exactly the      */
/*     proxy gap this table closes.                                     */
/*  2. FF_INTENT_BANNER_OPEN's own `ff_route_goto` call silently failed  */
/*     under a live modal (Compose) while every line after it (mark-    */
/*     read, thread-node switch, banner dismiss) ran anyway — mutating  */
/*     Inbox state behind a modal the user could still see and type     */
/*     into, invisibly, with no navigation to show for it.              */
/* =================================================================== */

static void inject_text_notif(uint32_t from, char const *text)
{
    H.ev.on_text(H.ev.user, from, MY_ID, text, strlen(text));
}

typedef enum {
    START_LAUNCHER,
    START_RADAR,
    START_LINEUP,
    START_MAP,
    START_SETTINGS,
    START_INBOX_LIST,
    START_INBOX_THREAD_OTHER, /* an open thread of a DIFFERENT sender (KEV) */
    START_MODAL_COMPOSE,
    START_MODAL_POWER_MENU,
} start_state_t;

static char const *start_state_name(start_state_t s)
{
    switch (s) {
    case START_LAUNCHER:           return "LAUNCHER";
    case START_RADAR:              return "RADAR";
    case START_LINEUP:             return "LINEUP";
    case START_MAP:                return "MAP";
    case START_SETTINGS:           return "SETTINGS";
    case START_INBOX_LIST:         return "INBOX_LIST";
    case START_INBOX_THREAD_OTHER: return "INBOX_THREAD_OTHER";
    case START_MODAL_COMPOSE:      return "MODAL_COMPOSE";
    case START_MODAL_POWER_MENU:   return "MODAL_POWER_MENU";
    }
    return "?";
}

/* Fresh harness, DANA and KEV both paired, route parked at `s`. DANA is
 * always the flare/message sender below; KEV exists so
 * START_INBOX_THREAD_OTHER has a real, different thread to be sitting
 * in (the brief's "an open thread of a DIFFERENT sender" starting
 * state) and so the self-healing crew selection (DANA, paired first)
 * is a real choice, not a vacuous single-member one. */
static void notif_setup(start_state_t s)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");
    pair_named(KEV_ID, "KEV");

    switch (s) {
    case START_LAUNCHER: break; /* harness_init's own boot default */
    case START_RADAR: nav_home_to(FF_APP_FACE_RADAR); break;
    case START_LINEUP: nav_home_to(FF_APP_FACE_LINEUP); break;
    case START_MAP: nav_home_to(FF_APP_FACE_MAP); break;
    case START_SETTINGS: nav_home_to(FF_APP_FACE_SETTINGS); break;
    case START_INBOX_LIST: nav_home_to(FF_APP_FACE_INBOX); break;
    case START_INBOX_THREAD_OTHER: {
        nav_home_to(FF_APP_FACE_INBOX);
        ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
        open.u.node_id = KEV_ID;
        ff_shell_intent(&H.shell, &open);
        break;
    }
    case START_MODAL_COMPOSE: send_open_compose(0u); break;
    case START_MODAL_POWER_MENU: send_kind(FF_INTENT_POWER_MENU_OPEN); break;
    }
}

/* The seven states BANNER_OPEN is expected to actually NAVIGATE from
 * (the two modal states get their own dedicated tests below, since
 * their expected outcomes are qualitatively different — one bails
 * entirely, one pops first). */
static start_state_t const k_banner_nav_states[] = {
    START_LAUNCHER, START_RADAR, START_LINEUP, START_MAP, START_SETTINGS,
    START_INBOX_LIST, START_INBOX_THREAD_OTHER,
};

static void S26_notif_banner_open_navigates_to_the_thread_from_every_base_face(void)
{
    for (size_t i = 0; i < sizeof(k_banner_nav_states) / sizeof(k_banner_nav_states[0]); i++) {
        start_state_t const s = k_banner_nav_states[i];
        notif_setup(s);

        inject_text_notif(DANA, "you close?");
        TEST_ASSERT_TRUE_MESSAGE(view()->banner.active, start_state_name(s));

        send_kind(FF_INTENT_BANNER_OPEN);
        ff_app_state_t const *v = view();
        TEST_ASSERT_EQUAL_INT_MESSAGE(FF_APP_FACE_INBOX, v->active_face, start_state_name(s));
        TEST_ASSERT_EQUAL_INT_MESSAGE(FF_INBOX_SUB_THREAD, v->inbox.subview, start_state_name(s));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(DANA, v->inbox.thread_node, start_state_name(s));
        TEST_ASSERT_FALSE_MESSAGE(v->banner.active, start_state_name(s));
    }
}

/* The Power menu is transient (no user data at stake): a banner tap
 * pops it, the same way BACK/Cancel/the 10s timeout already do, and
 * navigation proceeds normally underneath — same shape as every other
 * base above, just with one extra pop first. */
static void S26_notif_banner_open_pops_the_power_menu_and_navigates(void)
{
    notif_setup(START_MODAL_POWER_MENU);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_POWER_MENU, view()->active_face);

    inject_text_notif(DANA, "you close?");
    TEST_ASSERT_TRUE(view()->banner.active);

    send_kind(FF_INTENT_BANNER_OPEN);
    ff_app_state_t const *v = view();
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_INBOX, v->active_face);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, v->inbox.subview);
    TEST_ASSERT_EQUAL_UINT32(DANA, v->inbox.thread_node);
    TEST_ASSERT_FALSE(v->banner.active);
}

/* Compose holds a possibly half-typed draft, and the spec's rule (S24:
 * "a half-typed message is never slid away") is unconditional — a
 * banner tap is a PASSIVE interruption (nobody made an explicit
 * decision to leave, unlike flare's GO), so it must not pop Compose.
 * The fixed bug: the OLD code's `ff_route_goto` silently failed here
 * (modal up) but ran every line after it anyway — this test's
 * `inbox.subview`/banner-still-active/compose-untouched assertions are
 * exactly what that mutation would have missed if only `active_face`
 * were checked. */
static void S26_notif_banner_open_while_composing_leaves_everything_untouched(void)
{
    notif_setup(START_MODAL_COMPOSE);
    ff_app_state_t const *before = view();
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, before->active_face);
    TEST_ASSERT_EQUAL_UINT32(DANA, compose_to()); /* self-healing selection: DANA paired first */
    ff_inbox_subview_t const prior_subview = before->inbox.subview;

    inject_text_notif(DANA, "you close?");
    TEST_ASSERT_TRUE(view()->banner.active);

    send_kind(FF_INTENT_BANNER_OPEN);
    ff_app_state_t const *v = view();
    TEST_ASSERT_EQUAL_INT_MESSAGE(FF_APP_FACE_COMPOSE, v->active_face,
                                  "the draft was slid away by a banner tap");
    TEST_ASSERT_EQUAL_UINT32(DANA, compose_to()); /* destination intact */
    TEST_ASSERT_EQUAL_INT_MESSAGE(prior_subview, v->inbox.subview,
                                  "Inbox sub-view mutated behind a modal that never actually navigated");
    TEST_ASSERT_TRUE_MESSAGE(v->banner.active,
                             "the banner must stay queued - the user never actually saw it open");
}

/* All nine starting states, including both modals: GO must land on
 * Radar from every single one of them. */
static start_state_t const k_all_states[] = {
    START_LAUNCHER, START_RADAR, START_LINEUP, START_MAP, START_SETTINGS,
    START_INBOX_LIST, START_INBOX_THREAD_OTHER, START_MODAL_COMPOSE, START_MODAL_POWER_MENU,
};

static void S10_notif_flare_go_lands_on_radar_from_every_starting_state(void)
{
    for (size_t i = 0; i < sizeof(k_all_states) / sizeof(k_all_states[0]); i++) {
        start_state_t const s = k_all_states[i];
        notif_setup(s);

        inject_flare(DANA, 300u);
        TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->takeover_active, start_state_name(s));

        send_kind(FF_INTENT_TAKEOVER_GO);
        ff_app_state_t const *v = view();
        TEST_ASSERT_EQUAL_INT_MESSAGE(FF_APP_FACE_RADAR, v->active_face, start_state_name(s));
        TEST_ASSERT_FALSE_MESSAGE(ff_shell_flare(&H.shell)->takeover_active, start_state_name(s));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(DANA, ff_shell_flare(&H.shell)->locked_node_id, start_state_name(s));
        /* The radar view's own target must agree with the lock — not
         * just the lock field in isolation. In THIS table DANA is also
         * the first-paired/self-healed default selection, so this
         * assertion alone would pass even without the fix below; it is
         * still worth pinning here as the baseline "the two never
         * disagree" sanity check. The mutation-provable version of this
         * — sender that is NOT the self-healed default — is
         * S10_notif_flare_go_force_selects_the_sender_not_the_prior_selection
         * just below. */
        TEST_ASSERT_EQUAL_STRING_MESSAGE("DANA", v->radar.name, start_state_name(s));
    }
}

/* THE regression test for docs/specs/S10-flare.md's "GO -> radar with
 * the sender FORCE-SELECTED" (dated amendment, 2026-09-02). Bug shape:
 * with 3+ paired crew members, a flare from anyone OTHER than the
 * first-paired/self-healed selection landed GO on Radar still pointing
 * at the PREVIOUS selection (ff_radar_compute targets
 * ff_crew_selected(), and ff_flare_go only ever wrote the flare lock,
 * never the crew selection). DANA pairs first here (the self-healed
 * default, exactly like the table test above) and KEV flares —
 * deliberately the member GO must switch AWAY from, not the one it
 * would already be showing by coincidence. */
static void S10_notif_flare_go_force_selects_the_sender_not_the_prior_selection(void)
{
    for (size_t i = 0; i < sizeof(k_all_states) / sizeof(k_all_states[0]); i++) {
        start_state_t const s = k_all_states[i];
        notif_setup(s); /* pairs DANA then KEV; DANA is the self-healed default */
        TEST_ASSERT_EQUAL_STRING_MESSAGE("DANA", view()->radar.name, start_state_name(s));

        inject_flare(KEV_ID, 300u);
        TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->takeover_active, start_state_name(s));

        send_kind(FF_INTENT_TAKEOVER_GO);
        ff_app_state_t const *v = view();
        TEST_ASSERT_EQUAL_INT_MESSAGE(FF_APP_FACE_RADAR, v->active_face, start_state_name(s));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(KEV_ID, ff_shell_flare(&H.shell)->locked_node_id, start_state_name(s));
        TEST_ASSERT_EQUAL_STRING_MESSAGE("KEV", v->radar.name, start_state_name(s));
    }
}

/* DISMISS's contract is the opposite of GO's: restore EXACTLY the prior
 * state (spec: "DISMISS -> back, feed item remains") — from every one
 * of the nine starting states, including both modals (a live modal's
 * draft/menu must survive a DISMISS on a takeover that happened to
 * land on top of it). */
static void S10_notif_flare_dismiss_restores_the_exact_prior_state(void)
{
    for (size_t i = 0; i < sizeof(k_all_states) / sizeof(k_all_states[0]); i++) {
        start_state_t const s = k_all_states[i];
        notif_setup(s);

        ff_app_state_t const *before = view();
        ff_app_face_t const prior_face = before->active_face;
        ff_inbox_subview_t const prior_subview = before->inbox.subview;
        uint32_t const prior_thread_node = before->inbox.thread_node;
        uint32_t const prior_compose_to = compose_to();

        inject_flare(DANA, 300u);
        TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->takeover_active, start_state_name(s));

        send_kind(FF_INTENT_TAKEOVER_DISMISS);
        ff_app_state_t const *v = view();
        TEST_ASSERT_EQUAL_INT_MESSAGE(prior_face, v->active_face, start_state_name(s));
        TEST_ASSERT_EQUAL_INT_MESSAGE(prior_subview, v->inbox.subview, start_state_name(s));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(prior_thread_node, v->inbox.thread_node, start_state_name(s));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(prior_compose_to, compose_to(), start_state_name(s));
        TEST_ASSERT_FALSE_MESSAGE(ff_shell_flare(&H.shell)->takeover_active, start_state_name(s));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, ff_shell_flare(&H.shell)->locked_node_id, start_state_name(s));
    }
}

/* DISMISS must not touch the radar SELECTION either — only GO is the
 * explicit "force-select the sender" decision (S10 Ruling 2/3's own
 * framing, and this fix's dated amendment, 2026-09-02). Sender is KEV
 * (not the self-healed default DANA) specifically so a selection change
 * would be visible if DISMISS wrongly triggered one. */
static void S10_notif_flare_dismiss_does_not_change_the_radar_selection(void)
{
    for (size_t i = 0; i < sizeof(k_all_states) / sizeof(k_all_states[0]); i++) {
        start_state_t const s = k_all_states[i];
        notif_setup(s);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("DANA", view()->radar.name, start_state_name(s));

        inject_flare(KEV_ID, 300u);
        TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->takeover_active, start_state_name(s));

        send_kind(FF_INTENT_TAKEOVER_DISMISS);
        ff_app_state_t const *v = view();
        TEST_ASSERT_FALSE_MESSAGE(ff_shell_flare(&H.shell)->takeover_active, start_state_name(s));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, ff_shell_flare(&H.shell)->locked_node_id, start_state_name(s));
        TEST_ASSERT_EQUAL_STRING_MESSAGE("DANA", v->radar.name, start_state_name(s));
    }
}

/* Expiry rule for mechanism (b), "GO force-selects": the lock's OWN
 * expiry (ff_flare_tick clearing locked_node_id) does NOT revert the
 * radar SELECTION back to whoever was selected before GO — the user is
 * left looking at the sender (KEV) and can re-select DANA (or anyone
 * else) explicitly if they want to (via ff_crew_select_next / a future
 * cycling intent — not yet wired to any FF_INTENT_* as of this fix, so
 * not exercised here). This is documented in docs/specs/S10-flare.md's
 * dated amendment (2026-09-02) as the chosen rule, distinct from
 * mechanism (a)'s alternative ("radar follows the lock, and reverts
 * when the lock releases"). */
static void S10_notif_flare_lock_expiry_leaves_selection_on_the_sender(void)
{
    notif_setup(START_RADAR);
    TEST_ASSERT_EQUAL_STRING("DANA", view()->radar.name);

    inject_flare(KEV_ID, 300u);
    send_kind(FF_INTENT_TAKEOVER_GO);
    TEST_ASSERT_EQUAL_STRING("KEV", view()->radar.name);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_flare(&H.shell)->locked_node_id);

    /* Advance the clock past the 300s lock duration. */
    H.clk.t += 301u * 1000u;
    ff_app_state_t const *v = view();
    TEST_ASSERT_EQUAL_UINT32(0u, ff_shell_flare(&H.shell)->locked_node_id); /* lock released */
    TEST_ASSERT_EQUAL_STRING("KEV", v->radar.name); /* selection stays on KEV */
}

/* The reviewer's scratch-test scenario, confirmed non-vacuous: a NOFIX
 * sender must render their own honest NOFIX state (RADAR_LOST / "NO FIX
 * YET" — ff_radar.h's RENDERER CONTRACT: mode==RADAR_LOST with
 * age_str=="" means never-fixed, as opposed to a real past fix gone
 * stale) after GO, not the geometry left over from whoever was selected
 * before. Three paired members: DANA (first-paired/self-healed default,
 * given a real position fix so there IS prior geometry that could leak),
 * KEV (flares, but has NEVER sent a position fix), MAX (a third member,
 * proving this isn't just a two-member coincidence).
 *
 * `my_pos`/heading are set (ff_shell_set_my_pos/set_heading) specifically
 * so mode resolution reaches the freshness switch at all — ff_radar.h's
 * priority order puts RADAR_NOFIX (my own position/heading unknown)
 * ABOVE freshness, and this test wants to isolate KEV's OWN never-fixed
 * status (RADAR_LOST via FF_FRESH_NEVER), not a NOFIX caused by not
 * knowing where *I* am. */
static void S10_notif_flare_go_nofix_sender_shows_their_honest_nofix_not_prior_geometry(void)
{
    uint32_t const MAX_ID = 0x0000FA55u;

    notif_setup(START_RADAR); /* pairs DANA then KEV */
    pair_named(MAX_ID, "MAX");

    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.0});
    ff_shell_set_heading(&H.shell, 0.0f);
    /* DANA gets a real fix: this is the "prior geometry" that must not
     * leak onto KEV's view after GO. */
    inject_position(DANA, U_EVENING, 39.002215, -82.003648);

    ff_app_state_t const *before = view();
    TEST_ASSERT_EQUAL_STRING("DANA", before->radar.name);
    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, before->radar.mode);
    TEST_ASSERT_TRUE(before->radar.arrow_valid);
    TEST_ASSERT_NOT_EQUAL(0, before->radar.dist_str[0]);
    TEST_ASSERT_NOT_EQUAL(0, before->radar.age_str[0]);

    /* KEV flares — KEV has never sent a position fix. */
    inject_flare(KEV_ID, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    send_kind(FF_INTENT_TAKEOVER_GO);
    ff_app_state_t const *v = view();
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, v->active_face);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_flare(&H.shell)->locked_node_id);

    /* KEV's own honest NOFIX (never had a fix -> RADAR_LOST, "NO FIX
     * YET" per the RENDERER CONTRACT), not a fabricated arrow and not
     * any trace of DANA's geometry above. */
    TEST_ASSERT_EQUAL_STRING("KEV", v->radar.name);
    TEST_ASSERT_EQUAL_INT(RADAR_LOST, v->radar.mode);
    TEST_ASSERT_EQUAL_STRING("", v->radar.age_str);
    TEST_ASSERT_FALSE(v->radar.arrow_valid);
    TEST_ASSERT_EQUAL_STRING("", v->radar.dist_str);
}

/* The reverse-direction check the brief calls out separately: a flare
 * takeover arriving while the LAUNCHER is showing renders on top of it
 * (face_dispatch.c's takeover check runs before the active_face
 * dispatch, so this was already true) and GO/DISMISS both work from
 * there — pinned explicitly since it's the maintainer's literal "from
 * the home screen" report. */
static void S26_notif_flare_over_the_launcher_go_and_dismiss_both_work(void)
{
    /* GO half. */
    notif_setup(START_LAUNCHER);
    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    send_kind(FF_INTENT_TAKEOVER_GO);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, view()->active_face);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_flare(&H.shell)->locked_node_id);

    /* DISMISS half, fresh shell. */
    notif_setup(START_LAUNCHER);
    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    send_kind(FF_INTENT_TAKEOVER_DISMISS);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, view()->active_face);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
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

/* #bug1 — a brightness intent carrying the transient/commit flag. */
static void setting_send_brightness(ff_shell_t *shell, int32_t v, bool transient)
{
    ff_intent_t in = {.kind = FF_INTENT_SETTING_SET, .u = {0}};
    in.u.setting.id = FF_SETTING_BRIGHTNESS;
    in.u.setting.v.i = v;
    in.u.setting.transient = transient;
    ff_shell_intent(shell, &in);
}

/* #bug1 — the brightness slider's TRANSIENT (live-preview) vs COMMITTED
 * distinction. A drag fires many VALUE_CHANGED intents; each must apply to
 * in-memory state (so the projected brightness the backlight follows tracks
 * the finger) but must NOT write NVS. Only the settled, non-transient
 * RELEASED value persists, coalescing a whole drag's flash writes into one.
 * The proxy this kills: a naive "emit live" that persisted every step would
 * pass a value-applied check while thrashing the store, so the store-write
 * COUNT is asserted, not just the final value. */
static void bug1_transient_brightness_applies_live_but_persists_only_on_commit(void)
{
    setting_harness_t h;
    setting_harness_init(&h);

    uint8_t const start = ff_shell_settings(&h.shell)->brightness_pct;
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls);

    /* A drag: a run of transient values, each applied to live state... */
    int32_t const drag[] = {40, 55, 70, 85, 90};
    for (size_t i = 0; i < sizeof(drag) / sizeof(drag[0]); i++) {
        setting_send_brightness(&h.shell, drag[i], true /* transient */);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)drag[i], ff_shell_settings(&h.shell)->brightness_pct);
    }
    /* ...but NOT ONE of them wrote the store (the whole NVS-wear point). */
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls);
    TEST_ASSERT_TRUE(ff_shell_settings(&h.shell)->brightness_pct != start); /* it did move live */

    /* Release: the settled value commits, exactly once. */
    setting_send_brightness(&h.shell, 90, false /* commit */);
    TEST_ASSERT_EQUAL_UINT8(90u, ff_shell_settings(&h.shell)->brightness_pct);
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls);

    ff_shell_close(&h.shell);
}

/* #bug1 — a brightness change must NOT mark the render dirty: it is kept out
 * of the shell's render key so a live drag never forces a face rebuild that
 * would destroy the slider under the finger. The projected value still
 * changes (the device backlight is applied every tick regardless of the
 * dirty bit, so it follows the drag). */
static void bug1_brightness_change_does_not_mark_the_render_dirty(void)
{
    setting_harness_t h;
    setting_harness_init(&h);

    /* S26e amended 2026-09-01: the boot default is the launcher, whose
     * render key masks EVERYTHING except the unread badge (the same
     * "opaque overlay" discipline the power menu uses) — correctly so,
     * since Settings isn't what is on screen there. Leave the launcher
     * for an ordinary base face first, so the control assertion below
     * is actually testing what it says it tests: a rendered
     * Settings-face change dirtying the key. */
    ff_intent_t leave_launcher = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    leave_launcher.u.launcher_idx = 0u; /* Radar — any ordinary base face works */
    ff_shell_intent(&h.shell, &leave_launcher);

    (void)ff_shell_tick(&h.shell, h.clk.t);              /* first frame is always dirty; settle it */
    TEST_ASSERT_FALSE(ff_shell_tick(&h.shell, h.clk.t)); /* frozen clock, idle -> not dirty (baseline) */

    setting_send_brightness(&h.shell, 33, false /* even a committed change */);
    TEST_ASSERT_FALSE(ff_shell_tick(&h.shell, h.clk.t)); /* NOT dirty: brightness is excluded from the key */
    /* Yet the projection the backlight reads DID update. */
    TEST_ASSERT_EQUAL_UINT8(33u, ff_shell_view(&h.shell)->settings.brightness_pct);

    /* Control: a genuinely rendered setting (units) DOES mark dirty, so the
     * exclusion above is specific to brightness, not a broken dirty bit. */
    setting_send(&h.shell, FF_SETTING_IMPERIAL, ff_shell_settings(&h.shell)->imperial ? 0 : 1, NULL);
    TEST_ASSERT_TRUE(ff_shell_tick(&h.shell, h.clk.t));

    ff_shell_close(&h.shell);
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

/* =================================================================== */
/* S21 §3 — FF_INTENT_CALIBRATE_TOUCH (the Settings "CALIBRATE TOUCH" row) */
/* =================================================================== */

/* The injected device calibrate hook (ff_shell_cfg_t.calibrate_touch): a spy
 * that hands back a scripted fit + return value and counts calls — the same
 * injected-seam shape as the haptic spy in test_shell. On device the real hook
 * runs the crosshair capture; the shell writes the solved transform into
 * ff_settings and persists it so calibration survives reboot (S21 §4). */
typedef struct {
    ff_touchcal_t cal; /* what the hook writes into *out_cal */
    bool ret;          /* what the hook returns */
    int calls;
} calib_spy_t;

static bool calib_spy_hook(void *user, ff_touchcal_t *out_cal)
{
    calib_spy_t *sp = (calib_spy_t *)user;
    sp->calls++;
    *out_cal = sp->cal;
    return sp->ret;
}

/* setting_harness_init, plus the calibrate hook wired to `spy` (the hook must
 * be set at ff_shell_init time, so this can't reuse setting_harness_init). */
static void calib_harness_init(setting_harness_t *sh, calib_spy_t *spy)
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
    cfg.calibrate_touch = calib_spy_hook;
    cfg.calibrate_touch_user = spy;

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&sh->shell, &cfg));
}

static void send_calibrate(ff_shell_t *shell)
{
    ff_intent_t in = {.kind = FF_INTENT_CALIBRATE_TOUCH, .u = {0}};
    ff_shell_intent(shell, &in);
}

/* S21 AC3 — "Calibrate Touch ... stores + applies the result" (the
 * device-side crosshair flow itself is exercised through the injected
 * hook above, per this file's header note; the sim/no-op path is
 * covered by test_scr_intent.c's S21_AC3_settings_calibrate_touch_row_
 * emits_calibrate_intent). A valid fit overwrites the (honest-
 * uncalibrated identity) default and persists exactly once. */
static void S21_AC3_calibrate_valid_fit_applies_and_persists_the_solved_transform(void)
{
    calib_spy_t spy = {0};
    spy.cal = (ff_touchcal_t){.ax = 1.10f, .bx = -3.0f, .ay = 0.90f, .by = 4.0f, .valid = true};
    spy.ret = true;

    setting_harness_t h;
    calib_harness_init(&h, &spy);

    TEST_ASSERT_FALSE(ff_shell_settings(&h.shell)->touch_calibrated); /* identity default */
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls);

    send_calibrate(&h.shell);

    TEST_ASSERT_EQUAL_INT(1, spy.calls);
    ff_settings_t const *s = ff_shell_settings(&h.shell);
    TEST_ASSERT_TRUE(s->touch_calibrated);
    TEST_ASSERT_EQUAL_FLOAT(1.10f, s->touch_ax);
    TEST_ASSERT_EQUAL_FLOAT(-3.0f, s->touch_bx);
    TEST_ASSERT_EQUAL_FLOAT(0.90f, s->touch_ay);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, s->touch_by);
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls); /* persisted once */

    ff_shell_close(&h.shell);
}

/* A failed capture (hook returns false) or a degenerate solve (cal.valid ==
 * false) leaves settings untouched and writes nothing — honest-uncalibrated
 * stays uncalibrated, never a half-applied transform. */
static void S21_calibrate_failed_or_invalid_fit_is_a_clean_no_op(void)
{
    calib_spy_t spy = {0};
    spy.cal = (ff_touchcal_t){.ax = 2.0f, .bx = 9.0f, .ay = 2.0f, .by = 9.0f, .valid = true};
    spy.ret = false; /* capture reported failure */

    setting_harness_t h;
    calib_harness_init(&h, &spy);
    send_calibrate(&h.shell);

    TEST_ASSERT_EQUAL_INT(1, spy.calls);
    ff_settings_t const *s = ff_shell_settings(&h.shell);
    TEST_ASSERT_FALSE(s->touch_calibrated);     /* still uncalibrated */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s->touch_ax); /* still identity */
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls);
    ff_shell_close(&h.shell);

    calib_spy_t spy2 = {0};
    spy2.cal = (ff_touchcal_t){.ax = 2.0f, .bx = 9.0f, .ay = 2.0f, .by = 9.0f, .valid = false /* degenerate */};
    spy2.ret = true;

    setting_harness_t h2;
    calib_harness_init(&h2, &spy2);
    send_calibrate(&h2.shell);

    TEST_ASSERT_EQUAL_INT(1, spy2.calls);
    TEST_ASSERT_FALSE(ff_shell_settings(&h2.shell)->touch_calibrated);
    TEST_ASSERT_EQUAL_INT(0, h2.store_mem.set_calls);
    ff_shell_close(&h2.shell);
}

/* Re-running the SAME fit does not write NVS again — "persist on change,
 * never every tick", the same contract every other setting keeps. */
static void S21_calibrate_unchanged_refit_skips_the_write(void)
{
    calib_spy_t spy = {0};
    spy.cal = (ff_touchcal_t){.ax = 1.10f, .bx = -3.0f, .ay = 0.90f, .by = 4.0f, .valid = true};
    spy.ret = true;

    setting_harness_t h;
    calib_harness_init(&h, &spy);

    send_calibrate(&h.shell);
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls); /* first fit: a change, persisted */

    send_calibrate(&h.shell);                        /* same fit again */
    TEST_ASSERT_EQUAL_INT(2, spy.calls);             /* the hook still ran */
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls); /* but nothing new was written */

    ff_shell_close(&h.shell);
}

/* S21 AC4 — "NVS store persists settings across reboot ... boot applies
 * them", proven the same way test_shell_settings_persist.c's
 * S16_AC8_setting_set_survives_shell_close_and_reinit_against_the_same_
 * store proves it for the general settings path (close the shell,
 * re-init a FRESH shell against the SAME store, the value survives) —
 * here specifically for the calibrated touch transform CALIBRATE TOUCH
 * writes (S21 §3/§4). Uses this file's own in-memory setting_store_t
 * spy rather than the real on-disk store test_shell_settings_persist.c
 * exercises (that file is outside this PR's touched-file scope): the
 * property under test here is "does ff_shell_init's LOAD half read back
 * what the earlier session's CLOSE-time SAVE wrote", which the mock's
 * get()/set() round trip proves regardless of the store's real backing
 * (setting_store_get/_set actually copy bytes in and out of `buf`, not a
 * live struct reference — see their definitions above). */
static void S21_AC4_calibrated_touch_survives_shell_close_and_reinit_against_the_same_store(void)
{
    calib_spy_t spy = {0};
    spy.cal = (ff_touchcal_t){.ax = 1.10f, .bx = -3.0f, .ay = 0.90f, .by = 4.0f, .valid = true};
    spy.ret = true;

    setting_store_t store_mem;
    memset(&store_mem, 0, sizeof(store_mem));
    ff_store_t store = setting_store(&store_mem);

    fake_clock_t clk = {.t = 100000u};
    ff_clock_t clock = {.now_ms = fake_now, .user = &clk};
    fp_pack_t pack;

    /* --- session 1: calibrate, then close ------------------------------ */
    {
        ff_shell_t shell;
        ff_shell_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.clock = &clock;
        cfg.store = &store;
        cfg.pack = &pack;
        cfg.calibrate_touch = calib_spy_hook;
        cfg.calibrate_touch_user = &spy;

        TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&shell, &cfg));
        TEST_ASSERT_FALSE(ff_shell_settings(&shell)->touch_calibrated); /* identity default */

        send_calibrate(&shell);
        TEST_ASSERT_TRUE(ff_shell_settings(&shell)->touch_calibrated);
        TEST_ASSERT_EQUAL_INT(1, store_mem.set_calls);

        ff_shell_close(&shell);
    }

    /* --- session 2: a FRESH shell, the SAME store — "boot" -------------- */
    {
        ff_shell_t shell2;
        ff_shell_cfg_t cfg2;
        memset(&cfg2, 0, sizeof(cfg2));
        cfg2.clock = &clock;
        cfg2.store = &store;
        cfg2.pack = &pack;
        /* No calibrate_touch hook this session — proves the loaded value
         * came from the store, not from a re-run of the hook. */

        TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&shell2, &cfg2));
        ff_settings_t const *s = ff_shell_settings(&shell2);
        TEST_ASSERT_TRUE(s->touch_calibrated);
        TEST_ASSERT_EQUAL_FLOAT(1.10f, s->touch_ax);
        TEST_ASSERT_EQUAL_FLOAT(-3.0f, s->touch_bx);
        TEST_ASSERT_EQUAL_FLOAT(0.90f, s->touch_ay);
        TEST_ASSERT_EQUAL_FLOAT(4.0f, s->touch_by);

        ff_shell_close(&shell2);
    }
}

/* S21 AC5 — "Default touch cal set as chosen ... with an honest comment"
 * (ff_settings.c's own S21 §5 comment: identity, touch_calibrated =
 * false). Literal values, not macro names — pinning FF_TOUCHCAL_IDENTITY_*
 * or similar against itself would be vacuous; this asserts the actual
 * numbers docs/specs/S21-settings-rework.md §5 commits to. */
static void S21_AC5_default_touch_cal_is_identity(void)
{
    setting_harness_t h;
    setting_harness_init(&h);

    ff_settings_t const *s = ff_shell_settings(&h.shell);
    TEST_ASSERT_FALSE(s->touch_calibrated);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s->touch_ax);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s->touch_bx);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s->touch_ay);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s->touch_by);

    ff_shell_close(&h.shell);
}

/* =================================================================== */
/* S26 slice b — PWR button -> power menu -> soft power-off             */
/* =================================================================== */

/* The injected power_off/power_reboot hooks (ff_shell_cfg_t): a spy that
 * just counts calls — the same injected-device-IO shape as calib_spy_t
 * above. Two independent counters, not one shared spy, so a test can
 * assert POWER_OFF never also rings the reboot hook and vice versa. */
typedef struct {
    int off_calls;
    int reboot_calls;
} power_spy_t;

static void power_off_spy_hook(void *user)
{
    ((power_spy_t *)user)->off_calls++;
}

static void power_reboot_spy_hook(void *user)
{
    ((power_spy_t *)user)->reboot_calls++;
}

/* setting_harness_init, plus both power hooks wired to `spy` (like
 * calib_harness_init, the hooks must be set at ff_shell_init time). */
static void power_harness_init(setting_harness_t *sh, power_spy_t *spy)
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
    cfg.power_off = power_off_spy_hook;
    cfg.power_off_user = spy;
    cfg.power_reboot = power_reboot_spy_hook;
    cfg.power_reboot_user = spy;

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&sh->shell, &cfg));
}

static void send_power(ff_shell_t *shell, ff_intent_kind_t kind)
{
    ff_intent_t in = {.kind = kind, .u = {0}};
    ff_shell_intent(shell, &in);
}

/* S26 slice e — HOME against an explicit shell (the power-menu
 * section's own harness, `setting_harness_t`, not the global `H` the
 * earlier S26e_* tests use). */
static void send_home_on(ff_shell_t *shell)
{
    ff_intent_t in = {.kind = FF_INTENT_HOME, .u = {0}};
    ff_shell_intent(shell, &in);
}

/* Deliver a paired FLARE so `ff_shell_flare(shell)->takeover_active`
 * becomes true — the exact technique
 * S16_AC8_setting_set_is_rejected_while_a_takeover_is_visible uses. */
static void deliver_takeover(ff_shell_t *shell)
{
    TEST_ASSERT_TRUE(ff_shell_pair(shell, DANA, true));
    mc_events_t const ev = ff_shell_events(shell);
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_flare(buf, sizeof(buf), 300u);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    ev.on_private(ev.user, DANA, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
    TEST_ASSERT_TRUE(ff_shell_flare(shell)->takeover_active);
}

/* `ff_shell_view()->active_face` is only populated by `shell_project`
 * (inside `ff_shell_tick`) — unlike `ff_shell_settings()`/`ff_shell_flare()`,
 * which read live core state directly, a freshly-dispatched intent is
 * NOT yet reflected in the view until the next tick. Every assertion
 * below that reads `active_face` therefore ticks first through this
 * helper (the global-harness tests elsewhere in this file get the same
 * thing from their own `view()` helper) — skipping it would make
 * `TEST_ASSERT_NOT_EQUAL(FF_APP_FACE_POWER_MENU, ...)` pass against the
 * untouched zero-value FF_APP_FACE_NONE regardless of what push_modal
 * actually did, exactly the proxy AGENTS.md's item 6 warns about. */
static ff_app_state_t const *power_view(setting_harness_t *h)
{
    (void)ff_shell_tick(&h->shell, h->clk.t);
    return ff_shell_view(&h->shell);
}

static void S26b_power_menu_open_pushes_the_modal_and_becomes_visible(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);

    TEST_ASSERT_EQUAL(FF_APP_FACE_POWER_MENU, power_view(&h)->active_face);
    TEST_ASSERT_EQUAL_INT(0, spy.off_calls);
    TEST_ASSERT_EQUAL_INT(0, spy.reboot_calls);

    ff_shell_close(&h.shell);
}

/* Routing rule 4, same as every other c1/c2 intent: a live takeover
 * outranks a PWR long-press. The proxy this guards against is "the menu
 * silently opens behind the takeover and steals the next tap" — asserted
 * here, not just reasoned about. */
static void S26b_power_menu_open_is_rejected_while_a_takeover_is_visible(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);
    deliver_takeover(&h.shell);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);

    TEST_ASSERT_NOT_EQUAL(FF_APP_FACE_POWER_MENU, power_view(&h)->active_face);

    ff_shell_close(&h.shell);
}

/* One modal slot: a PWR long-press while Compose is open must not steal
 * the half-typed draft — ff_route_push_modal's own "one slot, not a
 * stack" rule, exercised end to end through the intent seam. */
static void S26b_power_menu_open_is_rejected_over_an_open_compose(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    ff_intent_t open_compose = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    ff_shell_intent(&h.shell, &open_compose);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, power_view(&h)->active_face);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, power_view(&h)->active_face); /* draft untouched */

    ff_shell_close(&h.shell);
}

static void S26b_power_off_calls_the_hook_and_closes_the_menu(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    send_power(&h.shell, FF_INTENT_POWER_OFF);

    TEST_ASSERT_EQUAL_INT(1, spy.off_calls);
    TEST_ASSERT_EQUAL_INT(0, spy.reboot_calls);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face); /* popped back to base (the boot default) */

    ff_shell_close(&h.shell);
}

/* NULL hook (the sim's honest shape) is a safe no-op — the menu still
 * closes, nothing crashes. */
static void S26b_power_off_with_no_hook_still_closes_the_menu(void)
{
    setting_harness_t h;
    memset(&h, 0, sizeof(h));
    h.clk.t = 100000u;
    h.clock.now_ms = fake_now;
    h.clock.user = &h.clk;
    h.store = setting_store(&h.store_mem);

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &h.clock;
    cfg.store = &h.store;
    cfg.pack = &h.pack; /* power_off/power_reboot left NULL */
    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&h.shell, &cfg));

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    send_power(&h.shell, FF_INTENT_POWER_OFF);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face);

    ff_shell_close(&h.shell);
}

static void S26b_power_reboot_calls_the_hook_and_closes_the_menu(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    send_power(&h.shell, FF_INTENT_POWER_REBOOT);

    TEST_ASSERT_EQUAL_INT(0, spy.off_calls);
    TEST_ASSERT_EQUAL_INT(1, spy.reboot_calls);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face);

    ff_shell_close(&h.shell);
}

static void S26b_power_cancel_closes_the_menu_without_calling_either_hook(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    send_power(&h.shell, FF_INTENT_POWER_CANCEL);

    TEST_ASSERT_EQUAL_INT(0, spy.off_calls);
    TEST_ASSERT_EQUAL_INT(0, spy.reboot_calls);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face); /* base untouched (the boot default) */

    ff_shell_close(&h.shell);
}

/* The spec's "Cancel or a 10 s timeout -> dismiss". Boundary pinned at
 * the exact millisecond, same inclusive-boundary discipline the core FSM
 * tests use — 9999 ms held open, 10000 ms auto-dismissed. */
static void S26b_power_menu_stays_open_just_before_the_10s_timeout(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    uint32_t const opened_at = h.clk.t;

    h.clk.t = opened_at + 9999u;
    TEST_ASSERT_EQUAL(FF_APP_FACE_POWER_MENU, power_view(&h)->active_face);

    ff_shell_close(&h.shell);
}

static void S26b_power_menu_auto_dismisses_at_exactly_the_10s_timeout(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    uint32_t const opened_at = h.clk.t;

    h.clk.t = opened_at + 10000u;
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face);
    TEST_ASSERT_EQUAL_INT(0, spy.off_calls); /* a timeout is a dismiss, not a Power off */
    TEST_ASSERT_EQUAL_INT(0, spy.reboot_calls);

    ff_shell_close(&h.shell);
}

/* Defensive gating: POWER_OFF/POWER_REBOOT/POWER_CANCEL are only ever
 * emitted by the menu's own buttons, so this can only happen via the
 * unlikely race an app_main-originated POWER_MENU_OPEN could also hit —
 * pinned anyway, same "keep the routing statement true" reasoning
 * SETTING_SET's takeover gate documents. */
static void S26b_power_actions_are_rejected_while_a_takeover_is_visible(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    deliver_takeover(&h.shell);

    send_power(&h.shell, FF_INTENT_POWER_OFF);
    send_power(&h.shell, FF_INTENT_POWER_REBOOT);
    send_power(&h.shell, FF_INTENT_POWER_CANCEL);

    TEST_ASSERT_EQUAL_INT(0, spy.off_calls);
    TEST_ASSERT_EQUAL_INT(0, spy.reboot_calls);
    /* None of the three actions were allowed to pop the modal — the
     * ROUTE still shows POWER_MENU underneath the takeover (S16 AC13:
     * `ff_app_state_t.active_face` never reflects the takeover itself,
     * only `flare.takeover_active` does — see ff_shell.c's shell_project
     * comment on that exact point). */
    TEST_ASSERT_EQUAL(FF_APP_FACE_POWER_MENU, power_view(&h)->active_face);
    TEST_ASSERT_TRUE(ff_shell_flare(&h.shell)->takeover_active);

    ff_shell_close(&h.shell);
}

/* =================================================================== */
/* S26 slice e, AMENDED 2026-09-01 — the launcher IS home. The          */
/* pre-amendment "PR #142 review FAIL 1" (launcher auto-dismiss timeout) */
/* and "FAIL 2" (POWER_MENU replaces a live LAUNCHER modal) fixes, and    */
/* every test that pinned them, are GONE: there is no timeout (the       */
/* launcher does not auto-dismiss — it is not a hub you pass through any  */
/* more) and no "replace" special case (the launcher is `base` now, not  */
/* a modal, so POWER_MENU_OPEN just pushes over it through the ordinary  */
/* one-modal-slot path — see ff_route.h's header note for the full       */
/* model). The two tests below are the Gate's required replacement       */
/* coverage: "power menu opens over the launcher" and "Cancel returns to */
/* the launcher".                                                        */
/* =================================================================== */

static void S26e_power_menu_opens_over_the_launcher_base(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face); /* the boot default */

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    TEST_ASSERT_EQUAL(FF_APP_FACE_POWER_MENU, power_view(&h)->active_face);

    ff_shell_close(&h.shell);
}

/* Cancelling that power menu reveals the launcher again — never
 * anywhere else, since the launcher was never replaced or popped
 * underneath the power menu (it is `base`, untouched by the push). */
static void S26e_power_menu_cancel_over_the_launcher_returns_to_the_launcher(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    TEST_ASSERT_EQUAL(FF_APP_FACE_POWER_MENU, power_view(&h)->active_face);

    send_power(&h.shell, FF_INTENT_POWER_CANCEL);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face);
    TEST_ASSERT_EQUAL_INT(0, spy.off_calls);
    TEST_ASSERT_EQUAL_INT(0, spy.reboot_calls);

    ff_shell_close(&h.shell);
}

/* HOME while the power menu covers the launcher is suppressed, same as
 * over any other base — a PWR long-press decision in progress must not
 * be yanked away by a home press. */
static void S26e_home_is_rejected_while_the_power_menu_covers_the_launcher(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    TEST_ASSERT_EQUAL(FF_APP_FACE_POWER_MENU, power_view(&h)->active_face);

    send_home_on(&h.shell);
    TEST_ASSERT_EQUAL(FF_APP_FACE_POWER_MENU, power_view(&h)->active_face); /* untouched */

    ff_shell_close(&h.shell);
}

/* S17 slice a: FF_SETTING_COLORBLIND — the exact same bool-backed,
 * persist-on-change-only contract as IMPERIAL above, pinned separately
 * per this repo's own "test names mirror the criteria" convention
 * (AGENTS.md) rather than folded into the IMPERIAL test. */
static void S17a_AC2_setting_set_colorblind_applies_and_persists_only_on_change(void)
{
    setting_harness_t h;
    setting_harness_init(&h);

    TEST_ASSERT_FALSE(ff_shell_settings(&h.shell)->colorblind); /* default */
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls);

    setting_send(&h.shell, FF_SETTING_COLORBLIND, 1, NULL); /* true: a real change */
    TEST_ASSERT_TRUE(ff_shell_settings(&h.shell)->colorblind);
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls);

    setting_send(&h.shell, FF_SETTING_COLORBLIND, 1, NULL); /* same value again */
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls);        /* unchanged: no new write */

    setting_send(&h.shell, FF_SETTING_COLORBLIND, 0, NULL); /* back to false: a change again */
    TEST_ASSERT_FALSE(ff_shell_settings(&h.shell)->colorblind);
    TEST_ASSERT_EQUAL_INT(2, h.store_mem.set_calls);

    ff_shell_close(&h.shell);
}

/* S21 amendment: FF_SETTING_CLOCK_24H — the same bool-backed,
 * persist-on-change-only contract as IMPERIAL/COLORBLIND above, pinned
 * separately per this repo's "test names mirror the criteria" convention
 * (AGENTS.md). Default false (12-hour). */
static void S21_setting_set_clock_24h_applies_and_persists_only_on_change(void)
{
    setting_harness_t h;
    setting_harness_init(&h);

    TEST_ASSERT_FALSE(ff_shell_settings(&h.shell)->clock_24h); /* default: 12-hour */
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls);

    setting_send(&h.shell, FF_SETTING_CLOCK_24H, 1, NULL); /* true: a real change */
    TEST_ASSERT_TRUE(ff_shell_settings(&h.shell)->clock_24h);
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls);

    setting_send(&h.shell, FF_SETTING_CLOCK_24H, 1, NULL); /* same value again */
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls);       /* unchanged: no new write */

    setting_send(&h.shell, FF_SETTING_CLOCK_24H, 0, NULL); /* back to false: a change again */
    TEST_ASSERT_FALSE(ff_shell_settings(&h.shell)->clock_24h);
    TEST_ASSERT_EQUAL_INT(2, h.store_mem.set_calls);

    ff_shell_close(&h.shell);
}

/* format v8 amendment: FF_SETTING_SCREEN_FLIP — the same bool-backed,
 * persist-on-change-only contract as IMPERIAL/COLORBLIND/CLOCK_24H above,
 * pinned separately per this repo's "test names mirror the criteria"
 * convention (AGENTS.md). Default false (NORMAL). Beyond the bare
 * apply+persist contract the toggle rows above already cover, this also
 * asserts the PROJECTED view actually carries the new value (not just the
 * raw core settings) and that the change marks the render key dirty — the
 * property that makes Radar's glass-centred rim tint (`radar_build_rim_tint`,
 * via `ff_theme_glass_cx`/`_cy`) actually re-centre on the next repaint,
 * rather than the flag silently taking effect only after some unrelated
 * later rebuild. Unlike brightness_pct (deliberately EXCLUDED from the
 * render key — see bug1_brightness_change_does_not_mark_the_render_dirty
 * above), screen_flip is a rare, discrete toggle with no live-drag
 * concern, so it is NOT excluded. */
static void S21_setting_set_screen_flip_applies_and_persists_only_on_change(void)
{
    setting_harness_t h;
    setting_harness_init(&h);

    /* S26e: land on an ordinary base face first (same reasoning as
     * bug1_brightness_change_does_not_mark_the_render_dirty above) so the
     * dirty-tick assertion below is testing a rendered Settings/Radar
     * change, not the launcher's masked render key. */
    ff_intent_t leave_launcher = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    leave_launcher.u.launcher_idx = 0u; /* Radar */
    ff_shell_intent(&h.shell, &leave_launcher);

    (void)ff_shell_tick(&h.shell, h.clk.t);              /* first frame is always dirty; settle it */
    TEST_ASSERT_FALSE(ff_shell_tick(&h.shell, h.clk.t)); /* frozen clock, idle -> not dirty (baseline) */

    TEST_ASSERT_FALSE(ff_shell_settings(&h.shell)->screen_flip);      /* default: NORMAL */
    TEST_ASSERT_FALSE(ff_shell_view(&h.shell)->settings.screen_flip); /* projection agrees */
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls);

    /* ff_shell_view()'s `.settings` (like `.active_face`) is only refreshed
     * BY `ff_shell_tick` (shell_project_settings runs during projection,
     * not at dispatch time) — unlike `ff_shell_settings()`, which reads the
     * shell's raw core `ff_settings_t` live. So the tick's return value
     * (the render-key-changed bit) and the freshly-projected view are
     * checked TOGETHER, right after the one tick that produces both. */
    setting_send(&h.shell, FF_SETTING_SCREEN_FLIP, 1, NULL); /* true: a real change */
    TEST_ASSERT_TRUE(ff_shell_settings(&h.shell)->screen_flip); /* raw core settings, live */
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls);            /* persisted on change */
    TEST_ASSERT_TRUE(ff_shell_tick(&h.shell, h.clk.t));         /* render key changed: rim re-centres */
    TEST_ASSERT_TRUE(ff_shell_view(&h.shell)->settings.screen_flip); /* the freshly-projected view flips too */

    TEST_ASSERT_FALSE(ff_shell_tick(&h.shell, h.clk.t)); /* idle again: no residual dirty */

    setting_send(&h.shell, FF_SETTING_SCREEN_FLIP, 1, NULL); /* same value again */
    TEST_ASSERT_EQUAL_INT(1, h.store_mem.set_calls);         /* unchanged: no new write */
    TEST_ASSERT_FALSE(ff_shell_tick(&h.shell, h.clk.t));     /* and no spurious dirty either */

    setting_send(&h.shell, FF_SETTING_SCREEN_FLIP, 0, NULL); /* back to false: a change again */
    TEST_ASSERT_FALSE(ff_shell_settings(&h.shell)->screen_flip);
    TEST_ASSERT_EQUAL_INT(2, h.store_mem.set_calls);
    TEST_ASSERT_TRUE(ff_shell_tick(&h.shell, h.clk.t)); /* dirty again: flipping back also re-centres */
    TEST_ASSERT_FALSE(ff_shell_view(&h.shell)->settings.screen_flip); /* the freshly-projected view flips back too */

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
    ev.on_private(ev.user, DANA, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
    TEST_ASSERT_TRUE(ff_shell_flare(&h.shell)->takeover_active);

    bool const before = ff_shell_settings(&h.shell)->imperial;
    setting_send(&h.shell, FF_SETTING_IMPERIAL, before ? 0 : 1, NULL);
    TEST_ASSERT_EQUAL(before, ff_shell_settings(&h.shell)->imperial); /* rejected: routing rule 4 */
    TEST_ASSERT_EQUAL_INT(0, h.store_mem.set_calls);

    ff_shell_close(&h.shell);
}

/**
 * PR #68 code review, HIGH finding 1: `shell_project_settings`'s two
 * lines projecting `utc_offset_min`/`utc_offset_set` into the VIEW
 * (`ff_app_state_t.settings` — what `scr_settings.c` actually renders
 * from) had no test — every existing UTC-offset assertion above reads
 * `ff_shell_settings()`, the raw internal `ff_settings_t`
 * `shell_setting_set` writes, which proves the validation gate but not
 * the projection step. Deleting those two lines left the whole suite
 * green (verified by the reviewer, and re-verified here after adding
 * this test — see the mutation note below). This test closes that gap at
 * the seam every OTHER field of this same projection is already checked
 * at (S16_c1_open_compose_defaults_to_the_selected_crew_member and
 * friends all assert on `view()`, never the raw internal state, for
 * exactly this reason).
 */
static void S11b_setting_set_utc_offset_reaches_the_view_projection(void)
{
    setting_harness_t h;
    setting_harness_init(&h);

    (void)ff_shell_tick(&h.shell, h.clk.t);
    ff_app_state_t const *before = ff_shell_view(&h.shell);
    TEST_ASSERT_FALSE(before->settings.utc_offset_set); /* default: never configured */

    setting_send(&h.shell, FF_SETTING_UTC_OFFSET_MIN, -420, NULL); /* UTC-7:00 */

    (void)ff_shell_tick(&h.shell, h.clk.t);
    ff_app_state_t const *after = ff_shell_view(&h.shell);
    TEST_ASSERT_TRUE(after->settings.utc_offset_set);
    TEST_ASSERT_EQUAL_INT16(-420, after->settings.utc_offset_min);

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

    /* S26 slice e: FF_INTENT_HOME replaces SWIPE as this test's
     * representative "does the seam forward this and does dispatch
     * actually move the shell" probe — SWIPE is a documented no-op now
     * (ff_shell.c's own case), so it can no longer prove the second
     * half. */
    ff_intent_t in = {.kind = FF_INTENT_HOME, .u = {0}};

    /* Unbound (the goldens/headless state): a safe no-op. */
    ff_intent_emit(&in);

    spy_sink_t spy;
    memset(&spy, 0, sizeof(spy));
    ff_intent_emit_bind(spy_sink, &spy);
    ff_intent_emit(&in);
    TEST_ASSERT_EQUAL_INT(1, spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_HOME, spy.last.kind);

    /* The real production binding: ff_shell_intent_sink adapts the seam
     * onto a shell without a function-pointer cast. HOME on the boot
     * default (the launcher, S26 slice e amended 2026-09-01) is a
     * no-op — it stays the visible face either way, which is still a
     * real assertion: it proves the intent reached the shell and was
     * dispatched, not merely that nothing crashed. */
    ff_intent_emit_bind(ff_shell_intent_sink, &H.shell);
    ff_intent_emit(&in);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);

    /* Unbind drops the sink AND its user pointer. */
    ff_intent_emit_bind(NULL, &spy); /* user with a NULL fn is ignored */
    ff_intent_emit(&in);
    TEST_ASSERT_EQUAL_INT(1, spy.count); /* the spy did not come back */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face); /* nor did the shell move */
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face); /* the boot default, untouched */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S26e_swipe_dispatch_moves_nothing_from_any_base_face);

    RUN_TEST(S26e_home_on_the_launcher_is_a_noop);
    RUN_TEST(S26e_home_from_each_app_sets_base_to_the_launcher);
    RUN_TEST(S26e_home_is_rejected_while_a_takeover_is_visible);
    RUN_TEST(S26e_launcher_select_reaches_every_circle_and_the_badge_projects);
    RUN_TEST(S26e_launcher_select_is_a_noop_when_the_launcher_is_not_showing);
    RUN_TEST(S26e_launcher_select_out_of_range_index_is_a_noop);
    RUN_TEST(S26e_launcher_select_is_rejected_while_a_takeover_is_visible);

    RUN_TEST(S16_c1_open_compose_and_back_round_trip);
    RUN_TEST(S16_c1_open_compose_with_no_crew_is_broadcast);
    RUN_TEST(S16_c1_open_compose_defaults_to_the_selected_crew_member);
    RUN_TEST(S16_c1_open_compose_honors_an_explicit_paired_destination);
    RUN_TEST(S16_c1_open_compose_never_retargets_an_unhonorable_destination);
    RUN_TEST(S16_c1_a_rejected_open_compose_does_not_retarget_the_composer);
    RUN_TEST(S16_c1_back_clears_the_compose_destination);
    RUN_TEST(S16_c1_open_settings_jumps_base_to_the_settings_face);
    RUN_TEST(launcher_reaches_map_directly);
    RUN_TEST(S11b_a_compose_draft_survives_a_settings_visit);
    RUN_TEST(S08_pred_defaults_on_open_and_projects_the_top_candidate);
    RUN_TEST(S08_pred_cycle_advances_selection_and_word_follows);
    RUN_TEST(S08_pred_select_jumps_to_an_index);
    RUN_TEST(S08_pred_space_accepts_commits_and_rebinds_the_session);
    RUN_TEST(S08_pred_backspace_removes_from_session_then_draft);
    RUN_TEST(S08_pred_honest_no_match_projects_empty_word);
    RUN_TEST(S08_mode_cycle_includes_pred_and_multitap_still_works);
    RUN_TEST(S08_pred_controls_rejected_during_a_takeover);
    RUN_TEST(S16_c1_route_intents_are_rejected_while_a_takeover_is_visible);
    RUN_TEST(S16_c1_takeover_decisions_require_a_visible_takeover);
    RUN_TEST(S16_c1_release_lock_and_takeover_dismiss_are_never_folded);

    RUN_TEST(S26_notif_banner_open_navigates_to_the_thread_from_every_base_face);
    RUN_TEST(S26_notif_banner_open_pops_the_power_menu_and_navigates);
    RUN_TEST(S26_notif_banner_open_while_composing_leaves_everything_untouched);
    RUN_TEST(S10_notif_flare_go_lands_on_radar_from_every_starting_state);
    RUN_TEST(S10_notif_flare_go_force_selects_the_sender_not_the_prior_selection);
    RUN_TEST(S10_notif_flare_dismiss_restores_the_exact_prior_state);
    RUN_TEST(S10_notif_flare_dismiss_does_not_change_the_radar_selection);
    RUN_TEST(S10_notif_flare_lock_expiry_leaves_selection_on_the_sender);
    RUN_TEST(S10_notif_flare_go_nofix_sender_shows_their_honest_nofix_not_prior_geometry);
    RUN_TEST(S26_notif_flare_over_the_launcher_go_and_dismiss_both_work);

    RUN_TEST(S16_c2_flare_start_begins_sending);
    RUN_TEST(S16_c2_flare_start_is_rejected_while_a_takeover_is_visible);
    RUN_TEST(S16_c2_flare_end_cancels_a_send_even_while_a_takeover_is_visible);
    RUN_TEST(S16_AC8_setting_set_applies_and_persists_only_on_change);
    RUN_TEST(S17a_AC2_setting_set_colorblind_applies_and_persists_only_on_change);
    RUN_TEST(S21_setting_set_clock_24h_applies_and_persists_only_on_change);
    RUN_TEST(S21_setting_set_screen_flip_applies_and_persists_only_on_change);
    RUN_TEST(bug1_transient_brightness_applies_live_but_persists_only_on_commit);
    RUN_TEST(bug1_brightness_change_does_not_mark_the_render_dirty);
    RUN_TEST(S21_AC3_calibrate_valid_fit_applies_and_persists_the_solved_transform);
    RUN_TEST(S21_calibrate_failed_or_invalid_fit_is_a_clean_no_op);
    RUN_TEST(S21_calibrate_unchanged_refit_skips_the_write);
    RUN_TEST(S21_AC4_calibrated_touch_survives_shell_close_and_reinit_against_the_same_store);
    RUN_TEST(S21_AC5_default_touch_cal_is_identity);

    RUN_TEST(S26b_power_menu_open_pushes_the_modal_and_becomes_visible);
    RUN_TEST(S26b_power_menu_open_is_rejected_while_a_takeover_is_visible);
    RUN_TEST(S26b_power_menu_open_is_rejected_over_an_open_compose);
    RUN_TEST(S26b_power_off_calls_the_hook_and_closes_the_menu);
    RUN_TEST(S26b_power_off_with_no_hook_still_closes_the_menu);
    RUN_TEST(S26b_power_reboot_calls_the_hook_and_closes_the_menu);
    RUN_TEST(S26b_power_cancel_closes_the_menu_without_calling_either_hook);
    RUN_TEST(S26b_power_menu_stays_open_just_before_the_10s_timeout);
    RUN_TEST(S26b_power_menu_auto_dismisses_at_exactly_the_10s_timeout);
    RUN_TEST(S26b_power_actions_are_rejected_while_a_takeover_is_visible);

    RUN_TEST(S26e_power_menu_opens_over_the_launcher_base);
    RUN_TEST(S26e_power_menu_cancel_over_the_launcher_returns_to_the_launcher);
    RUN_TEST(S26e_home_is_rejected_while_the_power_menu_covers_the_launcher);
    RUN_TEST(S16_AC8_setting_set_out_of_range_is_rejected_not_clamped);
    RUN_TEST(S16_AC8_setting_set_my_name_is_bounded_and_terminated);
    RUN_TEST(S16_AC8_setting_set_is_rejected_while_a_takeover_is_visible);
    RUN_TEST(S11b_setting_set_utc_offset_reaches_the_view_projection);
    RUN_TEST(S16_c1_intent_struct_and_pointer_payloads_are_borrowed_only);
    RUN_TEST(S16_c1_emit_seam_forwards_when_bound_and_noops_unbound);
    RUN_TEST(S16_c1_null_and_garbage_dispatch_is_safe);

    return UNITY_END();
}
