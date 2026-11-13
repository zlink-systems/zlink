Model gpt-6-luna, effort high: fix one defect in the PUBSUB catch-up tracker found by the supervisor, then re-measure. Earlier job briefs are history, not instructions.

# Task: #4162 PUBSUB catch-up — the paced-receive count is never fed, so no run can catch up

Worktree /root/worktree/zlink-v21-s033/f4162b (branch with commits 7a65e1f013 and e068171c77). Commit on it; do not push.
Read first: /root/jobs/fx/cx/v21/s033/f4162c/report.md and its raw/ (previous job), doc/perf/PERF_POLICY.md §"측정 재현성 판정" (PUBSUB catch-up definition, already correct).

## Finding (supervisor)
All five PUBSUB runs in /root/jobs/fx/cx/v21/s033/f4162c/raw/pubsub-0[1-5].log end `caught_up,no`, but the diagnostics show the receiver is NOT behind: e.g. raw/pubsub-01.log BENCH_DIAG `target_sender_rate,9133.12 ... received_rate,11416.4,paced_received_rate,0,caught_up,no`. The receiver receives faster than the paced target, yet `paced_received_rate` is 0. So the in-flight estimate (scheduled paced sends − least-received subscriber's paced receives) never sees paced receives and stays large. The defect is in how paced receives are counted/attributed (phase marker, sequence/phase stamp the subscriber checks, the "paced 수신 시작" anchor, or which counter the tracker reads), not in the catch-up rule.

## Do
1. Find why paced receives are 0 (file:line), fix it in the C multi PUBSUB perf (bindings/c/perf/multi/...) only. Keep the catch-up rule exactly as in PERF_POLICY.md; no new thresholds.
2. Add a unit case that fails before the fix: a subscriber receiving paced-phase messages at the target rate must produce paced_received > 0 and catch up.
3. Re-measure PUBSUB tcp 256 B ×5 with the same command as /root/jobs/fx/cx/v21/s033/f4162c/raw/run-pubsub-five.sh, under the approved conditions: `flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE for the runs only, cpu0-15 governor performance inside the lock and powersave restored on exit (trap; template /root/jobs/fx/cx/v21/s033/f4162/aa-clean.sh), log uptime/load, free memory and cpu0-15 scaling_cur_freq before/after each run. Report per run: catch-up seconds, valid/invalid, mean/p50/p95, throughput, max in-flight at catch-up, paced_received_rate.
4. Keep every raw file in /root/jobs/fx/cx/v21/s033/f4162d. Build outputs inside the worktree; remove at the end.

## Rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Every claim file:line or raw path. Report in Korean, short: 원인(file:line), 바꾼 것, 수정 전 실패한 시험, 5회 표, 측정 조건, 리팩토링 점검 한 줄, 남은 문제.

## Output: /root/jobs/fx/cx/v21/s033/f4162d/report.md
