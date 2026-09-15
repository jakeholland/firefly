#!/usr/bin/env bash
# store_media.sh — generate App Store screenshots and the app-preview
# tour video for Firefly, end to end, from the demo stack. No manual
# simulator wrangling, no hand-typed launch arguments: run this and read
# marketing/screenshots/<size>/ and marketing/recordings/tour.mp4.
#
# WHAT IT DOES
#   1. Creates (or reuses) one iOS Simulator per required screenshot
#      size (see SIZES below), sets a clean status bar on each.
#   2. Builds Firefly.app for the simulator ONCE
#      (`-destination 'generic/platform=iOS Simulator'`), then installs
#      that same build onto every simulator it made — never rebuilds
#      per size.
#   3. For each shot in SHOTS: launches with `-FireflyDemo
#      -FireflyDemoScreen <name>` (app/README.md, "Debug launch
#      arguments"; the full menu of recognised names lives in
#      `RootView.runInitialDemoScreen()`), waits for the async demo
#      seeding to settle, and captures `xcrun simctl io … screenshot`
#      into marketing/screenshots/<size>/NN_name.png — numbered in the
#      order they are meant to appear in the App Store gallery.
#   4. Unless SKIP_VIDEO=1: records marketing/recordings/tour.mp4 with
#      `simctl io … recordVideo` while `FireflyUITests/StoreTourUITests`
#      (gated behind a marker file this script drops and removes around
#      the run — see that file's own header for why it's a file and not
#      an env var, and why it never runs in CI) taps through the same
#      screens with deliberate pauses.
#
# SIZES — WHICH APP STORE SCREENSHOT SIZES ARE ACTUALLY REQUIRED
#   Source, checked 2026-09-15: https://developer.apple.com/help/app-store-connect/reference/app-information/screenshot-specifications/
#   Apple's own current rule: a new iPhone submission needs screenshots
#   for EITHER the 6.9" class OR the 6.5" class, not both — whichever one
#   you omit, App Store Connect scales from the one you provided. 6.3"
#   is optional on top of either (falls back to 6.5" if not supplied).
#   Firefly ships 6.9" only (iPhone 17 Pro Max: the newest, highest-
#   resolution size, and the one every smaller class scales down from
#   when nothing else is uploaded) — this is a deliberate "smallest set
#   that satisfies the requirement" choice, not an oversight. Add a row
#   to SIZES below (any device type `xcrun simctl list devicetypes`
#   knows about) if Jake wants 6.5"/6.3" too; the rest of this script is
#   already written to build once and fan the same install out to
#   however many simulators SIZES names.
#   Firefly is iPhone-only (project.yml: TARGETED_DEVICE_FAMILY "1" —
#   "the app is a pocket compass; iPad runs it in compatibility mode"),
#   so no iPad row belongs here.
#
# USAGE
#   app/tools/store_media.sh
#
#   Environment overrides (all optional):
#     SHOW_BADGE=1            keep the DEMO strip on the STILL SCREENSHOTS
#                             (SHOTS below) instead of the default of
#                             passing -FireflyDebugHideBadge (DEBUG-only,
#                             FireflyDebugHideBadgeLaunch.swift) to every
#                             shot. Store screenshots are customer-facing
#                             App Store media, not a bench artifact — Jake
#                             decided 2026-09-15 they ship without the
#                             strip by default; see marketing/README.md,
#                             "The DEMO badge". The tour video and every
#                             OTHER use of demo mode are untouched by this
#                             flag and keep the badge, same as always —
#                             this only changes the SHOTS loop below.
#     SKIP_VIDEO=1            screenshots only, skip the recording step
#                             (and the extra xcodebuild test it runs).
#     CLEANUP=1                shut down and DELETE every simulator this
#                             run touched (created or reused) once done.
#                             Default 0 — simulators are left booted so a
#                             re-run reuses them and skips the create/
#                             boot cost.
#     FIREFLY_STORE_MEDIA_DERIVED_DATA=<path>
#                             where the build goes. Defaults to a
#                             per-machine temp dir OUTSIDE the repo/any
#                             worktree — never Xcode's own shared
#                             DerivedData, and never committed.
#
# Never run against the main checkout of this repo — run it from a
# worktree, like every other agent task here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$APP_DIR/.." && pwd)"
MARKETING_DIR="$REPO_ROOT/marketing"
PROJECT="$APP_DIR/Firefly.xcodeproj"
SCHEME="Firefly"
BUNDLE_ID="com.jakeholland.Firefly"

SHOW_BADGE="${SHOW_BADGE:-0}"
SKIP_VIDEO="${SKIP_VIDEO:-0}"
CLEANUP="${CLEANUP:-0}"
DERIVED_DATA="${FIREFLY_STORE_MEDIA_DERIVED_DATA:-${TMPDIR:-/tmp}/firefly-store-media-dd}"

