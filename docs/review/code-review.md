# Independent code review brief

You are reviewing a PR you did not write. You have fresh eyes — use them. Read the linked spec FIRST, then the diff (`gh pr diff N`), then the tests.

## What to verify, in order

1. **Spec conformance:** every acceptance criterion has a correspondingly named test, and the test actually asserts the criterion (not a vacuous pass). Behavior decisions in the spec (thresholds, formats, edge policies) are implemented exactly — not approximately.
2. **Honest-data rule:** no code path invents freshness, positions, times, or hides unknowns (project value, see CLAUDE.md).
3. **Boundary bugs:** off-by-ones at spec thresholds, wraparound (angles, midnight, ring buffers), integer overflow on ms timestamps, unchecked lengths on anything parsed from the radio (this device eats untrusted RF bytes).
4. **Layering:** core stays pure (no I/O/LVGL includes); libraries don't reach into app; new public API is header-documented.
5. **Allocation & size:** no hidden malloc in core/libs; fixed buffers respect their caps; struct growth flagged.
6. **Tests are real:** run them locally. Try one deliberate mutation (revert a line mentally) — would a test catch it? If not, say which criterion is under-tested.

   **And check the test's proxy.** A test rarely measures the property that matters; it measures a stand-in. The recurring bug in this repo is an *exception in that correspondence* — the proxy holds, the property doesn't, and the test passes for the wrong reason. Ask the finite question: **what input satisfies the proxy and violates the property?** If you can name one, the test is wrong even when it's green.

   Four real instances, all from one PR (#41):

   | proxy | property | the exception |
   |---|---|---|
   | a byte cap on a name | the chip fits the round glass | the font is proportional — 11 `W`s are 80px wider than 11 `I`s |
   | one name truncates correctly | two names stay distinguishable | truncation isn't injective — `ALEXANDRIA` and `ALEXANDRINA` both became `ALEXANDRI` |
   | the round-glass sweep passes | the chip fits its slot | a two-line chip is narrow *and* taller — it fit, wrapped, and dropped the ellipsis |
   | `CC=gcc` built cleanly | a second compiler checked it | `gcc` on macOS **is** Apple clang (#44) |

   The caveat that keeps this from becoming ritual: in every one of those, what resolved it was **measuring, not reasoning harder**. Reviewers had read the code carefully and still missed them. Instrument it, render it, print the number — a sweep you ran beats a sweep you reasoned about.

## Output

PR comment via `gh pr comment N --body ...`, structured:
- Verdict line: `REVIEW: APPROVE` or `REVIEW: CHANGES`
- Findings list: file:line, what, why it matters, suggested fix. Severity-ordered. No style nitpicks unless they mask bugs.
- One line on what you verified by running (build/tests/goldens).

Approve when remaining findings are genuinely minor (typos, naming). Anything touching a spec criterion, a boundary, or parsed input is CHANGES.
