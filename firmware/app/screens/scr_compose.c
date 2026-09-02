/**
 * scr_compose.c — see scr_compose.h.
 *
 * ## Render vs. live input — the split, and why
 * The INITIAL paint of the message bubble is a pure function of
 * `*compose` (the fixture snapshot), exactly like every other screen in
 * this codebase (`ff_scr_radar_build`/`ff_scr_signals_build`) — that's
 * what keeps `--headless --screenshot` deterministic and goldens
 * reproducible: a headless single-frame render never processes a click
 * event, so it always shows exactly what the fixture says, nothing more.
 *
 * As of S16 slice c3 this file no longer holds a live `ff_t9_t` at all —
 * it moved to shell-owned draft state (`app/ff_shell.c`'s `compose_draft`,
 * per the spec's Intents section: "the draft is shell-owned T9 state").
 * Every keypad control below is a pure emitter, same shape as SEND/BACK
 * already were: it reports WHICH key was pressed, in the context of the
 * mode this screen itself is currently showing (`s_mode`, still a local
 * snapshot from `*compose` at build time — deciding "is this a multi-tap
 * letter key or a literal insert" is what makes the intent semantic
 * rather than a bare hardware scancode), and the shell decides what, if
 * anything, mutates. This screen never touches `ff_t9.h` directly anymore
 * — no engine, no local echo, nothing to reset. A live session's bubble
 * only reflects a keypress once the shell's next projection reaches the
 * screen through the render lifecycle (S16 slice d); until then, exactly
 * like every other button in this codebase, a click here is a report, not
 * a repaint.
 *
 * ## Keypad mode paging + which emoji tier this PR ships
 * S08's Amendments (2026-08-23, owner ruling) resolves the open digits/
 * symbols question with three keypad pages, cycled by the mode chip in
 * the header (shows the CURRENT mode's name — never a mystery toggle,
 * per the ruling's own requirement): ABC (the original v1 multi-tap
 * scope, letter legends only — the engine still cannot produce digits in
 * this mode, so it still never gets a digit legend), 123 (literal
 * digits, via the new `ff_t9_insert_text()`), and SYM.
 *
 * SYM ships the **tier-2 ASCII-emoticon fallback**, not tier-1 real
 * emoji: a curated custom LVGL font subset (~12-16 emoji codepoints) is
 * a real asset-authoring task (font generation tooling, a licensed glyph
 * source, size/legibility tuning at 37mm) that doesn't fit this PR's
 * scope alongside the feed/wiring/Signals/Compose work it's already
 * carrying. ASCII shortcuts (":)", "<3", ...) transmit as plain text
 * MC_TEXT_MAX bytes — they render correctly on every Meshtastic receiver
 * (phone apps, older pucks), which real emoji would not without every
 * receiver also carrying the same font. Follow-up for real emoji:
 * tracked in https://github.com/jakeholland/firefly/issues/22 (tier-1,
 * per the ruling's own fallback clause).
 *
 * ## Round-glass layout (PR #25 UX review, blocking finding)
 * The first pass of this file positioned header/footer chrome against
 * the puck's SQUARE bounding box (`LV_ALIGN_TOP_LEFT`/`TOP_RIGHT` with
 * flat pixel offsets) — the back button ended up ~42px entirely off the
 * round glass, the mode chip ~35px off, DEL/SEND lost roughly half their
 * tappable area. Every position below is now computed from
 * `ff_layout_chord_half_width` (app/screens/ff_layout.h — hoisted out of
 * this bug specifically so the next face doesn't repeat it) against the
 * puck's REAL circle, not eyeballed against its square corners: each
 * row's horizontal margin is derived from how wide the circle actually
 * is at that row's OWN worst-case (farthest-from-center) y, with a fixed
 * `FF_COMPOSE_SAFETY_PX` buffer subtracted for slack. This is why the
 * grid's rows get progressively MORE inset the closer they sit to the
 * puck's bottom pole (the reviewer's own suggested fix: "a grid inscribed
 * in a circle wants a narrower row" near the edge, not a uniform one) —
 * and it's asserted, not just designed-and-hoped: see
 * targets/sim/tests/test_face_hit_targets.c, which builds this exact
 * screen from every committed fixture and fails if any hit-rect ever
 * drifts back outside the circle.
 *
 * ## SEND relocation + key-size audit (maintainer: "move SEND away from
 * SPACE to avoid accidental press; audit buttons, make them as large as
 * possible")
 *
 * The bottom DEL/0/SEND row put SEND immediately adjacent to SPACE (8px
 * apart, the adjacency FLOOR, zero slack) — exactly the shape of mis-tap
 * this codebase's own FF_HIT_MIN_GAP_PX doctrine exists to prevent, except
 * the sweep never caught it because SEND and SPACE are each individually
 * >=44px and >=8px apart: the floor was satisfied to the pixel while the
 * real-world risk (a fat thumb aiming for the space bar overshooting onto
 * a full-message SEND) was not what the floor was ever sized to rule out.
 * Fix: SEND and the mode chip TRADE PLACES. SEND moves to the header's
 * top-right corner (the mode chip's old slot) — far from SPACE (they no
 * longer share a row at all: edge-to-edge vertical gap measures ~269px,
 * SEND's bottom edge at y=67 to SPACE's top edge at y=336 — the full
 * height of the draft/keypad stack, nowhere near "one key-width" or the
 * 12px dead-gap floor the maintainer asked for).
 * The mode chip drops into SEND's old bottom-row slot, which is harmless
 * to fat-finger: mis-hitting it only flips ABC/123/SYM/T9, never sends a
 * message. This is a straight swap, not a new control, so it costs zero
 * extra vertical budget — the header row width is untouched (BACK stays
 * put; only the right-hand control's identity changes), and TO's centered
 * label needs no repositioning of its own.
 *
 * Freed from having to share a row with SEND, the bottom row's three
 * remaining controls (DEL / SPACE / MODE) no longer need equal thirds:
 * DEL and MODE are sized to the exact FF_THEME_MIN_HIT_PX floor (44px —
 * both are lower-frequency, non-destructive-if-mistapped actions) and
 * SPACE — by far the most-tapped key on this keypad — gets every
 * remaining pixel (FF_COMPOSE_BOTTOM_SIDE_W below).
 *
 * Separately, the audit re-ran this file's own "largest key that still
 * fits the glass" search (ff_layout_safe_margin_x, the same primitive
 * every row here already used — never hand math, per PR #86's lesson) and
 * found 2px of unclaimed headroom: FF_COMPOSE_KEY_H/FF_COMPOSE_BOTTOM_ROW_H
 * grow 54->56, and the bottom row's now-unnecessary
 * FF_COMPOSE_BOTTOM_ROW_GAP_EXTRA (4px, pure cosmetic breathing room
 * between row2 and the bottom row, never required by the adjacency floor)
 * is removed and spent on height too. 56px is the actual ceiling for a
 * three-column bottom row at this Y — 57px measures under 44px column
 * width at the pole (139px row width, short of the 148px three-column
 * floor of 3*44 + 2*8) — verified by sweeping FF_COMPOSE_KEY_H against
 * ff_layout_safe_margin_x in this PR's body, then confirmed against the
 * REAL rendered click-area by test_face_hit_targets.c, same as every
 * other number in this file.
 */
