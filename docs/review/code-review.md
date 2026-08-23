# Independent code review brief

You are reviewing a PR you did not write. You have fresh eyes — use them. Read the linked spec FIRST, then the diff (`gh pr diff N`), then the tests.

## What to verify, in order

1. **Spec conformance:** every acceptance criterion has a correspondingly named test, and the test actually asserts the criterion (not a vacuous pass). Behavior decisions in the spec (thresholds, formats, edge policies) are implemented exactly — not approximately.
2. **Honest-data rule:** no code path invents freshness, positions, times, or hides unknowns (project value, see CLAUDE.md).
3. **Boundary bugs:** off-by-ones at spec thresholds, wraparound (angles, midnight, ring buffers), integer overflow on ms timestamps, unchecked lengths on anything parsed from the radio (this device eats untrusted RF bytes).
4. **Layering:** core stays pure (no I/O/LVGL includes); libraries don't reach into app; new public API is header-documented.
5. **Allocation & size:** no hidden malloc in core/libs; fixed buffers respect their caps; struct growth flagged.
6. **Tests are real:** run them locally. Try one deliberate mutation (revert a line mentally) — would a test catch it? If not, say which criterion is under-tested.

## Output

PR comment via `gh pr comment N --body ...`, structured:
- Verdict line: `REVIEW: APPROVE` or `REVIEW: CHANGES`
- Findings list: file:line, what, why it matters, suggested fix. Severity-ordered. No style nitpicks unless they mask bugs.
- One line on what you verified by running (build/tests/goldens).

Approve when remaining findings are genuinely minor (typos, naming). Anything touching a spec criterion, a boundary, or parsed input is CHANGES.
