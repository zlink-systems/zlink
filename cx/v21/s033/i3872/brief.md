Model gpt-6-luna, effort high: measurement of Node cells after #3894. Earlier job briefs are history, not instructions.

# Task: #3872 Node send-saturation 4K and borderline cells — re-measure on main

Worktree /root/worktree/zlink-v21-s033/i3872 (detached at origin/main). Issue state: #3894 (dispatch worker duplicate await removal) raised Node request-serial 1024 to 0.749, 4096 to 0.827, request-backpressure 1024 to 0.718 (4-round medians). Remaining: send-saturation 4096 (was 0.597, later 0.62). Prior Node work: /root/jobs/fx/cx/v21/s033/nprof/, nfix/, nserial/, nsend/, nseg*/ reports.

## Do
1. Build the Node binding and Framework as the bench README / earlier Node jobs do (no C++ build needed beyond what the Node binding uses).
2. Under the measure lock: Node send-saturation 1024 and 4096, request-serial 1024/4096 and request-backpressure 1024, binding and Framework, 3 alternating rounds, condition B, same session. Report Framework/binding per cell with the round spread.
3. If send-saturation 4096 is below 0.6 or within the spread of it: per-thread CPU (TID union) on both processes and an event-loop profile (--cpu-prof or perf with --perf-basic-prof) of one Framework 4K run; name the largest Framework-only cost with file:line and propose a fix (do not implement).

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- C++/Core compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, 25 GB cap, build dir inside your worktree; Framework C++ builds use the binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON), never /root/.cache/zlink/zlink-cpp/<ver>.
- Any benchmark/measurement runs under `flock -o /root/jobs/fx/cx/measure.lock` for the measurement only; build/analysis never holds it. Per-thread sums from before/after snapshots use the TID union (template /root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py). Keep every raw file; never delete raw outputs.
- Every claim file:line or raw-file path. Report in Korean, short. Remove build dirs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/i3872/report.md
