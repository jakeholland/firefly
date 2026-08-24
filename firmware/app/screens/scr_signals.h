/**
 * scr_signals.h — app/screens: the Signals face (S08 slice c).
 *
 * Pure rendering: reads an `ff_app_signals_t` and draws the event feed —
 * icon + sender + text + relative age per row, newest first, plus the
 * canned-reply chip row (OMW / 5 MIN / PULSE). No domain logic here
 * (CLAUDE.md: "UI code only renders core state and forwards input") —
 * sending a canned reply, marking the feed read, or targeting a rally
 * point are all real behavior that needs core+meshclient (app/ff_wiring.c,
 * core/include/ff_crew.h's deferred `ff_crew_select_rally`), which a pure
 * render file must not reach into directly.
 *
 * As of S16 slice c1 the "+" (open Compose) forwards through the intent
 * seam (`ff_intent_emit`, app/include/ff_intent.h) — still zero domain
 * logic here; the shell decides. The rally-row and canned-reply chips
 * remain clearly-marked stub callbacks until S16 slice c2 converts them
 * to intents, same convention as scr_radar.c's FLARE-button stub.
 */
#ifndef FF_SCR_SIGNALS_H
#define FF_SCR_SIGNALS_H

#include "ff_app_state.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_signals_build — renders `*signals` into `parent` (expected to be
 * `FF_THEME_PUCK_PX` square — the shell, scr_nav.c, hands it a tileview
 * tile sized to the puck, same convention as `ff_scr_radar_build`).
 *
 * Renders, newest first: up to `FF_APP_SIGNALS_MAX_ITEMS` event rows (icon
 * keyed off `kind`, sender, text, relative `age_str`, an unread dot when
 * `unread`), an empty-state message when `n_items == 0` (CLAUDE.md: honest
 * empty state, never a fake/sample row), a "+" button reserved for
 * opening Compose, and the OMW / 5 MIN / PULSE canned-reply chip row
 * (each ≥`FF_THEME_MIN_HIT_PX` tall).
 */
void ff_scr_signals_build(lv_obj_t *parent, ff_app_signals_t const *signals);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_SIGNALS_H */
