Model gpt-6.1-sol, effort high: counting batch and pending ratios inside the C++ Framework's concurrent receive, scheduler and completion paths needs care not to change their timing or ordering.

# Task: step 0 of the approved C++ path design (#1639) — count K, K_c, p_s, p_r on current main

Earlier job briefs are history, not instructions. The design is /root/jobs/fx/cx/v21/s033/cpath/report.md (read §4, §5 and §7; PM-approved 2026-11-10). Its §5 leaves four values unmeasured. This job measures them on current main so the design's paper sums can be closed.

## Definitions (from the design §5)
- K: application records a worker processes per worker notification/wake on the target (send-saturation and request cells). Also report records per receive rotation (ingress batch size) separately.
- K_c: completions delivered per completion notification on the source (Framework completion dispatcher → application resume), for send and request.
- p_s: fraction of Framework sends whose binding admission is not immediately accepted (the pending path with a logical pending entry and observer coroutine), source side.
- p_r: fraction of Framework requests whose binding admission is pending (admitted not immediately ready), source side.
Give each as mean, p50, p99 and max per cell where it is a distribution, plus the raw totals (numerator and denominator) per run.

## Do
1. Worktree /root/worktree/zlink-v21-s033/p0 (detached at origin/main 532cd83b67). Add counters with a CMake option that defaults OFF (for example ZLINK_FRAMEWORK_CPP_PATH_COUNTERS); with the option OFF the compiled code must be identical in behaviour and must not add any branch, atomic or field on the hot path (use a macro that expands to nothing). With it ON use thread-local or per-owner plain counters, aggregated at process exit or through the existing bench stats endpoint; no new lock or atomic RMW on the hot path. This patch is a measurement aid for this job only: do not open a PR. Save it as p0/counters.diff.
2. Show the counting build's effect: run each cell's throughput with the option ON and OFF (3 runs each) and report the difference, so the overhead is visible.
3. Cells: C++ Framework send-saturation, request-backpressure, request-serial × 1024, 4096 (the 6 cells of /root/jobs/fx/cx/ratio051pr2/run.sh, same bench, same condition B: source CPUs 0-7, target 8-15). RUNS=3 with counters ON. Record the libzlink SHA-256.
4. State for each counter the exact file:line where it increments and why that point matches the definition (for K: where a worker wakes or is notified and where it takes a record; for K_c: where a completion notification is posted and where each completion is run).
5. Put the four values into the design's §5 formulas (paper_model in /root/jobs/fx/cx/v21/s033/cpath/analysis/paper_model.py; copy it into p0/, do not edit the cpath folder) and report the recomputed per-cell sums against the budgets in cpath/report.md §1 and §5.

## Measurement rules
- Do NOT build, test or take any lock until the file /root/jobs/fx/cx/v21/s033/core124b/verdict.md exists (a Core release gate is measuring). Before that, only read code and write the patch.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` called bare (no outer flock), linux-ninja-release preset, no -g; keep p0/ plus its build dirs under 25 GB (`du -sh` after each build). Bench package/build as in /root/jobs/fx/cx/ratio051pr2/run.sh.
- Each measurement inside `flock -o /root/jobs/fx/cx/measure.lock`, one cell set at a time, MemAvailable >= 12 GiB, log uptime. SIGTERM/SIGINT only; no background jobs with &.
- Keep every raw output (bench JSON, counter dumps, logs) under p0/raw/; never delete them.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only: the worktree, /root/jobs/fx/cx/v21/s033/cpath/ (report.md and analysis/), /root/jobs/fx/cx/ratio051pr2/run.sh, /root/jobs/fx/cx/ratio040/pin.patch and table.py. Write only in /root/jobs/fx/cx/v21/s033/p0/ and the worktree.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or bindings/.
- 리팩토링 점검: while placing counters, note any duplicated wake, notification or completion hop on these paths with file:line (list only; do not fix).
- Time box about 5 hours after the gate verdict appears. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/p0/report.md (Korean, plain sentences)
(1) libzlink SHA-256 and builds; (2) counter placement table with file:line; (3) per cell K, rotation batch size, K_c, p_s, p_r with totals per run; (4) counter ON vs OFF throughput; (5) recomputed design §5 sums vs budget per cell; (6) 리팩토링 점검 list.