# label|device type name (must match `xcrun simctl list devicetypes`)
SIZES=(
  "6.9in|iPhone 17 Pro Max"
)

# output-name|human label (for log output only)|-FireflyDemoScreen value
# Order here IS the App Store gallery order — Find/Radar hero first.
SHOTS=(
  "01_radar|Find — Radar (crew visible, hero)|radar"
  "02_map|Find — Map|map"
  "03_flare|Inbound FLARE alert|flare"
  "04_thread|Inbox — a real thread|thread"
  "05_lineup|Lineup|lineup"
  "06_crew|Crew screen|crew"
  "07_join|Join a crew (QR)|crew-join"
)

command -v jq >/dev/null 2>&1 || { echo "error: jq is required (brew install jq)" >&2; exit 2; }
command -v xcrun >/dev/null 2>&1 || { echo "error: xcrun not found — install Xcode command line tools" >&2; exit 2; }
[ -d "$PROJECT" ] || { echo "error: $PROJECT not found — run xcodegen first (app/README.md)" >&2; exit 2; }

log() { echo "== $*" >&2; }

RUNTIME_ID="$(xcrun simctl list runtimes --json \
  | jq -r '[.runtimes[] | select(.platform=="iOS" and .isAvailable==true)] | sort_by(.version) | last | .identifier')"
[ -n "$RUNTIME_ID" ] && [ "$RUNTIME_ID" != "null" ] || {
  echo "error: no available iOS simulator runtime found (xcrun simctl list runtimes)" >&2
  exit 2
}
log "using runtime $RUNTIME_ID"

devicetype_identifier() {
  local name="$1"
  xcrun simctl list devicetypes --json \
    | jq -r --arg name "$name" '.devicetypes[] | select(.name==$name) | .identifier' | head -1
}

find_or_create_sim() {
  local device_type_name="$1" sim_name="$2" identifier udid
  identifier="$(devicetype_identifier "$device_type_name")"
  [ -n "$identifier" ] || {
    echo "error: unknown simulator device type \"$device_type_name\" (xcrun simctl list devicetypes)" >&2
    exit 2
  }
  udid="$(xcrun simctl list devices --json \
    | jq -r --arg name "$sim_name" '.devices | to_entries[] | .value[] | select(.name==$name and .isAvailable==true) | .udid' \
    | head -1)"
  if [ -z "$udid" ]; then
    udid="$(xcrun simctl create "$sim_name" "$identifier" "$RUNTIME_ID")"
    log "created simulator \"$sim_name\" ($udid)"
    CREATED_SIMULATORS+=("$udid")
  else
    log "reusing simulator \"$sim_name\" ($udid)"
  fi
  echo "$udid"
}

boot_sim() {
  local udid="$1" state
  state="$(xcrun simctl list devices --json \
    | jq -r --arg udid "$udid" '.devices | to_entries[] | .value[] | select(.udid==$udid) | .state')"
  if [ "$state" != "Booted" ]; then
    xcrun simctl boot "$udid"
  fi
  xcrun simctl bootstatus "$udid" -b >/dev/null
}

CREATED_SIMULATORS=()
ALL_SIMULATORS=()
# Parallel arrays, not `declare -A` — macOS ships bash 3.2 as
# `/usr/bin/env bash`'s first PATH hit on a clean machine (no Homebrew
# bash installed), and 3.2 predates associative arrays entirely: a
# `declare -A` there fails immediately with "invalid option", before
# this script does anything else. Two parallel arrays plus a linear
# lookup (`udid_for_size`, below) cost nothing at this scale — SIZES has
# one row today and is expected to stay in the single digits — and run
# unmodified on both bash 3.2 and anything newer.
SIZE_LABELS=()
SIZE_UDIDS=()

udid_for_size() {
  local want="$1" i
  for i in "${!SIZE_LABELS[@]}"; do
    if [ "${SIZE_LABELS[$i]}" = "$want" ]; then
      echo "${SIZE_UDIDS[$i]}"
      return 0
    fi
  done
  echo "error: no simulator recorded for size \"$want\"" >&2
  exit 1
}

