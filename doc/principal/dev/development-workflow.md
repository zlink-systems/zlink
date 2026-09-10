# Development workflow — Issue · branch/worktree · PR

> In effect from 2026-09-10 (after the framework 0.11.0 release, for the 1.0 preparation). User decision.
> This page owns "where a piece of work is registered, on which branch it is done, and how it lands
> on main". Commit messages, versions and release procedure stay with [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) §9;
> agent operating rules and the paths jobs may not touch stay with [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) §10 and
> [`AGENTS.md`](../../../AGENTS.md) (this page references them and does not restate them).
> Revised after the 2026-09-10 codex review (`.artifacts/codex/workflow-doc-review/summary.md`, adoption table `adoption.md`).

## 1. At a glance

```
Issue ──> branch + worktree ──> work (person or codex job) ──> PR ──> CI + supervisor review ──> merge ──> clean up
  │                                                              │
  └── grouped by Milestone (release) and Project (board)          └── the last PR's "Closes #N" closes the Issue
```

`main` changes **only through PRs**. Every exception is listed in §6 (other sections only point there).
One branch = one Issue = one worktree. One Issue may have several PRs (§5).

## 2. Registering work — GitHub Issues

### 2.0 Three kinds of work

The procedure exists for **changes that land in main**. Not every task carries the same weight.

| Kind | Issue | Branch and worktree | PR | Board |
|---|---|---|---|---|
| **A change that lands in main** | yes | yes | yes | listed |
| **A defect found** | yes (stays open until fixed) | when fixing | when fixing | listed |
| **A throwaway experiment or measurement** | **no** | a temporary one if code must change, not on the board | **no** | not listed |

**When an experiment produces something worth keeping, open a PR from it directly, with no Issue.**
Branch protection asks for a PR, not for an Issue. Naming the report path and the measurements in the
PR body is traceability enough. An Issue is for the other cases — work that spans sessions or people,
work deferred rather than done now, and work that has to appear in a release-scope decision. So the
default path is **experiment → (if good) PR**, and an Issue is created when the work must be handed
over or postponed.

An experiment leaves only its brief and report under `.artifacts/codex/<name>/`. When it must change
code it gets a worktree without an Issue, a PR or a board entry, and that worktree is removed
afterwards with the sweep in [§4.3](#43-the-other-development-scripts). When an experiment **finds a
defect, that is when an Issue is created** — the experiment itself never becomes one.

There is one test: **does the result of this work stay in main?** When it does not, the procedure
does not apply.

### 2.1 Writing the Issue

- Every piece of work is an Issue before it starts. The title is a one-line outcome; the body has
  three sections — **scope / done criteria / evidence** — and none may be empty. Evidence names the
  decision record number (`FB-nnn`, `D-nnn`) **with its file path and anchor**
  (`doc/plan/fw-bench-worklog/decisions.ko.md#fb-056`), measurements and report paths.
- Two label axes only:
  - `area:` `core` · `bindings` · `framework-dotnet` · `framework-java` · `framework-node` · `framework-cpp` · `bench` · `ci` · `docs`
  - `kind:` `bug` · `perf` · `feature` · `chore`
- One Issue is one cause and one outcome. The same fix in several languages becomes one Issue per
  language, cross-linked. Follow-up stages with the same cause (diagnosis, then fix) stay in **one
  Issue** and are split into PRs (§5).
- Decision records (`doc/plan/**/decisions*.md`) are the evidence; an Issue is that evidence turned
  into a to-do. Cross-reference format: the Issue body says `evidence: decisions.ko.md#fb-056`, the
  record's heading says `(Issue #7)`. Add the back-link in the record in the same commit that creates
  the Issue (a §6 exception document).

## 3. Grouping — Milestones and Projects

- **Milestone** = release. Create one per release (`1.0` for Core 1.0 · bindings 1.0.0 · framework 1.0.0)
  and attach the Issues that must ship in it. The pre-tag condition is "every Issue in the milestone
  was closed **by a merged PR**, and the main commit to be tagged contains all of those PRs" (manual
  closes do not count; `work.sh status --milestone 1.0` checks both). The milestone name is given by
  `work.sh start --milestone`; the default is `current_milestone` in `scripts/dev/work.conf` (one place
  to change after a release).
