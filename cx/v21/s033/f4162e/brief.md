Model gpt-6-luna, effort high: fix the PUBSUB catch-up schedule anchor found by the supervisor, then re-measure. Earlier job briefs are history, not instructions.

# Task: #4162 PUBSUB catch-up — the schedule is anchored at receive time, so a constant lag looks caught up

Worktree /root/worktree/zlink-v21-s033/f4162b (HEAD 4cfb084fa8). Commit on it; do not push.
Read first: /root/jobs/fx/cx/v21/s033/f4162d/report.md and its raw/, doc/perf/PERF_POLICY.md lines 80-94 (catch-up definition; do not change it).

## Finding (supervisor)
- bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:231-238 sets `paced_epoch_ns = now_ns()` when the LAST subscriber receives its FIRST paced message, and zeroes every counter. :264-268 then compute scheduled = target_rate × (now − that epoch). Any lag the subscribers already had when their first paced message arrived is dropped from the in-flight estimate.
- Evidence it is wrong: f4162d raw/final-pubsub-04.log is "caught up" (0.76 s, in-flight ≤ 11 at 11,861 msg/s, i.e. ≤ ~1 ms of traffic) yet latency p50 = 144.7 ms, p95 255.9 ms. By Little's law a 145 ms lag at 11.8k msg/s is ~1,700 messages in flight. raw/client-pubsub-diag-06.txt shows the gap floor ~17 with spikes to 1,606.
- PERF_POLICY says "목표 송신 일정상 전송될 메시지 수": the schedule belongs to the SENDER's paced start, not to the receiver's first receive.

## Do
1. Anchor the schedule at the sender's paced start (e.g. the send timestamp the paced messages already carry in the header — same host, same clock as the latency samples — or a paced sequence number if one exists; pick the one that needs no new protocol field, say which, file:line). Do not reset counters at a receive-side moment. Keep the catch-up rule and thresholds unchanged.
2. Unit case that fails on 4cfb084fa8: subscribers that start receiving paced messages 150 ms after the sender's paced start and then receive exactly at the target rate (constant 150 ms lag) must NOT catch up; a subscriber with no lag must catch up.
3. Consistency check in the report for every valid run: in_flight_at_catch_up / target_rate versus the post-catch-up p50. They must agree within an order of magnitude; if not, explain.
4. Pass PUBSUB_CATCHUP_DIAG / PUBSUB_CATCHUP_PROGRESS client lines into the raw run log (runner currently drops them), file:line.
5. Re-measure PUBSUB tcp 256 B ×5 with the f4162d command (raw/run-pubsub-five.sh or its final variant) under the approved conditions: `flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE for the runs only, cpu0-15 governor performance inside the lock and powersave restored on exit (trap; template /root/jobs/fx/cx/v21/s033/f4162/aa-clean.sh), uptime/load, free memory and cpu0-15 scaling_cur_freq before/after each run. Waiting for the lock or the measure gate is normal; wait, do not give up. Report per run: catch-up s, valid/invalid, mean/p50/p95, throughput, in-flight at catch-up, paced_received_rate. If runs still never catch up, that may be the true result: show the progress lines and say so, do not loosen anything.
6. Compile through /root/jobs/fx/cx/cpp-build-slot.sh only (-j3). Keep every raw file in /root/jobs/fx/cx/v21/s033/f4162e (never delete raw outputs). Build outputs inside the worktree; remove at the end.

## Rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Every claim file:line or raw path. Report in Korean, short: 원인(file:line), 바꾼 것, 수정 전 실패한 시험, 5회 표, 일관성 점검, 측정 조건, 리팩토링 점검 한 줄, 남은 문제.

## Output: /root/jobs/fx/cx/v21/s033/f4162e/report.md
