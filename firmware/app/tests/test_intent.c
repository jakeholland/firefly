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
#include "ff_idle.h" /* S26 slice e — FF_IDLE_T_DIM_MS, the launcher-timeout ordering test */
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

/* Navigate straight from Radar to one of the four launcher circles —
 * this file's replacement for what a run of `send_swipe` calls used to
 * do. `face` must be NOW/SIGNALS/MAP/SETTINGS; the caller is assumed to
 * be on Radar already (every test below calls this right after
 * harness_init, matching ff_route_init's opening face). */
static void nav_from_radar_to(ff_app_face_t face)
{
    uint8_t idx;
    switch (face) {
    case FF_APP_FACE_NOW: idx = 0; break;
    case FF_APP_FACE_SIGNALS: idx = 1; break;
    case FF_APP_FACE_MAP: idx = 2; break;
    case FF_APP_FACE_SETTINGS: idx = 3; break;
    default: TEST_FAIL_MESSAGE("nav_from_radar_to: not a launcher circle"); return;
    }
    send_home(); /* Radar -> launcher */
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
/* S26e_AC1_swipe_is_suppressed_while_the_launcher_is_open pins one      */
/* layer down (for the launcher-open case specifically).                */
/* =================================================================== */

static void S26e_swipe_dispatch_moves_nothing_from_any_base_face(void)
{
    harness_init(100000u);

    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face); /* ff_route_init's opening face */

    send_swipe(+1);
    send_swipe(-1);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);

    /* From every other base face too — a leftover call site is not
     * quietly re-wiring navigation through the intent that used to own
     * it. */
    ff_app_face_t const bases[] = {FF_APP_FACE_NOW, FF_APP_FACE_SIGNALS, FF_APP_FACE_MAP, FF_APP_FACE_SETTINGS};
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        harness_init(100000u);
        nav_from_radar_to(bases[i]);
        TEST_ASSERT_EQUAL(bases[i], view()->active_face);
        send_swipe(+1);
        send_swipe(-1);
        TEST_ASSERT_EQUAL(bases[i], view()->active_face);
    }
}

/* =================================================================== */
/* S26 slice e — HOME + LAUNCHER_SELECT, through the seam                */
/* =================================================================== */

static void S26e_home_from_radar_opens_launcher_then_returns(void)
{
    harness_init(100000u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);

    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);

    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);
}

static void S26e_home_is_rejected_while_a_takeover_is_visible(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");
    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face); /* route untouched underneath */
}

static void S26e_launcher_select_reaches_every_circle_and_the_badge_projects(void)
{
    harness_init(100000u);
    ff_app_face_t const circles[] = {FF_APP_FACE_NOW, FF_APP_FACE_SIGNALS, FF_APP_FACE_MAP, FF_APP_FACE_SETTINGS};
    for (size_t i = 0; i < sizeof(circles) / sizeof(circles[0]); i++) {
        harness_init(100000u);
        nav_from_radar_to(circles[i]);
        TEST_ASSERT_EQUAL(circles[i], view()->active_face);
    }
}

static void S26e_launcher_select_is_a_noop_when_the_launcher_is_not_open(void)
{
    harness_init(100000u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);
    send_launcher_select(1u); /* Signals — but the launcher was never opened */
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);
}

static void S26e_launcher_select_out_of_range_index_is_a_noop(void)
{
    harness_init(100000u);
    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);
    send_launcher_select(4u); /* one past the last real circle (0..3) */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face); /* still open */
    send_launcher_select(255u);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);
}

static void S26e_launcher_select_is_rejected_while_a_takeover_is_visible(void)
{
    harness_init(100000u);
    pair_named(DANA, "DANA");
    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);

    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    send_launcher_select(1u); /* Signals */
    /* The route's own modal is untouched underneath the takeover (S16
     * AC13: active_face never reflects the takeover itself). */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, view()->active_face);
}

/* =================================================================== */
/* OPEN_COMPOSE / BACK — the modal round trip                           */
/* =================================================================== */

static void S16_c1_open_compose_and_back_round_trip(void)
{
    harness_init(100000u);
    nav_from_radar_to(FF_APP_FACE_SIGNALS); /* where the real "+" lives */
    TEST_ASSERT_EQUAL(FF_APP_FACE_SIGNALS, view()->active_face);

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
    nav_from_radar_to(FF_APP_FACE_NOW); /* prove the jump works from any base, not just Radar */
    TEST_ASSERT_EQUAL(FF_APP_FACE_NOW, view()->active_face);

    /* One dispatch jumps straight to the far-right Settings face,
     * skipping Signals and Map. */
    send_kind(FF_INTENT_OPEN_SETTINGS);
    TEST_ASSERT_EQUAL(FF_APP_FACE_SETTINGS, view()->active_face);

    /* Settings is a base face, not a modal — you leave it via HOME (S26
     * slice e: "from any app -> Radar"), not BACK. */
    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);

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
 * left via HOME straight back to Radar.
 */
