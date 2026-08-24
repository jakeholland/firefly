/**
 * radar_layout.h — pure-geometry layout resolver for the radar face.
 *
 * PR #16 UX review round 3 + code review round 2 (both apply to the same
 * rework): rectangle-by-rectangle "push to nearest edge" does not
 * converge when two reserved regions overlap each other (push-out-of-A
 * lands inside B, push-out-of-B lands back inside A — an oscillation a
 * fixed pass cap just stops mid-way through, exiting still-colliding).
 * The code reviewer proved this empirically: sweeping all 3600
 * tenth-degree bearings against the old algorithm's actual declared
 * rects left ~35% of bearings still resting inside a keep-out region,
 * including in this repo's own shipped regression fixture.
 *
 * This version replaces iterative push with SEARCH: every candidate
 * position is tested against the FULL reserved-region set in one shot
 * (a true union test, not one rectangle at a time), so overlapping
 * reserved rectangles cannot cause oscillation — there is nothing to
 * oscillate between, only "does this candidate work, yes or no".
 *   - The arrow resolves by shortening along its fixed bearing (a 1-D
 *     monotonic search over length) until its head clears the union, or
 *     it hits its floor. It never moves off-axis — CLAUDE.md's honesty
 *     rule: an arrow may go short, it must never point somewhere it
 *     doesn't mean.
 *   - Each ring dot resolves by searching for the nearest CLEAR ANGLE
 *     at the ring's fixed radius (also 1-D, over angle instead of
 *     length), tested the same way.
 * Both searches use a fixed step size and a fixed maximum bound, so both
 * always terminate in a fixed number of candidate tests, regardless of
 * how many reserved rectangles exist or how they overlap.
 *
 * ORCHESTRATOR RULING (round 4): dots that still end up at the same
 * resolved position after that search — i.e. several crew members
 * converged on nearly the same bearing — are never hidden. Hiding a
 * crew member the app knows about is an honesty failure (CLAUDE.md:
 * "unknown = explicitly unknown... never fake freshness, positions, or
 * times" — silently dropping a *known* member from the ring is the same
 * category of lie in the other direction: pretending fewer people are
 * nearby than the app actually knows about). Instead, dots that collide
 * with each other are merged into a single CLUSTER marker showing how
 * many crew members it represents — see radar_layout_resolve_dots.
 *
 * No LVGL dependency whatsoever — plain C11 + <math.h> — specifically so
 * this is unit-testable (app/screens/tests/test_radar_layout.c) with
 * real geometric assertions swept across the full bearing space, not
 * only a golden PNG a human has to notice a few overlapping pixels in.
 * scr_radar.c (the render layer) draws exactly the coordinates this
 * module resolves — it does not recompute or duplicate any placement
 * decision, so the thing under test is provably the same geometry that
 * ends up on screen.
 */
#ifndef FF_RADAR_LAYOUT_H
#define FF_RADAR_LAYOUT_H

#include <stdbool.h>

#include "ff_radar.h" /* radar_mode_t, FF_CREW_MAX (via ff_crew.h) */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Geometry constants. Canonical here (not duplicated in ff_theme.h or
 * scr_radar.c) — both the registry builder below and scr_radar.c's
 * render functions use these same values, so what gets collision-tested
 * is exactly what gets drawn.
 * ------------------------------------------------------------------- */

#define RADAR_LAYOUT_ARROW_LEN_PX 140.0f     /* S06 spec: "arrow 140 px glyph" — full length before shortening */
#define RADAR_LAYOUT_ARROW_MIN_LEN_PX 20.0f  /* never shorten past this — a nub, not nothing */
#define RADAR_LAYOUT_ARROW_HEAD_LEN_PX 46.0f
#define RADAR_LAYOUT_ARROW_HEAD_WIDTH_PX 34.0f
#define RADAR_LAYOUT_RING_RADIUS_PX 185.0f
#define RADAR_LAYOUT_DOT_PX 34.0f

/* Angular search bounds for both the arrow-shortening search and the
 * per-dot clear-angle search — see radar_layout_resolve_arrow /
 * radar_layout_resolve_dots. */
#define RADAR_LAYOUT_ANGLE_STEP_DEG 2.0f
#define RADAR_LAYOUT_MAX_ANGLE_OFFSET_DEG 180.0f

/* Cross-mode chrome, present on every render. */
#define RADAR_LAYOUT_STATUS_BAR_DY (-195.0f)
#define RADAR_LAYOUT_PAGE_DOT_DY 186.0f

