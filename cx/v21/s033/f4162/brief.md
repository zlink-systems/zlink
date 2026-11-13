Model gpt-6-luna, effort high: implement an approved, fully specified bench-statistics change (one shared helper) and run a fixed 20-run A/A check. Earlier job briefs are history, not instructions.

# Task: #4162 — constant per-message latency statistics in the C perf bench (single + multi share one helper), then 20-run A/A

Folder JOB=/root/jobs/fx/cx/v21/s033/f4162. Worktree /root/worktree/zlink-v21-s033/f4162 on branch perf/4162-bench-latency-stats (= origin/main). Read first: /root/jobs/fx/cx/v21/s033/i4162/report.md (diagnosis: reservoir replacement after the cap fills — bindings/c/perf/single/common/perf_single_latency.hpp:36-50, next_random()%_count + random slot write — makes receive cost change mid-run; queue and mean latency follow it).

## Approved design (supervisor decision; do not substitute another)
- Replace reservoir sampling with a fixed log-linear histogram over ALL received messages: preallocated bucket array, per sample = bucket index from the value (leading-zero count + a fixed number of sub-bucket bits, e.g. 7 → relative bucket width ≤ 1/128) + one increment; also keep exact count, sum, min, max. No branch that changes after a size is reached, no RNG, no allocation on the hot path. Percentiles (p50/p95/p99/...) are read from the histogram at report time as the bucket's representative value; mean stays exact (sum/count).
- One helper for both single and multi: the multi copy (bindings/c/perf/multi/common/perf_multi_metrics.hpp:147 and whatever it calls) and the single one become one header used by both (decision sites for percentile storage 2→1). Remove the old reservoir code and the sample-cap option/env if nothing else needs it (say what you removed; if a script or doc passes the cap, update it). Also fold the duplicate received-count state the diagnosis lists (perf_single_one_way.hpp:310 vs perf_single_latency.hpp:39) into the helper's count if it is the same number (2→1).
- Output JSON fields keep their names and units so existing parsers keep working; if a field's meaning changes (e.g. sample count now = all messages), say so.
- Policy doc: one line in doc/perf/PERF_POLICY.md (near :106/:120, the percentile clause) stating percentiles come from all messages in a log-linear histogram with relative error ≤ the bucket width. Find the matching English/Korean pair if both exist and update both.

## Verify
1. Build the C perf bench (same build as /root/jobs/fx/cx/v21/s033/i4162/build.sh, adapted to this worktree; through /root/jobs/fx/cx/cpp-build-slot.sh, -j3) against the same Core candidate library the diagnosis used (/root/jobs/fx/cx/v21/s033/core124/prefix-1.24). Run the bench's existing unit/self tests if any (find them) and a small correctness check of the histogram: feed known values, compare p50/p99 with exact sorted percentiles within the bucket error.
2. A/A: ROUTER_ROUTER inproc 1024B, 20 runs of the new binary, same command and CPU placement as the diagnosis natural runs (i4162/run-natural.sh, observe.py), under `flock -o /root/jobs/fx/cx/measure.lock` for the measurement only. Report per-run mean/p95/throughput/queue approximation and the spread: max/min of mean and whether all 20 runs are within 5% of the median (the PM's criterion). Also 3 runs each of PUBSUB tcp 256B and DEALER_DEALER tls 64B (multi) with the same before-binary (i4162/bin) and the new binary, alternating, to show whether the multi bimodality also goes away.
3. If the 20-run A/A still shows two groups, stop and report (do not try another design): the supervisor raises option D.

## Report sections
결과(표), 판정 위치(전/후 수: percentile 저장·교체 2→1, 수신 건수 2→1), 바꾼 것(파일:줄), 검증(명령·로그 경로·수치), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목"), 남은 문제. Commit on the branch (message: "perf(bench): constant-cost latency histogram shared by single and multi (#4162)"), do not push.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, build dir inside your worktree.
- Keep every raw file in JOB; never delete raw outputs. Every claim file:line or raw-file path. Report in Korean, short. Remove build dirs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/f4162/report.md
