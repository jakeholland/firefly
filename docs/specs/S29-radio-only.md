# S29 · radio-only fallback — signal view + FIND mode

## Motivation
Owner direction, 2026-09-07: "we should fall back to working better with no GPS signal, for
indoor mode / in the trees." Lost Lands (18–20 September) is tents, tree cover, stages — GPS
will drop constantly, but the LoRa radio keeps hearing friends the whole time. Today, the moment
our own fix is missing, Radar shows `RADAR_NOFIX` — "NO FIX · RADIO ONLY" — and stops being
useful even though the radio is still doing real work. This spec makes the radio's own evidence
(RSSI/SNR, direct-vs-relay, "heard how long ago") a first-class, honestly-labeled reading instead
of a dead end, and (PR 2) lets a puck actively probe a specific friend's signal on demand.

**Signal is never distance.** Every label this spec adds says "signal", never implies metres —
CLAUDE.md's "never fake... positions" extends to never dressing up a dBm reading as a place.

## Existing building blocks this spec reuses (read these first)
- `ff_crew_member_t.rssi_dbm` / `rssi_age_ms` / `rssi_hist` / `ff_crew_rssi_trend()`
  (`core/include/ff_crew.h`, `core/src/ff_crew.c`) — the last DIRECT-packet RSSI sample, its
  absolute rx timestamp, and the 5s-window hot/cold trend. Only `MC_RX_PATH_DIRECT` packets ever
  reach `ff_crew_on_rssi` (`app/ff_shell.c`'s `shell_ev_rx_meta`, paired-gated) — a relayed
  packet's RSSI belongs to the relay, not the sender (`mc_client.h`'s `mc_rx_path_t` doc comment).
- `ff_sigview_presence()` (`core/include/ff_sigview.h`) — the existing SEEN/LOST/LINKED
  classifier the Inbox (S24) already uses, built from position freshness + direct-RSSI age. This
  spec's SIGNAL view is a *different* surface (Radar's selected-friend headline + ring), not a
  replacement for `ff_sigview_presence`; the two share no code but must not drift apart in
  vocabulary (both call a fresh direct reading "recent", never "distance").
- `radar_mode_t` / `ff_radar_compute` (`core/include/ff_radar.h`, `core/src/ff_radar.c`) — the
  mode ladder this spec extends (`docs/specs/S06-radar-face.md`, including the 2026-09-05 NOHDG
  amendment, whose "add an honest mode rather than special-case an existing one" house style this
  spec follows).
- `ff_proto` (`core/include/ff_proto.h`, spec S04) — the Firefly private-protocol envelope PR 2's
  PING/PONG rides.
- `mc_send_private(c, dest, FF_PORTNUM, payload, len, want_ack)` — direct (non-broadcast) send;
  `mc_events_t.on_routing_ack` — S04's `want_ack` reply plumbing, already wired in `ff_shell.c`'s
  `shell_ev_routing_ack`.
- S27 sounds / `haptic_cb` (`ff_wiring.c`) — the haptic emission seam PR 2's warmer/colder pulses
  use.
- S28 gestures — the tap-target/gesture chrome conventions PR 2's FIND action follows.