#include "scr_compose.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ff_intent.h" /* S16c1 — the emit seam; see compose_back_cb */
#include "ff_layout.h"
#include "ff_theme.h"

/* ---------------------------------------------------------------------
 * Layout constants.
 *
 * Row Y positions/heights are the fixed design choices; horizontal
 * margins are NOT independently chosen numbers — they're derived from
 * these Y values via compose_safe_margin_x() below, so the two can never
 * drift out of sync the way the original off-glass layout did (a Y
 * value could change here and the margins would automatically follow).
 * ------------------------------------------------------------------- */

#define FF_COMPOSE_SAFETY_PX 10.0f /* inset from the true glass edge — comfortable slack,
                                     * same "generous inset" spirit as radar_layout.h's
                                     * RING_RADIUS_PX (185) vs the puck's own 220px radius */

/* S15 slice c: the header sat at y=56 with an 84px bubble on the old 440
 * puck, which put the four-row T9 keypad's bottom (DEL/0/SEND) row at
 * y=372..418 — past the 412 panel's own bottom edge, so on the real glass
 * that row fell off the display AND, well before that, the narrowing circle
 * crushed its keys below the 44px hit floor. Lifting the header to y=38 and
 * trimming the bubble to 76px raised the whole stack so the bottom row
 * landed at y=346..392.
 *
 * #99 (bigger T9 keys — the maintainer's on-glass feedback: the keypad is
 * the tightest thing to hit even sober): reclaim another 24px off the top of
 * the stack (header 38->30, bubble 76->60) and spend it on TALLER keys
 * (46->50) that also start HIGHER. Both moves help: taller keys are a bigger
 * vertical target everywhere, and starting the grid higher (GRID_Y 174->150)
 * lifts the bottom DEL/0/SEND row out of the narrowest part of the pole
 * (346..392 -> 334..384), where the widening circle gives its three keys
 * real extra WIDTH — the one row that was scraping the floor. Every key's
 * final width/height is verified against the rendered click-area by
 * test_face_hit_targets.c's sweep, not hand math (PR #86's lesson); the
 * before/after measurements are in this file's PR body. */
/* Device-feedback follow-up (Jake, on-glass: "difficult to press ... buttons
 * as large as possible"): reclaim the ~16px of slack that sat above the grid
 * (header 30->24, and the PRED draft/strip lifted to match) and spend it on
 * TALLER keys (50->54) without pushing the bottom DEL/0/SEND row any deeper
 * into the narrow pole; also drop the key gap to the 8px adjacency floor
 * (10->8), which both frees a little more vertical AND widens every key by a
 * couple px — the one move that helps the tight bottom row's WIDTH, not just
 * height. Final rendered sizes are verified by test_face_hit_targets.c's
 * sweep, never this hand-math (PR #86's lesson). */
#define FF_COMPOSE_HEADER_Y 24
#define FF_COMPOSE_HEADER_H FF_THEME_MIN_HIT_PX /* back button / SEND (SEND relocation: was back/mode chip) */

/* SEND's header slot — kept at the mode chip's OLD width (64px, already
 * >= the hit floor by a comfortable 20px) rather than grown further: TO's
 * centered label already runs right up against this corner control at
 * this width (a pre-existing cosmetic overlap, not something this PR
 * introduces — see this file's header comment), and widening this slot
 * only crops more of "TO: <name>" for no functional gain. See this
 * file's header comment ("SEND relocation") for why this is a straight
 * swap that costs no extra vertical budget. */
#define FF_COMPOSE_SEND_HEADER_W 64
_Static_assert(FF_COMPOSE_SEND_HEADER_W >= FF_THEME_MIN_HIT_PX, "header SEND must clear the 44px hit-target floor");

#define FF_COMPOSE_BUBBLE_Y (FF_COMPOSE_HEADER_Y + FF_COMPOSE_HEADER_H + 8)
#define FF_COMPOSE_BUBBLE_H 60

#define FF_COMPOSE_GRID_Y (FF_COMPOSE_BUBBLE_Y + FF_COMPOSE_BUBBLE_H + 8)
/* S17 slice b (AC2, docs/specs/S17-usability-hardening.md): was 6px — the
 * dense T9 grid is exactly the stress case that slice's task brief named,
 * and 6px measured under `FF_HIT_MIN_GAP_PX` (8px, ff_theme.h) on every
 * adjacent key pair, both within a row and between rows (this constant
 * doubles as both — see compose_build_keys below). Caught by
 * targets/sim/tests/test_face_hit_targets.c's new adjacency pass (72
 * violations, every compose_*.json fixture, all at gap=6.0px) before this
 * fix; bumped to 10px — comfortably above the floor, matching this
 * codebase's own other "modest gap" precedent (scr_settings.c's
 * FF_SETTINGS_ROW_GAP). Every key's own width/height stays well clear of
 * FF_THEME_MIN_HIT_PX at the new gap (verified against the real rendered
 * click-area, not hand math — PR #86 code review caught an earlier draft
 * of this comment quoting 64x46, a stale pre-fix number that never
 * accounted for the Y-position cascade the gap bump causes: the narrowest
 * row, bottom DEL/0/SEND, actually measures 50x46; row2 (7/8/9) is
 * 100x46, row1 (4/5/6) is 122x46, row0 (1/2/3) is 132x46 — see this
 * file's PR body for the full before/after). */
