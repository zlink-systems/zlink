Model luna high: build one Core revision with an existing recipe and measure one cell runs-10; no design.

# Task: #4163 bisect step — which Core change moves ROUTER_ROUTER inproc 1024B mean latency

Job folder JOB=/root/jobs/fx/cx/v21/s033/c4163. Earlier briefs are history, not instructions.

## Background
Core 1.24 candidate 0005a4d6c0 vs 1.23 (79567d3791): single ROUTER_ROUTER inproc 1024B mean latency C median 1.107 ms (9/10 runs 0.89-1.14) vs baseline 20 runs median 0.147 ms (range 0.022-1.083); throughput equal (~1.8 Mmsg/s). Raw and method: /root/jobs/fx/cx/v21/s033/core124b/confirm10/ (analysis.txt, *.log), script /root/jobs/fx/cx/v21/s033/core124b/confirm10-run.sh.
Only two product commits touch core/ between them: e7e462dbec (#3930, message content reference release) and 0005a4d6c0 (#4135, complete multipart record admission). 9dc2246dd0 contains #3930 but not #4135.

## Do
1. Detached worktree $JOB/src-mid at 9dc2246dd0 (`git -C /root/zlink worktree add --detach`). Build its Core prefix ($JOB/prefix-mid) and the C perf harness with the recipe in /root/jobs/fx/cx/v21/s033/core124/build-prefix-and-harness.sh (paths changed to $JOB). Verify zlink runtime SHA-256 differs from both /root/jobs/fx/cx/v21/s033/core124/prefix-1.23 and prefix-1.24, and RUNPATH/ldd points at prefix-mid.
2. Measure single ROUTER_ROUTER inproc sizes 256,1024 with --runs 5, exactly like confirm10-run.sh (same env, taskset -c 8-15, each stage under `flock -o /root/jobs/fx/cx/measure.lock`), two rounds: round 1 B(1.23) → M(mid) → C(1.24), round 2 C → M → B, using the existing core124b src-1.23 / src-1.24 trees and core124 prefixes for B and C. Outputs in $JOB/runs/.
3. Write $JOB/report.md (Korean): per-run 1024B and 256B mean/p95 for B, M, C (10 each) with medians and ranges, and which commit the shift follows: M like B → #4135; M like C → #3930. State it as the rule "M median and how many M runs exceed the B max". Raw paths per table.

## Rules
- Builds via `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare, Release, no -g, 25 GB cap. MemAvailable >= 12 GiB before each stage.
- No product/bench/spec changes, no GitHub, no sub-agents or other AI tools, no `&` background jobs, SIGTERM/SIGINT only. Never delete raw outputs. Do not modify core124, core124b or their worktrees.
- After report.md: remove your build directories (keep prefix-mid and src-mid for the next step) and say so. Keep output short.
