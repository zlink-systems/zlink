Model gpt-6-luna, effort high: measurement job. Earlier job briefs are history, not instructions.

# Task: base Spot stage timing, and why base app-worker µs/msg differs 2× between two sessions (#1639)

## Background
- psched8 (/root/jobs/fx/cx/v21/s033/psched8) measured Spot stage timing on the chained build only: HTTP entry → gate submit p50 1,158 / p99 6,197 µs dominates. Harness: psched8/run-stage.sh, prepare-probes-queue.py, stage-collector.py, analyze-stages.py, run-B.sh, processes.py. The PM asks: is that span large on base too? If yes it becomes a separate issue.
- Base build: /root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build (HEAD 168c679590, Release -O3, already built). Do not rebuild it unless probes cannot attach; if a rebuild is needed, use a separate build dir under this job via /root/jobs/fx/cx/cpp-build-slot.sh with the Release preset and a 25 GB disk cap.
- 22k Framework request-backpressure, same base build and same rate (~32.1k msg/s): psmeas session (/root/jobs/fx/cx/v21/s033/psmeas, shell launch) gave app-worker 22.1–22.5 µs/msg and all bench_cpp_frame target threads 37.9 µs/msg; psched8 session (Python process supervisor launch, processes.py) gave app-worker 11.5–11.8 and 27.7. Both summaries: */measure/*/22k/summary-22k.json. Explain the gap.

## Do
1. Base Spot stage timing: copy psched8's stage harness into this job folder, point it at the base build, run once (same 30 s warm-up/duration, PM=1, same probes). Report the same 4 spans (p50/p99) next to psched8's chained values, with raw paths. Remove your perf probes at the end.
2. Base gap: diff the psmeas and psched8 22k runners (run scripts, launch method, env, CPU pinning, thread-role assignment, which processes are counted). Then measure the base 22k Framework cell 2× with the psmeas launch and 2× with the psched8 launch, alternating (A B A B). Report app-worker, receive, all-target-thread µs/msg, wakes/msg, sendto/msg, GHz per run, with raw paths, and name the cause if found (file:line or env). Mark anything inferred as 가설.

## Rules
- Every measurement under `flock -o /root/jobs/fx/cx/measure.lock`, one at a time. Source CPU 0-7, target CPU 8-15.
- No source, test or spec changes, no commits, no GitHub, no sub-agents or other AI tools. Do not delete raw outputs.
- Open the raw files before writing any number. Write progress to progress.md; write report.md only once at the end.
- Time box about 2 hours.

## Output: /root/jobs/fx/cx/v21/s033/bstage/report.md (Korean, short tables)
