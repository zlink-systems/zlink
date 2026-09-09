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
- Local packages come from a **content-addressed shared cache** (§4.1); worktrees do not rebuild them.
- A codex job gets the worktree path via `-C` and commits **only on that branch**; the supervisor
  pushes. Jobs still never touch `doc/**`, specs or `.github/**` (report as BLOCKERS).
- Catch up with main using `git fetch && git merge origin/main` (no rebase on shared branches).

### 4.1 Shared local-package cache (content-addressed)

The inputs of the binding local packages (nuget, npm, maven, C++ install) are only the `bindings/`
source tree, `BINDINGS_VERSION` and the Core version. Equal inputs give equal outputs whichever
worktree built them, so they are shared by hash (the same principle as the vcpkg binary cache, the
Conan cache and the Gradle build cache).

- Key: the first 16 characters of `sha256(git rev-parse HEAD:bindings ‖ BINDINGS_VERSION ‖ core_version)`.
- Location: `~/.cache/zlink/packages/<key>/{nuget,npm,maven,install}` plus a `.complete` marker (so a
  key still being built is never read by another worktree).
- A worktree's `.artifacts/wsl` is a symlink to that directory. `scripts/local-package/build-wsl.sh`
  links when the key exists with `.complete`, otherwise builds and then links. The Core release prefix
  `~/.cache/zlink/core/<ver>` stays shared by version as today.
- A branch that changes bindings gets a different key and therefore its own outputs automatically.
  Framework sources are consumed through workspaces/ProjectReferences, not packages, so they are not cached.
- Pruning: `scripts/local-package/cache-prune.sh --keep 5` keeps the five most recent keys.

### 4.2 One command per step — `scripts/dev/work.sh`

Each step is one command so that nothing is forgotten. The supervisor and people start, submit and
finish work only through it.

| Command | What it does |
|---|---|
| `work.sh start "<title>" --area <area> --kind <kind> [--body <file>]` | creates the Issue (labels, milestone `1.0`, Project `Todo`) → branch `<area>/<number>-<slug>` → worktree `~/project/zlink-<slug>` → local-package link (§4.1) → Project `In progress`. Prints the worktree path |
| `work.sh pr [--body <file>]` | pushes the current worktree's branch → creates the PR (first line `Closes #<number>`, body per §5) → Project `Review` |
| `work.sh status` | table of this machine's worktrees, branches, Issues, PRs and CI checks |
| `work.sh done` | merges the PR (`--merge`) → deletes the remote branch → removes the worktree → Project `Done` (the Issue closes through `Closes`) |

`start` refuses an Issue body that lacks the three sections (scope / done criteria / evidence). A codex
job receives the worktree path printed by `start` via `-C`.

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

- Milestone `1.0` and the §2 labels (done 2026-09-10). Project `ZLink` follows once the gh token has the `project` scope.
- `scripts/dev/work.sh` (§4.2) and the shared package cache (§4.1) are the first PR (registered as an Issue).
- Open work as Issues: framework messaging performance P1–P3 (per language), gRPC bench
  DEALER→ROUTER and ClientServer rows, Node framework codec `bytes`, Node/Java raw request-window loss
  (FB-049/050), C++ framework send peer loss (FB-054), HTTP host bind failure propagation (FB-053),
  remaining Unsafe→FFM regressions, cleaning framework references to `MaxMessageSize`/BLOCKY before
  bindings 1.0, PR CI for Core/bindings/Java/C++, the .NET macOS DrainCoordinator hang, the Node
  Windows Chromium E2E hang.
