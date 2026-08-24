/**
 * radar_layout.c — see radar_layout.h. Pure C11 geometry, no LVGL.
 */
#include "radar_layout.h"

#include <math.h>

#define RADAR_LAYOUT_PI 3.14159265358979323846f

/* Direction convention shared with ff_radar_compute/ff_geo: 0 deg =
 * straight "up" (ahead), clockwise. */
static void deg_to_offset(float deg, float radius, float *dx, float *dy)
{
    float rad = deg * (RADAR_LAYOUT_PI / 180.0f);
    *dx = radius * sinf(rad);
    *dy = -radius * cosf(rad);
}

static bool point_in_rect(radar_layout_rect_t const *r, float x, float y, float margin)
{
    return x >= r->x1 - margin && x <= r->x2 + margin && y >= r->y1 - margin && y <= r->y2 + margin;
}

/* Tests `(x,y)` against the FULL registry in one shot — a true union
 * test, not one rectangle at a time. This is what makes overlapping
 * reserved rectangles harmless: there is no notion of "push out of A,
 * which happens to be inside B" here, only "is this single candidate
 * point clear of everything, yes or no". */
static bool point_in_registry(radar_layout_registry_t const *reg, float x, float y, float margin)
{
    for (int i = 0; i < reg->count; i++) {
        if (point_in_rect(&reg->rects[i], x, y, margin)) {
            return true;
        }
    }
    return false;
}

static void add_rect(radar_layout_registry_t *reg, float x1, float y1, float x2, float y2)
{
    if (reg->count >= RADAR_LAYOUT_MAX_RESERVED) {
        return; /* defensive: never overflow the fixed array */
    }
    reg->rects[reg->count].x1 = x1;
    reg->rects[reg->count].y1 = y1;
    reg->rects[reg->count].x2 = x2;
    reg->rects[reg->count].y2 = y2;
    reg->count++;
}

static float dist2(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return sqrtf(dx * dx + dy * dy);
}

/* ---------------------------------------------------------------------
 * Registry.
 * ------------------------------------------------------------------- */

void radar_layout_build_registry(radar_mode_t mode, bool never_fixed, radar_layout_registry_t *out)
{
    if (!out) {
        return;
    }
    out->count = 0;

    /* Cross-mode chrome, present on every render, no exceptions. */
    add_rect(out, -120.0f, RADAR_LAYOUT_STATUS_BAR_DY - 17.0f, 120.0f, RADAR_LAYOUT_STATUS_BAR_DY + 17.0f);
    add_rect(out, -70.0f, RADAR_LAYOUT_PAGE_DOT_DY - 10.0f, 70.0f, RADAR_LAYOUT_PAGE_DOT_DY + 10.0f);

    switch (mode) {
    case RADAR_LIVE:
    case RADAR_STALE:
        add_rect(out, -140.0f, RADAR_LAYOUT_STACK_NAME_DY - 20.0f, 140.0f, RADAR_LAYOUT_STACK_CHIP_DY + 24.0f);
        break;
    case RADAR_LOST:
        if (never_fixed) {
            add_rect(out, -170.0f, RADAR_LAYOUT_NEVER_HEADLINE_DY - 18.0f, 170.0f,
                      RADAR_LAYOUT_NEVER_SUB_DY + 15.0f);
        } else {
            add_rect(out, -140.0f, RADAR_LAYOUT_STACK_NAME_DY - 20.0f, 140.0f, RADAR_LAYOUT_STACK_CHIP_DY + 24.0f);
        }
        break;
    case RADAR_CLOSE:
        /* Ring stack + big distance. */
        add_rect(out, -100.0f, RADAR_LAYOUT_CLOSE_RING_CY - RADAR_LAYOUT_CLOSE_RING_MAX_R, 100.0f,
                  RADAR_LAYOUT_CLOSE_RING_CY + RADAR_LAYOUT_CLOSE_RING_MAX_R);
        /* Name + trend chip. */
        add_rect(out, -110.0f, RADAR_LAYOUT_CLOSE_NAME_DY - 15.0f, 110.0f, RADAR_LAYOUT_CLOSE_CHIP_DY + 20.0f);
        /* FLARE button. */
        add_rect(out, -104.0f, RADAR_LAYOUT_CLOSE_FLARE_DY - 26.0f, 104.0f, RADAR_LAYOUT_CLOSE_FLARE_DY + 26.0f);
        break;
    case RADAR_NOFIX:
        add_rect(out, -170.0f, RADAR_LAYOUT_NOFIX_HEADLINE_DY - 18.0f, 170.0f, RADAR_LAYOUT_NOFIX_CHIP_DY + 20.0f);
        break;
    case RADAR_NOSEL:
    default:
        add_rect(out, -170.0f, RADAR_LAYOUT_NOSEL_HEADLINE_DY - 18.0f, 170.0f, RADAR_LAYOUT_NOSEL_SUB_DY + 15.0f);
        break;
    }
}