static void launcher_reaches_map_directly(void)
{
    harness_init(100000u);

    nav_from_radar_to(FF_APP_FACE_MAP);
    TEST_ASSERT_EQUAL(FF_APP_FACE_MAP, view()->active_face);

    /* Leave Map via HOME — straight back to Radar, no modal, no BACK
     * involved (S26 slice e: "from any app -> Radar"). */
    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);

    /* Every other launcher circle is reachable the same way. */
    ff_app_face_t const others[] = {FF_APP_FACE_NOW, FF_APP_FACE_SIGNALS, FF_APP_FACE_SETTINGS};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        nav_from_radar_to(others[i]);
        TEST_ASSERT_EQUAL(others[i], view()->active_face);
        send_home();
        TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);

    send_kind(FF_INTENT_OPEN_SETTINGS); /* jumps base to the Settings face */
    TEST_ASSERT_EQUAL(FF_APP_FACE_SETTINGS, view()->active_face);
    /* The draft is still projected — unconditionally, every tick,
     * regardless of active_face (ff_shell.c's shell_project) — even
     * while it isn't the visible face. */
    TEST_ASSERT_EQUAL_STRING("omw", view()->compose.text);

    /* Leave Settings via HOME — straight back to Radar (S26 slice e) —
     * the draft is untouched. */
    send_home();
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);
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

/* A valid fit overwrites the (honest-uncalibrated identity) default and
 * persists exactly once. */
static void S21_calibrate_valid_fit_applies_and_persists(void)
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

/* S26 slice e — HOME/LAUNCHER_SELECT against an explicit shell (the
 * power-menu section's own harness, `setting_harness_t`, not the
 * global `H` the earlier S26e_* tests use). */
static void send_home_on(ff_shell_t *shell)
{
    ff_intent_t in = {.kind = FF_INTENT_HOME, .u = {0}};
    ff_shell_intent(shell, &in);
}

static void send_launcher_select_on(ff_shell_t *shell, uint8_t launcher_idx)
{
    ff_intent_t in = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    in.u.launcher_idx = launcher_idx;
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, power_view(&h)->active_face); /* popped back to base */

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
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, power_view(&h)->active_face);

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
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, power_view(&h)->active_face);

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
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, power_view(&h)->active_face); /* base untouched */

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
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, power_view(&h)->active_face);
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
/* S26 slice e, PR #142 review — FAIL 1: the launcher's transient        */
/* timeout, and FAIL 2: POWER_MENU reaching the user from the launcher   */
/* =================================================================== */

/* AC (FAIL 1): "Radar is the watchface... what the screen wakes to" —
 * the launcher must never be able to outlive a dim/OFF/wake cycle, so it
 * pops itself back to Radar well before FF_IDLE_T_DIM_MS could ever fire.
 * Boundary pinned exactly like the power menu's own timeout tests just
 * above (9999 held open, 10000 auto-dismissed). */
static void S26e_launcher_timeout_stays_open_just_before_10s(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_home_on(&h.shell); /* Radar -> launcher */
    uint32_t const opened_at = h.clk.t;

    h.clk.t = opened_at + 9999u;
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face);

    ff_shell_close(&h.shell);
}

static void S26e_launcher_timeout_auto_dismisses_at_exactly_10s(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_home_on(&h.shell);
    uint32_t const opened_at = h.clk.t;

    h.clk.t = opened_at + 10000u;
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, power_view(&h)->active_face);

    ff_shell_close(&h.shell);
}

/* "reset by any press inside the launcher" — a press well before the
 * deadline (here, an out-of-range LAUNCHER_SELECT: a real mistap that
 * misses every circle, still "a press", still inside the launcher)
 * pushes the deadline out by another full FF_LAUNCHER_TIMEOUT_MS from
 * the press, not from the original open. Proven by landing PAST the
 * original 10s mark (opened_at + 10000) while STILL open. */
static void S26e_launcher_timeout_is_reset_by_a_press_inside(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_home_on(&h.shell);
    uint32_t const opened_at = h.clk.t;

    h.clk.t = opened_at + 9000u;
    send_launcher_select_on(&h.shell, 99u); /* out of range: a no-op nav, still "a press" */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face); /* still open */

    /* Without the reset this would already be past the original
     * deadline (opened_at + 10000); with it, the deadline is now
     * (opened_at + 9000 + 10000). */
    h.clk.t = opened_at + 10000u;
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face);

    h.clk.t = opened_at + 9000u + 10000u;
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, power_view(&h)->active_face);

    ff_shell_close(&h.shell);
}

