# S20 · demo mode — Firefly Fields (seeded festival + crew, every screen alive)

## Purpose

Boot the puck (and the sim) into a fully-populated fictional world so **every
face shows real-looking data** with no mesh, no comms brain, no real festival:
a lineup on Now, an arrow locking onto a friend on Radar, dots on the Map,
chatter on Signals. For demos, screenshots, dev, and just seeing the thing work.

Honesty note: demo data is REAL state seeded through the REAL core APIs
(`ff_crew_on_position`, `ff_feed`, the festpack parser, the wall clock) — it is
not faked freshness or a canned splash. Within the demo world it is true; the
mode is clearly labeled DEMO so it never masquerades as live field data.

## The festival — FIREFLY FIELDS 2026 (invented; have fun with it)

A three-night bioluminescent night-forest rave. Tagline energy: *find your
people in the dark.* All artists are FICTIONAL (no real names/likenesses —
brand-safe and funnier). Dates **Fri Sep 4 – Sun Sep 6, 2026** (deliberately a
different weekend from the real Lost Lands test so it's unmistakably the demo).
Venue origin (map projection anchor): **lat 43.7000, lon -121.5000** (an open
high-desert field — fictional). Place all landmarks/features within ~400 m of
origin (≈ ±0.0035°) so the Map has spread without absurd scale.

### Stages (id · name · color)
- `beacon` · **The Beacon** · `#FFC66B` (mainstage, headliners)
- `hollow` · **Bass Hollow** · `#B08CFF` (bass / dubstep)
- `grove` · **Sunrise Grove** · `#4FD8C4` (house / melodic / sunrise sets)
- `lantern` · **The Lantern** · `#FF5CA8` (techno tent)
- `glowworm` · **Glowworm** · `#9BE07B` (discovery / small)

### Lineup (fictional acts — ~20 sets across the 3 nights; agent lays out
plausible times, one headliner per night on The Beacon, a couple starred)
- **The Beacon** (headliners): DJ COMPASS · LOST + FOUND (b2b) · FIREFLY ·
  PHOTON · NITE OWL
- **Bass Hollow**: BASS FAUNA · SUBWOOFER SAM · NEON YETI · GRIMEWORM · THE GRID
- **Sunrise Grove**: LUMEN · MEADOWLARK · DEEP FIELD · GLOW COYOTE · TWILIGHT FUNCTION
- **The Lantern**: VOLTAGE · KANDI KART · BIOLUMA · MOTH + FLAME · STROBE CAT
- **Glowworm**: WISP · EMBER · PIXIE DUST · DJ GLOWSTICK · FEN

Nice touches: **DJ COMPASS** and **LOST + FOUND** wink at the friend-compass
premise; **FIREFLY** headlines its own festival. Star FIREFLY (Sat headliner)
and one Sunrise Grove set so Now shows a starred-set countdown.

### Map landmarks / features (positioned near origin — the Map anchors on these)
- **The Firefly Tower** (central meetup landmark — the "find your people" beacon)
- **Main Gate**, **Medical**, **Water Refill** (x2), **Food Row**,
  **Silent Disco**, **The Art Car**, **Ferris Wheel**
- **Camp Glow** and **Camp Ember** (two camping areas, as features/polygons if
  handy, else landmarks)
Give the 5 stages map positions too (they're the anchors people navigate to).

## Seeded crew, positions, and signals (Radar / Map / Signals come alive)

Seed via the real core APIs so freshness/close-range/no-fix all render honestly:
- **DANA** — near The Beacon, LIVE, ~30 m. (fresh arrow + distance)
- **KEV** — at Bass Hollow, LIVE, ~120 m.
- **RILEY** — by the Ferris Wheel, LIVE and **~15 m** → exercises Radar
  **close-range** mode (rings, not a distance).
- **MAYA** — Camp Glow, **STALE** (last position ~25 min ago → "LAST SEEN 25 MIN").
- **SAM** — paired but **no fix** (honest unknown / no-position state).
So Radar shows the full spread: live arrow, close-range rings, stale, no-fix.

**Signals feed** (a few, via ff_feed):
- two statuses from **KEV**: "at bass hollow!" / "who's got water?" (2026-09-02:
  the second used to be a PULSE — retired, see S04's Amendments — replaced
  with a second status so the seed keeps its item count)
- a **rally point** dropped at **The Firefly Tower**
- canned reply from **MAYA**: "omw"
- a message: "lineup is stacked tonight 🔥" (emoji only if the font has it; else plain)

**Demo wall clock**: latch to **Sat Sep 5 2026, ~21:30 local** (peak) so The
Beacon's Saturday headliner (FIREFLY) reads mid-set with a live progress bar,
neighbors read UPCOMING, and the starred-set countdown is ticking.

## Implementation (agent)

- **The festpack**: author `firmware/assets/demo/firefly-fields.festpack.json`
  to the festpack v0.1 schema (match an existing full fixture, e.g.
  `firmware/festpack/tests/fixtures/lost-lands-2026.festpack.json`): `festival`
  {name/year/start/end/venue lat,lon}, `stages[]` {id,name,color}, `schedule[]`
  {artist,stage,day,start,end}, `map.features[]`/`landmarks[]` with real
  lat/lon near origin. Keep within FP_MAX_* caps. This is DEMO/fixture data and
  lives in the Firefly repo (NOT fest-almanac, which is real festivals only).
- **Demo seeding**: a `ff_demo_seed(shell)` entry (app/target layer, not core)
  that parses the embedded festpack into the shell AND seeds the crew/positions/
  feed above through the existing `ff_crew_*` / `ff_feed_*` / wall APIs. Reuse
  the sim's fixture/seed patterns if any. Core stays pure — the seed lives at
  the app/target boundary.
- **Gate**: SIM — a `--demo` flag (or a committed demo fixture the sim can load)
  so `ffsim --demo` renders every populated face + goldens/screenshots.
  DEVICE — a bring-up STAGE or `CONFIG_FF_DEMO_MODE` that, on boot, `EMBED_FILES`
  the demo festpack and runs `ff_demo_seed`, so the puck shows the full world.
- **Screenshots**: generate sim screenshots of the populated Radar/Now/Signals/
  Map/Compose in demo mode (the S13 screenshot path) so we can see it without a
  device, and (optional, nice) a social GIF cycling the faces.

## Acceptance criteria
1. `ffsim --demo` (or the demo fixture) boots into Firefly Fields with EVERY
   face populated: Radar shows DANA (live arrow) / RILEY (close-range) / MAYA
   (stale) / SAM (no-fix); Now shows the Saturday lineup with FIREFLY mid-set +
   a starred countdown; Map shows stages + landmarks + crew dots + the rally
   pin; Signals shows the seeded feed.
2. All seeded state flows through the real core APIs (honest — no faked
   freshness); demo is clearly labeled DEMO.
3. Device: `CONFIG_FF_DEMO_MODE` build embeds the festpack + seeds on boot,
   `idf.py build` exit 0.
4. Sim gate stays green; new demo screenshots committed; core/ unchanged
   (seeding at app/target boundary; the only core touch, if any, is a tiny
   demo-seed helper that uses existing APIs — prefer none).
5. Fictional-only content (no real artist names/venues); festival dates in the
   wall-clock plausibility window.

## Out of scope
Persisting demo state, a Settings toggle for demo (a build/flag is enough for
now), real festival data (that's fest-almanac).