/* ---------------------------------------------------------------------
 * Arrow — 1-D monotonic search over length.
 * ------------------------------------------------------------------- */

void radar_layout_resolve_arrow(radar_layout_registry_t const *reg, float arrow_deg, radar_layout_arrow_t *out)
{
    if (!out) {
        return;
    }

    float rad = arrow_deg * (RADAR_LAYOUT_PI / 180.0f);
    float fx = sinf(rad), fy = -cosf(rad); /* forward unit vector, toward the tip */
    float px = cosf(rad), py = sinf(rad);  /* perpendicular unit vector, the head's base spread */
    float half_w = RADAR_LAYOUT_ARROW_HEAD_WIDTH_PX / 2.0f;

    /* Bounded shortening loop: (ARROW_LEN_PX - ARROW_MIN_LEN_PX) / 4 =
     * 30 steps needed in the worst case; MAX_STEPS is deliberately far
     * more generous than that so this is a hard, provable cap, not a
     * tight fit to today's constants. Every candidate is tested against
     * the FULL registry union fresh each step (point_in_registry), never
     * an incremental push toward one violated rectangle — so overlapping
     * reserved rectangles cannot make this oscillate. */
    enum { MAX_STEPS = 64 };

    float tip_r = RADAR_LAYOUT_ARROW_LEN_PX;
    bool shortened = false;
    float tip_x = 0.0f, tip_y = 0.0f, base_x = 0.0f, base_y = 0.0f, left_x = 0.0f, left_y = 0.0f, right_x = 0.0f,
          right_y = 0.0f;

    for (int step = 0; step <= MAX_STEPS; step++) {
        float base_r = tip_r - RADAR_LAYOUT_ARROW_HEAD_LEN_PX;
        if (base_r < 0.0f) {
            base_r = 0.0f; /* degenerate guard: only reachable if MIN_LEN < HEAD_LEN */
        }
        tip_x = fx * tip_r;
        tip_y = fy * tip_r;
        base_x = fx * base_r;
        base_y = fy * base_r;
        left_x = base_x - px * half_w;
        left_y = base_y - py * half_w;
        right_x = base_x + px * half_w;
        right_y = base_y + py * half_w;

        bool hit = point_in_registry(reg, tip_x, tip_y, 0.0f) || point_in_registry(reg, left_x, left_y, 0.0f) ||
                   point_in_registry(reg, right_x, right_y, 0.0f);
        if (!hit || tip_r <= RADAR_LAYOUT_ARROW_MIN_LEN_PX) {
            break;
        }
        tip_r -= 4.0f;
        if (tip_r < RADAR_LAYOUT_ARROW_MIN_LEN_PX) {
            tip_r = RADAR_LAYOUT_ARROW_MIN_LEN_PX;
        }
        shortened = true;
    }

    out->tip_dx = tip_x;
    out->tip_dy = tip_y;
    out->base_dx = base_x;
    out->base_dy = base_y;
    out->left_dx = left_x;
    out->left_dy = left_y;
    out->right_dx = right_x;
    out->right_dy = right_y;
    out->len_px = tip_r;
    out->shortened = shortened;
}

/* ---------------------------------------------------------------------
 * Dots — 1-D monotonic search over angle, then union-find clustering.
 * ------------------------------------------------------------------- */

/* Nearest clear angle to `start_deg` at the fixed ring radius, searching
 * outward in alternating +/- steps, each candidate tested against the
 * FULL registry union fresh (never an incremental push) — see this
 * file's top comment for why that's what makes overlapping reserved
 * rectangles harmless here. Bounded: fixed step size, fixed maximum
 * offset, so this always terminates within a fixed number of candidate
 * tests regardless of `*reg`'s contents. */
static void find_clear_angle(radar_layout_registry_t const *reg, float start_deg, float *out_dx, float *out_dy)
{
    float half = RADAR_LAYOUT_DOT_PX / 2.0f;
    int max_steps = (int)(RADAR_LAYOUT_MAX_ANGLE_OFFSET_DEG / RADAR_LAYOUT_ANGLE_STEP_DEG);

    float best_dx, best_dy;
    deg_to_offset(start_deg, RADAR_LAYOUT_RING_RADIUS_PX, &best_dx, &best_dy);
    bool found = !point_in_registry(reg, best_dx, best_dy, half);

    for (int step = 1; step <= max_steps && !found; step++) {
        for (int v = 0; v < 2 && !found; v++) {
            float offset = (float)step * RADAR_LAYOUT_ANGLE_STEP_DEG * (v == 0 ? 1.0f : -1.0f);
            float dx, dy;
            deg_to_offset(start_deg + offset, RADAR_LAYOUT_RING_RADIUS_PX, &dx, &dy);
            if (!point_in_registry(reg, dx, dy, half)) {
                best_dx = dx;
                best_dy = dy;
                found = true;
            }
        }
    }
    /* !found: defensive fallback for a pathological registry that blocks
     * the entire swept arc (shouldn't happen with today's constants —
     * best-effort is the untouched original bearing, already in
     * best_dx/best_dy from the offset-0 attempt above). A test asserting
     * no overlap would still correctly fail in that scenario rather than
     * silently accept it. */

    *out_dx = best_dx;
    *out_dy = best_dy;
}