## Merge-point note (concurrent presence work) — RESOLVED at rebase onto main
`fix/presence-heard-vs-position` (merged to `main` as #236, ahead of this branch) landed a
`last_heard`-based presence predicate (heard age independent of position age) in parallel with
this branch. At rebase time the two "was this member heard at all" trackers were folded into one
record on `ff_crew_member_t`, keeping #236's field names as the base (`last_heard_ms`/`has_heard`,
since it reached `main` first) and adding the one fact this spec needed that #236 didn't already
carry: `heard_direct` — whether the *latest* sighting (not necessarily the latest direct RSSI)
arrived DIRECT vs. via a relay. `ff_crew_on_heard`'s signature gained the `direct` parameter for
this. There is now exactly one "heard via any path" record and one writer
(`ff_crew_on_heard`); `ff_sigview_presence` (S24 Inbox/CREW) and this spec's `RADAR_SIGNAL`/signal
fields both read it, just via different reducers (`ff_crew_presence` vs. this spec's own
tier/age/relay projection below). The code samples below are updated to the shipped field name
(`last_heard_ms`, not the draft's `heard_age_ms`).

## Scope cuts (flagged per AGENTS.md, not silently dropped)
1. **SNR refinement (owner's "SNR can refine") is deferred, not implemented.** Tiering here is
   RSSI-only. Wiring SNR in would mean widening `ff_crew_on_rssi`'s signature (three existing
   non-test call sites, three test files) for a refinement whose actual value — LoRa SNR near the
   demodulation floor while RSSI reads strong usually means "elevated noise floor from
   interference," not "weak signal" — is a nice-to-have polish on top of a correctly-classified
   RSSI reading, not a behavior this milestone depends on. Left as a named follow-up in
   `ff_radar_signal_tier`'s own doc comment.
2. **"Last known" ghost only covers the friend's position, not a cached copy of *our own*
   historical fix.** `ff_radar_compute` has no input for "my last-known position before GPS
   dropped" (it only ever receives the *current* `my_pos`/`my_pos_ok`), and adding one means a new
   persisted-across-calls cache (analogous to `ff_radar_smooth_t`) plus its own tests/fixtures.
   The ghost therefore only renders when **our own fix is currently good** but the *friend's*
   position is what has gone stale/absent (`RADAR_SIGNAL` reached via the "member's position is
   LOST/NEVER" path, not the "my own fix is missing" path) — see Behavior below. The
   my-own-fix-just-dropped case still gets the live SIGNAL headline (tier/age/trend), just no
   ghost arrow. Recorded as a known gap for a future slice, not silently narrowed.
3. **Signal-ring dot placement does not go through `radar_layout`'s collision resolver.** That
   resolver's whole reason for existing (S06's "Layout resolution" section) is placing
   *bearing-based* dots without colliding with fixed chrome; signal-only dots have no bearing by
   definition (no fabricated direction — that's the entire point of a separate ring), so they are
   laid out by simple even angular spacing over a fixed upper arc in `scr_radar.c`, not integrated
   into `radar_layout_registry_t`. Documented as an interpretation call; a future pass could widen
   the resolver to accept "no preferred angle, just avoid the registry" if this ring turns out to
   collide with chrome in practice.

## PR 1 — signal view (no new radio traffic)

### Core data contract

`firmware/core/include/ff_crew.h` / `.c` — additive:
```c
typedef struct {
    ...
    /* Unified "heard via any path" record — see the Merge-point note above.
     * `last_heard_ms`/`has_heard` are #236's (presence-heard-vs-position);
     * `heard_direct` is the one field this spec added to that record. */
    uint32_t last_heard_ms;  /* absolute rx clock timestamp, same convention as pos_age_ms */
    bool     has_heard;
    bool     heard_direct;   /* true iff the LATEST sighting (not necessarily the latest
                               * direct RSSI) was direct, i.e. rx_path == MC_RX_PATH_DIRECT */
} ff_crew_member_t;

void ff_crew_on_heard(ff_crew_t *c, uint32_t node_id, uint32_t rx_time_ms, bool direct);
```
`ff_crew_on_heard` find-or-creates the slot (same contract as `ff_crew_on_rssi`/`on_position`) and
unconditionally overwrites `last_heard_ms`/`has_heard`/`heard_direct` — the latest sighting always
wins, same "latest fix wins, never sticky" rule `ff_crew_on_position` already documents.
`app/ff_shell.c`'s `shell_ev_rx_meta` calls it once per rx_meta event for a **paired** sender
(mirrors `ff_crew_on_rssi`'s trust gate), for **every** `rx_path` — unlike `ff_crew_on_rssi`,
which stays direct-only. `ff_crew_on_rssi` itself is unchanged (RSSI/SNR semantics are untouched
by this spec — see Scope cut 1).

`firmware/core/include/ff_radar.h` / `.c` — additive:
```c
typedef enum { FF_SIGNAL_NONE, FF_SIGNAL_FAINT, FF_SIGNAL_WEAK, FF_SIGNAL_GOOD, FF_SIGNAL_STRONG } ff_signal_tier_t;

#define FF_SIGNAL_STRONG_MIN_DBM (-80)  /* > -80 dBm: STRONG */
#define FF_SIGNAL_GOOD_MIN_DBM   (-95)  /* [-95, -80]: GOOD (exactly -80 is GOOD, not STRONG) */
#define FF_SIGNAL_WEAK_MIN_DBM   (-110) /* [-110, -95): WEAK; < -110: FAINT */

ff_signal_tier_t ff_radar_signal_tier(int16_t rssi_dbm);
```
Thresholds are a product judgment call (same category as `FF_CREW_RSSI_TREND_THRESHOLD_DBM`),
anchored to ordinary SX1262 LoRa practice rather than derived from a spec number: a LONG_FAST
(SF11/BW250) link's usable range on this hardware runs roughly -80 dBm (very strong, effectively
line-of-sight/close) down to the modem's own demodulation floor near -125…-130 dBm at 0 dB SNR;
-110 dBm still carries several dB of margin above that floor on a clean channel, so it is the
line between "still comfortably decodable" (WEAK) and "surviving on margin, expect drops" (FAINT).
-95 dBm splits STRONG from an ordinary, perfectly serviceable link (GOOD) at roughly the midpoint
of the practical range. `radar_mode_t` gains `RADAR_SIGNAL`.

`ff_radar_view_t` gains:
```c
ff_signal_tier_t signal_tier;      /* FF_SIGNAL_NONE when no usable DIRECT reading exists */
bool  signal_heard;                /* true iff heard at all, direct or relay */
bool  signal_via_relay;            /* true iff signal_heard && latest sighting was NOT direct */
char  signal_age_str[FF_RADAR_STR_LEN]; /* "" if !signal_heard, else age since last heard */
/* `trend` (existing field) doubles as SIGNAL's WARMER(+1)/COLDER(-1)/STEADY(0) — same
 * ff_crew_rssi_trend() call CLOSE mode already makes, no new field needed. */

typedef struct {
    char    initial;
    uint8_t color_idx;
    ff_signal_tier_t tier;  /* FF_SIGNAL_NONE if via_relay */
    bool    via_relay;
} ff_radar_signal_dot_t;
ff_radar_signal_dot_t signal_dots[FF_CREW_MAX];
uint8_t n_signal_dots;      /* sorted STRONG -> FAINT -> (via-relay/NONE last); tie-break: roster order */
```

### Behavior — mode resolution (supersedes the relevant slice of `ff_radar_compute`'s doc comment)

`RADAR_SIGNAL` is reached only where the old logic had **nothing left to say** (a dead end) *and*
the radio has some evidence to offer instead (`member->has_heard`). Every case where the old logic
already had something honest and better than a signal reading (LIVE/STALE/PLACE/CLOSE/NOHDG) is
completely unchanged — `RADAR_SIGNAL` never preempts a mode that already carries real geometry.

1. `RADAR_NOSEL` — unchanged.
2. `!my_pos_ok` (my own fix missing): `member->has_heard ? RADAR_SIGNAL : RADAR_NOFIX` (was
   unconditionally `RADAR_NOFIX`). No ghost arrow possible here (Scope cut 2) — `arrow_valid`
   stays false.
3. `my_pos_ok && !heading_ok`: `member->has_pos ? RADAR_NOHDG : (member->has_heard ? RADAR_SIGNAL : RADAR_NOFIX)`
   — unchanged when the member has a position (NOHDG still wins, it has real geometry); was
   unconditionally `RADAR_NOFIX` when the member has none.
4. `RADAR_CLOSE` — unchanged (reachable only once 2/3 above have already passed, exactly as
   today).
5. Freshness switch — `FF_FRESH_LIVE`/`STALE`/`ASSERTED` unchanged. `FF_FRESH_LOST` /
   `FF_FRESH_NEVER`: `member->has_heard ? RADAR_SIGNAL : RADAR_LOST` (was unconditionally
   `RADAR_LOST`, further disambiguated by the existing `age_str`-empty convention). Reached here,
   `my_pos_ok && heading_ok` are both already true, so the function's existing (unchanged)
   `v->arrow_valid = have_bearing;` line at the bottom already does the right thing for free:
   `have_bearing` is true iff the member also `has_pos` (a real, if very old, fix) — the **ghost**
   case — and false for `FF_FRESH_NEVER` (nothing to ghost). `dist_str`/`age_str` are *already*
   computed unconditionally earlier in the function from the member's last-known fix, so they need
   no new fields: in the ghost sub-case they already hold exactly "last known distance"/"last
   known age" — the renderer's "last known 12 min ago, 210 m NE" is `age_str`/`dist_str` plus
   `ff_geo_compass_point(bearing_deg)` for the compass-point suffix, verbatim. Positions never
   expire out of the crew model (`ff_crew_on_position`'s own doc comment), so there is no separate
   "position-LOST window" gate to add — the ghost is available for exactly as long as
   `RADAR_LOST`'s own "LAST SEEN" chip has always been available for an arbitrarily-old real fix
   (identical, pre-existing convention, not a new cutoff invented for this spec).

`signal_tier`/`signal_heard`/`signal_via_relay`/`signal_age_str` are computed **unconditionally**
(independent of mode), immediately after the existing `place`/`stale` reduction:
```c
v->signal_heard = member->has_heard;
v->signal_age_str[0] = '\0';
if (member->has_heard) {
    ff_fmt_age(v->signal_age_str, sizeof(v->signal_age_str), now_ms - member->last_heard_ms);
}
v->signal_via_relay = member->has_heard && !member->heard_direct;
bool tier_usable = member->heard_direct && member->rssi_dbm != INT16_MIN;
v->signal_tier = tier_usable ? ff_radar_signal_tier(member->rssi_dbm) : FF_SIGNAL_NONE;
```
`v->trend` (existing field, existing `ff_crew_rssi_trend` call) is left exactly as-is — it already
answers WARMER(+1)/COLDER(-1)/STEADY(0) from the same rssi history this mode reads.

`signal_dots[]`/`n_signal_dots` are computed unconditionally (parallel to `dots[]`, independent of
selection and of `my_pos_ok`/heading — these dots need no bearing frame at all): every paired
member with `!has_pos && has_heard` gets an entry (a member *with* a position is already on the
ordinary ring via `dots[]`, never both). Sorted STRONG → GOOD → WEAK → FAINT → NONE(via relay),
stable within a tier by roster order.

### Rendering (`app/screens/scr_radar.c`, `radar_layout.h`/`.c`)

- **Headline (no ghost, i.e. reached via path 2 or 3 above):** name + a signal chip. Chip text:
  `"<TIER> SIGNAL"` (STRONG/GOOD/WEAK/FAINT) in tier-appropriate color (STRONG/GOOD →
  `FF_THEME_COLOR_LIVE_GREEN`, WEAK → `FF_THEME_COLOR_STALE_AMBER`, FAINT → `FF_THEME_COLOR_MUTED`)
  when `signal_tier != FF_SIGNAL_NONE`; `"VIA RELAY"` (`FF_THEME_COLOR_SURFACE`/`INK`) when
  `signal_via_relay`; a distinct `"RADIO SILENT"` muted chip when `!signal_heard` (member has
  never been heard at all — should be unreachable given the mode-resolution rule above always
  falls back to NOFIX/LOST in that case, but the renderer stays honest rather than assuming). A
  second, smaller line: `"heard <signal_age_str> ago"`. A trend chip identical in spirit to
  CLOSE's (`WARMER`/`COLDER`/`STEADY`, same three colors) directly below, only drawn when
  `signal_tier != FF_SIGNAL_NONE` (a trend on top of "no direct reading" would be a fabricated
  refinement of nothing). No arrow.
- **Headline (ghost, path 5):** the same signal chip/age/trend stack, PLUS the existing ghost
  arrow (`radar_render_lost`'s exact `RADAR_ARROW_GHOST` styling — outline head, dashed tail,
  `FF_THEME_COLOR_MUTED` at reduced opacity, reusing `radar_layout_resolve_arrow` verbatim) and a
  `"LAST KNOWN <age_str>, <dist_str> <compass-point>"` chip, `compass-point` from
  `ff_geo_compass_point(r->bearing_deg)` when `bearing_valid` (mirrors `RADAR_NOHDG`'s existing
  ASCII-hyphen-substitution convention for the same LVGL built-in-font limitation).
- **Inner signal ring:** dots for `signal_dots[]`, drawn at a fixed radius smaller than
  `RADAR_LAYOUT_RING_RADIUS_PX` (new `RADAR_LAYOUT_SIGNAL_RING_RADIUS_PX`, 120px — corrected from
  this spec's original draft of 130px, see below), evenly spaced over a fixed upper arc (new
  `RADAR_LAYOUT_SIGNAL_RING_ARC_START_DEG`/`_END_DEG`, -80°..80°, i.e. clear of the bottom-center
  name/dist/chip stack every mode uses) so no bearing is ever implied (Scope cut 3).
  **Correction to this spec's original draft** (flagged per AGENTS.md): the draft named
  "-160°..-20°" for the arc, reasoning it was "clear of the bottom-center stack" — checked
  against `radar_layout.c`'s own `deg_to_offset()` convention (0° = straight up, positive =
  clockwise), and that range is wrong: -160° is DOWN-and-left, not up, so the draft's arc
  actually swept the LEFT side of the puck (down-left → left → up-left), not an upper arc clear
  of the bottom stack at all. Every mode's headline stack lives at `dy >= 0` (LIVE/STALE/LOST/
  SIGNAL's own stack all start at DY >= 40), so the arc that is actually clear of them is the
  upper semicircle where `dy <= 0`, i.e. θ ∈ [-90°, 90°] — narrowed to [-80°, 80°], with the
  radius trimmed from 130 to 120, so a dot near θ=0 (closest approach to the status bar's
  reserved rect, y -177..-143) keeps a real pixel margin (130 left ~1px; 120 leaves ~11px).
  Visually distinct from a placed ring dot: a small filled tier-colored
  disc (STRONG/GOOD/WEAK/FAINT → the same four chip colors above) with NO initial letter and a
  thin ring outline (`via_relay`/`NONE` entries: hollow outline only, `FF_THEME_COLOR_DIM`, no
  fill) — "a signal, not a placed friend," same "different silhouette for a different kind of
  fact" idiom `RADAR_PLACE`'s square marker already established.
- **Home ring / launcher "no fix" copy (owner ask #4):** out of scope for this PR's file list —
  `scr_launcher.c`'s status row has no per-friend ring in the current codebase (grepped; the
  "compass-ring home" language in project notes refers to Radar itself, not a separate launcher
  ring). The one place on `main` that already renders a bare "LOST" for a quiet member,
  `ff_sigview_presence`'s SEEN/LOST/LINKED (Inbox row chrome, `scr_inbox.c`), is deliberately left
  untouched — it belongs to the concurrent presence branch (this spec's Merge-point note above),
  and duplicating a second heard-aware label generator ahead of that branch landing would create
  exactly the drift this instruction told us to avoid. Recorded as an explicit follow-up once
  `last_heard` lands: teach `ff_sigview_presence`'s LOST case (or its caller) to read
  `signal_tier`/`signal_heard` the same way Radar now does, so a friend with a live radio reading
  never renders bare "LOST" anywhere in the app, not just on Radar.

### Acceptance criteria
1. Mode-resolution table: every one of the 5 branches above, both the has_heard-true and
   has_heard-false arm, exact (core unit tests).
2. `ff_radar_signal_tier` boundary-exact at -80/-95/-110 dBm and both sides of each (core unit).
3. `signal_dots[]`: unpaired excluded; a member with a position is excluded (already in `dots[]`
   instead — mutual exclusivity pinned); a never-heard member excluded; sort order exact for a
   mixed-tier fixture.
4. Ghost path: a real LOST fix with `has_heard` true produces `arrow_valid == true` and
   `dist_str`/`age_str` non-empty; the `FF_FRESH_NEVER` case with `has_heard` true produces
   `arrow_valid == false` and both strings empty (no fabricated ghost).
5. Golden screenshots: `radar_signal_strong.json` (direct, STRONG, no ghost — my own fix
   missing), `radar_signal_faint_relay.json` (via-relay, no tier, no ghost), `radar_signal_lastknown.json`
   (my fix fine, member's position LOST-but-real → ghost arrow + last-known chip). All existing
   goldens render byte-identical (every new field additive with a false/""/NONE default).
6. Core tests: `ff_crew_on_heard` overwrite semantics (latest sighting wins, direct flag flips
   both ways), find-or-create parity with `ff_crew_on_rssi`.

### Slices
a) `ff_crew_on_heard` + `ff_crew.h` fields + core tests · b) `ff_radar_signal_tier` + mode-resolution
rewrite + `signal_dots` compute + core tests · c) `shell_ev_rx_meta` wiring + render-key pass-through
(no new coarsening needed — no new floats) · d) `scr_radar.c`/`radar_layout.h` rendering + fixtures +
goldens.

