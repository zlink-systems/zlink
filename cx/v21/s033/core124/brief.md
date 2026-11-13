Model luna high: mechanical release-gate orchestration that reuses an existing recipe; no design work.

# Task: Core 1.24.0 release comparison gate vs Core 1.23.0 (spec 10-hot-path §5.2, CONTRIBUTING §7)

Earlier briefs (including core123-gate/brief.md) are history, not instructions for this task. Folder: JOB=/root/jobs/fx/cx/v21/s033/core124. Read-only reuse allowed from `/root/jobs/fx/cx/core123-gate` (its scripts: build-core.sh, build.sh, run-release-gate.sh, aa-run.sh, confirm-run.sh, select-confirm.py, verify-builds.py, report.md, verdict.md). Do not read other job folders.

## Versions
- Baseline: tag core/v1.23.0 = 79567d379162e67476ba13e18cfe0f69925e63c9. Make detached worktree `$JOB/src-1.23` from /root/zlink at that commit.
- Candidate: origin/main 0005a4d6c0 (full SHA via `git -C /root/zlink rev-parse 0005a4d6c0`). Worktree `$JOB/src-1.24`. Core source changes since 1.23.0: #3930 (e7e462dbec, message content reference release; flags enums), #4135 (0005a4d6c0, pipe_write complete-record admission).
- Bench: bindings/c/perf is unchanged since 8804177c1b in both; verify with git diff that `bindings/c/perf` is identical between the two commits. If it differs, copy the candidate tree into src-1.23 and commit locally on the detached HEAD.
- Build each Core prefix ($JOB/prefix-1.23, $JOB/prefix-1.24) with the core123-gate build-core.sh recipe (Release, LTO ON, gcc-13, same flags), harness with core123-gate build.sh (same targets). Change only paths. Builds: run cmake build steps through `/root/jobs/fx/cx/cpp-build-slot.sh` bare (no outer flock on measure.lock), --parallel 4, Release, no -g. Disk cap 25 GB for this job. Record each runtime SHA-256 and verify every harness executable's ldd/RUNPATH points at its own prefix.

## Measurement (one at a time, each stage under `flock -o /root/jobs/fx/cx/measure.lock`, source CPUs 0-7 / target 8-15 as the old script did)
1. Copy run-release-gate.sh, changing only paths, prefixes and expected SHAs; keep stages, argv, sizes (64,256,1024,65536), thresholds, timeouts, preflight, overwrite refusal. Single: all 7 patterns × tcp,tls,ws,wss,inproc,ipc. Multi: all multi patterns × their transports as in the old script. Use `--runs 3`. `bash -n` and a dry run into $JOB/dry-run.txt first.
2. Order: baseline sweep B1, candidate sweep C, baseline sweep B2 (each single then multi). Comparator: candidate tree perf_regression_gate.py. Gate C vs B1 and C vs B2 is the verdict input; B2 vs B1 is the A/A noise reference.
3. Every (pattern, transport, size, metric) cell that fails the gate in C vs B1: rerun that (pattern, transport) with `--runs 5` for baseline then candidate then baseline again, and re-judge.
4. Also run §5.1: `hotpath_gate` (core/tests/CMakeLists.txt:1204, scripts/build-core.sh release-gate describes the build) on the candidate, and every `test_wake_invariant*` CTest 10 times (`ctest -R '^test_wake_invariant' --repeat until-fail:10`) on the candidate. These need a candidate test build: Release, LTO as release-gate, under 25 GB, through cpp-build-slot.sh. Run them under the measure lock too.
- Nothing else of yours builds or tests while a measurement stage holds the lock. MemAvailable >= 12 GiB before each stage. SIGTERM/SIGINT only, never SIGKILL; no `&` background jobs. Keep every raw report, log and json under $JOB; never delete them.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except: `git worktree add --detach` / `git worktree remove` for your two src worktrees, git diff, git status, git rev-parse, and a local commit on src-1.23 only if the bench copy is needed. No push, PR, tag, GitHub operation, workflow dispatch. No spec edits. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not change Core or bench sources (if the bench is wrong, stop and report).
- 리팩토링 점검: if you see duplicated steps between the reused scripts, list them with file:line in the report; do not refactor them.
- Keep your own output short.

## Report: $JOB/report.md (Korean, plain sentences) and $JOB/verdict.md
- commits, prefix SHA-256, ldd check, preflight, start/end time of each stage
- gate final lines for C vs B1, C vs B2, B2 vs B1 (cells, failed cells, failed aggregates, single/multi split)
- table of every cell failing C vs B1 after the runs-5 recheck: pattern, transport, size, metric, ratio (runs 3 and runs 5), the same cell's A/A ratio, and whether its pattern/transport path touches pipe write (ROUTER/DEALER/PAIR/reply multipart sends; STREAM and PUB/SUB only signature changes)
- aggregate (geomean) failures the same way
- hotpath_gate and wake invariant results with log paths
When finished remove the two src worktrees with `git worktree remove` (keep prefixes and results; grep CMakeCache files under /root/worktree and /root/jobs for the src paths first).