/* The ordering guarantee the whole fix depends on: the launcher is
 * ALWAYS gone before the screen could ever dim with it showing. A
 * 10000 -> 20000 mutation of FF_LAUNCHER_TIMEOUT_MS flips this false
 * (20000 > FF_IDLE_T_DIM_MS's 15000) and fails loudly here, independent
 * of (and in addition to) the boundary tests above timing out
 * differently under such a mutation. */
static void S26e_launcher_timeout_is_safely_below_dim(void)
{
    TEST_ASSERT_TRUE(FF_LAUNCHER_TIMEOUT_MS < FF_IDLE_T_DIM_MS);
}

/* FAIL 2 — a PWR long-press must reach the user even while the launcher
 * is open: POWER_MENU_OPEN replaces it instead of being silently
 * swallowed by the one-modal-slot rule. */
static void S26e_power_menu_open_replaces_the_launcher(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_home_on(&h.shell);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, power_view(&h)->active_face);

    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    TEST_ASSERT_EQUAL(FF_APP_FACE_POWER_MENU, power_view(&h)->active_face); /* the launcher is gone */

    ff_shell_close(&h.shell);
}

/* Cancelling that power menu lands on Radar, never back on the
 * launcher — the replace discarded it; there is nothing left to "go
 * back" to. */
static void S26e_power_menu_cancel_after_replacing_the_launcher_returns_to_radar(void)
{
    setting_harness_t h;
    power_spy_t spy = {0};
    power_harness_init(&h, &spy);

    send_home_on(&h.shell);
    send_power(&h.shell, FF_INTENT_POWER_MENU_OPEN);
    TEST_ASSERT_EQUAL(FF_APP_FACE_POWER_MENU, power_view(&h)->active_face);

    send_power(&h.shell, FF_INTENT_POWER_CANCEL);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, power_view(&h)->active_face);
    TEST_ASSERT_EQUAL_INT(0, spy.off_calls);
    TEST_ASSERT_EQUAL_INT(0, spy.reboot_calls);

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
     * onto a shell without a function-pointer cast. From Radar, HOME
     * opens the launcher. */
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, view()->active_face);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S26e_swipe_dispatch_moves_nothing_from_any_base_face);

    RUN_TEST(S26e_home_from_radar_opens_launcher_then_returns);
    RUN_TEST(S26e_home_is_rejected_while_a_takeover_is_visible);
    RUN_TEST(S26e_launcher_select_reaches_every_circle_and_the_badge_projects);
    RUN_TEST(S26e_launcher_select_is_a_noop_when_the_launcher_is_not_open);
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
    RUN_TEST(S16_c2_flare_start_begins_sending);
    RUN_TEST(S16_c2_flare_start_is_rejected_while_a_takeover_is_visible);
    RUN_TEST(S16_c2_flare_end_cancels_a_send_even_while_a_takeover_is_visible);
    RUN_TEST(S16_AC8_setting_set_applies_and_persists_only_on_change);
    RUN_TEST(S17a_AC2_setting_set_colorblind_applies_and_persists_only_on_change);
    RUN_TEST(bug1_transient_brightness_applies_live_but_persists_only_on_commit);
    RUN_TEST(bug1_brightness_change_does_not_mark_the_render_dirty);
    RUN_TEST(S21_calibrate_valid_fit_applies_and_persists);
    RUN_TEST(S21_calibrate_failed_or_invalid_fit_is_a_clean_no_op);
    RUN_TEST(S21_calibrate_unchanged_refit_skips_the_write);

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

    RUN_TEST(S26e_launcher_timeout_stays_open_just_before_10s);
    RUN_TEST(S26e_launcher_timeout_auto_dismisses_at_exactly_10s);
    RUN_TEST(S26e_launcher_timeout_is_reset_by_a_press_inside);
    RUN_TEST(S26e_launcher_timeout_is_safely_below_dim);
    RUN_TEST(S26e_power_menu_open_replaces_the_launcher);
    RUN_TEST(S26e_power_menu_cancel_after_replacing_the_launcher_returns_to_radar);
    RUN_TEST(S16_AC8_setting_set_out_of_range_is_rejected_not_clamped);
    RUN_TEST(S16_AC8_setting_set_my_name_is_bounded_and_terminated);
    RUN_TEST(S16_AC8_setting_set_is_rejected_while_a_takeover_is_visible);
    RUN_TEST(S11b_setting_set_utc_offset_reaches_the_view_projection);
    RUN_TEST(S16_c1_intent_struct_and_pointer_payloads_are_borrowed_only);
    RUN_TEST(S16_c1_emit_seam_forwards_when_bound_and_noops_unbound);
    RUN_TEST(S16_c1_null_and_garbage_dispatch_is_safe);

    return UNITY_END();
}