## PR 2 — FIND mode (active pings)

Branches from PR 1's head (`feat/s06-radio-only` → `feat/s29-find-mode`); depends on PR 1 merging
(or at least landing) first for `ff_radar_view_t.signal_tier`/the SIGNAL mode chrome FIND's button
lives on.

### Wire format (`ff_proto.h`/`.c`, extends S04)

Two new types, **not** a reuse of the existing reserved `0x07 ACK_PING` (that type's documented
shape/semantics — `[nonce:4]`, "delivery UX v1.5" — is a different feature; reusing it here would
retroactively redefine a wire value S04 already reserved for something else):

| type | name  | body                              | semantics |
|------|-------|-----------------------------------|-----------|
| 0x08 | PING  | `[nonce:4]`                       | "how do you hear me" probe, direct-addressed, unicast |
| 0x09 | PONG  | `[nonce:4][rssi:2 i16][has_snr:1][snr_x10:2 i16]` | reply: the nonce echoed, plus the RSSI/SNR *the replier* measured on the PING that prompted it |

`nonce` correlates a PONG to the PING that triggered it (a peer may see pings from more than one
sender in a session). `snr_x10` is SNR×10 as a signed i16 (one decimal place, matches
`mc_rx_meta_t.snr_db`'s own float without shipping a float on the wire); `has_snr` mirrors
`mc_rx_meta_t.has_snr`'s own presence rule (proto3 float 0.0 is indistinguishable from absent —
see `mc_client.h` — so PONG must carry its own explicit flag rather than re-deriving one).
`ff_proto_decode` strict-length rules (S04 Amendments, PR #10) apply identically: exactly 4 bytes
for PING, exactly 9 for PONG, anything else rejected.

### Behavior

- **Starting FIND:** from Radar in a SIGNAL-tier-bearing mode (`RADAR_SIGNAL`, or any other mode —
  FIND is available whenever a member is selected; a friend already LIVE can still be FIND'd,
  e.g. to confirm "can they hear ME") with a member selected, a FIND action (S28 gesture: tap the
  signal chip, or a dedicated button per S06 chrome conventions) starts pinging that node.
- **Cadence & timeout:** one PING every 10 s, direct-addressed (`dest = member->node_id`, not
  broadcast — S04's addressing table gains this row), `want_ack = false` (a PING that never
  arrives is itself information — silence after 5 missed pings is "not hearing them," not a
  routing failure to retry). Stops automatically after 5 minutes (30 pings) or on leaving the
  Radar face, whichever first — `ff_find_t` (new, `core/include/ff_find.h`/`.c`) owns this timer,
  ticked from the same cadence `ff_radar_compute` is (`ff_find_tick(f, now_ms)` returns "send a
  ping now" as a bool the shell acts on, mirroring `ff_flare_t`'s own tick-returns-an-action
  shape).
- **Auto-reply:** any puck receiving a PING (regardless of pairing — a probe is answered
  honestly, same reasoning FLARE's receiver-side pairing filter does NOT apply here: PING already
  IS direct/addressed, so there is no "am I in scope" ambiguity a broadcast has) replies once,
  immediately, with a PONG carrying `mc_rx_meta_t`'s reading of THAT ping packet (the RSSI/SNR our
  own radio just measured receiving it) — "they hear us at −xx dBm." No pairing filter, no rate
  limit on the REPLY side beyond "one PONG per PING received" (a flood of pings from an
  unpaired/hostile sender costs that sender airtime, not us — each PONG is exactly as cheap as the
  PING that provoked it, and the SENDER's own 10s cadence is what's rate-limited, not receipt).
