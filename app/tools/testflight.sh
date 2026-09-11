#!/usr/bin/env bash
# testflight.sh — archive, export and upload a Release build of Firefly
# to TestFlight.
#
#   app/tools/testflight.sh                 archive, export, upload
#   app/tools/testflight.sh --archive-only   archive only (dry run)
#
# What the default (full) run does, in order:
#
#   1. Signing. Ensures app/Config/Local.xcconfig (git-ignored,
#      per-developer — app/README.md, "Signed local runs") carries
#      DEVELOPMENT_TEAM = SU4T96VBX6 and CODE_SIGNING_ALLOWED/REQUIRED
#      = YES. Creates the file from Config/Local.xcconfig.example if it
#      does not exist yet — never touches anything else a developer
#      already put there. This is the ONLY thing that turns signing on;
#      nothing below passes a signing override on the `xcodebuild`
#      command line, and a checkout with no Local.xcconfig (every CI
#      run) is completely unaffected, so CI's existing unsigned Debug
#      builds stay exactly as they are.
#
#      CODE_SIGN_STYLE stays Automatic throughout, and this script
#      never hard-codes a signing IDENTITY (Development vs
#      Distribution) anywhere — doing that for Release specifically
#      was tried and it broke Automatic signing outright ("conflicting
#      provisioning settings"). Instead, the ARCHIVE step below signs
#      with whatever identity is already available (typically
#      Development, from an ordinary `Xcode -> Settings -> Accounts`
#      login) and the EXPORT step is what actually asks Apple for a
#      Distribution certificate and an App Store provisioning profile
#      and RE-SIGNS with those — standard Xcode behavior for
#      `signingStyle: automatic` + `method: app-store-connect`
#      (ExportOptions.plist), not something this script has to force.
#
#   2. Version. CURRENT_PROJECT_VERSION (project.yml) is wired to
#      ${FIREFLY_BUILD_NUMBER} — see that file's own comment. This
#      script sets it to `git rev-list --count HEAD` and regenerates
#      Firefly.xcodeproj with `xcodegen generate` so every archive
#      TestFlight sees carries a build number it has never seen
#      before (App Store Connect rejects a repeat). MARKETING_VERSION
#      (the human-facing 0.1.0) is bumped by hand in project.yml when
#      it actually changes.
#
#      That regenerate touches a file this repo COMMITS
#      (Firefly.xcodeproj), so on exit this script restores it from
#      git — the archive it already produced keeps the real build
#      number regardless; there is nothing to gain by leaving that
#      regenerated project sitting as an uncommitted diff. If
#      Firefly.xcodeproj already had uncommitted changes before this
#      script ran, it leaves them alone and says so, rather than
#      discarding something a developer was in the middle of editing.
#
#   3. Archive. `xcodebuild archive`, Release, `generic/platform=iOS`,
#      `-allowProvisioningUpdates` (Xcode fetches/creates the
#      Distribution certificate and App Store provisioning profile
#      for team SU4T96VBX6 on its own — no manual profile wrangling).
#
#   4. Export + upload. `xcodebuild -exportArchive` with
#      app/ExportOptions.plist (method: app-store-connect). With
#      ASC_KEY_ID, ASC_ISSUER_ID and ASC_KEY_PATH all set (and the .p8
#      at ASC_KEY_PATH actually present), this exports AND uploads to
#      App Store Connect in one step (`destination: upload`).
#      Without them, it exports a LOCAL .ipa only (a temporary copy of
#      ExportOptions.plist with `destination` patched to `export` —
#      the checked-in file always says `upload`) and then this script
#      FAILS with the exact one-time App Store Connect setup the
#      owner needs to do before a real upload can happen — see
#      `print_asc_checklist` below. `--archive-only` stops before this
#      step entirely (see USAGE above) and always exits 0: that is the
#      form to use for a plain "does the archive still succeed" check,
#      e.g. in CI or before credentials exist at all.
#
# Environment:
#   ASC_KEY_ID      App Store Connect API key ID
#   ASC_ISSUER_ID   App Store Connect API issuer ID
#   ASC_KEY_PATH    Path to the .p8 private key, e.g.
#                   ~/.private_keys/AuthKey_<ASC_KEY_ID>.p8
#   FIREFLY_ARCHIVE_DIR   Optional. Where to put the .xcarchive and the
#                         exported .ipa (default: a fresh directory
#                         under $TMPDIR). Not cleaned up on exit, so
#                         the paths this script prints stay valid
#                         afterward.
#
# None of the three ASC_* values above are ever printed, logged, or
# written anywhere but into the `xcodebuild` invocation's own argv —
# this script only ever checks whether they are SET and whether
# ASC_KEY_PATH names a file that exists. See docs/app/testflight.md
# for the one-time App Store Connect setup and the full walkthrough.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$APP_DIR/.." && pwd)"

