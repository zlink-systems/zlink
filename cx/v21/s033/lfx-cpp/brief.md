Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: C++ ladder quick compare L1 → L2 on the C++ fix branch (6 cells × 1 run). NO PRODUCT CODE CHANGES.

Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh or any git fetch/pull, never delete raw files (yours or earlier jobs'). Do not touch other worktrees or their build outputs.

## What is being measured
Worktree (already created, do not change its commits): /root/worktree/zlink-v21-s033/cfx, branch exp/4350-cpp-fixes, HEAD 70158ead2a =
- PM's L2 fix perf/4352-cpp-l2-envelope b5afcf4c55 (main 6350275fac + envelope header copy removal),
- + merge of c4json perf/4354-cpp-typed-json aae182694c (L4 typed JSON; does not affect L1/L2 but is part of the branch),
- + the C++ ladder bench from exp/3868-cpp-ladder-next 62e766f88d (patch /root/jobs/fx/cx/v21/s033/cfx.ladder.patch; one conflict in raw_mesh_node_owner.cpp resolved by keeping main's owner-generation code; commit 70158ead2a).
Steps are selected in one build by environment variable ZLINK_CPP_LADDER_LEVEL (1 = L1, 2 = L2). Definitions: /root/jobs/fx/cx/v21/s033/ladder-steps.md.
Previous numbers for comparison only (main, l3868-s1, 10 s-era timing): L1→L2 loss send 1K 12.63% / 4K −1.01%; serial 1K 11.21% / 4K 6.08%; backpressure 1K 14.02% / 4K 12.26%.

## Step 1 — build (outside the measurement lock)
1. mkdir /root/jobs/fx/cx/v21/s033/lfx-cpp/{tools,raw}. Copy /root/jobs/fx/cx/v21/s033/l3868-s1/tools/* to lfx-cpp/tools. Edit only the copies: every path to the old worktree/build dir (e.g. `cpp-cell-runner.sh:14` BUILD_DIR, configure-build-inner.sh, build.sh, run-group-locked.sh) → worktree /root/worktree/zlink-v21-s033/cfx and build dir /root/worktree/zlink-v21-s033/cfx/build-cfx. `grep -rn "l3868" lfx-cpp/tools` must print nothing afterwards except comments.
2. Build through `/root/jobs/fx/cx/cpp-build-slot.sh <your build command>` (call it bare, no outer flock): Release, `-DUSE_BINDINGS_SOURCE=ON`, Core prefix /root/.cache/zlink/core/1.25.0/linux-x64, `--parallel 3`, build dir inside the worktree, ≤25 GB disk. Use the same configure options l3868-s1 used (its configure-build-inner.sh). Save the log to raw/build.log.
3. Gate: build rc must be exactly 0 (rc 147 = paused by guard, wait and retry, never treat as success); bench_cpp_framework_server and bench_cpp_client must exist and be newer than every source file changed in the branch (`git -C cfx diff --name-only 6350275fac HEAD`). Record SHA256 of both binaries in raw/build-meta.txt with HEAD and `git status --short` (must be clean).
If the build fails, stop and write the error with file:line in the report; do not edit product code.

## Step 2 — measure (timing set by 대표님 2026-11-14, binding perf timing)
- Cells: send-saturation, request-serial, request-backpressure × 1K, 4K (same args as l3868-s1 plan-L0-L1.json / plan-L2-L3.json).
- Timing: WARMUP_SECONDS=1 (runner minimum; no separate time warmup), active 5 s, 3 s sleep between runs, NO discarded warm run, 1 counted run per (step, cell).
- Order per cell: L1 then L2 back to back (12 runs total), source CPUs 0-5, target CPUs 8-13, every process pinned by the tool itself.
- Lock: build first, then run the whole set inside ONE exclusive hold: `flock -x -w 5400 /root/jobs/fx/cx/measure.lock <runner>` (blocking; never -n). The hold must stay under 10 minutes; if it would not, split into two holds (send+serial, then backpressure).
- Every run: errors 0 and submitted = completed = received, SCHED_OTHER and Cpus_allowed_list of all source/target threads recorded at start and mid-active (the tools already do this — check the files exist). An invalid run is repeated once in the same hold; keep the invalid raw.
- Boundary: a cell whose L1→L2 loss is between 5% and 9% gets 2 more L1/L2 pairs in a later hold (still blocking lock); use the median of 3.

## Output
/root/jobs/fx/cx/v21/s033/lfx-cpp/report.md (Korean, short):
- one table: cell | L1 msg/s | L2 msg/s | loss `1 − L2/L1` | PASS (≤ 7%) / FAIL / boundary median — each number linked to its raw results.json and scheduling file;
- build fingerprint (HEAD, binary SHA256, rc), hold windows (UTC start/end, rc);
- previous l3868-s1 loss next to each cell for reference.
Time cap 2 h (most of it may be lock wait). Keep build-cfx after the job (L3/L4 will follow).