- **Rate limiting (sender side):** `ff_find_t` tracks last-ping-sent-per-session (single active
  FIND target at a time — starting FIND on a new member cancels any prior session, mirroring
  `ff_flare_t`'s single-active-flare convention) and refuses to send more than one PING per 10s
  regardless of caller cadence — a defensive floor, not just "the caller happens to call it every
  10s," so a bug in the caller's tick loop can't turn this into a flood.
- **Updating readings:** a PONG updates our reading of them via the SAME path a direct packet
  always would (`on_rx_meta` fires for the PONG packet itself, feeding `ff_crew_on_rssi`/
  `ff_crew_on_heard` exactly as any other direct receipt does — FIND generates no new *core*
  plumbing for "how WE hear them," only for "how THEY hear US"). The PONG's own payload
  (`rssi`/`snr` fields) is the new fact: "they hear us at −xx dBm," stored in `ff_find_t` as
  `their_rssi_of_us`/`their_snr_of_us` (+ age), surfaced by the FIND screen alongside the ordinary
  SIGNAL chip so both directions of the link are visible at once.
- **Warmer/colder haptics:** a haptic pulse when the trend improves (rising RSSI) by ≥3 dB
  averaged over the last three PONG-driven samples versus the three before them (reuses
  `ff_crew_rssi_trend`'s own windowed-average shape at a smaller, FIND-specific window —
  `ff_find.c`'s own 3-sample comparison, not `FF_CREW_RSSI_TREND_WINDOW_MS`'s 5s clock window,
  since PING/PONG's own cadence is a fixed 10s per sample and "last three samples" is therefore
  already a 20-30s window); a distinct pattern when it worsens by the same margin. Sound plays
  through the S27 pathway (`ff_sound_should_play`/priority) when enabled, via two new
  `ff_sound_event_t` entries (`FF_SOUND_FIND_WARMER`/`FF_SOUND_FIND_COLDER`), same table-driven
  shape as every existing event. No haptic on the first two samples (nothing to compare against
  yet) or on a change under 3 dB (steady).
  **Interpretation call, flagged per AGENTS.md:** `ff_wiring_ctx_t.haptic_cb` (`app/ff_wiring.h`)
  is `void (*)(void *user)` — a bare single-shot buzz, no duration/frequency/pattern parameter at
  all, and grepping the esp32s3 target confirms there is no haptic HAL yet either
  (`app_main.c`: "cfg.haptic left zeroed — see slice a (no haptic HAL yet)"). "A distinct
  (longer/lower) pattern" is not implementable through the seam that actually exists today. The
  honest approximation this PR ships: the shell's existing single-buzz hook is called once for
  WARMER and twice back-to-back for COLDER (`shell_haptic_find_*` in `ff_shell.c`) — tellable
  apart by count, not by pattern shape. A real pattern (as S27 sounds already have) needs a
  richer haptic HAL this spec does not add; recorded as a follow-up once that HAL exists.
- **Console (`ff_dbgcmd.c`/`ff_debug_console.c`, mirroring the existing `dm` command's shape):**
  `ping <node_hex>` sends one immediate PING outside the 10s/5min FIND session machinery (a
  single bench probe); `find <node_hex>` starts an ordinary FIND session on that crew member
  exactly as the UI gesture would; `find off` cancels the active session early.
  **Correction to this spec's original draft:** the draft claimed these would resolve a
  case-insensitive short/long-name match "the same as `dm` already uses" — checked against
  `firmware/core/src/ff_dbgcmd.c`, and that's wrong: `dm <node_hex> <text>` parses a hex node id
  (`parse_node_hex`, optional `!`/`0x` prefix), not a name — there is no name-to-node-id
  resolver anywhere in this codebase (grepped for `strcasecmp`/case-insensitive matching; the
  only hit is `ff_t9pred.c`'s unrelated word de-dup). `ping`/`find` follow `dm`'s *actual* shape
  instead: a bare hex node id, identical `parse_node_hex` parsing, `dest == 0` rejected the same
  way `dbgconsole_dm` rejects it for `dm`. Flagged per AGENTS.md rather than silently
  implementing the (nonexistent) name-resolution behavior the draft assumed.

### Airtime budget

Meshtastic's LONG_FAST preset (SF11, BW250 kHz, CR4/5) is the default and this fleet's configured
preset — nothing in `docs/specs/S03-meshclient.md`/`S15-esp32s3-target.md` overrides it, so no
per-node lookup is needed. At SF11/BW250:
- A PING (`[ver:1][type:1][nonce:4]` = 6 B app payload, plus the Meshtastic MeshPacket/RadioHead
  framing overhead of roughly 16 B for a direct/encrypted packet) is ~22 B on air. Time-on-air for
  a ~22 B LoRa packet at SF11/BW250 is approximately 150-180 ms (SF11 symbol time at 250 kHz is
  2.048 ms; a short packet at 4/5 coding runs roughly 70-85 symbols including preamble/header).
- A PONG (`[ver:1][type:1][nonce:4][rssi:2][has_snr:1][snr_x10:2]` = 11 B app payload, ~27 B on
  air) is marginally longer, ~180-210 ms.
- One ping+pong round trip every 10 s is therefore roughly **330-390 ms of air time per 10,000 ms
  window ≈ 3.3-3.9%** — **SUPERSEDED, see the 2026-09-16 correction at the end of this file: the
  per-packet figure above is ~2x too low; the real round trip is ~700 ms** for the two nodes directly involved — this is higher than the "~1-2%"
  figure named in the task brief once actually computed against SF11/BW250 (not SF7/BW250, which
  this fleet does not use); flagged explicitly here per AGENTS.md rather than silently matched to
  the brief's number. Meshtastic's own duty-cycle/airtime-fairness limiter
  (`AirTime::isTxAllowedChannelUtil`) applies per-node, not per-conversation, so a single active
  FIND session (one ping/pong pair per 10s) is a small fraction of any node's overall duty-cycle
  budget even at these numbers; this is not a channel-wide broadcast (both packets are
  direct-addressed unicast), so it does not compound across the whole mesh the way a periodic
  broadcast would. Two pucks running FIND on each other simultaneously would double this to
  ~6.6-7.8% between just that pair — acceptable for a bounded 5-minute session, not something to
  run continuously.

### Console commands (ff_dbgcmd)
`ping <node_hex>` / `find <node_hex>` / `find off`, registered in `core/src/ff_dbgcmd.c`
alongside `dm` — see the "Behavior" section's correction above for why these take a hex node id,
not a name.

### Tests
1. `ff_proto` round-trip for PING/PONG (table-driven alongside the existing types), strict-length
   rejection, golden bytes fixture extension.
2. `ff_find_t`: 10s rate limit (a caller ticking faster than 10s never sends twice inside the
   window), 5-minute/30-ping cap, cancel-on-face-leave, cancel-on-new-target.
3. Auto-reply: a received PING always produces exactly one PONG with the receiver's own
   `mc_rx_meta_t` reading of that packet, regardless of pairing.
4. Trend-haptic trigger: ≥3 dB improvement over the trailing 3-sample window fires the "warmer"
   pulse exactly once per crossing (not once per sample above threshold); ≥3 dB degradation fires
   the "colder" pattern; <3 dB fires neither.
5. FIND screen golden(s) (fixture name(s) TBD at implementation time, listed in the PR body).

## Gates (both PRs)
clang and gcc-14 sim builds, zero warnings; `ctest` all green; every new/changed golden listed in
the PR body; ESP32-S3 device build (sdkconfig + `CONFIG_FF_DEBUG_CONSOLE=y` +
`CONFIG_FF_COMPASS=y`) compiles clean in a scratch build dir, not flashed.

## Amendments

- **2026-09-16, fix/close-range-honest-distance (owner's own bench observation) — FIND's cadence
  halved (10s → 5s) and trend window halved (3-vs-3 → 2-vs-2) so warmer/colder resolves fast
  enough to be usable at walking pace.** The owner's own field report: the no-GPS close-quarters
  FIND mode was unusable in practice. The arithmetic that made it so, unchanged since PR 2 first
  shipped: at the original 10s cadence / `FF_FIND_TREND_SAMPLES = 3` (a 3-vs-3 window, 6 samples
  total), the FIRST warmer/colder verdict needed 6 × 10s = 60s, and every verdict thereafter
  compared two windows whose midpoints sat 30s apart — at a walking pace of a few m/s, a verdict
  fired 60-90s into a session was telling the user where they'd BEEN, not where their friend
  currently is. Between verdicts, the screen showed nothing new at all: `ff_find_on_pong` updates
  `their_rssi_of_us` on every accepted PONG, but the only thing the UI read from it was the
  trend-haptic direction, which stayed `.none` until the window filled.

  **The fix, in the core (`core/include/ff_find.h`/`.c`, `[api]`):**
  - `FF_FIND_PING_INTERVAL_MS`: `10 * 1000` → `5 * 1000`.
  - `FF_FIND_TREND_SAMPLES`: `3` → `2` (ring buffer `2 * FF_FIND_TREND_SAMPLES` shrinks 6 → 4
    samples accordingly — no other code change needed, every loop in `ff_find_on_pong` already
    keyed off the constant symbolically).
  - `FF_FIND_MAX_PINGS`: `30` → `60`. Not asked for directly, but a necessary consequence of the
    other two: halving the cadence while leaving the ping-count cap at 30 would have silently cut
    every session's real duration from 5 minutes to 2.5 minutes (30 pings × 5s), since
    `ff_find_tick` stops on whichever of the ping-count cap or the wall-clock cap
    (`FF_FIND_SESSION_MAX_MS`, unchanged at 5 minutes per this task's own instruction) is reached
    FIRST — and 30 × 5s would always reach the ping cap first. Doubling `FF_FIND_MAX_PINGS`
    restores the pairing (60 × 5s = 5 minutes = `FF_FIND_SESSION_MAX_MS`), so both caps still agree
    on the same session length, exactly as they did before this change. Flagged here per AGENTS.md
    as an interpretation call the task brief didn't spell out, not silently assumed.
  - `FF_FIND_TREND_THRESHOLD_DBM`: `3.0` → `4.0` — see its own paragraph below; **this one is a
    judgment call, not a derived number**, stated plainly rather than dressed up as arithmetic.

  **Result:** first verdict at 4 samples × 5s = **20s** (was 60s — 3x faster), and the two compared
  windows' midpoints sit `FF_FIND_PING_INTERVAL_MS × FF_FIND_TREND_SAMPLES` = 5s × 2 = **10s** apart
  (was 30s — 3x tighter). The task brief's own framing ("4x better on both," from independently
  halving both the cadence and the sample count) is a *ceiling* on the combined improvement, not
  the exact multiplier on each individual number — 60s → 20s is a 3x improvement, matching
  `(new_samples/old_samples) × (new_interval/old_interval) = (2/3) × (5/10) ≈ 0.33`, not the naive
  `0.5 × 0.5 = 0.25` (4x) the "halve both, compounds to 4x" framing suggests for the interval alone;
  restated honestly here rather than silently overclaiming the round number. The 10s window
  separation, by contrast, IS exactly `FF_FIND_PING_INTERVAL_MS × FF_FIND_TREND_SAMPLES`, which
  halves independently for each of the two halved inputs when held constant against the other —
  `(10s → 5s)` alone would give 3 × 5s = 15s, and `(3 → 2 samples)` alone would give 2 × 10s = 20s;
  both changing together compounds multiplicatively to the actual `2 × 5s = 10s`, a genuine 3x, not
  4x — this document is deliberately precise on the exact numbers per-field rather than reusing one
  round "4x" figure everywhere it doesn't quite apply.

  **Trend threshold — the judgment call, argued explicitly.** A 2-sample average removes less
  single-sample RSSI noise than a 3-sample one: the variance of the mean of *n* i.i.d. samples
  scales as σ²/n, so the variance of the DIFFERENCE of two 2-sample means (2 · σ²/2 = σ²) is 1.5×
  the variance of the difference of two 3-sample means (2 · σ²/3 = 2σ²/3), i.e. the new window's
  standard deviation is √1.5 ≈ 1.22× the old one's. Holding `FF_FIND_TREND_THRESHOLD_DBM` at the
  old 3.0 dB against a noisier delta would raise the false-crossing rate on ordinary RSSI jitter
  (ordinary LoRa RSSI readings wobble a few dB between consecutive samples on a static link — the
  same noise floor `FF_CREW_RSSI_TREND_THRESHOLD_DBM`'s own doc comment in `ff_crew.h` describes).
  The 1.22× factor above is a real, derived number, but it only says "raise the threshold somewhat
  to hold the noise-rejection margin roughly constant" — it does not by itself pick 4.0 over, say,
  3.5 or 4.5 dB. 4.0 dB is chosen as a round number comfortably above the derived 1.22× floor
  (3.0 × 1.22 ≈ 3.67 dB) while still small enough that a genuine multi-metre approach/retreat
  (which moves LoRa RSSI by considerably more than 4 dB over two ping intervals at typical festival
  walking speeds) still crosses it promptly. Stated plainly: **this is a judgment call, not a
  derived number** — the arithmetic bounds it from below, taste picked the exact value.

  **Raw per-ping reading, surfaced every 5s instead of only at a verdict.** The task's own framing
  ("the point is that something visibly moves on every ping") turned out to already be true of this
  codebase's existing render path, once the cadence itself sped up — no new core field or render
  logic was needed. `scr_radar.c`'s `radar_render_signal` (S29 PR 2's own "THEY HEAR YOU" chip, added
  when this spec's PR 2 first shipped) already reclassifies `find->their_rssi_of_us` through
  `ff_radar_signal_tier` — the SAME tiering vocabulary (`FF_SIGNAL_STRONG`/`GOOD`/`WEAK`/`FAINT`)
  this spec's PR 1 defined for the ordinary SIGNAL headline — on every render, not merely on a
  trend crossing; that chip already updates on every accepted PONG, which now arrives twice as
  often. Reusing the existing tier vocabulary rather than inventing a second one (as the task
  explicitly asked) required no code change at all here, only the cadence change above to make the
  existing behavior fast enough to read as "responsive" instead of "static."

  **Airtime recomputation for the new cadence** (this section's own numbers below are unchanged by
  this amendment — packet sizes/wire format are untouched — only the cadence they're divided into
  changes):
  - PING (~22 B on air) and PONG (~27 B on air) time-on-air at SF11/BW250 are exactly as this
    spec's own "Airtime budget" section above already computed: ~150-180 ms and ~180-210 ms
    respectively, since neither the PING/PONG payload shape nor the LONG_FAST preset changed.
  - One ping+pong round trip is therefore still **~330-390 ms of air time**, but now recurring every
    `FF_FIND_PING_INTERVAL_MS` = **5,000 ms** instead of 10,000 ms: **330-390 ms / 5,000 ms ≈
    6.6-7.8%** for the two nodes directly involved in a SINGLE FIND session — exactly the band this
    spec's original "Airtime budget" section named for the DOUBLED case (two pucks running FIND on
    each other simultaneously at the OLD 10s cadence) and called "acceptable for a bounded 5-minute
    session, not something to run continuously." My own recomputation agrees with the task brief's
    stated expectation ("a 5 s cadence for ONE session lands in that same band") — both arrive at
    6.6-7.8% by the same arithmetic, so this is not flagged as a disagreement requiring a stop.
  - **New fact worth stating plainly, not silently absorbed:** two pucks running FIND on each other
    SIMULTANEOUSLY at the new 5s cadence would double again, to **~13.2-15.6%** of duty cycle for
    each node directly involved — double the figure this spec's original text called the acceptable
    upper bound. This is a real escalation from the pre-amendment numbers, not hidden by only
    reporting the single-session figure. It is still, by this spec's own pre-existing reasoning,
    bounded (both packets remain direct-addressed unicast, never a broadcast, so it does not compound
    across the wider mesh) and time-limited (`FF_FIND_SESSION_MAX_MS`, unchanged at 5 minutes, still
    caps how long any single node can sustain this rate, and Meshtastic's own per-node
    `AirTime::isTxAllowedChannelUtil` fairness limiter is an independent hard backstop this spec
    already relies on and did not change here) — but it is a materially different number from what
    was previously reviewed, and is called out here explicitly rather than left for a future reader
    to discover by re-deriving it. Not a stop-and-flag case per the task's own pre-stated expectation
    for the single-session number, but recorded so the owner can weigh in on the doubled-session case
    specifically if desired.

  **`ff_find_t` is untouched structurally** — `sample_hist[2 * FF_FIND_TREND_SAMPLES]` resizes from
  the constant alone (4 entries instead of 6), and every loop bound in `ff_find_on_pong` already
  read `FF_FIND_TREND_SAMPLES` symbolically rather than hardcoding 3, so no logic change was needed
  beyond the constants themselves.

  **App parity (`app/FireflyKit/Sources/FireflyModel`).** The production FIND path
  (`FindBridge.swift` → `CoreFindSession`, `Live/LiveAdapters.swift`) calls the real `ff_find_t`
  state machine directly (same symlinked `firmware/core` sources the puck compiles), so it picked
  up every constant change above automatically — no code change needed there. `RadarViewModel.swift`
  does carry one hand-transcribed MIRROR of the same constants, `FindSessionConstants` (feeding
  `MockFindSession`, an "honest stand-in" used only where no live mesh connection exists yet — demo
  mode, previews, and this file's own unit tests), which has no compile-time link to `ff_find.h` and
  needed updating by hand: `pingIntervalSeconds` 10→5, `maxPings` 30→60, `trendSamples` 3→2,
  `trendThresholdDbm` 3.0→4.0. `RadarViewModel.findRepliesCap` (32) also needed raising to 64: it
  was sized ("far more replies than a session... can usefully show, so nothing is lost") against the
  OLD 30-ping ceiling, and doubling `maxPings` to 60 without moving it would have started silently
  dropping real FIND replies partway through any session that ran to its new full length — the
  ORIGINAL promise that cap made ("nothing is lost") is honored by raising it, not left to quietly
  break.

  **Tests.** Core (`core/tests/test_find.c`): the six 3-vs-3 trend tests rewritten for the 2-vs-2
  window with new sample values (documented per-test why an arithmetic-ramp signal was chosen over
  a plateau, to avoid an artifact where a smaller window converges back toward "steady" faster than
  the old window did for the same number of post-crossing samples); `S29_no_haptic_below_6_samples`
  → `S29_no_haptic_below_4_samples`; new `S29_first_verdict_timing_is_20s_windows_10s_apart` pins
  the exact "20s first verdict, windows 10s apart" claim as a literal assertion, not merely an
  emergent property of the sample-count tests. `S29_stops_after_30_pings` →
  `S29_stops_after_max_pings` (drives `FF_FIND_MAX_PINGS` symbolically so it doesn't silently stop
  meaning "30" once the constant changes again). App (`app/ff_shell.c`'s end-to-end wiring,
  `test_shell.c`): `S29_find_session_sends_ping_at_10s_cadence` →
  `..._at_configured_cadence` (drives `FF_FIND_PING_INTERVAL_MS` symbolically); the two
  trend-crossing/haptic wiring tests re-derived for 4 samples. FireflyKit (Swift):
  `BridgeFindTests.testPingCadenceIsRateLimitedRegardlessOfCallFrequency` (literal ms constants
  updated 10,000/9,999 → 5,000/4,999 — this test drives the REAL `ff_find_t` via `FindBridge`, so
  only the transcribed literals needed to change, not any logic) and
  `testSessionCapsAtThirtyPings` → `testSessionCapsAtMaxPings` (30/40 → 60/70);
  `RadarViewModelTests.testFindWarmerCrossingFiresExactlyOncePerCrossing` rewritten for the 2-sample
  window against `MockFindSession`.

  **Gates.** clang and gcc-14 sim builds, zero warnings; `ctest` all green (105/105); FireflyKit's
  full Swift test suite green (1114 tests, 5 pre-existing skips unrelated to this change, 0
  failures) on both the direct `swift test` run and through the app's own package graph.

## 2026-09-16 correction — the airtime baseline was wrong by ~2x

The "Airtime" section above (written with PR 2, 2026-09-07) estimates a ~22 B
packet at LONG_FAST as **150-210 ms** on air, and every cadence decision since —
including this same day's 10 s -> 5 s halving — was reasoned against it. That
figure is **too low by roughly a factor of two**. Found by the independent
reviewer of the cadence change, then recomputed twice from the Semtech
SX1261/2 datasheet formula before being accepted.

Run `tools/lora_toa.py` (added with this correction) rather than trusting any
number quoted in prose:

```
LONG_FAST = SF11 / BW250 kHz / CR 4-5, 8-symbol preamble, CRC on
  symbol time      8.192 ms (< 16 ms, so low-data-rate optimisation is OFF)
  PING  (22 B)     28 payload symbols  ->  330 ms
  PONG  (27 B)     33 payload symbols  ->  371 ms
  round trip                                  700 ms
  at 10 s (pre-2026-09-16)  7.0% of channel for one session, 14.0% for two
  at 5 s (current)          14.0% of channel for one session, 28.0% for two
```

Where the original went wrong: it counted roughly 70-85 symbols total for a
short packet, when the preamble alone is 12.25 symbols (100.4 ms) and the
28-symbol payload adds 229 ms. The symbol count is right; the arithmetic that
turned symbols into milliseconds was not.

**What this changes.** The honest numbers for a FIND session are **14%** of the
channel for one, **28%** for two running at once — not the 6.6-7.8% / 13.2-15.6%
this spec claimed an hour earlier. Both are below Meshtastic's own
`AirTime::isTxAllowedChannelUtil` backstop, and a session is still hard-capped
at 5 minutes and 60 pings, so the 5 s cadence **stands** — it was shipped on the
strength of the usability argument (a first warmer/colder verdict at 20 s
instead of 60 s), and that argument does not depend on the airtime figure. But
the margin is thinner than the earlier number implied, and this is a shared
physical resource: unicast addressing stops packets being *forwarded* across the
mesh, it does not stop them occupying the air that every nearby node competes
for. Several simultaneous FIND pairs at a festival all draw on the same budget.

**The lever this points at.** Every number above is a consequence of SF11.
LONG_FAST buys range this project does not need at a festival where the whole
crew is inside a few hundred metres. A faster modem preset cuts symbol time
geometrically — and with it airtime, transmit energy (see the 2026-09-16 power
audit) and the floor under the FIND cadence all at once. Not changed here: the
preset is a fleet-wide setting that every node must adopt together, and the
range cost has not been measured on real hardware. Named as the next thing to
test, not as a decision taken.

**Estimate discipline.** The 330/371 ms figures are computed, not measured — the
datasheet formula is exact for the modem, but real occupancy also includes
Meshtastic's own CSMA backoff before each transmission, which this does not
model and which only ever makes the true figure larger. No ammeter or radio
capture has ever been taken on this fleet; when one is, it supersedes this.
