#!/usr/bin/env bash
# run_goldens.sh — S14 slice b golden-screenshot test runner.
#
# For every tests/fixtures/*.json:
#   1. Renders it headlessly TWICE into separate scratch dirs and requires
#      the two PNGs to be byte-identical (S13 AC2 / the task's determinism
#      proof: "rendering the same fixture twice must produce byte-identical
#      PNGs").
#   2. Compares one of those renders against tests/golden/<name>.png via
#      build/compare_png (<=0.5% differing pixels, docs/specs/S14-testing-ci.md
#      slice b). On failure, writes a side-by-side diff PNG under
#      tests/golden/_diff_out/ for CI artifact upload.
#
# Usage:
#   tests/run_goldens.sh                 # compare against committed goldens
#   tests/run_goldens.sh --update-golden  # regenerate tests/golden/*.png instead
#
# Must be run after a build (needs build/ffsim and build/compare_png).
# Assumes CWD-independence: paths are resolved relative to this script's
# own location, matching firmware/'s other scripts.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURES_DIR="$ROOT/tests/fixtures"
GOLDEN_DIR="$ROOT/tests/golden"
DIFF_OUT_DIR="$GOLDEN_DIR/_diff_out"
BUILD_DIR="$ROOT/build"
FFSIM="$BUILD_DIR/ffsim"
COMPARE_PNG="$BUILD_DIR/compare_png"
THRESHOLD_PCT="0.5"

UPDATE_GOLDEN=false
for arg in "$@"; do
    case "$arg" in
        --update-golden) UPDATE_GOLDEN=true ;;
        *)
            echo "run_goldens.sh: unrecognized argument '$arg'" >&2
            echo "usage: $0 [--update-golden]" >&2
            exit 2
            ;;
    esac
done

if [[ ! -x "$FFSIM" ]]; then
    echo "run_goldens.sh: $FFSIM not found or not executable — build first" \
        "(cmake -S . -B build -DFF_TARGET=sim && cmake --build build -j8)" >&2
    exit 2
fi
if [[ ! -x "$COMPARE_PNG" ]]; then
    echo "run_goldens.sh: $COMPARE_PNG not found or not executable — build first" >&2
    exit 2
fi

shopt -s nullglob
fixtures=("$FIXTURES_DIR"/*.json)
shopt -u nullglob
if [[ ${#fixtures[@]} -eq 0 ]]; then
    echo "run_goldens.sh: no fixtures found under $FIXTURES_DIR" >&2
    exit 2
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

mkdir -p "$GOLDEN_DIR"
rm -rf "$DIFF_OUT_DIR"

fail=0
checked=0

for fixture_path in "${fixtures[@]}"; do
    stem="$(basename "$fixture_path" .json)"
    run_a_dir="$WORKDIR/run_a"
    run_b_dir="$WORKDIR/run_b"
    mkdir -p "$run_a_dir" "$run_b_dir"

    # --- Determinism proof: two independent renders must be byte-identical. ---
    "$FFSIM" --headless --screenshot "$run_a_dir" --fixture "$fixture_path" --mock-clock >/dev/null
    "$FFSIM" --headless --screenshot "$run_b_dir" --fixture "$fixture_path" --mock-clock >/dev/null

    rendered_a="$run_a_dir/$stem.png"
    rendered_b="$run_b_dir/$stem.png"
    if [[ ! -f "$rendered_a" ]] || [[ ! -f "$rendered_b" ]]; then
        echo "FAIL  $stem: ffsim did not produce the expected PNG" >&2
        fail=1
        continue
    fi
    if ! cmp -s "$rendered_a" "$rendered_b"; then
        echo "FAIL  $stem: two headless renders of the same fixture were NOT byte-identical" \
            "(determinism violated)" >&2
        fail=1
        continue
    fi

    # --- Compare against (or regenerate) the committed golden. ---
    golden_path="$GOLDEN_DIR/$stem.png"

    if $UPDATE_GOLDEN; then
        cp "$rendered_a" "$golden_path"
        echo "UPDATE $stem: wrote $golden_path"
        checked=$((checked + 1))
        continue
    fi

    if [[ ! -f "$golden_path" ]]; then
        echo "FAIL  $stem: no golden at $golden_path (run with --update-golden to create it)" >&2
        fail=1
        continue
    fi

    mkdir -p "$DIFF_OUT_DIR"
    diff_out_path="$DIFF_OUT_DIR/$stem.diff.png"
    if "$COMPARE_PNG" "$golden_path" "$rendered_a" --threshold "$THRESHOLD_PCT" --diff-out "$diff_out_path"; then
        echo "PASS  $stem"
        rm -f "$diff_out_path" # only keep diff artifacts for failures
    else
        echo "FAIL  $stem: differs from golden by more than ${THRESHOLD_PCT}% — see $diff_out_path" >&2
        fail=1
    fi
    checked=$((checked + 1))
done

echo
if $UPDATE_GOLDEN; then
    echo "run_goldens.sh: updated $checked golden(s)."
    exit 0
fi

if [[ $fail -ne 0 ]]; then
    echo "run_goldens.sh: FAILED ($checked fixture(s) checked)." >&2
    exit 1
fi

echo "run_goldens.sh: all $checked fixture(s) passed (determinism + <= ${THRESHOLD_PCT}% golden diff)."
