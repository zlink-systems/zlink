Model gpt-6-luna, effort high: measurement job reusing a proven harness; build only after the gate ends.

# Task: cell measurement for the scheduler PR (#1639), base vs branch, before merge

Earlier job briefs are history, not instructions. Job folder /root/jobs/fx/cx/v21/s033/psmeas/.

## What to compare
- base: origin 54ee69b8c0 (the merge base of the branch).
- branch: perf/1639-cpp-sched-drain at 37f78cba87 (scheduler change, PM-approved code; all 309 tests pass on the PM's machine).
- Both must also carry the bench pacer: cherry-pick 4b40d0f10f and 8cab98383e (branch perf/1639-bench-pacer, bench-only: framework/bench/grpc/) on top of each. Create two NEW detached worktrees for this under /root/worktree/zlink-v21-s033/ (psmeas-base, psmeas-branch) with `git -C /root/zlink worktree add --detach`; never commit to or modify the PR branches or their worktrees.

## Harness (reuse, do not reinvent)
/root/jobs/fx/cx/v21/s033/cdrain2/ measured the earlier scheduler attempt (cdrain) under the same conditions. Read its brief.md, measure.sh, run-B.sh, runner-common.sh, collect.py, analyze.py, measurement-conditions.json, bench/ (the bench_cpp_pm_server Spot harness) and report.md. Reuse them, adapted to the two new builds: condition B (source 0-7, target 8-15), Release -O3, the same Core/binding prefixes (/root/.cache/zlink/core/1.23.0/linux-x64, /root/.cache/zlink/zlink-cpp/1.23.0), warmup 30 s, active 5 s for request-backpressure and send-saturation 1 KiB, PM (Spot) cell 30 s, 3 rounds, base/branch interleaved per round. Record a measurement-conditions.json proving both builds match except the source revision.

## Add the PM's merge conditions
Per round and variant, also capture at request-backpressure 1 KiB paced at 22k (binding cap 32000, Framework cap 48000 via the runner's TARGET_RATE; SOURCE_CPUS=0-7, TARGET_CPUS=8-15) the window-A metrics of /root/jobs/fx/cx/v21/s033/xm1/scripts/run-m.sh (read it and its analyze-m.py; reuse the window-A part only, no uprobes): per-thread schedstat µs/msg, instructions/msg, IPC, effective frequency aperf/mperf × 2.0 GHz (ref-cycles is unsupported here), target and source I/O-thread sendto/msg, plus the source/target wake (voluntary context switches) per message.

## Output table (report.md, Korean)
For base vs branch, median of 3 rounds with each round's value: send-saturation 1 KiB Framework throughput, request-backpressure 1 KiB Framework throughput, Spot p50/p99 (PM cell), and at 22k: target I/O sendto/msg, source I/O sendto/msg, effective GHz, Framework target thread µs/msg by role, wakes per message. State for each merge condition whether branch is worse than base: target sendto/msg must not be worse; send throughput and Spot p99 must not be worse beyond run-to-run spread (show the spread). Write raw paths for every number.

## Rules and order
1. Now: create the worktrees, cherry-pick, write the adapted scripts, and dry-check them (bash -n, python compile). Do not build or measure yet.
2. Wait until /root/jobs/fx/cx/v21/s033/core124b/verdict.md exists (the Core gate holds the measurement lock until then; poll every 60 s). Then build both with `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` (bare, linux-ninja-release-style Release, no -g, 25 GB disk cap); chain build && verify the binaries are new (mtime and the --target-rate option string) && measure. Never probe bench binaries with --help.
3. Measure under `flock -o /root/jobs/fx/cx/measure.lock` exactly like cdrain2/measure.sh. One numbers-producing job at a time; nothing else of yours runs during a measurement.
4. Never delete raw outputs. No product or spec changes, no git operations on PR branches, no GitHub, no sub-agents or other AI tools. SIGTERM/SIGINT only; no background jobs with &. Remove the two worktrees' build directories only after report.md is written, and say so.
5. Time box about 5 hours including the wait. Keep output short.