# Registered now, with both arrays already `()` — safe under `set -u`
# even if nothing has been created yet — so a failure ANYWHERE below
# (a bad build, an interrupted `xcodebuild test`, Ctrl-C) still runs
# this instead of leaking a booted-and-forgotten simulator. Only
# simulators THIS RUN created or found by the exact `SIM_NAME` this
# script uses ever land in `ALL_SIMULATORS` (`find_or_create_sim`'s own
# `jq` filter matches on that name) — nothing that is actually one of
# Jake's own simulators is ever in this array, so this can never touch
# one. Preserves the real exit status: `$?` is captured before anything
# else runs and returned at the end, so a failed step still fails the
# script even though the trap itself always succeeds.
cleanup_on_exit() {
  local status=$?
  # Belt-and-suspenders with the two explicit `rm -f "$TOUR_MARKER"`
  # calls in the video block below: those cover the ordinary
  # success/failure paths, this covers a Ctrl-C landing between them —
  # a stray marker must never outlive this script and silently arm
  # StoreTourUITests for a later, unrelated `xcodebuild test`.
  [ -n "${TOUR_MARKER_PATH:-}" ] && rm -f "$TOUR_MARKER_PATH"
  if [ "$CLEANUP" = "1" ]; then
    if [ "${#ALL_SIMULATORS[@]}" -gt 0 ]; then
      for udid in "${ALL_SIMULATORS[@]}"; do
        log "cleaning up simulator $udid"
        xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true
        xcrun simctl delete "$udid" >/dev/null 2>&1 || true
      done
    fi
  elif [ "${#ALL_SIMULATORS[@]}" -gt 0 ]; then
    log "leaving simulators booted for reuse (set CLEANUP=1 to delete them): ${ALL_SIMULATORS[*]}"
  fi
  return "$status"
}
trap cleanup_on_exit EXIT

mkdir -p "$MARKETING_DIR/screenshots" "$MARKETING_DIR/recordings"

for size_row in "${SIZES[@]}"; do
  IFS='|' read -r SIZE_LABEL DEVICE_TYPE_NAME <<<"$size_row"
  SIM_NAME="Firefly Store Media — $SIZE_LABEL"
  UDID="$(find_or_create_sim "$DEVICE_TYPE_NAME" "$SIM_NAME")"
  boot_sim "$UDID"
  # Clean, deterministic status bar — never the wall-clock time or the
  # host machine's real battery/signal, which would make the screenshot
  # set undated only by accident and non-reproducible besides.
  xcrun simctl status_bar "$UDID" override \
    --time "9:41" --batteryState charged --batteryLevel 100 --wifiBars 3 --cellularBars 4
  SIZE_LABELS+=("$SIZE_LABEL")
  SIZE_UDIDS+=("$UDID")
  ALL_SIMULATORS+=("$UDID")
  mkdir -p "$MARKETING_DIR/screenshots/$SIZE_LABEL"
done

log "building Firefly.app for the simulator (once) — derived data: $DERIVED_DATA"
xcodebuild \
  -project "$PROJECT" \
  -scheme "$SCHEME" \
  -destination 'generic/platform=iOS Simulator' \
  -derivedDataPath "$DERIVED_DATA" \
  build

APP_PATH="$(find "$DERIVED_DATA/Build/Products" -maxdepth 1 -name 'Debug-iphonesimulator' -type d)/Firefly.app"
[ -d "$APP_PATH" ] || { echo "error: built app not found at $APP_PATH" >&2; exit 1; }
log "built $APP_PATH"

for size_row in "${SIZES[@]}"; do
  IFS='|' read -r SIZE_LABEL DEVICE_TYPE_NAME <<<"$size_row"
  UDID="$(udid_for_size "$SIZE_LABEL")"
  log "installing on $SIZE_LABEL ($UDID)"
  xcrun simctl install "$UDID" "$APP_PATH"

  for shot in "${SHOTS[@]}"; do
    IFS='|' read -r NAME LABEL SCREEN <<<"$shot"
    xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
    LAUNCH_ARGS=(-FireflyDemo -FireflyDemoScreen "$SCREEN")
    # Store screenshots hide the DEMO strip by default — SHOW_BADGE=1
    # opts back in. The tour video below is untouched by this: it keeps
    # the badge, same as every other demo-mode use (marketing/README.md,
    # "The DEMO badge").
    [ "$SHOW_BADGE" != "1" ] && LAUNCH_ARGS+=(-FireflyDebugHideBadge)
    xcrun simctl launch "$UDID" "$BUNDLE_ID" "${LAUNCH_ARGS[@]}" >/dev/null
    # RootView's own demo-screen handling awaits DemoRunner.start() and,
    # for a few names (lineup-picks, rally, flare), an extra real
    # EventHub round trip — a fixed pause is generous enough for all of
    # them rather than special-casing each (see that function's own
    # "waitForLineupFestpack" comment for why a too-short fixed pause
    # once produced an empty screenshot).
    sleep 4
    OUT="$MARKETING_DIR/screenshots/$SIZE_LABEL/${NAME}.png"
    xcrun simctl io "$UDID" screenshot "$OUT" >/dev/null
    log "captured $OUT ($LABEL)"
  done
  xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
done