/* LIVE / STALE / LOST-with-a-real-fix shared vertical stack. */
#define RADAR_LAYOUT_STACK_NAME_DY 60.0f
#define RADAR_LAYOUT_STACK_DIST_DY 100.0f
#define RADAR_LAYOUT_STACK_CHIP_DY 148.0f

/* LOST, never fixed. */
#define RADAR_LAYOUT_NEVER_HEADLINE_DY (-10.0f)
#define RADAR_LAYOUT_NEVER_NAME_DY 40.0f
#define RADAR_LAYOUT_NEVER_SUB_DY 90.0f

/* NOFIX. */
#define RADAR_LAYOUT_NOFIX_HEADLINE_DY (-10.0f)
#define RADAR_LAYOUT_NOFIX_SUB_DY 40.0f
#define RADAR_LAYOUT_NOFIX_CHIP_DY 80.0f

/* NOSEL. */
#define RADAR_LAYOUT_NOSEL_HEADLINE_DY (-10.0f)
#define RADAR_LAYOUT_NOSEL_SUB_DY 40.0f

/* CLOSE. */
#define RADAR_LAYOUT_CLOSE_RING_CY (-55.0f)
#define RADAR_LAYOUT_CLOSE_RING_MAX_R 90.0f
#define RADAR_LAYOUT_CLOSE_NAME_DY 59.0f
#define RADAR_LAYOUT_CLOSE_CHIP_DY 98.0f
#define RADAR_LAYOUT_CLOSE_FLARE_DY 147.0f

/** An axis-aligned rectangle, center-relative (puck center = (0,0)). */
typedef struct {
    float x1, y1, x2, y2;
} radar_layout_rect_t;

#define RADAR_LAYOUT_MAX_RESERVED 8

/**
 * radar_layout_registry_t — every fixed chrome rectangle for one render
 * of one mode. Built by radar_layout_build_registry, BEFORE any movable
 * element is resolved. Rectangles in the same registry are allowed to
 * overlap each other — the search-based resolvers below test candidates
 * against the union of the whole list, so overlap between reserved
 * rectangles is not a hazard the way it was for the old push-based
 * algorithm.
 */
typedef struct {
    radar_layout_rect_t rects[RADAR_LAYOUT_MAX_RESERVED];
    int count;
} radar_layout_registry_t;

/**
 * radar_layout_build_registry — the fixed-chrome rectangle list for
 * `mode` (and, for RADAR_LOST, `never_fixed` per ff_radar.h's RENDERER
 * CONTRACT). Rectangles are generous, matched to (never narrower than)
 * what scr_radar.c actually draws at each named *_DY constant above.
 * Always includes the status bar and page-dot row.
 */
void radar_layout_build_registry(radar_mode_t mode, bool never_fixed, radar_layout_registry_t *out);

/** Resolved arrow geometry, center-relative — everything scr_radar.c
 * needs to draw the tail and the (filled or outline) triangular head
 * without recomputing any of this module's trigonometry itself. */
typedef struct {
    float tip_dx, tip_dy;
    float base_dx, base_dy; /* head base == tail end */
    float left_dx, left_dy; /* head's two base corners */
    float right_dx, right_dy;
    float len_px;    /* the (possibly-shortened) tip radius actually used */
    bool shortened;  /* true if it had to give up length to clear the registry */
} radar_layout_arrow_t;

/**
 * radar_layout_resolve_arrow — the arrow's on-screen geometry for
 * `arrow_deg`. A 1-D monotonic search over length: starts at
 * RADAR_LAYOUT_ARROW_LEN_PX, and on each step tests the CURRENT
 * candidate's tip and both head-base corners against the full registry
 * union (a fresh test every step, not an incremental push), shortening
 * by a fixed amount until every point clears every rectangle or the
 * length hits RADAR_LAYOUT_ARROW_MIN_LEN_PX. The bearing itself never
 * changes. Bounded by a hard iteration cap — terminates in a fixed
 * maximum number of steps regardless of `*reg`'s contents.
 */
void radar_layout_resolve_arrow(radar_layout_registry_t const *reg, float arrow_deg, radar_layout_arrow_t *out);

