# Agent workflow

How implementation agents (and humans) ship work in this repo.

## The loop

1. **Claim a spec** from `docs/specs/` (or a slice of one — specs list their slices).
2. Branch: `feat/sNN-short-name` (e.g. `feat/s01-geo`).
3. Implement **to the acceptance criteria**, with tests written alongside (test names mirror the criteria numbering: `S01_AC3_...`).
4. Local gate: `cmake -S firmware -B build -DFF_TARGET=sim && cmake --build build -j8 && ctest --test-dir build --output-on-failure` — all green, zero warnings.
5. UI work: run the sim's screenshot dump (`build/ffsim --screenshot out/`) and commit the PNGs (goldens under `firmware/tests/golden/`, extra context shots under `docs/screens/`). In the PR body use the dual pattern — `![name](https://raw.githubusercontent.com/jakeholland/firefly/<commit-sha>/<path>.png)` embeds (these render once the repo is public) immediately followed by matching `[name](https://github.com/jakeholland/firefly/blob/<commit-sha>/<path>.png)` blob links (these work for authorized viewers while the repo is private). Pin the commit SHA, never a branch name.
6. PR via `gh pr create`: body = spec link, criteria checklist (checked), screenshots if UI, notes on any interpretation calls.
7. **Independent review — tiered by risk** (policy set 2026-08-24 to control orchestration cost; the full-adversarial process was correct for bootstrapping and caught real defects nearly every round, but not every PR warrants it).
   - **Tier 1 — no agent review**: docs, tools, gitignore/CI config, comment-only changes. CI green + the orchestrator's own read suffices. The orchestrator merges directly.
   - **Tier 2 — single-pass review**: screens, sim target, tests, small `[api]` additions that follow an established convention. One reviewer, one pass. Findings come back; the author fixes; **the orchestrator verifies the fixes directly** (run the named test, re-run the specific mutation) instead of resuming the reviewer. Reviewers must state in their verdict exactly what would flip CHANGES→APPROVE, so confirmation is mechanical.
   - **Tier 3 — full adversarial**: `core/` logic, protocol/wire changes, trust/honesty rules, spec contracts, anything S15/device-facing. Author never reviews own work; reviewer runs the gate independently; mutation checks on the riskiest properties (**3–5 targeted mutations, not exhaustive sweeps** — pick the mutations that would ship a confidently-wrong screen, per `docs/review/code-review.md` item 6).
   - UI PRs at any tier additionally get the `docs/review/ux-raver.md` persona pass when they change what Bailey sees, not for pixel-identical refactors.
8. Merge when CI is green AND review approved: `gh pr merge --squash --auto`. **Branch protection is active on `main`** (as of 2026-08-23): the `build-test` check is required and strict (branches must be up to date with main before merging), force-pushes and deletions are blocked. Auto-merge now genuinely waits for CI. The review verdict remains process-enforced — the orchestrator does not merge unapproved work.

8. Small PRs. One spec slice per PR. A PR that touches core AND ui AND meshclient is three PRs.

## Rules

- Never push to `main`. Never force-push shared branches.
- Don't edit another in-flight branch's files; check open PRs (`gh pr list`) before claiming.
- New public API = header + doc comment + unit test, or it doesn't exist.
- Vendored deps are pinned in `firmware/third_party/` via CMake FetchContent with commit hashes — never floating tags.
- Commits end with: `Co-Authored-By: <your model name> <noreply@anthropic.com>`
- If blocked by a spec gap: open a PR that adds a `## Questions` section to the spec file instead of guessing, and pick up a different slice.

## Standing brief (read this so per-task briefs can be short)

Everything below was learned the expensive way in this repo's own history. A task brief that says "standing brief applies" means all of it.

- **The proxy check** (`docs/review/code-review.md` item 6) is the house failure mode — five confirmed instances. For any test you write, ask: what input satisfies the proxy and violates the property? Resolve by **measuring, not reasoning harder**.
- **Compilers**: local gate is clang; CI's GCC is the authority; `gcc-14` (never plain `gcc`) for a second local opinion. Say which compiler you verified under.
- **Mutation checks need fresh builds**: stale incremental binaries have produced false results twice. Verify the binary actually changed (hash the object file, not the executable — `ar` embeds timestamps). Revert mutations with targeted edits, not `git checkout` (which has twice reverted real work).
- **proto3 drops zero values**; enums arrive as strings through the Python lib; `rx_time`/`precision_bits` exist only on live packets, never the NodeInfo replay. A replayed timestamp is a *summary*, not an observation — never age or latch from a value that defines the clock it's measured against.
- **Positions**: `LOC_MANUAL` means "not measured," not "placed deliberately or recently." Channel `position_precision` can silently truncate to km-scale (#47). RSSI on a relayed packet measures the relay.
- **Worktree discipline**: work only in your assigned worktree; before pushing, `git ls-files | grep -E 'build|\.venv'` must be empty.
- **Honesty rules bind debug surfaces too** — a ctl dump or log line that mislabels provenance is a real finding, not a nit.

## Definition of done

Spec criteria all pass as named tests · CI green · screenshots attached (UI) · no TODOs without an issue number · README of the touched module updated if behavior changed.