if [ "$SKIP_VIDEO" != "1" ]; then
  # The tour records on the FIRST size in SIZES — the same device the
  # hero screenshot came from, so the still gallery and the video agree
  # on what device they were captured from.
  read -r VIDEO_SIZE_LABEL VIDEO_DEVICE_TYPE <<<"$(echo "${SIZES[0]}" | tr '|' ' ')"
  UDID="$(udid_for_size "$VIDEO_SIZE_LABEL")"
  RECORDING="$MARKETING_DIR/recordings/tour.mp4"
  rm -f "$RECORDING"

  # Build the UI TEST TARGET first, BEFORE the recording starts —
  # `build-for-testing` compiles `FireflyUITests` (and re-links the app
  # host if anything changed) against this exact destination/derived
  # data, so the `test` invocation below that runs WHILE recording finds
  # everything already built. Skipping this step once produced a
  # ~240-SECOND recording for a ~20-second tour: the FIRST `xcodebuild
  # test` against a cold derived data cache spends minutes compiling
  # `FireflyUITests` before `StoreTourUITests` ever calls `app.launch()`,
  # and `simctl recordVideo` — already running, so it could capture the
  # app the instant it appeared — has nothing to show but an idle
  # simulator for that whole window. Caught by `ffprobe`-ing the
  # committed output, not by watching it run.
  log "pre-building FireflyUITests (outside the recording window)"
  (
    cd "$APP_DIR"
    xcodebuild build-for-testing \
      -project "$PROJECT" \
      -scheme "$SCHEME" \
      -testPlan FireflyUITests \
      -destination "platform=iOS Simulator,id=$UDID" \
      -derivedDataPath "$DERIVED_DATA"
  )

  log "recording $RECORDING from $VIDEO_SIZE_LABEL ($UDID)"
  xcrun simctl io "$UDID" recordVideo --codec h264 "$RECORDING" &
  RECORD_PID=$!
  sleep 2 # give simctl a moment to actually attach before the app launches

  # A marker FILE, not an env var — see `StoreTourUITests.swift`'s own
  # header comment for why: an iOS Simulator test runner is launched by
  # CoreSimulator, not forked from this shell, so nothing here inherits
  # into `ProcessInfo.processInfo.environment` on the other side — a
  # `FIREFLY_STORE_TOUR=1` shell prefix AND a trailing xcodebuild
  # build-setting override were both tried and both left the test
  # silently SKIPPED (`xcodebuild -showBuildSettings` resolves the
  # override fine; the test plan's `$(FIREFLY_STORE_TOUR)` macro simply
  # never expands for an iOS Simulator destination in this toolchain —
  # confirmed by having the test dump its own environment, not by
  # reasoning about it). The plain filesystem does reach across that
  # boundary. `TOUR_MARKER` must match `StoreTourUITests
  # .tourMarkerPath` exactly.
  TOUR_MARKER="/tmp/firefly-store-tour.enabled"
  TOUR_MARKER_PATH="$TOUR_MARKER" # seen by cleanup_on_exit's trap
  rm -f "$TOUR_MARKER"
  : >"$TOUR_MARKER"

  TEST_STATUS=0
  (
    cd "$APP_DIR"
    # Same target/destination/derived-data as the `build-for-testing`
    # step above, so this is an incremental (near-instant) build check
    # followed immediately by the actual test run — not a second full
    # compile — keeping the RECORDED window close to the ~20-25s the
    # test itself paces out.
    xcodebuild test \
      -project "$PROJECT" \
      -scheme "$SCHEME" \
      -testPlan FireflyUITests \
      -destination "platform=iOS Simulator,id=$UDID" \
      -only-testing:FireflyUITests/StoreTourUITests \
      -derivedDataPath "$DERIVED_DATA"
  ) || TEST_STATUS=$?
  # Never leave the marker behind — an interrupted run must not leave
  # this suite silently armed for a later, unrelated `xcodebuild test`
  # (CI's included) on the same machine.
  rm -f "$TOUR_MARKER"

  sleep 1 # let the last frame land before stopping the recording
  kill -INT "$RECORD_PID" 2>/dev/null || true
  wait "$RECORD_PID" 2>/dev/null || true

  if [ "$TEST_STATUS" -ne 0 ]; then
    echo "error: StoreTourUITests failed (status $TEST_STATUS) — $RECORDING may be incomplete or missing" >&2
    exit "$TEST_STATUS"
  fi
  [ -s "$RECORDING" ] || { echo "error: $RECORDING was not produced" >&2; exit 1; }
  log "recorded $RECORDING ($(du -h "$RECORDING" | cut -f1))"
else
  log "SKIP_VIDEO=1 — skipping the tour recording"
fi

# Simulator (and, if the video ran, TOUR_MARKER) cleanup happens in
# cleanup_on_exit via the EXIT trap registered above — not here — so it
# also runs on a failed/interrupted run, not just this happy path.
log "done. Screenshots: $MARKETING_DIR/screenshots/  Recording: $MARKETING_DIR/recordings/tour.mp4"