/**
 * radar_layout_dot_result_t — one original ring dot's resolved marker.
 * Dots that end up needing to share a marker (see
 * radar_layout_resolve_dots) get the SAME (dx, dy, cluster_id,
 * cluster_size): the caller groups original indices by `cluster_id` and
 * renders exactly one visual marker per distinct id — a plain dot when
 * `cluster_size == 1`, a count marker when it's greater. Every original
 * index is always present in exactly one group; none are ever dropped.
 */
typedef struct {
    float dx, dy;      /* the group's shared, registry-clear anchor position */
    int cluster_id;    /* == the lowest original index in this dot's group */
    int cluster_size;  /* how many original dots (including this one) share that marker */
} radar_layout_dot_result_t;

/**
 * radar_layout_resolve_dots — placement for `n` ring dots at
 * `ring_deg[0..n)` (`n` <= FF_CREW_MAX; `out` must have room for `n`
 * entries). Two phases:
 *   1. Each dot independently searches for the nearest CLEAR ANGLE to
 *      its own bearing, at the fixed RADAR_LAYOUT_RING_RADIUS_PX radius:
 *      alternating +/- RADAR_LAYOUT_ANGLE_STEP_DEG offsets from its
 *      starting bearing, each candidate tested against the full registry
 *      union, up to +/- RADAR_LAYOUT_MAX_ANGLE_OFFSET_DEG. The radius
 *      never changes, so every resolved dot stays exactly
 *      RADAR_LAYOUT_RING_RADIUS_PX from center — well inside any sane
 *      puck radius by construction, no separate puck-boundary clamp
 *      needed. Bounded by the fixed step/offset — terminates in a fixed
 *      maximum number of candidate tests regardless of `*reg`.
 *   2. Union-find clustering (not hiding — see this header's top
 *      comment): any two dots whose phase-1 positions land within
 *      RADAR_LAYOUT_DOT_PX of each other are merged into one group, with
 *      transitive closure (A close to B, B close to C => all three
 *      merge, even if A and C aren't directly close). A group's anchor
 *      position is its lowest-original-index member's own (already
 *      registry-clear) position — never a re-averaged point that could
 *      reintroduce a collision. This is exhaustive pairwise comparison
 *      over at most FF_CREW_MAX dots (a handful of iterations either
 *      way) — no iterative convergence risk, so it's trivially bounded.
 */
void radar_layout_resolve_dots(radar_layout_registry_t const *reg, float const *ring_deg, int n,
                                radar_layout_dot_result_t *out);

/* -------------------------------------------------------------------
 * Cluster marker ring (issue #18).
 *
 * A cluster marker used to be drawn as a plain white outline around a
 * digit. That is the visual vocabulary of a NOTIFICATION BADGE, not of
 * this app's crew: every other crew element on the ring is
 * colour + letter, so the one marker that means "several of your
 * friends are standing together over there" was the one marker that
 * looked like generic chrome (PR #16 UX review; the reviewer's words:
 * it "reads closer to generic chrome than to a crew dot").
 *
 * The fix is to spend the marker's ring on the members themselves: one
 * WEDGE per clustered member, in that member's own crew colour, so the
 * marker's shape says "friends" before the digit is parsed. The count
 * digit stays — the clustering ruling ("cluster, never hide") is
 * unchanged and every member is still represented in exactly one
 * marker; the wedges just make the marker say WHICH friends, which the
 * neutral version could not say at all.
 * ------------------------------------------------------------------- */

/** Ring thickness of a cluster marker's wedges, in px — one value per
 * freshness, mirroring the treatment a LONE dot already uses for the
 * same fact. Sized against RADAR_LAYOUT_DOT_PX so the wedges live INSIDE
 * the marker's existing footprint: the marker stays exactly one
 * dot-diameter wide, so nothing about the collision geometry above
 * changes, and the count digit still has clear middle to sit in.
 *
 * PR #41 UX review, BLOCKING 3. The first pass drew every wedge at one
 * thickness and expressed staleness by crushing its OPACITY to 40%. That
 * fails two ways. It is *relative* — a dimmed wedge only reads as dimmed
 * next to a bright one, so a cluster where EVERY member is stale looks
 * like an ordinary live marker and the user walks toward a
 * forty-minute-old fix. And it is the WRONG IDIOM: a lone stale dot is
 * hollow-and-bright (transparent fill, 2px crew-colored ring), so the
 * face teaches "hollow means old" everywhere except here, where the
 * first pass taught "dark means old" — and dark on a near-black puck
 * reads as dropping out, a scarier and different fact than the true one.
 * Measured: the stale violet wedge came out at 2.2:1 against the puck
 * background, i.e. effectively invisible at arm's length.
 *
 * So freshness is now carried by THICKNESS at full color, which is
 * absolute (it needs no neighbour to compare against) and matches the
 * lone dot exactly: STALE_W is the lone ghost dot's own 2px border, at
 * the same outer radius (lv_arc puts an arc's outer edge at the object
 * bound, just as a border sits at the object edge). */