#define FF_COMPOSE_KEY_GAP 8  /* device follow-up: 10 -> 8 (the FF_HIT_MIN_GAP_PX floor); wider keys, tighter stack */
#define FF_COMPOSE_KEY_H   56 /* SEND-relocation audit: 54 -> 56, the reclaimed BOTTOM_ROW_GAP_EXTRA spent on height;
                                * >= FF_THEME_MIN_HIT_PX (assert below); see this file's header comment for the
                                * "why 56, not 57" chord-width search. */
#define FF_COMPOSE_BOTTOM_ROW_H 56 /* kept equal to FF_COMPOSE_KEY_H — see above */

/* DEL and MODE (the bottom row's two low-frequency neighbors) are pinned
 * to the exact hit floor; SPACE — by far this row's most-tapped key —
 * takes every pixel left over (computed at build time from the row's own
 * chord-derived width in compose_build_bottom_row, same "never hand
 * math" discipline as every other number here). See this file's header
 * comment, "SEND relocation". */
#define FF_COMPOSE_BOTTOM_SIDE_W FF_THEME_MIN_HIT_PX
_Static_assert(FF_COMPOSE_BOTTOM_SIDE_W >= FF_THEME_MIN_HIT_PX, "DEL/MODE must clear the 44px hit-target floor");

_Static_assert(FF_COMPOSE_KEY_H >= FF_THEME_MIN_HIT_PX, "compose grid keys must clear the 44px hit-target floor");
_Static_assert(FF_COMPOSE_BOTTOM_ROW_H >= FF_THEME_MIN_HIT_PX,
               "compose bottom row must clear the 44px hit-target floor");
_Static_assert(FF_COMPOSE_KEY_GAP >= FF_HIT_MIN_GAP_PX,
               "compose grid key gap (both within-row and between-row) must clear the adjacency floor");

/* Row 0 (keys 1-3), row 1 (keys 4-6), row 2 (keys 7-9), then the DEL/SPACE/MODE row
 * (SEND relocation: this row no longer carries SEND — see this file's header comment). */
#define FF_COMPOSE_ROW0_Y (FF_COMPOSE_GRID_Y)
#define FF_COMPOSE_ROW1_Y (FF_COMPOSE_ROW0_Y + FF_COMPOSE_KEY_H + FF_COMPOSE_KEY_GAP)
#define FF_COMPOSE_ROW2_Y (FF_COMPOSE_ROW1_Y + FF_COMPOSE_KEY_H + FF_COMPOSE_KEY_GAP)
#define FF_COMPOSE_BOTTOM_ROW_Y (FF_COMPOSE_ROW2_Y + FF_COMPOSE_KEY_H + FF_COMPOSE_KEY_GAP)

/* S15c guard: the whole T9 stack must stay inside the 412 puck square (the
 * bottom DEL/SPACE/MODE row is the lowest thing on this face). Square-fit
 * backstop only; the tighter in-circle key-width check the narrowing pole
 * imposes is enforced by test_face_hit_targets.c's sweep (it needs the chord
 * math, so it can't be a static assert). */
_Static_assert(FF_COMPOSE_BOTTOM_ROW_Y + FF_COMPOSE_BOTTOM_ROW_H <= FF_THEME_PUCK_PX,
               "compose T9 keypad's bottom row must stay inside the puck square");

/* ---------------------------------------------------------------------
 * Predictive-T9 (PRED) draft line + candidate strip geometry (S08
 * addendum, PR2). These REPLACE the surface message bubble in PRED mode
 * only — the design (firefly-design/composer/Main.dc.html, Artist.dc.html)
 * puts a compact one-line draft (committed ink + amber underlined
 * in-progress word + caret) above a horizontal candidate strip, sitting in
 * the same header-to-keypad band the bubble otherwise fills. ABC/123/SYM
 * keep the bubble untouched (byte-identical goldens).
 *
 * The strip's chips are NEW tap targets high on the face where the circle
 * is wide; each is sized to the FF_THEME_MIN_HIT_PX floor and the whole
 * strip clears the top keypad row by at least the adjacency floor — both
 * asserted below and swept by test_face_hit_targets.c against the real
 * rendered rects (the design's own ~30px pill height is below the 44px hit
 * floor, so the honest tappable chip is taller than the mockup pill; see
 * this PR's body). The draft line itself is a plain label — not clickable —
 * so it is exempt from the sweep and free to sit closer. */
#define FF_COMPOSE_PRED_DRAFT_Y  70                  /* one-line draft label, just under the header band (lifted with header 30->24) */
#define FF_COMPOSE_PRED_STRIP_Y  90                  /* candidate chip strip top (lifted to keep 8px clearance above the taller grid) */
#define FF_COMPOSE_PRED_CHIP_H   FF_THEME_MIN_HIT_PX /* 44px — the hit-target floor, taller than the mockup pill */
#define FF_COMPOSE_PRED_CHIP_GAP 10                  /* >= FF_HIT_MIN_GAP_PX; matches the keypad's own gap */

_Static_assert(FF_COMPOSE_PRED_CHIP_GAP >= FF_HIT_MIN_GAP_PX,
               "compose PRED candidate chip gap must clear the adjacency floor");
_Static_assert(FF_COMPOSE_PRED_STRIP_Y + FF_COMPOSE_PRED_CHIP_H + FF_HIT_MIN_GAP_PX <= FF_COMPOSE_GRID_Y,
               "compose PRED candidate strip must clear the top keypad row by the adjacency floor");

/* ---------------------------------------------------------------------
 * Chord-aware horizontal margin — see this file's header comment and
 * ff_layout.h's own doc comment for ff_layout_chord_half_width.
 * ------------------------------------------------------------------- */

/**
 * compose_safe_margin_x — thin int32_t/ceil wrapper around
 * ff_layout.h's shared `ff_layout_safe_margin_x`, bound to this puck's
 * own center/radius (ff_theme.h) and this file's safety buffer. Kept
 * local (not inlined at every call site) purely so the six call sites
 * below don't each repeat the `FF_THEME_PUCK_RADIUS_PX,
 * FF_THEME_PUCK_RADIUS_PX, FF_COMPOSE_SAFETY_PX` argument triple.
 */
static int32_t compose_safe_margin_x(int32_t top_y, int32_t h)
{
    float margin = ff_layout_safe_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                            (float)FF_THEME_PUCK_RADIUS_PX, FF_COMPOSE_SAFETY_PX);
    return (int32_t)ceilf(margin);
}

