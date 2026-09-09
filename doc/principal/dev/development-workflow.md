# Development workflow — Issue · branch/worktree · PR

> In effect from 2026-09-10 (after the framework 0.11.0 release, for the 1.0 preparation). User decision.
> This page owns "where a piece of work is registered, on which branch it is done, and how it lands
> on main". Commit messages, versions and release procedure stay with
> [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) §9; agent operating rules with [`AGENTS.md`](../../../AGENTS.md).

## 1. At a glance

```
Register an Issue ──> branch + worktree ──> work (person or codex job) ──> PR ──> CI + supervisor review ──> merge ──> clean up
        │                                                                     │
        └── grouped by Milestone (release) and Project (board)                └── "Closes #N" closes the Issue
```

`main` changes **only through PRs** (exceptions in §6). One branch = one Issue = one worktree = (if any) one codex job.

## 2. Registering work — GitHub Issues

- Every piece of work is an Issue before it starts. The title is a one-line outcome ("what must be
  true afterwards"); the body has three parts: **scope / done criteria / evidence** (FB-nnn·D-nnn
  decision records, measurements, report paths).
- Two label axes only:
  - `area:` `core` · `bindings` · `framework-dotnet` · `framework-java` · `framework-node` · `framework-cpp` · `bench` · `ci` · `docs`
  - `kind:` `bug` · `perf` · `feature` · `chore`
- One Issue is one cause and one outcome. The same fix in several languages becomes one Issue per
  language, cross-linked (the same rule as `AGENTS.md`'s "one job = one cause").
- The existing record system stays. FB/D numbers in `doc/plan/**/decisions*.md` are the evidence;
  an Issue is that evidence turned into a to-do. The Issue links the record, the record names the Issue.

## 3. Grouping — Milestones and Projects

- **Milestone** = release. Create one per release (`1.0` for the Core 1.0 · bindings 1.0.0 ·
  framework 1.0.0 batch) and attach the Issues and PRs that must ship in it. GitHub counts open
  versus closed. The pre-tag checklist is "zero open Issues in the milestone".
- **Project** = board. One org-level project (`zlink-systems` / `ZLink`). Columns
  `Todo → In progress → Review → Done`. Custom fields `area` (same values as the label) and
  `runner` (`astra` / `sol` / `direct`) show which job is doing what right now. Workflows move an
  opened Issue to Todo, an opened PR to Review and a merged PR to Done.
- Labels stop at the two axes in §2; state and assignee live only in Project fields (no duplicate bookkeeping).

## 4. Branches and worktrees

- Branch name: `<area>/<issue-number>-<slug>` — e.g. `framework-java/57-mesh-pump-blocking-wait`,
  `bench/61-dealer-router-rows`.
- Worktree: `git worktree add ~/project/zlink-<slug> -b <branch> origin/main`. One directory per
  branch; never open the same branch in two worktrees.
- Local packages (`.artifacts/wsl`) are per worktree; only the Core release prefix
  (`~/.cache/zlink/core/<ver>`) is shared (user decision 2026-09-09).
- A codex job gets the worktree path via `-C` and commits **only on that branch**; the supervisor
  pushes. Jobs still never touch `doc/**`, specs or `.github/**` (report as BLOCKERS).
- Catch up with main using `git fetch && git merge origin/main` (no rebase on shared branches).

## 5. PRs and merging

- `gh pr create --base main --head <branch>`; the first body line is `Closes #N`. The title follows
  the commit rule `<module>: <one line>`.
- The PR body carries (1) what changed, (2) verification — the gates, tests and measurement tickets
  run, with numbers, (3) what remains / known reds. Link the codex report when there is one
  (`.artifacts/codex/<job>/summary.md`).
- CI: pull_request workflows run under production-code path filters (framework .NET/Node exist;
  Core, bindings and framework Java/C++ are tracked in an Issue). Areas without CI substitute the
  local verification record in the PR body.
- Merge conditions: (a) the supervisor (or the user) read the diff, (b) the area's gate and tests
  passed (PR CI or the local record), (c) changes that need measurement carry the ticket-queue 3-run
  result in the PR. Jobs known to be flaky (currently .NET macOS DrainCoordinator, Node Windows
  Chromium E2E) are excluded from required checks and their state is written in the PR body.
- Merge with `gh pr merge --merge` (merge commit, branch history preserved). Afterwards
  `git worktree remove ~/project/zlink-<slug>` and delete the remote branch.
- main protection: GitHub branch protection requires a PR and forbids force-pushes and deletion.
  Direct pushes by admins (the user, the supervisor) are used only for the §6 exceptions
  (`enforce_admins` stays off for that reason).

## 6. Exceptions — direct commits to main

- Plans, decision records and worklogs (`doc/plan/**`), memory-like records (`.artifacts/**` is
  gitignored), and typo fixes in release notes: **documents with no effect on code, CI or specs**.
  The supervisor commits them with an explicit pathspec (`git commit -- <paths>`).
- Release workflow dispatches and tag pushes (tags are not PR material).
- Everything else (production code, tests, bench runners and aggregator, specs and guides, CI) goes through a PR.

## 7. Relation to releases

- Releases start from a tag as in `CONTRIBUTING.md` §9 and `doc/building/release-pipeline.md`. The
  tag goes on a main commit whose milestone has no open Issues.
- Fixes discovered during a release (workflow checks, missing version fields, …) also go through
  PRs. A fix that blocks a release in flight may land directly on main with the user's approval and
  gets an Issue afterwards for the record (the 2026-09-09 0.11.0 cases: the `release-dotnet.yml`
  check, the HttpClient version, the Node `repository` field).

## 8. First application (2026-09-10)

- Milestone `1.0`, the §2 labels, Project `ZLink`.
- Open work as Issues: framework messaging performance P1–P3 (per language), gRPC bench
  DEALER→ROUTER and ClientServer rows, Node framework codec `bytes`, Node/Java raw request-window loss
  (FB-049/050), C++ framework send peer loss (FB-054), HTTP host bind failure propagation (FB-053),
  remaining Unsafe→FFM regressions, cleaning framework references to `MaxMessageSize`/BLOCKY before
  bindings 1.0, PR CI for Core/bindings/Java/C++, the .NET macOS DrainCoordinator hang, the Node
  Windows Chromium E2E hang.