#define RADAR_LAYOUT_CLUSTER_RING_W_PX 6.0f
#define RADAR_LAYOUT_CLUSTER_RING_STALE_W_PX 2.0f

/** Angular gap between adjacent wedges, in degrees — the dark marker
 * fill showing through, so two adjacent members' colours read as two
 * wedges rather than one blended ring. Applied only when there is more
 * than one wedge (a single wedge would be a lone dot, which never takes
 * this path). */
#define RADAR_LAYOUT_CLUSTER_WEDGE_GAP_DEG 12.0f

/**
 * radar_layout_wedge_t — one clustered member's slice of a cluster
 * marker's ring. Angles are in LVGL's arc convention (0 degrees = 3
 * o'clock, increasing CLOCKWISE, always normalized into [0, 360)) so the
 * render layer can hand them to lv_arc_set_bg_angles() unmodified.
 *
 * `end_deg` may be NUMERICALLY LESS than `start_deg` — that is a wedge
 * that crosses the 0/360 seam, which LVGL draws correctly (lv_arc.c:
 * "if(bg_angle_end < bg_angle_start) bg_end += 360") and which is
 * unavoidable given the first wedge starts at 12 o'clock. Callers must
 * not "fix" it by swapping the two.
 *
 * The ONE angle outside [0, 360) is a full ring — `{0, 360}`, LVGL's own
 * complete-arc form, emitted only for a single-member "cluster". A
 * normalized `{270, 270}` would be indistinguishable from a zero-width
 * wedge, so it is deliberately not normalized.
 */
typedef struct {
    int index;       /* original dot index (into the caller's dots array) this wedge represents */
    float start_deg; /* inclusive start, [0, 360) */
    float end_deg;   /* exclusive end, [0, 360) — or exactly 360 for a full ring; < start_deg iff it crosses the seam */
} radar_layout_wedge_t;

/**
 * radar_layout_cluster_wedges — the per-member wedges for the cluster
 * anchored at `cluster_id`, given the full `resolved[0..n)` array
 * radar_layout_resolve_dots produced. Returns how many wedges it wrote,
 * or **-1 if the cluster has more members than `out_max` can hold** —
 * it never writes a PARTIAL ring. Emitting out_max wedges and reporting
 * the short count would let this function satisfy its own signature by
 * hiding a crew member, which is the exact lie by omission the
 * clustering ruling above exists to prevent (PR #41 code review).
 * Callers should treat a negative return as "draw no wedges", degrading
 * to the plain count marker rather than to a ring showing some friends
 * and not others.
 *
 * Members are emitted in ascending ORIGINAL DOT INDEX order — the same
 * order the caller's own `dots` array is in, and a stable one, so the
 * same crew standing in the same place always produces the same-looking
 * marker rather than a ring that reshuffles its colours between frames.
 * `out[k].index` is the original index, so the caller looks up that
 * member's crew colour and freshness itself (this module knows nothing
 * about either — it is pure geometry, no LVGL, no theme).
 *
 * The ring is divided into equal slices starting at 12 o'clock, each
 * inset by half of RADAR_LAYOUT_CLUSTER_WEDGE_GAP_DEG at both ends.
 * Twelve o'clock is the start so a two-member cluster splits
 * left/right — two people side by side — rather than top/bottom.
 *
 * Returns 0 (writing nothing) for a NULL `resolved`/`out`, a non-positive
 * `n`/`out_max`, or a `cluster_id` no dot belongs to. Passing a
 * `cluster_id` whose cluster has only one member returns a single
 * `{0, 360}` full-ring wedge with no gap — well-defined, though the
 * render layer never takes this path (a lone dot draws its own initial
 * instead).
 */
int radar_layout_cluster_wedges(radar_layout_dot_result_t const *resolved, int n, int cluster_id,
                                 radar_layout_wedge_t *out, int out_max);

#ifdef __cplusplus
}
#endif

#endif /* FF_RADAR_LAYOUT_H */
