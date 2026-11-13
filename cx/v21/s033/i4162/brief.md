Model gpt-6.1-sol, effort high: read-only diagnosis of a bimodal latency (ambiguous cause across Core/bench/environment → middle tier), with small repro runs; no code changes. Earlier job briefs are history, not instructions.

# Task: #4162 Core gate latency cells split into two value groups between runs

Worktree /root/worktree/zlink-v21-s033/i4162 (detached at origin/main). Issue summary: repeated runs of the same Core build land in two separate modes, so the release-gate A/A comparison already fails (single 105/840, multi 129/560). Examples: PUBSUB tcp 256B latency is either 2–3 ms or 300–900 ms per run; DEALER_DEALER tls/ws 64B p95 swings 41–55×; ROUTER_ROUTER_REQREP tls 64B; ROUTER_ROUTER inproc 1024B mean latency shows two modes on 1.23.0, the 1.24.0 candidate and the candidate with #4135 reverted, and queue depth follows the run, not the build. Prior data: /root/jobs/fx/cx/v21/s033/core124r/ (modes.txt), core124/, core124b/, c4163/, f4163/, f4163b/, s4163/ reports.

## Do
1. From the raw data and the bench source (bindings/c/perf/, core/perf or wherever the latency benches live — find them), explain what decides which mode a run lands in: e.g. a queue that fills during warm-up and never drains (HWM, send-ahead window), Nagle/delayed-ACK on TCP, poller/io-thread placement (#3541 is the io thread round-robin issue — note any link), timer granularity, or the bench's own pacing. Give file:line for the mechanism and the evidence that separates the hypotheses.
2. Small repro under the measure lock: the cheapest cell (e.g. ROUTER_ROUTER inproc 1024B or PUBSUB tcp 256B) 20 runs of the same binary, recording the mode and the variable you think decides it (queue depth at start of measurement, io thread of each session, etc.). Then one run with that variable forced, showing the mode follows it.
3. Classify: bench defect (the bench measures a warm-up artefact) vs Core defect vs environment, and change class A/B/C/D (supervisor guide §4). Give the decision-owner table: for each decision involved, the spec clause that owns it, every place that decides it now (file:line), and the single owner a fix would use, with decision-site counts before/after. Do NOT change any code (product or bench); the supervisor approves a design before implementation.
4. Report sections: 결과, 판정 위치(전/후 수), 검증(명령·로그 경로·핵심 수치), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목" — in a diagnosis, 고친 항목 is 0), 남은 문제.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- C++/Core compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, 25 GB cap, build dir inside your worktree; Framework C++ builds use the binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON), never /root/.cache/zlink/zlink-cpp/<ver>.
- Any benchmark/measurement runs under `flock -o /root/jobs/fx/cx/measure.lock` for the measurement only; build/analysis never holds it. Per-thread sums from before/after snapshots use the TID union (template /root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py). Keep every raw file; never delete raw outputs.
- Every claim file:line or raw-file path. Report in Korean, short. Remove build dirs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/i4162/report.md
