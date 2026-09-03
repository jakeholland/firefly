/**
 * ff_rally.c — core/rally implementation. See ff_rally.h's top comment for
 * the move history and the fp_pack_t-avoidance rationale.
 */
#include "ff_rally.h"

#include <string.h>

#include "ff_proto.h" /* FF_PROTO_RALLY_NAME_MAX */

bool ff_rally_nearest_landmark(ff_rally_landmark_t const *landmarks, uint8_t n, float my_east_m,
                                float my_north_m, float near_m, uint8_t *out_index)
{
    if (landmarks == NULL || out_index == NULL) {
        return false;
    }
    bool found = false;
    /* Compare squared distances — no sqrt, no <math.h>. Strict `<` against
     * near_m^2: a landmark at EXACTLY near_m never displaces the initial
     * threshold, so it never qualifies (see FF_RALLY_LANDMARK_NEAR_M's doc
     * comment on the boundary). */
    float best2 = near_m * near_m;
    for (uint8_t i = 0; i < n; ++i) {
        ff_rally_landmark_t const *lm = &landmarks[i];
        if (!lm->has_pos || lm->name == NULL || lm->name[0] == '\0') {
            continue;
        }
        if (strlen(lm->name) > FF_PROTO_RALLY_NAME_MAX) {
            continue;
        }
        float const de = lm->east_m - my_east_m;
        float const dn = lm->north_m - my_north_m;
        float const d2 = de * de + dn * dn;
        if (d2 < best2) {
            best2 = d2;
            *out_index = i;
            found = true;
        }
    }
    return found;
}

char const *ff_rally_place_name(ff_rally_landmark_t const *landmarks, uint8_t n, float my_east_m,
                                 float my_north_m)
{
    uint8_t idx = 0;
    if (ff_rally_nearest_landmark(landmarks, n, my_east_m, my_north_m, FF_RALLY_LANDMARK_NEAR_M,
                                   &idx)) {
        return landmarks[idx].name;
    }
    return FF_RALLY_DEFAULT_NAME;
}

bool ff_rally_landmark_displayable(bool has_pos, char const *name)
{
    return has_pos && name != NULL && name[0] != '\0';
}

char const *ff_rally_when_suffix(int when_sel)
{
    switch (when_sel) {
    case 1: return " +15m";
    case 2: return " +30m";
    case 0:
    default: return "";
    }
}

char const *ff_rally_when_echo(int when_sel)
{
    switch (when_sel) {
    case 1: return "+15m";
    case 2: return "+30m";
    case 0:
    default: return "Now";
    }
}

bool ff_rally_compose_name(char const *place, int when_sel, char *out, size_t out_cap)
{
    if (place == NULL || place[0] == '\0' || out == NULL ||
        out_cap < (size_t)FF_PROTO_RALLY_NAME_MAX + 1u) {
        return false;
    }
    char const *suffix = ff_rally_when_suffix(when_sel);
    size_t const suffix_len = strlen(suffix);
    /* The suffix must leave room for at least one place byte. */
    if (suffix_len + 1u > (size_t)FF_PROTO_RALLY_NAME_MAX) {
        return false;
    }
    size_t const place_budget = (size_t)FF_PROTO_RALLY_NAME_MAX - suffix_len;
    size_t place_len = strlen(place);
    if (place_len > place_budget) {
        place_len = place_budget; /* truncate the PLACE only */
    }
    memcpy(out, place, place_len);
    memcpy(out + place_len, suffix, suffix_len + 1u); /* includes the NUL */
    return true;
}