/* ---------------------------------------------------------------------
 * Static screen state.
 *
 * Same "one screen built per process" convention/hazard already
 * documented in scr_radar.c's top comment (headless mode renders one
 * frame and exits; window mode builds a screen once and lets LVGL's own
 * timer loop repaint it) — see that file's comment and
 * https://github.com/jakeholland/firefly/issues/17 for what breaks if
 * that assumption ever stops holding (re-render/rebuild at a live tick
 * rate, or tearing down and rebuilding this screen without a fresh
 * process). `ff_scr_compose_build` resets every static below at entry,
 * so at least repeated calls WITHIN one process (not currently done
 * anywhere) start from a clean slate rather than leaking the previous
 * call's compose session. `s_mode` is the one piece of state this file
 * still keeps: a build-time snapshot of `*compose`, needed to decide
 * which intent a keypress means (T9_KEY vs. T9_INSERT — see
 * compose_key_pressed) — not live state, since nothing here mutates it
 * anymore (S16 slice c3 moved the mode chip's cycling to the shell too;
 * see compose_mode_chip_click_cb).
 * ------------------------------------------------------------------- */
static ff_app_compose_mode_t s_mode;
static lv_obj_t            *s_bubble_label;
static lv_obj_t            *s_mode_chip_label;
static lv_obj_t            *s_keys_container;

/* ---------------------------------------------------------------------
 * Per-mode key legends (S08 Amendments). Index 0 is the bottom-row
 * SPACE/0 key; indices 1-9 are the 3x3 grid, row-major (1,2,3 / 4,5,6 /
 * 7,8,9), matching a standard phone keypad's reading order.
 * ------------------------------------------------------------------- */

static char const *const kAbcLegends[10] = {
    "SPACE", ". , ? !", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ",
};
static char const *const k123Legends[10] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};
/* SYM legends double as what actually gets inserted (ff_t9_insert_text
 * takes the literal ASCII shortcut) — tier-2 fallback, see this file's
 * header comment. Key 0 is SPACE, same as ABC (typed sentences with
 * emoticons still need spaces; only the 123 page repurposes 0 as a
 * literal digit, per the owner's ruling). */
static char const *const kSymLegends[10] = {
    "SPACE", "!", "?", ":)", ";)", "<3", ":o", ":/", "\\o/", "...",
};
/* PRED (predictive T9) legends — once-per-letter, same letter groupings as
 * ABC but pressed a single time each (the engine ranks whole words). Key 1
 * is the punctuation key; the design's "·,?" uses a middle dot (U+00B7),
 * which LVGL's built-in Montserrat subset does not carry (it would render
 * as a missing-glyph box), so this substitutes an ASCII period — noted as a
 * judgment call in this PR's body, same "no mockup font vendored" spirit as
 * ff_theme.h's own size substitutions. Key 0 is SPACE, same as ABC/SYM. */
static char const *const kPredLegends[10] = {
    "SPACE", ".,?", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ",
};

static char const *compose_legend_for(ff_app_compose_mode_t mode, uint8_t key)
{
    switch (mode) {
    case FF_APP_COMPOSE_ABC: return kAbcLegends[key];
    case FF_APP_COMPOSE_123: return k123Legends[key];
    case FF_APP_COMPOSE_SYM: return kSymLegends[key];
    case FF_APP_COMPOSE_PRED: return kPredLegends[key]; /* S08 predictive addendum (PR2) */
    }
    return "?";
}

static char const *compose_mode_name(ff_app_compose_mode_t m)
{
    switch (m) {
    case FF_APP_COMPOSE_ABC: return "ABC";
    case FF_APP_COMPOSE_123: return "123";
    case FF_APP_COMPOSE_SYM: return "SYM";
    case FF_APP_COMPOSE_PRED: return "T9"; /* maintainer decision 1: PRED chip label is "T9" */
    }
    return "?";
}

/* ---------------------------------------------------------------------
 * Bubble rendering.
 * ------------------------------------------------------------------- */

/* Renders `text`/`has_pending` into `label`. When a pending char is live,
 * it's recolored amber (FF_THEME_COLOR_AMBER, the app's one "in
 * progress" accent) so the bubble visibly distinguishes "committed" from
 * "still typing this character" without needing the reader to notice a
 * blinking cursor (this is a single static render — nothing here
 * animates) — satisfies S08's "message bubble with live pending
 * character" requirement declaratively instead of via a timer. */
static void compose_render_bubble_text(lv_obj_t *label, char const *text, bool has_pending)
{
    size_t n = (text != NULL) ? strlen(text) : 0;

    if (n == 0 && !has_pending) {
        lv_label_set_recolor(label, false);
        lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_label_set_text(label, "Type a message...");
        return;
    }

    lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_INK), 0);

    if (has_pending && n > 0) {
        char committed[FF_APP_COMPOSE_TEXT_LEN];
        size_t cn = n - 1;
        if (cn >= sizeof(committed)) cn = sizeof(committed) - 1;
        memcpy(committed, text, cn);
        committed[cn] = '\0';
        char pending_ch = text[n - 1];

        char buf[FF_APP_COMPOSE_TEXT_LEN + 16];
        snprintf(buf, sizeof(buf), "%s#%06x %c#", committed, (unsigned)FF_THEME_COLOR_AMBER, pending_ch);
        lv_label_set_recolor(label, true);
        lv_label_set_text(label, buf);
    } else {
        lv_label_set_recolor(label, false);
        lv_label_set_text(label, text);
    }
}

/* ---------------------------------------------------------------------
 * Keypad input handlers — S16 slice c3: every path below is a pure
 * emitter now (see this file's header comment). `s_mode` (the build-time
 * snapshot) decides which INTENT a keypress means, matching exactly what
 * the removed `ff_t9_*` calls used to do directly:
 *   ABC key 0    -> T9_SPACE          ABC key 1-9  -> T9_KEY(key)
 *   123 key 0-9  -> T9_INSERT("0".."9")  (still a literal digit insert,
 *                   same as the engine-side call this replaces)
 *   SYM key 0    -> T9_SPACE          SYM key 1-9  -> T9_INSERT(legend)
 * The shell interprets T9_KEY as multi-tap and T9_INSERT as an atomic
 * append (ff_t9_key / ff_t9_insert_text respectively) — this file never
 * calls either again.
 * ------------------------------------------------------------------- */

