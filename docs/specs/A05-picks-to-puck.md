# A05 — My Lineup on the puck: picks, happenings and your own events

**Status:** SPEC'D 2026-09-16, **DEFERRED until after Lost Lands** by Jake's call — not
field-critical, and the two days before the festival go to what already exists
(battery, close-range finding, the field build). Decisions are all taken (§8);
nothing here is built.
**Surfaces:** Firefly app (iOS), puck firmware, fest-almanac pack, settimes (read-only mirror).
**Ask (Jake, 2026-09-16):** "a targeted feature for loading a user's favorites to a
puck from the Firefly app. This should include adding support for the sidequests
and meetups as well as users being able to add their own events. Very similar to
the set times website again."

## 1. What exists today (facts, verified 2026-09-16)

**settimes (github.com/jakeholland/settimes, `docs/SPEC-happenings.md`)** already
has the whole model this feature mirrors:
- a **pick** is a set id `"<stage>-<YYYY-MM-DD>-<HH:MM|tba>-<artist-slug>"`,
  shared as a 6-ish char FNV-1a/base36 **code** in `?picks=a.b.c`;
- a **happening** (`meet-greet`, `meetup`, `workshop`, `activity`, `side-quest`,
  `official`, open vocabulary) comes from the pack's `events` array, has id
  `"h:<pack event id>"`, gets a code the same way and is picked exactly like a set;
- an **own event** (`{title ≤40, night, startMin, endMin|null, where ≤80, note ≤140}`)
  is not a pick — existence is the pick — and travels as `?own=` base64url of
  `[[title, nightIndex, startMin, endMin|null, where], …]`, capped at 1,500 chars;
- a happening with no `end` is assumed 30 minutes; a cancelled pick stays,
  struck through; incoming links always **ask** before applying.

**Firefly app** already ports the pick model byte-for-byte: `PicksCodec.swift`
(`setID`, `shortCode`, `encodePicks`/`decodePicks`, `parsePicksInput`,
`shareURL` → `settimes.kandiwooks.com/<slug>/<year>/<day>?picks=…`), `PicksStore`
(per-festival, in Settings), `LineupScreen` "My picks" tab with ShareLink and
Import. It does **not** read `events`, has no happenings tab, no own events,
and nothing that talks to a puck.

**Puck** parses the pack at boot (`fp_parse`, embedded JSON; `events` is
skipped). `fp_set_t.starred` exists and `ff_sched_next_starred` drives the
Lineup face's one "IN N MIN" next-card, but **nothing sets `starred`** and
nothing persists it. `ff_sched_alarm_tick` (T-15 alert) is written and tested
and **never called**; `ff_notify` (BANNER / TAKEOVER, S26d) is wired and is the
delivery target. No NVS picks; `ff_store.h` names "starred-sets" as future.

**Links between phone and puck.** The puck has no BLE and no Wi-Fi; no camera.
Every app↔puck byte goes phone → its Heltec (BLE) → LoRa → the puck's comms
brain → UART. Private frames on portnum 269, `[ver][type][body]`, **200 B body
cap**, `mc_send_private` / `sendPrivate(_:to:wantAck:)` both exist. USB
debug-console verbs exist for the bench.

