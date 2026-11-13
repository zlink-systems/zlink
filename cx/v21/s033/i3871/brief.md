Model gpt-6-luna, effort high: diagnosis of Java request-serial round-trip cost. Earlier job briefs are history, not instructions.

# Task: #3871 Java request-serial below 0.6 (1K 0.582, 4K 0.579)

Worktree /root/worktree/zlink-v21-s033/i3871 (detached at origin/main). Issue state: the 4K drop from 0.658 was shown not to come from a commit (A/B on 2026-11-05: main, main with #3836 reverted and the 11-02 commit all measure the same) — it is run-to-run/environment. What remains is the serial round-trip latency gap to the binding. Prior Java profiling: /root/jobs/fx/cx/v21/s033/jprof/, jcost/, jcost2/, jcount/, jsat/, jdr*/ reports — read their conclusions first and do not repeat a rejected direction.

## Do
1. Measure under the measure lock: Java request-serial 1024 and 4096, binding and Framework, 3 alternating rounds, same session, condition B (source CPU 0-7, target 8-15; see how earlier jobs ran it). Record msg/s, p50/p99 RTT, per-thread CPU (TID union) on both processes and context switches per request.
2. Break one serial round trip into stages on both sides (caller submit → wire → target receive → handler → reply → source receive → completion → caller resume) with the cheapest reliable method (existing trace/flow log, or timestamps from an async-profiler/JFR wall-clock capture). Show where Framework adds latency over the binding, each stage with its evidence.
3. Propose the change that removes the largest Framework-only stage, with the decision-owner table (file:line) and the spec clause that allows it. Do not implement.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- C++/Core compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, 25 GB cap, build dir inside your worktree; Framework C++ builds use the binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON), never /root/.cache/zlink/zlink-cpp/<ver>.
- Any benchmark/measurement runs under `flock -o /root/jobs/fx/cx/measure.lock` for the measurement only; build/analysis never holds it. Per-thread sums from before/after snapshots use the TID union (template /root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py). Keep every raw file; never delete raw outputs.
- Every claim file:line or raw-file path. Report in Korean, short. Remove build dirs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/i3871/report.md