TEAM_ID="SU4T96VBX6"
BUNDLE_ID="com.jakeholland.Firefly"
SCHEME="Firefly"
PROJECT="$APP_DIR/Firefly.xcodeproj"
PROJECT_REL="app/Firefly.xcodeproj"
EXPORT_OPTIONS="$APP_DIR/ExportOptions.plist"
LOCAL_XCCONFIG="$APP_DIR/Config/Local.xcconfig"
LOCAL_XCCONFIG_EXAMPLE="$APP_DIR/Config/Local.xcconfig.example"

ARCHIVE_ONLY=0
case "${1:-}" in
  --archive-only) ARCHIVE_ONLY=1 ;;
  -h|--help)
    sed -n '2,60p' "$0"
    exit 0
    ;;
  "") ;;
  *)
    echo "error: unknown argument '$1' (see --help)" >&2
    exit 2
    ;;
esac

log()  { printf '==> %s\n' "$1"; }
warn() { printf 'warning: %s\n' "$1" >&2; }
fail() { printf 'error: %s\n' "$1" >&2; exit 1; }

print_asc_checklist() {
  cat >&2 <<MSG

The archive above (see path and version/build already printed)
SUCCEEDED. Getting from there to a TestFlight upload needs this, ONCE,
in App Store Connect (appstoreconnect.apple.com), team $TEAM_ID —
whether the step above failed outright for lack of a signed-in App
Store Connect API key, or it exported a local .ipa but had nothing to
upload it with:

  1. Create the app record.
       Apps -> "+" -> New App -> iOS
         Bundle ID: $BUNDLE_ID
       (Register that bundle ID first, under Certificates, Identifiers
       & Profiles -> Identifiers, if it is not offered in the picker.)

  2. Create an App Store Connect API key with the App Manager role.
       Users and Access -> Integrations -> App Store Connect API -> "+"
         Role: App Manager
       Apple shows the .p8 file ONCE — download it immediately to
         ~/.private_keys/AuthKey_<KEY_ID>.p8
       and note the Key ID and Issuer ID shown on that same page.

Then, in the environment (never in a file this repo tracks):
  export ASC_KEY_ID=<key id>
  export ASC_ISSUER_ID=<issuer id>
  export ASC_KEY_PATH=~/.private_keys/AuthKey_<key id>.p8

and re-run app/tools/testflight.sh. See docs/app/testflight.md for the
full walkthrough, including adding Taylor as a tester and the export
compliance answer (already wired: ITSAppUsesNonExemptEncryption = NO).
MSG
}

# ---------------------------------------------------------------------
# 1. Signing — app/Config/Local.xcconfig
#
# Only DEVELOPMENT_TEAM and CODE_SIGNING_ALLOWED/REQUIRED live here —
# see this script's own header comment for why no signing IDENTITY is
# ever forced, here or anywhere else.
# ---------------------------------------------------------------------
ensure_local_xcconfig() {
  if [ ! -f "$LOCAL_XCCONFIG" ]; then
    log "app/Config/Local.xcconfig not found — creating it for team $TEAM_ID (git-ignored, local only)"
    cat > "$LOCAL_XCCONFIG" <<EOF
// Local, git-ignored — created by app/tools/testflight.sh for team
// $TEAM_ID. See app/Config/Local.xcconfig.example and app/README.md,
// "Signed local runs" / "Shipping a TestFlight build".
DEVELOPMENT_TEAM = $TEAM_ID
CODE_SIGN_STYLE = Automatic
CODE_SIGN_IDENTITY = Apple Development
CODE_SIGNING_ALLOWED = YES
CODE_SIGNING_REQUIRED = YES
EOF
  elif ! grep -q 'DEVELOPMENT_TEAM' "$LOCAL_XCCONFIG"; then
    fail "app/Config/Local.xcconfig exists but has no DEVELOPMENT_TEAM — add \`DEVELOPMENT_TEAM = $TEAM_ID\` (see app/Config/Local.xcconfig.example)"
  fi
}

# ---------------------------------------------------------------------
# 2. Version — CURRENT_PROJECT_VERSION from the git commit count
# ---------------------------------------------------------------------
PROJECT_WAS_CLEAN=0
restore_project() {
  if [ "$PROJECT_WAS_CLEAN" = "1" ]; then
    git -C "$REPO_ROOT" checkout -- "$PROJECT_REL" 2>/dev/null || true
  fi
}