**Pack (fest-almanac 7abcbbd, PR #344).** 87 events: meetup 35, side-quest 25,
meet-greet 20, activity 4, workshop 3; 73 have no `end`, 28 no `start`, 22 no
`day` (all-weekend); 33 sit at a stage, 29 at a landmark, 32 free-text only.
**All 9 landmarks have null coordinates** — nothing at a landmark can be
navigated to yet, only read.

## 2. Goal

The person's **My Lineup** — picked sets, picked happenings, and their own
events — lands on their puck over the mesh, survives reboots, and the puck
uses it: a "MY LINEUP" list, the next-up card, and a T-15 minute nudge. No
internet anywhere in the loop. Honest-data: the puck shows only what it was
sent, says when it was sent, and never invents a time or a place.

## 3. Wire format — what the phone sends (RECOMMENDED: resolved items, not codes)

Two options were weighed:

- **A. settimes codes** (`a.b.c` for sets and happenings, `?own=` tuples for
  own). Smallest (30 picks ≈ 200 B). But the puck must re-derive every id —
  port `slugifyArtist` and the id grammar to C, parse `events` at boot to know
  happening ids, and any drift between the puck's embedded pack and the phone's
  live almanac copy silently drops picks with no way to say which.
- **B. resolved items.** The phone sends what the puck will show. A set is
  identified by fields the puck already has — `(stage id, night index,
  start_min)` — so starring is an exact match, no hashing, no slug port. A
  happening or own event carries its own title/time/place, so the puck needs
  **no `events` parsing at all** and works even when the phone's pack is newer.
  ~50–70 B per item; 40 items ≈ 2.5 KB ≈ 13 frames.

**Decision proposed: B.** Airtime for 13 frames is ~5 s on the bench channel;
it happens once when the person taps Send, not continuously.

Item records (CBOR-free, hand-packed little-endian, all strings NUL-padded):

```
kind u8      0 = set, 1 = happening (pack), 2 = own
night u8     index into the pack's festival nights (0 = first night)
start i16    minutes from that night's midnight, ≥1440 after midnight, -1 = time TBA
end   i16    same space, -1 = unknown (consumer assumes 30 min for kinds 1/2)
--- kind 0 (set):       stage_id char[16]            (fp_stage_t.id)
--- kind 1/2:           title char[40], where char[40], kind_label char[12]
                        (kind_label: "MEETUP", "SIDE QUEST", "YOURS", … as shown)
```
Sets: 22 B. Happenings/own: 98 B. 40 mixed items ≈ 2.4 KB. Caps: **48 items**
per lineup (`FF_PICKS_MAX`), titles/where truncated on the phone with an
ellipsis, never on the puck.

Envelope (private frame type `FF_PRIV_LINEUP`, one new type in `ff_proto.h`):

```
gen u16      generation: phone increments per Send; puck keeps only the newest
seq u8, total u8
slug_hash u32   FNV-1a of "<slug>-<year>" so a Bass Canyon lineup never lands on a Lost Lands puck
items[]      whole records only, ≤ 200 B per frame → 8 sets or 1 happening + 5 sets per frame
```
The puck reassembles by `gen`; applies **atomically** when `total` frames are
in; discards a partial set after 60 s; replies `FF_PRIV_LINEUP_ACK {gen, applied,
unmatched}` so the phone can say "puck has 23 of 24 · 1 set not in the puck's
pack". The codec lives in `firmware/core` (C), so the **app uses the same code**
through FireflyCore — one encoder/decoder, tested once.

## 4. Transport and addressing

- Mesh private frame on the crew channel, unicast to the **puck's node**, with
  `wantAck` per frame. Which node is "my puck"? The puck's SHOW CODE QR
  (`firefly://crew?…`) can carry its node id; the app remembers it as *my puck*
  after a scan, and shows it in Settings ("My puck · !8f48af24 · seen 2 min
  ago"). A crew member's puck is never a valid target.
- **Bench and dev:** a USB console verb `lineup <hex>` feeding the same decoder,
  and `lineup` alone printing what is stored (S15-style seam, no UI shortcuts).
- Not chosen: the phone connecting to the puck's own comms brain over BLE (fast,
  but the app becomes a two-radio client and that touches every connect flow
  two days before the field test).

## 5. Puck behaviour

- **Store:** `ff_picks` in core — `FF_PICKS_MAX` records + `gen` + `slug_hash` +
  received-at (wall time, honest "SENT FRI 4:12 PM" or "never"). Persisted as
  one NVS blob via `ff_store`; reapplied after every pack load. Sets whose
  `(stage, night, start)` no longer match the puck's pack are kept as
  *unmatched* (counted in the ack, shown nowhere).
- **Apply:** for kind 0, set `fp_set_t.starred` (through `ff_sched_toggle_star`'s
  sibling `ff_sched_set_star(idx, bool)`; every apply first clears all stars).
  Kinds 1/2 live only in `ff_picks`.
- **Lineup face:** the next-card considers all three kinds (`ff_sched_next_starred`
  grows a merge with `ff_picks`); a new **MY LINEUP** list (scrollable rows:
  time · title · where, kind chip for 1/2, set rows in stage colour like the
  app) reachable from the Lineup face — tap targets per `docs/hardware/tap-targets.md`
  (80 px rows). Empty state: "nothing sent yet — Send from the app's My Lineup".
- **Nudge:** wire `ff_sched_alarm_tick` for kind 0 and add the same for 1/2:
  BANNER at T-15 ("IN 15 MIN · Cyclops meet & greet · outside Wompy Woods"),
  never TAKEOVER, gated by quiet hours (S26). One nudge per item per boot.
- **Diagnostics row:** "LINEUP 23 items · gen 4 · sent Fri 4:12 PM".

## 6. App behaviour

- **Happenings:** parse `events` in Swift (`FestpackEvents.swift`, JSONDecoder,
  tolerant of unknown kinds and of `start`/`day` null) from the same bytes
  `FestpackParser` already receives — no C change. A **Meetups** segment in
  LineupScreen mirroring settimes C1: time-sorted, `time TBA` group at top,
  `all weekend` collapsed group at bottom, kind chip, `where`, host handle,
  source link (hidden offline), heart to pick. Picks of happenings use
  `"h:<id>"` ids and the existing codec, so **share links stay settimes-compatible**.
- **Own events:** `OwnEventsStore` (per festival, Settings-backed, caps 40/80/140)
  + `OwnEventSheet` (what / when with resolved-day caption / until / where with
  stage+landmark chips / note). Existence is the pick. `?own=` encode/decode
  ported from `picks.ts` so links round-trip with the site; incoming always asks.
- **Send to puck:** on My Lineup: a `Send to puck` primary button with a status
  line ("puck has 23 · sent 4:12 PM", "waiting for puck…", "not connected",
  "no puck paired — scan its code"). Manual send in v1; a badge when the lineup
  changed since the last send. Progress per frame; failure names the frame.
- **Telemetry (A04):** `lineup.send` {items, frames, ok, unmatched, ms}.

## 7. Slices (each: spec ref, tests, review, PR)

| # | slice | surface | size | LL-critical? |
|---|---|---|---|---|
| 1 | `ff_lineup` codec + `ff_picks` store + NVS + apply-to-sched + ack, unit tests, bench verb | core + puck | M | yes |
| 2 | "my puck" pairing (node id in SHOW CODE QR + app memory + Settings row) | puck + app | S | yes |
| 3 | App Send to puck (resolved-item builder from picks, frame sender, ack UI, telemetry) | app | M | yes |
| 4 | Puck MY LINEUP list face + next-card merge + T-15 nudge | puck | M | yes (list), nudge nice-to-have |
| 5 | App happenings: `events` parse + Meetups segment + picks of happenings + share compat | app | M | wanted |
| 6 | App own events: store + sheet + `?own=` share | app | M | wanted |
| 7 | Field test script: send 24-item lineup on the bench mesh, reboot puck, verify persistence + nudge | both | S | yes |

Sets-only (1–4, 7) is the smallest thing that is useful on Sep 18. 5 and 6
make the phone match settimes; they ride the same wire format with no puck
change, so they can land after the puck slices without reflashing.

## 8. Decisions — TAKEN (Jake, 2026-09-16)

1. **Wire format: B, resolved items.** Sets travel as `(stage id, night index,
   start_min)`; happenings and own events carry their own title/where/time. The
   puck never re-derives a settimes id, never parses `events`, and a newer pack
   on the phone cannot silently drop picks.
2. **Scheduling: deferred until after Lost Lands.** Not field-critical. The
   festival window goes to the battery question, close-range finding and the
   field build. Nothing in this spec is cut — it is queued, not trimmed.
3. **Nudge: BANNER at T-15**, never TAKEOVER, gated by quiet hours, one per item
   per boot. Wires the already-written, never-called `ff_sched_alarm_tick`.
4. **Pairing: the puck's SHOW CODE QR carries its node id**, and the app
   remembers it as "my puck". **There is no Wi-Fi or Bluetooth involved** — see
   §4's transport note and the correction below. A crew member's puck is never a
   valid target.
5. **Own-event titles: truncate at 40 characters** on the phone, with an
   ellipsis, matching settimes. The puck never truncates a string it was sent.
6. **Landmarks stay text-only.** All nine landmarks in the Lost Lands pack carry
   null coordinates, and a pack can never be assumed to place them, so an event
   at "the T-Rex" is a readable place and not a navigable one. No slice waits on
   fest-almanac surveying anything.

### Correction recorded: the puck has no Wi-Fi and no Bluetooth

Asked during decision 4 whether the phone would "hook up to puck Wi-Fi". It
cannot, today, on either radio:

- **Bluetooth is not built in.** `CONFIG_BT_ENABLED` is unset in the target's
  `sdkconfig` — there is no host stack in the image at all.
- **Wi-Fi is never brought up.** ESP-IDF leaves `CONFIG_ESP_WIFI_ENABLED=y` by
  default (the S3 silicon has the radio), but no Firefly code calls
  `esp_wifi_init`, `esp_wifi_start`, or creates a netif — grepped across the
  whole `firmware/` tree, zero hits. The option being on is a build default, not
  a feature.

So every byte between the phone and the puck goes phone → its own Heltec (BLE) →
LoRa → the puck's comms brain → UART, which is why §3's 200-byte frames and §4's
chunking exist.

**A future option worth naming, not chosen here:** the S3 *does* have Wi-Fi
silicon, so a SoftAP on the puck for bulk transfer (a whole lineup, or a fresh
festpack, in one shot instead of thirteen LoRa frames) is buildable. It costs
flash, RAM, a power budget that the 2026-09-16 battery work has not yet sized,
and an honest answer to "what happens when two pucks are in range". Revisit it
when the lineup feature comes back off the shelf, alongside the festpack-load
path S05 still lists as open.

## 9. Out of scope

Navigating to an event's landmark (needs coordinates); editing picks on the
puck; puck → phone sync; calendar export on the puck; happenings on the puck
without a phone send.
