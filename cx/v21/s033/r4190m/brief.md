Model gpt-6-luna, effort high: the #4190 re-measurement under the approved conditions, plus the #4162 A/A in the same window. No product or bench code change.

# Task: #4190 re-measurement — 4 languages × 3 patterns × 1K/4K with the new 8-stream drivers, and the A/A check

Folder JOB=/root/jobs/fx/cx/v21/s033/r4190m. Worktree /root/worktree/zlink-v21-s033/r4190m (detached at #4245 head 73b78f1c01; read-only except build outputs).

## Start order (PM 2026-11-11 07:34Z)
- Build everything NOW (#4245 is still in the merge queue; the measured bench code is its head).
- After the builds, do NOT take the lock until the file JOB/GO exists. The supervisor writes it after #4245 merges; if the merge changed bench/binding/framework runtime paths, GO names the commit and the paths — then `git checkout --detach <commit>` in the worktree and rebuild only those parts before measuring. Waiting for GO is normal; poll every 2 min, do not give up.
Read first: framework/bench/grpc/README.ko.md §4, §7.2, §10 (pattern definitions, the two judgement criteria, 8 submit streams), /root/jobs/fx/cx/v21/s033/s4190/common.md, and the per-language run_local*.sh runners.

## Measurement conditions (approved by the PM, 2026-11-11 — apply to every run)
- Hold `flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE only while runs execute (builds are closed then). Build everything first, outside the lock, through /root/jobs/fx/cx/cpp-build-slot.sh for C/C++ (no outer flock, -j3, binding from source ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON) and with ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core-release/1.24.0/linux-x64 for every language.
- Inside the lock: cpu0-15 scaling_governor = performance; restore powersave on exit with a trap (template /root/jobs/fx/cx/v21/s033/f4162/aa-clean.sh).
- Placement: condition B — source process CPUs 0–7, target process CPUs 8–15 (taskset on each process; the runners' existing pinning hooks if they have them).
- Before and after every cell: uptime/load, free memory, cpu0-15 scaling_cur_freq → JOB/raw/<cell>/conditions-{before,after}.txt.
- Release the lock between language groups (at most ~40 min held at a time) so queued builds can progress; re-take it for the next group.

## Do
1. Cells: for each language (C++, .NET, Java, Node), binding (zlink-<lang>), Framework (zlink-framework-<lang>) and gRPC (grpc-<lang>) rows × request-serial, request-backpressure, send-saturation × 1024, 4096; gRPC × request-backpressure is skipped by the runners (n/a). Plus the C raw reference (zlink-c) for the binding/C ratio, and Kotlin supplementary cells (request-serial @1024) via run_local_kotlin.sh. Use each runner's default warmup/active and the repository's default run count for a judged result (README §7); state the numbers you used.
2. Aggregate with framework/bench/grpc/tools/bench_aggregate.py (the #4230 judgement: framework÷binding ≥ 0.60 on all three patterns × both sizes; binding÷C ≥ 0.80 on request-backpressure). Keep its markdown and JSON outputs.
3. For every cell also record per-process CPU µs/msg (source and target) from /proc/<pid>/task before/after snapshots summed over the TID union (template /root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py).
4. A/A (#4162) in the same session and conditions: run /root/jobs/fx/cx/v21/s033/f4162/aa-clean.sh's measurement body (observe.py --runs 20 --label aa-clean --placement separate) inside your lock window (do not take a second lock — call the body directly while you hold it), then /root/jobs/fx/cx/v21/s033/f4162/analyze-aa.py. Criterion: every run's p50 within ±5% of the median p50 (single measurement, no widening). Report pass/fail with the p50 range.
5. Report (Korean, short): 결과 표 per language (msg/s, ratio, verdict per the two criteria, CPU µs/msg), language verdicts, A/A result, 측정 조건(governor·placement·lock windows·load/freq logs), 검증(commands, raw paths), 남은 문제. Also write JOB/docs-page.md: the same tables plus the conditions, ready for the docs site (the supervisor publishes it).

## Rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Never delete raw outputs; keep everything under JOB. A closed measure.gate or a busy lock is normal: wait, do not give up and never touch the gate/lock files.
- Remove build outputs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/r4190m/report.md
