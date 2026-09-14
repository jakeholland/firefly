#!/usr/bin/env bash
# check_project.sh — guard against a committed Firefly.xcodeproj/project.pbxproj
# carrying an unresolved xcodegen template placeholder.
#
# Concretely: CURRENT_PROJECT_VERSION used to be templated in project.yml
# as `${FIREFLY_BUILD_NUMBER}`, substituted by xcodegen at `xcodegen
# generate` time from an environment variable. Any regeneration that
# forgot to set that variable — the Xcode GUI's own "Generate", a
# clean `cd app && xcodegen generate`, an agent following stale
# instructions — did not fail; it silently baked the literal string
# `${FIREFLY_BUILD_NUMBER}` into the committed pbxproj as
# CFBundleVersion. Any build that then didn't override
# CURRENT_PROJECT_VERSION on the command line shipped that placeholder
# as its build number. project.yml no longer templates this setting at
# all (see its own comment on CURRENT_PROJECT_VERSION and
# Config/Firefly.xcconfig's plain `= 1` default), so `xcodegen
# generate` is now plain and idempotent — but this script is the
# backstop against the same class of mistake creeping back in, here or
# anywhere else in the committed project file.
#
# Usage:
#   app/tools/check_project.sh
#
# Exits non-zero (with the offending line[s] printed) if
# Firefly.xcodeproj/project.pbxproj contains an unresolved `${...}`
# template placeholder anywhere. Wired into .github/workflows/app.yml.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PBXPROJ="$APP_DIR/Firefly.xcodeproj/project.pbxproj"

[ -f "$PBXPROJ" ] || {
  echo "error: $PBXPROJ not found" >&2
  exit 1
}

# `${` is never legitimate inside project.pbxproj: xcodegen resolves
# every `$(VAR)`/`${VAR}` template substitution it recognizes at
# generate time, and a real, resolved build setting is a plain string
# or number. Any literal `${` left in the file is exactly the failure
# mode above, regardless of which build setting it lands on.
if grep -n '\${' "$PBXPROJ" >/dev/null 2>&1; then
  echo "error: $PBXPROJ has an unresolved template placeholder:" >&2
  grep -n '\${' "$PBXPROJ" >&2
  echo "" >&2
  echo "This is what happens when the checked-in project is regenerated with an environment variable unset that project.yml once templated a setting from. Fix: cd app && xcodegen generate (no environment variable needed — see project.yml's own comment) and commit the result." >&2
  exit 1
fi

echo "ok: no unresolved template placeholder in $PBXPROJ"
