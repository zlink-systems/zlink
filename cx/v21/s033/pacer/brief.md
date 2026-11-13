Model gpt-6-luna, effort high: bench-only change with a reference diff and fixed semantics.

# Task: put the submission pacer and CPU placement into the repository C++ bench, and give both drivers the same submission shape (#1639, design.md F4)

Earlier job briefs are history, not instructions. Worktree /root/worktree/zlink-v21-s033/pacer, branch perf/1639-bench-pacer at origin/main 007e3ce524.
Read first: /root/jobs/fx/cx/v21/s033/pacer/input/design.md §3, §4.2, §4.3, §6 F4; input/m-result.md (M1, M4); input/xio-client.diff and input/xio-runner.diff (the out-of-tree changes used for every 0.33.0 measurement so far).

## Why
Every 0.33.0 measurement used a `--target-rate` pacer and CPU placement (source on 0-7, target on 8-15) that exist only outside the repository. M4 showed the binding source sends requests in pairs because of the pacer, while the Framework driver submits at most one request per loop pass, so its source gaps are 25-34 us and its requests do not pair. The two drivers must receive the same submission bursts so that a comparison isolates the target.

## Do (bench only: framework/bench/grpc/cpp/)
1. client/bench_cpp_client.cpp: add `--target-rate <msgs/s>` (default 0 = unpaced, identical to today) with the pacer semantics of xio-client.diff `submission_pacer_t` (schedule = max(now, next); next = schedule + 1/rate). Use one pacer per driver. Do NOT port the P2 variants (`--p2-variant`, C1, C2, completion_waiter_t): they were diagnostics.
2. Same submission shape in both drivers when paced: after the pacer wakes, each driver submits every slot that is already due (`next <= now`) before it waits again, bounded by the existing window/admission limits. Give the pacer a call that returns how many slots are due, rather than adding a second rule elsewhere. The binding driver's coroutine slots and the Framework driver's loop (`can_submit` path around the `take_completed` loop) must both follow it. Unpaced behaviour (rate 0) must stay exactly as on main.
3. run_local.sh: pass `TARGET_RATE` to the client as `--target-rate` when set; add optional `SOURCE_CPUS` and `TARGET_CPUS` (taskset -c lists, unset = no taskset, identical to today). Document both in the script header comment and in the bench README section that lists runner variables, if one exists (grep first).
4. Keep the client's existing output format; add `target_rate` to the recorded cell options only if the cell JSON already records the other options (check how they are written).

## Rules
- Build only via `/root/jobs/fx/cx/cpp-build-slot.sh bash framework/bench/grpc/cpp/build.sh` (it waits for the measurement lock; if it has not started within 20 minutes, stop and write "ready to compile" in the report). Do not run the bench or any measurement.
- Read and write only that worktree and /root/jobs/fx/cx/v21/s033/pacer/. No product code changes (framework/languages, core, bindings), no spec edits, no git operations except diff/status/log/show, no GitHub, no sub-agents or other AI tools. SIGTERM/SIGINT only; no background jobs with &.
- Before finishing, read your diff line by line and count brackets and call arguments; run `bash scripts/format/format.sh --check cpp` and fix what it reports.
- Time box about 2 hours. Keep output short.

## Output: /root/jobs/fx/cx/v21/s033/pacer/report.md (Korean, plain sentences)
What each driver now does per pacer wake (file:line), that rate 0 is unchanged (file:line), the runner variables, build result or "ready to compile", and the diff saved as pacer/change.diff.