regenerate_project() {
  command -v xcodegen >/dev/null 2>&1 || \
    fail "xcodegen not found — brew install xcodegen (see app/README.md, 'Regenerating things')"

  if git -C "$REPO_ROOT" diff --quiet -- "$PROJECT_REL" 2>/dev/null && \
     git -C "$REPO_ROOT" diff --cached --quiet -- "$PROJECT_REL" 2>/dev/null; then
    PROJECT_WAS_CLEAN=1
    trap restore_project EXIT
  else
    warn "$PROJECT_REL already has uncommitted changes — leaving them as-is after this run instead of reverting them"
  fi

  BUILD_NUMBER="$(git -C "$REPO_ROOT" rev-list --count HEAD)"
  log "CURRENT_PROJECT_VERSION = $BUILD_NUMBER (git rev-list --count HEAD)"
  ( cd "$APP_DIR" && FIREFLY_BUILD_NUMBER="$BUILD_NUMBER" xcodegen generate )
}

# ---------------------------------------------------------------------
# 3. Archive
# ---------------------------------------------------------------------
ARCHIVE_ROOT="${FIREFLY_ARCHIVE_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/firefly-testflight.XXXXXX")}"
mkdir -p "$ARCHIVE_ROOT"
ARCHIVE_PATH="$ARCHIVE_ROOT/Firefly.xcarchive"
EXPORT_PATH="$ARCHIVE_ROOT/export"

do_archive() {
  log "archiving (Release, generic/platform=iOS) to $ARCHIVE_PATH"
  xcodebuild archive \
    -project "$PROJECT" \
    -scheme "$SCHEME" \
    -configuration Release \
    -destination 'generic/platform=iOS' \
    -archivePath "$ARCHIVE_PATH" \
    -allowProvisioningUpdates

  APP_INFO_PLIST="$ARCHIVE_PATH/Products/Applications/Firefly.app/Info.plist"
  [ -f "$APP_INFO_PLIST" ] || fail "archive succeeded but $APP_INFO_PLIST is missing — unexpected archive layout"
  ARCHIVED_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP_INFO_PLIST")"
  ARCHIVED_BUILD="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$APP_INFO_PLIST")"
  log "archive OK: $ARCHIVE_PATH (version $ARCHIVED_VERSION, build $ARCHIVED_BUILD)"
}

# ---------------------------------------------------------------------
# 4. Export + upload
# ---------------------------------------------------------------------
do_export() {
  local have_creds=0
  if [ -n "${ASC_KEY_ID:-}" ] && [ -n "${ASC_ISSUER_ID:-}" ] && [ -n "${ASC_KEY_PATH:-}" ] && [ -f "${ASC_KEY_PATH}" ]; then
    have_creds=1
  fi

  local export_options="$EXPORT_OPTIONS"
  if [ "$have_creds" = "0" ]; then
    export_options="$ARCHIVE_ROOT/ExportOptions.export-only.plist"
    cp "$EXPORT_OPTIONS" "$export_options"
    /usr/libexec/PlistBuddy -c 'Set :destination export' "$export_options"
    log "no ASC credentials — exporting a local .ipa only (destination: export)"
  else
    log "ASC credentials present — exporting and uploading to App Store Connect (destination: upload)"
  fi

  local export_args=(
    -exportArchive
    -archivePath "$ARCHIVE_PATH"
    -exportPath "$EXPORT_PATH"
    -exportOptionsPlist "$export_options"
    -allowProvisioningUpdates
  )
  if [ "$have_creds" = "1" ]; then
    export_args+=(
      -authenticationKeyPath "$ASC_KEY_PATH"
      -authenticationKeyID "$ASC_KEY_ID"
      -authenticationKeyIssuerID "$ASC_ISSUER_ID"
    )
  fi

  # NOT `local status=$?` inside the `then`/`else` of the `if` itself —
  # `$?` there reflects the `if` test's own (negated) result, not
  # xcodebuild's, and would always read back 0. Splitting the call from
  # the branch is what captures xcodebuild's real exit status.
  local status=0
  xcodebuild "${export_args[@]}" || status=$?

  if [ "$status" -ne 0 ]; then
    printf 'error: export/upload failed (xcodebuild exit %s)\n' "$status" >&2
    print_asc_checklist
    exit "$status"
  fi

  if [ "$have_creds" = "1" ]; then
    log "uploaded build $ARCHIVED_BUILD (version $ARCHIVED_VERSION) to App Store Connect — it will appear in TestFlight once Apple finishes processing"
  else
    log "exported to $EXPORT_PATH — no upload happened"
    print_asc_checklist
    exit 1
  fi
}

# ---------------------------------------------------------------------
ensure_local_xcconfig
regenerate_project
do_archive

if [ "$ARCHIVE_ONLY" = "1" ]; then
  log "--archive-only: stopping before export/upload"
  exit 0
fi

do_export
