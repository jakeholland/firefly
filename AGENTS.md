# Agent workflow

How implementation agents (and humans) ship work in this repo.

## The loop

1. **Claim a spec** from `docs/specs/` (or a slice of one — specs list their slices).
2. Branch: `feat/sNN-short-name` (e.g. `feat/s01-geo`).
3. Implement **to the acceptance criteria**, with tests written alongside (test names mirror the criteria numbering: `S01_AC3_...`).
4. Local gate: `cmake -S firmware -B build -DFF_TARGET=sim && cmake --build build -j8 && ctest --test-dir build --output-on-failure` — all green, zero warnings.
5. UI work: run the sim's screenshot dump (`build/ffsim --screenshot out/`) and commit the PNGs (goldens under `firmware/tests/golden/`, extra context shots under `docs/screens/`). In the PR body use the dual pattern — `![name](https://raw.githubusercontent.com/jakeholland/firefly/<commit-sha>/<path>.png)` embeds (these render once the repo is public) immediately followed by matching `[name](https://github.com/jakeholland/firefly/blob/<commit-sha>/<path>.png)` blob links (these work for authorized viewers while the repo is private). Pin the commit SHA, never a branch name.
6. PR via `gh pr create`: body = spec link, criteria checklist (checked), screenshots if UI, notes on any interpretation calls.
7. **Independent review — required before merge.** The author never reviews their own PR. The orchestrator spawns a separate reviewer agent (fresh context, different instance than the author) with the brief in `docs/review/code-review.md`; UI PRs additionally get the persona review in `docs/review/ux-raver.md` run against the PR's screenshots. Reviewers post findings as PR comments (`gh pr comment`) with verdict `APPROVE` or `CHANGES`; the author addresses `CHANGES` findings and re-requests. A PR is mergeable only with an APPROVE comment from a non-author reviewer.
8. Merge when CI is green AND review approved: `gh pr merge --squash --auto`. **Branch protection is active on `main`** (as of 2026-08-23): the `build-test` check is required and strict (branches must be up to date with main before merging), force-pushes and deletions are blocked. Auto-merge now genuinely waits for CI. The review verdict remains process-enforced — the orchestrator does not merge unapproved work.

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