static void compose_key_pressed(uint8_t key)
{
    ff_intent_t in = {.kind = FF_INTENT_T9_SPACE, .u = {0}};

    switch (s_mode) {
    case FF_APP_COMPOSE_ABC:
        if (key == 0) {
            in.kind = FF_INTENT_T9_SPACE;
        } else {
            in.kind = FF_INTENT_T9_KEY;
            in.u.t9_key = key;
        }
        break;
    case FF_APP_COMPOSE_123: {
        char digit[2] = {(char)('0' + key), '\0'};
        in.kind = FF_INTENT_T9_INSERT;
        in.u.text = digit;
        ff_intent_emit(&in); /* emit here: `digit` doesn't outlive this block */
        return;
    }
    case FF_APP_COMPOSE_SYM:
        if (key == 0) {
            in.kind = FF_INTENT_T9_SPACE;
        } else {
            in.kind = FF_INTENT_T9_INSERT;
            in.u.text = kSymLegends[key];
        }
        break;
    case FF_APP_COMPOSE_PRED:
        /* S08 predictive addendum (PR2): once-per-letter. Same emit shape as
         * ABC — key 0 is SPACE, keys 1-9 are a single T9_KEY press each; the
         * shell interprets T9_KEY mode-polymorphically (predictive ranking in
         * PRED vs. multi-tap in ABC). The › cycle and per-chip tap-to-select
         * are separate controls (compose_cycle_click_cb / compose_cand_click_cb),
         * not keypad keys. */
        if (key == 0) {
            in.kind = FF_INTENT_T9_SPACE;
        } else {
            in.kind = FF_INTENT_T9_KEY;
            in.u.t9_key = key;
        }
        break;
    }

    ff_intent_emit(&in);
}

/* Candidate chip tap (PRED) -> FF_INTENT_T9_SELECT, carrying the tapped
 * candidate's index in u.t9_key (the maintainer's "reuse the t9 int field"
 * choice — see ff_intent.h). The index is stashed in the button's LVGL
 * user_data at build time, same pattern as compose_key_click_cb's key. */
static void compose_cand_click_cb(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_T9_SELECT, .u = {0}};
    in.u.t9_key = (uint8_t)idx;
    ff_intent_emit(&in);
}

/* › chip tap (PRED) -> FF_INTENT_T9_CYCLE: advance the engine's candidate
 * selection. No payload. */
static void compose_cycle_click_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_T9_CYCLE, .u = {0}};
    ff_intent_emit(&in);
}

static void compose_key_click_cb(lv_event_t *e)
{
    uintptr_t key = (uintptr_t)lv_event_get_user_data(e);
    compose_key_pressed((uint8_t)key);
}

static void compose_backspace_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_T9_BACKSPACE, .u = {0}};
    ff_intent_emit(&in);
}

/* SEND -> emits FF_INTENT_SEND_TEXT through the intent seam (S16 slice
 * c2 wired the emit; c3 wires the shell's handling of it). No payload:
 * the draft is shell-owned T9 state (`app/ff_shell.c`'s `compose_draft`)
 * as of this slice, so `ff_shell_intent`'s FF_INTENT_SEND_TEXT case
 * actually sends now, via `sh->wiring.sender` — see ff_shell.c. Routing
 * rule 4 ("a touch landing where SEND was does not send" while a takeover
 * is up) is enforced there, not here: this button still only ever reports
 * the tap. Unbound (goldens/headless), the emit is a no-op. */
static void compose_send_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_SEND_TEXT, .u = {0}};
    ff_intent_emit(&in);
}

/* Back "<" -> emits FF_INTENT_BACK through the intent seam (S16 slice
 * c1 — this replaces the issue-#23 stub). Which face is revealed is
 * still not this file's decision: the shell pops its modal route and
 * the next projection says what to render. Unbound (goldens/headless),
 * the emit is a no-op. */
static void compose_back_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_intent_emit(&in);
}

static void compose_update_mode_chip_label(void)
{
    if (s_mode_chip_label != NULL) {
        lv_label_set_text(s_mode_chip_label, compose_mode_name(s_mode));
    }
}

/* Mode chip "ABC"/"123"/"SYM" -> emits FF_INTENT_T9_MODE (S16 slice c3).
 * This used to cycle `s_mode` locally and rebuild the keypad in place for
 * instant feedback; now, like every other control in this file, it only
 * reports the tap — the shell owns the mode (so SEND/backspace/keys stay
 * consistent with whatever mode is actually current even across a
 * takeover interruption) and the next projection reaches this screen
 * through the render lifecycle (S16 slice d), not through a local
 * rebuild here. */
static void compose_mode_chip_click_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_T9_MODE, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Keypad construction. Built once, at ff_scr_compose_build entry — S16
 * slice c3 retired the local "clear and rebuild in place" path a mode
 * chip tap used to trigger; a live mode change now reaches this screen
 * only through the render lifecycle (S16 slice d), same as everything
 * else the shell owns.
 * ------------------------------------------------------------------- */

/* Light a key up on touch-DOWN so a press is unmistakable — the keypad felt
 * unresponsive without it (no on-press feedback made it hard to tell a tap
 * landed). A press brightens the key: an amber fill (the theme's "lit" colour)
 * with dark ink, applied to LV_STATE_PRESSED so LVGL shows it the instant the
 * finger is down and clears it on release. Works on any base colour. */
static void compose_key_press_feedback(lv_obj_t *btn, lv_obj_t *label)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    if (label != NULL) {
        lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_BG), LV_STATE_PRESSED);
    }
}

static lv_obj_t *compose_make_key(lv_obj_t *parent, char const *legend, int32_t x, int32_t y, int32_t w, int32_t h,
                                   uint8_t key, uint32_t bg_hex, uint32_t fg_hex)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, compose_key_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)key);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, legend);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    lv_obj_center(label);
    compose_key_press_feedback(btn, label);
    return btn;
}

/* Builds ONE row of the grid (either the 3-key letter/digit/symbol row,
 * or the DEL/0/SEND row conceptually — callers pass their own widths)
 * with `margin_x` computed per-row by the caller via
 * compose_safe_margin_x — see this file's header comment for why each
 * row gets its OWN margin instead of one uniform value for the whole
 * grid: the circle narrows as y moves away from center, so a margin
 * generous enough for the bottom row would waste usable width on rows
 * closer to center, while a margin sized for the top row would leave the
 * bottom row off-glass — exactly the original bug. */
