# marketing/ — App Store screenshots and the tour video

Everything here is generated, not hand-captured: `app/tools/store_media.sh`
drives the iOS Simulator through the app's own demo stack
(`-FireflyDemo`, `-FireflyDemoScreen <name>` — `app/README.md`, "Debug
launch arguments") and captures the result with `xcrun simctl`. No
seeded review account, no manual screenshot-and-crop pass: re-running the
script reproduces this directory from a clean checkout.

## Layout

```
marketing/
  screenshots/<size>/NN_<name>.png   committed — see "What's committed" below
  recordings/tour.mp4                git-ignored — see "What's committed" below
  README.md                          this file
```

## Regenerate everything

```sh
cd app && xcodegen generate   # if project.yml changed
app/tools/store_media.sh
```

Takes one clean iOS Simulator build (a few minutes on a cold cache) plus
a few seconds per shot. Screenshots and the recording land in this
directory, numbered/named exactly as committed. See
`app/tools/store_media.sh`'s own header comment for every environment
override (`SHOW_BADGE`, `SKIP_VIDEO`, `CLEANUP`,
`FIREFLY_STORE_MEDIA_DERIVED_DATA`).

## Sizes — what Apple actually requires (checked 2026-09-15)

Source: [App Store Connect screenshot specifications](https://developer.apple.com/help/app-store-connect/reference/app-information/screenshot-specifications/).

Apple's current rule for a new iPhone submission: you need screenshots
for **either** the 6.9" size class **or** the 6.5" one, not both — App
Store Connect scales the one you omit from the one you provide. 6.3" is
optional on top of either and falls back to 6.5" if you skip it. There
is no per-model requirement beyond that — "6.9-inch" is a *size class*
covering iPhone Air / 17 Pro Max / 16 Pro Max / 16 Plus / 15 Pro Max /
15 Plus / 14 Pro Max, not one specific phone.

Firefly ships **6.9" only**, captured on the **iPhone 17 Pro Max**
simulator (the accepted pixel size for this class: 1320×2868, 1290×2796,
or 1260×2736 — the simulator's own native screenshot resolution is
whichever of those the device type actually renders at, verified against
the committed PNGs). That is the smallest set that satisfies Apple's
requirement, and 6.9" is the highest-resolution class, so it is also
what every smaller class scales down from when nothing else is
uploaded — there was no reason to also generate and maintain 6.5"/6.3"
sets that Apple does not require and that would need separate
simulators, separate builds-per-size bookkeeping, and separate review
every time a screen changes.

If that changes (Apple's own policy has moved before, and it may again),
add a row to `SIZES` in `app/tools/store_media.sh` — any device type
`xcrun simctl list devicetypes` knows about. The script already builds
once and fans the same install out across however many simulators
`SIZES` names; nothing else about it assumes exactly one size.

**No iPad row.** Firefly is iPhone-only —
`app/project.yml`'s `Firefly` target sets `TARGETED_DEVICE_FAMILY: "1"`
("the app is a pocket compass; iPad runs it in compatibility mode") — so
an iPad screenshot set is not applicable and none is generated.

## The shot list (`marketing/screenshots/6.9in/`)

Numbered in the order they are meant to appear in the App Store gallery
— the hero (Find/Radar, crew visible) first:

| # | File | Screen | `-FireflyDemoScreen` |
|---|------|--------|----------------------|
| 1 | `01_radar.png` | Find — Radar, crew visible | `radar` |
| 2 | `02_map.png` | Find — Map | `map` |
| 3 | `03_flare.png` | Inbound FLARE full-screen alert | `flare` |
| 4 | `04_thread.png` | Inbox — a real thread | `thread` |
| 5 | `05_lineup.png` | Lineup | `lineup` |
| 6 | `06_crew.png` | Crew screen | `crew` |
| 7 | `07_join.png` | Join a crew (QR) | `crew-join` |

One deliberate substitution from "inbox thread with a FLARE showing" as
originally scoped: `FlareTakeoverViewModel`'s takeover is a full-screen
overlay that renders **on top of whatever tab is selected, by design**
(S10: "regardless of current face" — `RootView.swift`'s own comment on
the `flare` demo screen) — it is not possible to have the takeover
visible **and** see the thread list/bubble underneath it in the same
frame without adding a UI-automation step to dismiss the takeover first,
which felt like scope creep for one shot. Shot 3 shows the FLARE alert
itself (arguably the more dramatic, more marketing-worthy of the two
anyway); shot 4 shows an ordinary inbox thread on its own. `ThreadView`
does render FLARE as a message bubble inline (`FlareRallyRow`/the
`.flare` message case) for anyone who wants that exact combination shot
later — it would need `StoreTourUITests`-style UI automation (trigger,
then tap through to dismiss), not a bare `-FireflyDemoScreen` launch.

Every shot ships WITHOUT the **DEMO badge strip** (`DemoBadge.swift`) by
default — see "The DEMO badge" below for why store screenshots are the
one place that strip is dropped, and how to opt it back in.

## The tour video (`marketing/recordings/tour.mp4`)

`xcrun simctl io <udid> recordVideo --codec h264` runs for the duration
of `FireflyUITests/StoreTourUITests` (`app/FireflyUITests/StoreTourUITests.swift`),
a scripted walk — Radar → Map → Field → Radar → Inbox → the CREW thread
→ Lineup — with deliberate multi-second pauses between stops (its own
header comment has the full shot-by-shot budget), landing at roughly
20-25 seconds of paced content. That test is gated behind a marker file
(`/tmp/firefly-store-tour.enabled` — `store_media.sh` drops it right
before `xcodebuild test` and removes it right after, win or lose) rather
than an environment variable: the test runs inside an iOS Simulator, and
nothing there inherits this shell's environment the way
`FireflyHardwareTests`' `FIREFLY_HARDWARE` gate does for its `platform
=macOS` destination (`StoreTourUITests.swift`'s own header has the full
story, including why the seemingly-equivalent `environmentVariableEntries`
test-plan mechanism was tried first and didn't work). Either way, the
property that matters is unchanged: it **never runs in CI**, because CI
never creates that marker.

### App Store app-preview requirements — trimming/converting

Apple's app-preview specs (checked 2026-09-15, same source pattern as
the screenshot sizes above:
[App Store Connect app preview specifications](https://developer.apple.com/help/app-store-connect/reference/app-preview-specifications/))
want 15–30 seconds, H.264 (or ProRes 422 HQ), up to 30 fps, and for the
**6.9" class specifically, 886×1920 portrait** (1920×886 landscape) —
the SAME accepted resolution Apple lists for the 6.5" and 6.1–6.3"
classes too, so this one clip covers all three once trimmed/converted;
5.5"/4.7"/4" use their own smaller sizes if you ever need those. Verify
against the live page before an actual upload — Apple has changed this
table before. `tour.mp4` comes straight off the simulator at its native
resolution (NOT 886×1920) and is not guaranteed to already match — trim/
convert it with `ffmpeg` if you have it installed (this pipeline never
requires it — the recording step above needs nothing but `simctl`).

**Trimming is not optional cosmetic polish here — `tour.mp4` starts with
DEAD AIR, not the tour.** `store_media.sh` starts `simctl recordVideo`
right before the `xcodebuild test` that drives `StoreTourUITests`, and
everything that command does before `app.launch()` actually appears on
screen — installing the `FireflyUITests` runner alongside `Firefly.app`,
starting the XCTest daemon on the simulator — gets recorded as an idle
Home Screen. The script pre-builds with `xcodebuild build-for-testing`
outside the recording window specifically to cut this down (it used to
be ~4 minutes of dead air on a cold cache; pre-building brought that to
roughly 1–2 minutes), but the remaining install/launch overhead is
inherent to how `xcodebuild test` drives a simulator and varies with
how loaded the machine is — it is not something a fixed `-ss` offset
can be relied on to skip correctly run to run. **Find the real start
before trimming**, don't assume it:

```sh
# Inspect duration/resolution
ffprobe -v error -show_entries stream=width,height,duration -of default=noprint_wrappers=1 marketing/recordings/tour.mp4

# Scrub for the frame where the Radar hero screen actually appears —
# QuickTime Player (open the file, drag the scrubber) is the easiest
# way; or pull test frames with ffmpeg and eyeball them:
ffmpeg -y -ss 90 -i marketing/recordings/tour.mp4 -frames:v 1 -update 1 /tmp/check.png   # adjust -ss and repeat

# Once you know the real start (a made-up example below: say the Radar
# hero screen actually appears at 108s — the tour itself paces out to
# ~20-25s from there, so 108+25=133 is a safe end), trim and scale/pad
# to the accepted 6.9" preview resolution without distorting the aspect
# ratio. Replace 108/133 with what you actually found above.
ffmpeg -i marketing/recordings/tour.mp4 -ss 108 -to 133 \
  -vf "scale=886:1920:force_original_aspect_ratio=decrease,pad=886:1920:(ow-iw)/2:(oh-ih)/2" \
  -c:v libx264 -pix_fmt yuv420p -an marketing/recordings/tour_preview.mp4
```

`-an` drops audio — the simulator recording carries none, and App Store
Connect accepts a silent preview.

## The DEMO badge

`DemoBadge.swift` draws the **DEMO** strip everywhere demo mode runs —
`docs/specs/S20-demo-mode.md`'s own rule, "the mode is clearly labeled
DEMO so it never masquerades as live field data" — and that stays the
default for every use of `-FireflyDemo` EXCEPT one: the still screenshots
`store_media.sh` generates for the App Store (`SHOTS`, above). Those are
customer-facing marketing material, not a bench artifact, so as of
2026-09-15 (Jake's call) they ship without the strip by default via
`-FireflyDebugHideBadge` (DEBUG-only,
`app/Firefly/Sources/FireflyDebugHideBadgeLaunch.swift`) — the seam that
suppresses `DemoBadge` for one launch. `SHOW_BADGE=1` opts a run back
into the strip on the screenshots, e.g. for an internal/bench review set:

```sh
SHOW_BADGE=1 app/tools/store_media.sh
```

**The tour video keeps the badge.** `StoreTourUITests` never passes
`-FireflyDebugHideBadge`, so `marketing/recordings/tour.mp4` shows the
strip the same as any other demo-mode run — this substitution is scoped
to the still gallery only, not blanket "hide it everywhere store media
appears."

`-FireflyDebugHideBadge` is DEBUG-only by construction — compiled out
entirely in a Release/TestFlight/App Store build, so it can never affect
what a real user sees even inertly, and only ever available to a
screenshot script that is itself only ever run against a Debug simulator
build.

## What's committed

- **`screenshots/**/*.png` — committed.** Small (a handful of PNGs),
  deterministic (the demo stack seeds the same content every run — no
  live network state, no timestamps beyond the fixed 9:41 status bar),
  and reviewable in a diff like any other asset in this repo
  (`docs/screens/*.png` already sets this precedent).
- **`recordings/*.mp4` — NOT committed** (`.gitignore`). Video is not
  small (tens of MB even for a 20-second clip) and not something a PR
  diff can usefully review frame-by-frame; regenerating it is one
  command (`app/tools/store_media.sh`, a few minutes) against the same
  deterministic demo stack, so there is nothing here that regeneration
  loses. Upload the exported preview to App Store Connect directly
  rather than routing it through git.