static int uf_find(int *parent, int x)
{
    while (parent[x] != x) {
        parent[x] = parent[parent[x]]; /* path halving */
        x = parent[x];
    }
    return x;
}

/* Union biased so the resulting root is always the SMALLER of the two
 * original indices — guarantees every cluster's root is its own
 * lowest-original-index member, so that member's already-verified-clear
 * position is always a safe, unambiguous anchor for the whole cluster. */
static void uf_union(int *parent, int a, int b)
{
    int ra = uf_find(parent, a);
    int rb = uf_find(parent, b);
    if (ra == rb) {
        return;
    }
    if (ra < rb) {
        parent[rb] = ra;
    } else {
        parent[ra] = rb;
    }
}

void radar_layout_resolve_dots(radar_layout_registry_t const *reg, float const *ring_deg, int n,
                                radar_layout_dot_result_t *out)
{
    if (!out || !ring_deg || n <= 0) {
        return;
    }
    if (n > FF_CREW_MAX) {
        n = FF_CREW_MAX; /* defensive clamp: ff_radar_view_t::n_dots is documented <= FF_CREW_MAX */
    }

    float dx[FF_CREW_MAX], dy[FF_CREW_MAX];
    int parent[FF_CREW_MAX];
    for (int i = 0; i < n; i++) {
        find_clear_angle(reg, ring_deg[i], &dx[i], &dy[i]);
        parent[i] = i;
    }

    /* Union-find clustering: never hide (ORCHESTRATOR RULING, round 4 —
     * see this header's top comment) — dots whose resolved positions
     * ended up within one dot-diameter of each other share a marker
     * instead. Exhaustive pairwise comparison over <= FF_CREW_MAX dots:
     * a fixed, small amount of work, no convergence loop. */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (dist2(dx[i], dy[i], dx[j], dy[j]) < RADAR_LAYOUT_DOT_PX) {
                uf_union(parent, i, j);
            }
        }
    }

    for (int i = 0; i < n; i++) {
        int root = uf_find(parent, i);
        int size = 0;
        for (int k = 0; k < n; k++) {
            if (uf_find(parent, k) == root) {
                size++;
            }
        }
        out[i].dx = dx[root];
        out[i].dy = dy[root];
        out[i].cluster_id = root;
        out[i].cluster_size = size;
    }
}

int radar_layout_cluster_wedges(radar_layout_dot_result_t const *resolved, int n, int cluster_id,
                                 radar_layout_wedge_t *out, int out_max)
{
    if (resolved == NULL || out == NULL || n <= 0 || out_max <= 0) {
        return 0;
    }
    if (n > FF_CREW_MAX) {
        n = FF_CREW_MAX; /* same defensive clamp radar_layout_resolve_dots applies */
    }

    /* Count first, then place: every wedge's width depends on how many
     * members share the ring, so nothing can be emitted until the whole
     * membership is known. Ascending original-index order falls out of
     * the single forward scan — see this function's doc comment for why
     * that stability matters. */
    int members[FF_CREW_MAX];
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (resolved[i].cluster_id == cluster_id) {
            members[count++] = i;
        }
    }
    if (count == 0) {
        return 0; /* no dot belongs to this cluster_id */
    }

    /* A lone member gets the whole circle with no gap (a gap would be a
     * seam in a ring with nothing to separate), expressed as LVGL's own
     * full-arc form 0..360 rather than a normalized 270..270 — the
     * latter is indistinguishable from a zero-width wedge. */
    if (count == 1) {
        out[0].index = members[0];
        out[0].start_deg = 0.0f;
        out[0].end_deg = 360.0f;
        return 1;
    }

    float slice = 360.0f / (float)count;
    float half_gap = RADAR_LAYOUT_CLUSTER_WEDGE_GAP_DEG / 2.0f;

    int written = 0;
    for (int k = 0; k < count && written < out_max; k++) {
        /* 270 == 12 o'clock in LVGL's arc convention (0 == 3 o'clock,
         * clockwise). */
        float base = 270.0f + (float)k * slice;
        float start = base + half_gap;
        float end = base + slice - half_gap;

        /* Normalize into [0, 360). A wedge that crosses the seam ends up
         * with end < start, which is exactly what LVGL's arc widget
         * expects for a wrapping arc — see radar_layout_wedge_t's doc
         * comment. */
        while (start >= 360.0f) {
            start -= 360.0f;
        }
        while (end >= 360.0f) {
            end -= 360.0f;
        }

        out[written].index = members[k];
        out[written].start_deg = start;
        out[written].end_deg = end;
        written++;
    }

    return written;
}
