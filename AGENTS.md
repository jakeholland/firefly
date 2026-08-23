# Agent workflow

How implementation agents (and humans) ship work in this repo.

## The loop

1. **Claim a spec** from `docs/specs/` (or a slice of one — specs list their slices).
2. Branch: `feat/sNN-short-name` (e.g. `feat/s01-geo`).
3. Implement **to the acceptance criteria**, with tests written alongside (test names mirror the criteria numbering: `S01_AC3_...`).
4. Local gate: `cmake -B build -DFF_TARGET=sim && cmake --build build && ctest --test-dir build` — all green, zero warnings.
5. UI work: run the sim's screenshot dump (`build/ffsim --screenshot out/`) and commit the PNGs under `docs/screens/` so the PR renders them.
6. PR via `gh pr create`: body = spec link, criteria checklist (checked), screenshots if UI, notes on any interpretation calls.
7. Merge when CI is green: `gh pr merge --squash --auto`. CI is the referee; no human gate needed for spec-conformant work.
8. Small PRs. One spec slice per PR. A PR that touches core AND ui AND meshclient is three PRs.

## Rules

- Never push to `main`. Never force-push shared branches.
- Don't edit another in-flight branch's files; check open PRs (`gh pr list`) before claiming.
- New public API = header + doc comment + unit test, or it doesn't exist.
- Vendored deps are pinned in `firmware/third_party/` via CMake FetchContent with commit hashes — never floating tags.
- Commits end with: `Co-Authored-By: <your model name> <noreply@anthropic.com>`
- If blocked by a spec gap: open a PR that adds a `## Questions` section to the spec file instead of guessing, and pick up a different slice.

## Definition of done

Spec criteria all pass as named tests · CI green · screenshots attached (UI) · no TODOs without an issue number · README of the touched module updated if behavior changed.