- **Project** = the board `ZLink` (https://github.com/users/zlink-systems/projects/1, also linked in the
  repository's Projects tab). Board rows are **Issues only** (PRs appear as linked information). Status
  `Todo → In progress → Review → Done`, fields `area` (same values as the label, filled automatically)
  and `runner` (`astra` / `sol` / `direct` / empty; optional). **Only `work.sh` moves the status**
  (built-in GitHub workflows that touch the same field stay off). A failed Project update is a warning
  and never blocks the work (§4.2).
- Labels stop at the two axes in §2; status and runner live only in Project fields.

## 4. Branches and worktrees

- Branch name: `<area>/<issue-number>-<slug>` — e.g. `framework-java/6-mesh-pump`, `bench/13-dealer-rows`.
- Worktree path: `~/project/zlink-<issue-number>-<slug>` (the number prevents slug collisions).
  `git worktree add <path> -b <branch> origin/main` — run `git fetch` first so the branch forks from
  the latest `origin/main`. Never open the same branch in two worktrees.
- A codex job receives the worktree path via `-C` and commits **only on that branch** (commit
  delegation only when the brief says so; a job without it delivers a diff and a report). The
  supervisor pushes, opens the PR and merges. The paths a job may not touch are owned by
  `CONTRIBUTING.md` §10.
- Catch up with main using `git fetch && git merge origin/main` (no rebase on shared branches).

### 4.1 Shared local-package cache (content-addressed)

Binding local packages (nuget `Zlink.*`, npm `@zlink-systems/zlink`, maven `systems.zlink:zlink*`,
C++ `install/zlink-cpp`) have equal outputs for equal inputs, so they are shared by hash (the same
principle as the vcpkg binary cache and the Conan cache). Rules:

- **Key** = first 16 characters of `sha256(tree hash of bindings/ ‖ BINDINGS_VERSION ‖ Core version ‖
  tree hash of scripts/local-package/ ‖ platform <os>-<arch> ‖ toolchain id)`. The toolchain id is one
  line combining the compiler, SDK, Node and JDK versions `build-wsl.sh` uses (the script prints it).
- **Share only from a clean tree**: with staged, unstaged or untracked changes under `bindings/` or
  `scripts/local-package/`, the shared cache is not used; the build goes to a worktree-private
  directory (`.artifacts/wsl-private/`). Dirty outputs are never published under a shared key.
- **Location and publication**: `~/.cache/zlink/packages/<key>/`. Build in `<key>.staging-<pid>/`,
  verify (presence and digest of each language's package), then publish atomically with `rename`. A
  published key is immutable. Concurrent builds are serialised by `<key>.lock` (`flock`) so there is one
  writer. `.complete` lists the per-language outputs and their digests.
- **Scope of sharing is the binding packages only**: a worktree's `.artifacts/wsl/` stays a per-worktree
  writable directory in which only the binding package files are symlinks into the cache. Packages
  produced by the framework (`Zlink.HttpClient`, `@zlink-systems/http-client`, …) and build trees are
  per worktree. The Core release prefix `~/.cache/zlink/core/<ver>/<platform>` stays shared as today.
- **The hit path also verifies**: even when only linking, run `sync-version.py --check` and
  `build-wsl.sh --verify-versions` (the same checks as the miss path). Consumption check: .NET keeps
  the gate's per-package-digest `NUGET_PACKAGES` scheme so same-version/different-content packages never
  mix; npm and Maven are checked by digest as well.
- **Pruning**: `scripts/local-package/cache-prune.sh --keep 5` keeps the five most recent keys but
  never removes a key linked by an existing worktree or by the baseline worktree. `work.sh done` removes
  links only, never cache entries.
- Issues that need no packages (documentation work) start with `work.sh start --no-packages`.

### 4.2 One command per step — `scripts/dev/work.sh`

Each step is one command so that nothing is forgotten; people and the supervisor start, submit and
finish work only through it. **Every command is safe to re-run**: it inspects the existing
Issue/branch/worktree/PR state first, skips finished steps, resumes from the first unfinished one and,
on failure, prints the created IDs, URLs and paths together with the resume command.

| Command | What it does |
|---|---|
| `work.sh start "<title>" --area <area> --kind <kind> --body <file> [--milestone <name>] [--no-packages]` | creates the Issue (refused when any of the three body sections is empty) → labels and milestone → branch → worktree `~/project/zlink-<number>-<slug>` → local packages (§4.1) → Project `In progress`. Prints the Issue number and the worktree path |
| `work.sh start --issue <N> [--no-packages]` | starts or resumes work on an **existing Issue** (never creates a duplicate); reuses an existing branch/worktree |
| `work.sh pr --body <file> [--closes\|--refs]` | validates title, the three body sections, the Issue number and base=main → pushes → creates the PR (updates an existing one). Only the final PR carries `Closes #N`; intermediate PRs carry `Refs #N` (default `--refs`; use `--closes` when the done criteria are all met) → Project `Review` |
| `work.sh status [--milestone <name>]` | table of this machine's worktrees, branches, Issues, PRs and CI checks. `--milestone` checks the §3 release condition (closed by merged PRs and contained in main) |
| `work.sh done --verified <sha>` | only when the PR HEAD equals `<sha>` (the commit the supervisor read and verified): `gh pr merge --merge --match-head-commit <sha>` → delete the remote branch → remove the worktree (refused when dirty or unpushed changes exist) → Project `Done`. For a `Refs` PR the Issue stays open and the worktree is kept |

- Project and milestone updates are best-effort: without permission or connectivity a warning is
  printed and the local steps continue. `status` always shows the local information.
- Until `work.sh` exists (§8) the manual procedure is the command list in §8.

### 4.3 The other development scripts

`work.sh` covers the flow of a single Issue. The work that repeats outside it belongs to the scripts
below. What exists and what does not is stated explicitly — **the document never describes a script
that has not been written.**

| Script | What it does | Status |
|---|---|---|
| `scripts/dev/work.sh` | Issue → branch and worktree → PR → merge and cleanup (§4.2) | exists |
| `scripts/local-package/package-cache.py`, `cache-prune.sh` | Shared cache for binding local packages, and its pruning (§4.1) | exists |
| `scripts/perf/perf-ticket.sh`, `perf-queue-runner.sh` | Every measurement runs as one serialized ticket | exists |
| `scripts/gate/*.sh` | Per-language gates | exists |
| `scripts/dev/job.sh` | Start, inspect, watch every three minutes and stop codex sub-agent jobs. Checks the log right after start so a wrong model id or an auth failure is reported at once. Stops a job only by its recorded pid | exists |
| `scripts/dev/worktree-sweep.sh` | Judge abandoned worktrees by safety (uncommitted, unpushed, contained in main, a running job) and remove the safe ones | exists |
| `scripts/dev/session-setup.sh` | At session start, settle bench port reservation, tmpfs headroom, the measurement queue and local packages in one pass | exists |
| `scripts/dev/release-check.sh` | Before tagging, check version synchronization, release notes, package metadata and the publish targets | exists |
| Bench result comparison tool | Turn two measurement sets into a scenario × payload table (throughput, latency, ratio, change) | Issue #37 |
| `scripts/dev/ci-watch.sh` | Keep a single CI watcher and poll every ten minutes (GitHub API limits) | exists |

Three rules apply to all of them.

- **Safe to re-run**: running the same command again never leaves the state inconsistent.
- **No silent failure**: a script that starts a background process checks that it is alive right
  after starting, and shows the last error from its log when it is not.
- **Stop by pid**: process termination uses the recorded pid only. A pattern search such as
  `pkill -f` also kills the calling shell.

## 5. PRs and merging

- PR title: `<module>: <one line>`. The first body line is `Closes #N` (done) or `Refs #N` (partial).
  The body carries (1) what changed and (2) **verification — commands run, target SHA, package
  digests, the key result numbers (as a table), remaining failures — written into the body itself**.
  Local report paths (`.artifacts/codex/<job>/summary.md`) are secondary: they are gitignored and
  invisible from another machine, so the tables used for the judgement are copied into the PR body
  (or into a `doc/plan/**` record).
- CI: pull_request workflows run under production-code path filters. Today that is framework
  .NET and Node; Core, bindings and framework Java/C++ are Issue #16. Documentation, bench, samples,
  scenario E2E, `VERSION` and `scripts/local-package` changes have no PR CI, so the local verification
  record in the body (previous item) stands in. Documentation PRs record the `mkdocs build --strict` result.
- Merge conditions: (a) the supervisor (or the user) read the diff **of the PR HEAD** (`done --verified <sha>`),
  (b) the area's gate and tests passed (PR CI or the body record), (c) changes that need measurement
  carry the ticket-queue 3-run result in the body. Pending, cancelled or not-run checks are not a PASS.
- Flakiness is handled **per test**: known flaky tests are tracked as Issues (currently #17 .NET macOS
  DrainCoordinator, #18 Node Windows Chromium E2E); only when a job failed on that test alone may the
  PR body cite the Issue and proceed. Whole jobs are never exempt. Required status checks are empty
  today and will be set by job name once the Issue #16 PR CI exists (only aggregate jobs that treat
  "skipped" as passing become required, so PRs outside the path filters never wait forever).
- Merge with `gh pr merge --merge` (merge commit). `done` deletes the remote branch and removes the worktree afterwards.
- main protection: PR required, force-pushes and deletion forbidden. Direct pushes by admins (the
  user, the supervisor) are used only for §6 (`enforce_admins` off).

## 6. Exceptions — direct commits to main (the only list)

1. **Record documents** with no effect on code, CI or specs: `doc/plan/**` (plans, decision records,
   worklogs, results), typo fixes in release notes. The supervisor commits them with an explicit pathspec
   (`git commit -- <paths>`).
2. Release workflow dispatches and tag pushes (tags are not PR material).
3. **A fix that blocks a release in flight** (a workflow check, a version field — something that only
   matters for that release) may land directly with the user's approval and gets an Issue afterwards for
   the record (the 0.11.0 cases: the `release-dotnet.yml` check, the HttpClient version, the Node `repository` field).

Everything else (production code, tests, bench runners and aggregator, specs and guides, CI, `scripts/**`) goes through a PR.

## 7. Relation to releases

- Releases start from a tag as in `CONTRIBUTING.md` §9 and `doc/building/release-pipeline.md`. The
  tagged commit is a main commit that satisfies the §3 milestone condition (`work.sh status --milestone`).
- Fixes discovered during a release follow §6-3.

## 8. First application (2026-09-10) — the manual procedure until `work.sh` exists

- Done: Milestone `1.0`, the §2 labels, Project `ZLink` (Issues #5–#20 registered).
- `work.sh` (§4.2) and the shared cache (§4.1) are built in the PR for Issue #20. Until that PR is
  merged, run the commands below by hand (same order and content as §4.2).

```bash
# start (existing Issue N)
git fetch origin
git worktree add ~/project/zlink-N-<slug> -b <area>/N-<slug> origin/main
mkdir -p ~/project/zlink-N-<slug>/.artifacts && ln -s ~/project/zlink/.artifacts/wsl ~/project/zlink-N-<slug>/.artifacts/wsl   # temporary until the shared cache exists
gh project item-edit --project-id PVT_kwHOErOLTs4Bi-bl --id <item> --field-id <Status> --single-select-option-id <In progress>
# submit
git push -u origin <area>/N-<slug>
gh pr create --base main --head <area>/N-<slug> --title "<module>: <summary>" --body-file <body>   # first line Refs #N or Closes #N
# finish (with the verified SHA)
gh pr merge <PR> --merge --match-head-commit <sha>
git push origin --delete <area>/N-<slug>
git worktree remove ~/project/zlink-N-<slug>
```
