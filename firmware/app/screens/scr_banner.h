/**
 * scr_banner.h — app/screens: the S26 slice d message banner (docs/specs/
 * S26-device-lifecycle.md, "(d) ff_notify + message banner").
 *
 * Pure rendering (CLAUDE.md: "UI code only renders core state and
 * forwards input") of the flattened `ff_app_banner_t` (ff_app_state.h) —
 * the shell's `ff_notify` queue's HEAD, projected each tick. No domain
 * logic here: the queue (push/coalesce/expiry/dismiss) lives entirely in
 * `core/include/ff_notify.h`; this file only draws whichever banner the
 * shell handed it and forwards the one tap it accepts.
 *
 * ## An OVERLAY, not a face
 * Built on top of `scr_nav.c`'s puck, LAST — the same "created after (so
 * drawn on top of) the tileview" placement the page dots and the S10
 * flare sender overlay already use (see `ff_scr_flare_build_sender_
 * overlay`'s doc comment and `ff_scr_nav_build`'s own call-order
 * comment). It survives a face swipe (it lives on the puck, not inside
 * any one tile) and paints over whichever of the five swipe faces is
 * currently showing — spec: "an overlay STRIP across the top of whatever
 * face is showing (not a face; built after the face, like the page
 * dots)". It is NEVER built during the flare takeover (the dispatcher
 * returns before reaching `ff_scr_nav_build` at all while
 * `flare.takeover_active`, so this file never even runs then) or during
 * a full-screen modal (Compose / the power menu) — the shell's
 * `active_face`/modal dispatch already decides that; this file has no
 * face-awareness of its own.
 *
 * ## What it shows (spec: "sender name in their crew color, kind glyph,
 * preview, honest age via ff_fmt_age")
 * A rounded pill strip: a kind glyph, the sender's name in their crew
 * palette color (`ff_theme_crew_color`), the preview body, and the real
 * age via `ff_fmt_age` — never a fabricated "now" (it reads the actual
 * `age_ms` the shell computed from the queue entry's real `at_ms`; that
 * age happens to render as "now" for virtually all of a banner's 6s
 * life, which is `ff_fmt_age`'s own honest sub-minute bucket, not a
 * special case here).
 *
 * ## Tap
 * The whole pill is one button, >= 48 px tall (spec) and entirely inside
 * the round glass (see scr_banner.c's layout-constant comment for the
 * chord math), with `LV_STATE_PRESSED` feedback. A tap emits
 * `FF_INTENT_BANNER_OPEN` (no payload — see ff_intent.h's doc comment):
 * the shell reads its own `ff_notify` head to decide what that means
 * (route to the sender's thread, mark it read, dismiss the banner), the
 * same "pure renderer, the shell owns the scope" convention every other
 * overlay in this codebase (the power menu, the Signals popup) follows.
 */
#ifndef FF_SCR_BANNER_H
#define FF_SCR_BANNER_H

#include "ff_app_state.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_banner_build — draws the banner strip on top of `parent`
 * (expected to be `scr_nav.c`'s puck object). No-op (draws nothing) if
 * `!banner->active` — same "renders exactly the state it's handed, no
 * invented fallback" contract as every other screen builder in this
 * codebase (e.g. `ff_scr_flare_build_sender_overlay`'s `!flare->sending`
 * no-op). `colorblind` selects the colorblind-safe crew palette for the
 * sender-name color, the same explicit-parameter convention
 * `ff_scr_radar_build`/`ff_scr_map_build` use (ff_theme.h's own doc
 * comment on why this is threaded explicitly rather than read from a
 * hidden global).
 */
void ff_scr_banner_build(lv_obj_t *parent, ff_app_banner_t const *banner, bool colorblind);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_BANNER_H */