static void compose_build_grid_row(lv_obj_t *container, ff_app_compose_mode_t mode, int32_t y, uint8_t first_key,
                                    int32_t margin_x)
{
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;
    int32_t key_w = (row_w - 2 * FF_COMPOSE_KEY_GAP) / 3;

    for (int32_t col = 0; col < 3; col++) {
        uint8_t key = (uint8_t)(first_key + col);
        int32_t x = margin_x + col * (key_w + FF_COMPOSE_KEY_GAP);
        compose_make_key(container, compose_legend_for(mode, key), x, y, key_w, FF_COMPOSE_KEY_H, key,
                          FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK);
    }
}

/* DEL / SPACE / MODE — SEND no longer lives in this row (see this file's
 * header comment, "SEND relocation": SEND moved to the header's top-right
 * corner specifically so it can never again sit edge-to-edge with SPACE).
 * DEL and MODE are pinned to FF_COMPOSE_BOTTOM_SIDE_W (the exact hit
 * floor); SPACE takes whatever width is left in the row once both gaps
 * and both side keys are accounted for — computed here from the row's
 * OWN chord-derived width, never hand-picked, same discipline as every
 * other size in this file. */
static void compose_build_bottom_row(lv_obj_t *container, ff_app_compose_mode_t mode, int32_t y, int32_t margin_x)
{
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;
    int32_t space_w = row_w - 2 * FF_COMPOSE_BOTTOM_SIDE_W - 2 * FF_COMPOSE_KEY_GAP;

    lv_obj_t *del = lv_button_create(container);
    lv_obj_remove_style_all(del);
    lv_obj_set_size(del, FF_COMPOSE_BOTTOM_SIDE_W, FF_COMPOSE_BOTTOM_ROW_H);
    lv_obj_set_pos(del, margin_x, y);
    lv_obj_set_style_bg_color(del, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(del, compose_backspace_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_lbl = lv_label_create(del);
    lv_label_set_text(del_lbl, "DEL");
    lv_obj_set_style_text_font(del_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(FF_THEME_COLOR_STALE_AMBER), 0);
    lv_obj_center(del_lbl);
    compose_key_press_feedback(del, del_lbl);

    int32_t space_x = margin_x + FF_COMPOSE_BOTTOM_SIDE_W + FF_COMPOSE_KEY_GAP;
    compose_make_key(container, compose_legend_for(mode, 0), space_x, y, space_w, FF_COMPOSE_BOTTOM_ROW_H, 0,
                      FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK);

    /* Mode chip — relocated here from the header (SEND relocation). Same
     * "always shows the current mode's name" contract the S08 Amendments
     * ruling requires (compose_update_mode_chip_label), just a new home:
     * mis-tapping it only cycles ABC/123/SYM/T9, never sends, so it's the
     * harmless neighbor SPACE can safely share this row with. */
    int32_t mode_x = space_x + space_w + FF_COMPOSE_KEY_GAP;
    lv_obj_t *mode_chip = lv_button_create(container);
    lv_obj_remove_style_all(mode_chip);
    lv_obj_set_size(mode_chip, FF_COMPOSE_BOTTOM_SIDE_W, FF_COMPOSE_BOTTOM_ROW_H);
    lv_obj_set_pos(mode_chip, mode_x, y);
    lv_obj_set_style_bg_color(mode_chip, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(mode_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mode_chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(mode_chip, compose_mode_chip_click_cb, LV_EVENT_CLICKED, NULL);
    s_mode_chip_label = lv_label_create(mode_chip);
    lv_obj_set_style_text_font(s_mode_chip_label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_mode_chip_label, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_center(s_mode_chip_label);
    compose_key_press_feedback(mode_chip, s_mode_chip_label);
    compose_update_mode_chip_label();
}

/* Builds the 3x3 letter/digit/symbol grid (keys 1-9) plus the DEL / 0 /
 * SEND bottom row into `container` — one row at a time, each with its
 * OWN chord-derived margin (see compose_safe_margin_x).
 *
 * `container` itself is positioned at puck-local y = FF_COMPOSE_GRID_Y
 * (set in ff_scr_compose_build), so every child position passed to
 * `lv_obj_set_pos` below must be CONTAINER-relative (subtract
 * FF_COMPOSE_GRID_Y) — but compose_safe_margin_x's circle math needs the
 * ABSOLUTE puck-local y (the circle's center is defined in that space,
 * not the container's). The FF_COMPOSE_ROW*_Y / FF_COMPOSE_BOTTOM_ROW_Y
 * constants are absolute; this function is the one place that converts
 * between the two spaces, so that conversion can't drift out of sync
 * between the margin calculation and the actual child placement. */
static void compose_build_keys(lv_obj_t *container, ff_app_compose_mode_t mode)
{
    int32_t margin_row0 = compose_safe_margin_x(FF_COMPOSE_ROW0_Y, FF_COMPOSE_KEY_H);
    int32_t margin_row1 = compose_safe_margin_x(FF_COMPOSE_ROW1_Y, FF_COMPOSE_KEY_H);
    int32_t margin_row2 = compose_safe_margin_x(FF_COMPOSE_ROW2_Y, FF_COMPOSE_KEY_H);
    int32_t margin_bottom = compose_safe_margin_x(FF_COMPOSE_BOTTOM_ROW_Y, FF_COMPOSE_BOTTOM_ROW_H);

    compose_build_grid_row(container, mode, FF_COMPOSE_ROW0_Y - FF_COMPOSE_GRID_Y, 1, margin_row0);
    compose_build_grid_row(container, mode, FF_COMPOSE_ROW1_Y - FF_COMPOSE_GRID_Y, 4, margin_row1);
    compose_build_grid_row(container, mode, FF_COMPOSE_ROW2_Y - FF_COMPOSE_GRID_Y, 7, margin_row2);
    compose_build_bottom_row(container, mode, FF_COMPOSE_BOTTOM_ROW_Y - FF_COMPOSE_GRID_Y, margin_bottom);
}

/* ---------------------------------------------------------------------
 * Predictive-T9 (PRED) draft line + candidate strip. HONEST-DATA
 * (CLAUDE.md): this renders ONLY what `*compose` carries — the amber word
 * is `compose->word` verbatim (never a fabricated word, never a key
 * string), the ★ badge comes straight from `cand[i].from_pack`, the ›
 * chip appears only when `total_cand > n_cand` (real extra matches exist),
 * and on an honest no-match nothing is invented. See scr_compose.c's split
 * comment: the initial paint is a pure function of the snapshot.
 * ------------------------------------------------------------------- */

/* The draft line: committed text (ink) + the in-progress predicted word
 * (amber, underlined) + a caret. On word_nomatch: committed text + a
 * neutral caret and NO amber word (never show a fabricated in-progress
 * word). Cold open (nothing committed, no word, not a no-match): the same
 * dim "Type a message..." placeholder the bubble uses, so the empty state
 * reads identically across modes. Built as a centered flex row of separate
 * labels rather than one recolored string specifically so the in-progress
 * word can carry a real underline decoration (recolor changes color only);
 * the amber color still comes from the same FF_THEME_COLOR_AMBER accent the
 * bubble's pending-char treatment uses. */
static void compose_build_pred_draft(lv_obj_t *puck, ff_app_compose_t const *compose)
{
    lv_obj_t *cont = lv_obj_create(puck);
    lv_obj_remove_style_all(cont);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_column(cont, 0, 0);
    lv_obj_set_size(cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, FF_COMPOSE_PRED_DRAFT_Y);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE); /* a display line, not a control */

    bool cold_open = (compose->text[0] == '\0' && compose->word[0] == '\0' && !compose->word_nomatch);
    if (cold_open) {
        lv_obj_t *ph = lv_label_create(cont);
        lv_label_set_text(ph, "Type a message...");
        lv_obj_set_style_text_font(ph, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(ph, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        return;
    }

    if (compose->text[0] != '\0') {
        lv_obj_t *committed = lv_label_create(cont);
        lv_label_set_text(committed, compose->text);
        lv_obj_set_style_text_font(committed, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(committed, lv_color_hex(FF_THEME_COLOR_INK), 0);
    }

    if (!compose->word_nomatch && compose->word[0] != '\0') {
        lv_obj_t *word = lv_label_create(cont);
        lv_label_set_text(word, compose->word);
        lv_obj_set_style_text_font(word, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(word, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_text_decor(word, LV_TEXT_DECOR_UNDERLINE, 0);
    }

    lv_obj_t *caret = lv_label_create(cont);
    lv_label_set_text(caret, "|");
    lv_obj_set_style_text_font(caret, FF_THEME_FONT_LABEL, 0);
    /* Amber caret while a prediction is live; a neutral (ink) caret on an
     * honest no-match, so the caret never lends the amber "in progress"
     * signal to a state that has no predicted word. */
    lv_obj_set_style_text_color(caret, lv_color_hex(compose->word_nomatch ? FF_THEME_COLOR_INK : FF_THEME_COLOR_AMBER),
                                 0);
}

/* One candidate chip. `selected` gets the amber filled treatment; the rest
 * are surface chips. `from_pack` prefixes a ★ (festpack vocabulary badge —
 * an ASCII '*' stand-in, since LVGL's built-in Montserrat carries no star
 * glyph; noted as a judgment call in the PR body). Sized to the hit-target
 * floor in BOTH axes (height fixed at the floor; a min-width floor covers
 * short words like "vie") so a real thumb always has a 44px target even
 * though the mockup pill is visually smaller. */
static void compose_make_cand_chip(lv_obj_t *strip, char const *text, bool from_pack, bool selected, uint8_t index)
{
    lv_obj_t *chip = lv_button_create(strip);
    lv_obj_remove_style_all(chip);
    lv_obj_set_height(chip, FF_COMPOSE_PRED_CHIP_H);
    lv_obj_set_width(chip, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(chip, FF_THEME_MIN_HIT_PX, 0);
    lv_obj_set_style_pad_hor(chip, 14, 0);
    lv_obj_set_style_radius(chip, 12, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(selected ? FF_THEME_COLOR_AMBER : FF_THEME_COLOR_SURFACE), 0);
    lv_obj_add_event_cb(chip, compose_cand_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    char buf[FF_APP_COMPOSE_WORD_LEN + 4];
    if (from_pack) {
        snprintf(buf, sizeof(buf), "* %s", text);
    } else {
        snprintf(buf, sizeof(buf), "%s", text);
    }
    lv_obj_t *lbl = lv_label_create(chip);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(selected ? FF_THEME_COLOR_BG : FF_THEME_COLOR_INK), 0);
    lv_obj_center(lbl);
    if (selected) {
        lv_obj_set_style_bg_opa(chip, LV_OPA_60, LV_STATE_PRESSED); /* already amber — dim on press */
    } else {
        compose_key_press_feedback(chip, lbl);
    }
}

/* The candidate strip: a centered horizontal row of chips from
 * `cand[0..n_cand)`, plus a trailing › chip when more matches exist than
 * are shown. On a no-match, a single dim "no match" affordance (not a chip,
 * not clickable) — never fabricated candidates. On the cold open
 * (n_cand==0, not a no-match), nothing at all. */
static void compose_build_pred_strip(lv_obj_t *puck, ff_app_compose_t const *compose)
{
    if (compose->word_nomatch) {
        lv_obj_t *nm = lv_label_create(puck);
        lv_label_set_text(nm, "no match");
        lv_obj_set_style_text_font(nm, FF_THEME_FONT_CHIP, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, FF_COMPOSE_PRED_STRIP_Y + (FF_COMPOSE_PRED_CHIP_H - 16) / 2);
        return;
    }
    if (compose->n_cand == 0) {
        return; /* cold open — no strip, never an invented chip */
    }

    lv_obj_t *strip = lv_obj_create(puck);
    lv_obj_remove_style_all(strip);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_style_pad_column(strip, FF_COMPOSE_PRED_CHIP_GAP, 0);
    lv_obj_set_size(strip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(strip, LV_ALIGN_TOP_MID, 0, FF_COMPOSE_PRED_STRIP_Y);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE); /* a layout row; its chips are the controls */

    /* PR1 review nit: sel_cand is a uint8 that can exceed the shown chips
     * (the selection cycled past the window). Guard it — highlight none
     * when sel_cand >= n_cand, and let `word` (rendered in the draft above)
     * stand as the authoritative selection. */
    bool highlight = (compose->sel_cand < compose->n_cand);
    for (uint8_t i = 0; i < compose->n_cand; i++) {
        compose_make_cand_chip(strip, compose->cand[i].text, compose->cand[i].from_pack,
                                highlight && (i == compose->sel_cand), i);
    }

    /* › chip: only when the engine really has more matches than are shown
     * (honest `total_cand`). Emits T9_CYCLE. */
    if (compose->total_cand > compose->n_cand) {
        lv_obj_t *more = lv_button_create(strip);
        lv_obj_remove_style_all(more);
        lv_obj_set_height(more, FF_COMPOSE_PRED_CHIP_H);
        lv_obj_set_width(more, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(more, FF_THEME_MIN_HIT_PX, 0);
        lv_obj_set_style_pad_hor(more, 14, 0);
        lv_obj_set_style_radius(more, 12, 0);
        lv_obj_set_style_bg_opa(more, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(more, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
        lv_obj_add_event_cb(more, compose_cycle_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(more);
        lv_label_set_text(lbl, ">");
        lv_obj_set_style_text_font(lbl, FF_THEME_FONT_CHIP, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_center(lbl);
        compose_key_press_feedback(more, lbl);
    }
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_compose_build(ff_app_compose_t const *compose)
{
    if (compose == NULL) {
        return;
    }

    s_mode = compose->mode;
    s_bubble_label = NULL;
    s_mode_chip_label = NULL;
    s_keys_container = NULL;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE); /* base lv_obj defaults clickable; this one is a plain backdrop */

    /* --- Header: back (dead-end escape, ux-raver checklist item 6) / TO / SEND. ---
     * Margin derived from the header row's own y band, per this file's
     * header comment — NOT the flat "16, 10" pixel offsets the original
     * (off-glass) version used. SEND relocation: this top-right slot used
     * to hold the mode chip; SEND and the mode chip traded places (see
     * this file's header comment) so SEND is never again adjacent to
     * SPACE. */
    int32_t header_margin = compose_safe_margin_x(FF_COMPOSE_HEADER_Y, FF_COMPOSE_HEADER_H);

    lv_obj_t *back = lv_button_create(puck);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, FF_THEME_MIN_HIT_PX, FF_THEME_MIN_HIT_PX);
    lv_obj_set_pos(back, header_margin, FF_COMPOSE_HEADER_Y);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(back, compose_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "<");
    lv_obj_set_style_text_font(back_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_center(back_lbl);
    compose_key_press_feedback(back, back_lbl); /* transparent normally; amber fill on press */

    lv_obj_t *to_lbl = lv_label_create(puck);
    char to_buf[FF_APP_NAME_LEN + 8];
    snprintf(to_buf, sizeof(to_buf), "TO: %s", (compose->to_name[0] != '\0') ? compose->to_name : "EVERYONE");
    lv_label_set_text(to_lbl, to_buf);
    lv_obj_set_style_text_font(to_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(to_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(to_lbl, LV_ALIGN_TOP_MID, 0, FF_COMPOSE_HEADER_Y + 12);

    /* SEND — the far top-right corner, the watch-composer convention, and
     * (per this file's header comment) ~269px of vertical separation from
     * SPACE at the bottom of the keypad: no shared row, no shared edge,
     * nowhere close to a mis-tap. Same amber CTA styling / press-dim
     * treatment SEND always had; only its position moved. */
    lv_obj_t *send = lv_button_create(puck);
    lv_obj_remove_style_all(send);
    lv_obj_set_size(send, FF_COMPOSE_SEND_HEADER_W, FF_THEME_MIN_HIT_PX);
    lv_obj_set_pos(send, FF_THEME_PUCK_PX - header_margin - FF_COMPOSE_SEND_HEADER_W, FF_COMPOSE_HEADER_Y);
    lv_obj_set_style_bg_color(send, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(send, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(send, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(send, compose_send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_lbl = lv_label_create(send);
    lv_label_set_text(send_lbl, "SEND");
    lv_obj_set_style_text_font(send_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_center(send_lbl);
    /* SEND is already amber, so it dims on press instead of lighting up. */
    lv_obj_set_style_bg_opa(send, LV_OPA_60, LV_STATE_PRESSED);

    /* --- Draft area. ---
     * PRED replaces the surface bubble with the compact predictive draft
     * line + candidate strip (S08 addendum, PR2). ABC/123/SYM keep the
     * bubble byte-for-byte — the block below is unchanged from before this
     * addendum, only guarded by the mode branch. */
    if (s_mode == FF_APP_COMPOSE_PRED) {
        compose_build_pred_draft(puck, compose);
        compose_build_pred_strip(puck, compose);
    } else {
        /* --- Message bubble (ABC/123/SYM). --- */
        int32_t bubble_margin = compose_safe_margin_x(FF_COMPOSE_BUBBLE_Y, FF_COMPOSE_BUBBLE_H);
        lv_obj_t *bubble = lv_obj_create(puck);
        lv_obj_remove_style_all(bubble);
        lv_obj_set_size(bubble, FF_THEME_PUCK_PX - 2 * bubble_margin, FF_COMPOSE_BUBBLE_H);
        lv_obj_set_pos(bubble, bubble_margin, FF_COMPOSE_BUBBLE_Y);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bubble, 14, 0);
        lv_obj_set_style_pad_all(bubble, 12, 0);
        lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE); /* a display box, not a control */

        s_bubble_label = lv_label_create(bubble);
        lv_label_set_long_mode(s_bubble_label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(s_bubble_label, FF_THEME_PUCK_PX - 2 * bubble_margin - 24);
        lv_obj_set_style_text_font(s_bubble_label, FF_THEME_FONT_LABEL, 0);
        lv_obj_align(s_bubble_label, LV_ALIGN_TOP_LEFT, 0, 0);
        compose_render_bubble_text(s_bubble_label, compose->text, compose->has_pending);
    }

    /* --- Keypad. --- */
    s_keys_container = lv_obj_create(puck);
    lv_obj_remove_style_all(s_keys_container);
    lv_obj_set_size(s_keys_container, FF_THEME_PUCK_PX,
                     (FF_COMPOSE_BOTTOM_ROW_Y + FF_COMPOSE_BOTTOM_ROW_H) - FF_COMPOSE_GRID_Y);
    lv_obj_set_pos(s_keys_container, 0, FF_COMPOSE_GRID_Y);
    lv_obj_clear_flag(s_keys_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_keys_container, LV_OBJ_FLAG_CLICKABLE); /* a layout container, not a control itself */
    compose_build_keys(s_keys_container, s_mode);
}
