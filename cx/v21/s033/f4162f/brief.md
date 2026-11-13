Model gpt-6-luna, effort high: find what the constant PUBSUB in-flight floor is, fix the count if it is a counting artifact, then re-measure. Earlier job briefs are history, not instructions.

# Task: #4162 PUBSUB catch-up — constant in-flight floor of ~19 after the sender-anchor fix

Worktree /root/worktree/zlink-v21-s033/f4162b (HEAD ec298149f7, the sender-anchor fix). Commit on it; do not push.
Read first: /root/jobs/fx/cx/v21/s033/f4162e/report.md and its raw/ (final-pubsub-01..05.log lines 55-66), doc/perf/PERF_POLICY.md lines 84-93 (catch-up definition; do not change it), bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp, perf_multi_pubsub_server.cpp:244-300, common/perf_multi_measurement.hpp.

## Finding (supervisor)
- f4162e: all 5 runs invalid. PUBSUB_CATCHUP_PROGRESS shows in-flight = 18-22 at every 1 s tick for 10 s while received grows at exactly the target rate (e.g. 01.log: 10743 → 21497 → ... at target 10753.3). The bound is max(1, 10753 × 1 ms) ≈ 10.75.
- A deficit that is CONSTANT over time does not look like latency (a lag would also show in latency p50). Two candidates:
  (a) counting artifact: `smallest_received` takes the minimum over 100 subscribers, and the epoch comes from the first paced header any subscriber got. A subscriber that attached/started counting a few messages later never received those (PUBSUB does not replay), so it carries a permanent deficit equal to its start skew.
  (b) real receive lag of ~1.7 ms at the slowest subscriber.
- The sender spins at sub-ms intervals (wait_ms = 0 below 1 ms, server.cpp:283-289), so sender lateness is not the cause; confirm from late_sends_gt_1ms.

## Do
1. Diagnose with data, no guess: per subscriber, record the seq of its first counted paced header and its count. Show, for the subscriber that is the minimum, (first_seq − global first paced seq) versus the in-flight floor, and a few latency samples (now − sent_ts) at the same ticks. Raw lines in the run log. One short diagnostic run (PUBSUB tcp 256 B, 5 s) is enough for this step, under the measurement conditions below.
2. If (a): count each subscriber from its own first paced seq (a message the subscriber could never have received because it was sent before it started counting is not in flight), keeping epoch = sender's first paced sent_ts and the bound unchanged. Drops AFTER a subscriber started must still count as in flight. Use header.seq; no new protocol field. Unit case: a subscriber starting 19 messages late and then receiving at rate catches up; one with a constant 150 ms lag (from f4162e test_perf_multi_metrics.cpp:210-232) still does not; a subscriber that loses messages after starting does not.
   If (b): change nothing in the rule; report the evidence (per-tick latency samples) and stop after step 3.
3. Re-measure PUBSUB tcp 256 B ×5 with /root/jobs/fx/cx/v21/s033/f4162e/run-pubsub-five.sh copied to f4162f (paths changed to f4162f), same conditions: `flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE for the runs only, cpu0-15 governor performance inside the lock and powersave restored on exit, uptime/load, free memory and cpu0-15 scaling_cur_freq before/after each run. Waiting for the lock or the measure gate is normal; wait, do not give up. Report per run: catch-up s, valid/invalid, mean/p50/p95, throughput, in-flight at catch-up, paced_received_rate, and the consistency check (in_flight_at_catch_up / target_rate vs post-catch-up p50, within an order of magnitude or explained).
4. Compile through /root/jobs/fx/cx/cpp-build-slot.sh only (-j3). Keep every raw file in /root/jobs/fx/cx/v21/s033/f4162f (never delete raw outputs). Build outputs inside the worktree; remove at the end.

## Rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Do not loosen the 1 ms bound, the 0.5 s hold, or the deadline.
- Every claim file:line or raw path. Report in Korean, short: 진단(데이터와 raw 경로), 원인(file:line), 바꾼 것, 수정 전 실패한 시험, 5회 표, 일관성 점검, 측정 조건, 리팩토링 점검 한 줄, 남은 문제.

## Output: /root/jobs/fx/cx/v21/s033/f4162f/report.md
