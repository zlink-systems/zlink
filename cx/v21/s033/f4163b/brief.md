Model sol high: one decisive A/B experiment with two Core builds; reuse the previous job's recipes.

# Task: #4163 — semantic change or code layout? (PM-approved experiment, criteria fixed in advance)

Job folder JOB=/root/jobs/fx/cx/v21/s033/f4163b. Earlier briefs are history, not instructions. Read /root/jobs/fx/cx/v21/s033/f4163/report.md first and reuse its scripts (build.sh, instrument.py, probe-run.sh, measure.sh, analyze-*.py, stage-preflight.py, manifest.py) with paths changed to $JOB. Do not modify f4163's files, prefixes or its branch fix/4163-router-inproc-queue-depth (keep that branch as is).

## Background
single ROUTER_ROUTER inproc 1024B mean latency is bimodal (~0.05-0.3 ms vs ~1.0-1.15 ms) in every Core version. "High" = mean >= 0.9 ms. Pooled: candidate C (0005a4d6c0, #4135) 20/30 high, 1.23 8/30, mid 9dc2246dd0 3/10. Sampled inproc FIFO depth median M 90 vs C 1,610 records. #4135's empty-queue exception never applies to these 1,152-byte records (HWM 4,096,000).

## Builds (each from 0005a4d6c0 in your own detached worktrees under $JOB, Core via f4163/build.sh recipe, Release -O3 LTO as there, no -g)
- R (caller revert): candidate with only the callers changed back to the pre-#4135 mode: socket_runtime.hpp:846 and socket_send_complete.cpp:388,395 and socket_request_reply_runtime_io.cpp:1461,1465 pass the mode the code had before #4135 (pipe_write_incremental / the old boolean behaviour; read 9dc2246dd0 to match exactly). Everything else identical to C.
- L (layout only): candidate source unchanged; change only code placement, e.g. add `-falign-functions=64` (or 32 if 64 equals the default layout) to CMAKE_CXX_FLAGS for the Core build. Record the flag and verify the libzlink.so SHA differs from C.
- Also keep C itself (shared prefix /root/jobs/fx/cx/v21/s033/core124/prefix-1.24) as control.

## Measure
1. runs-10 each for C, R, L: single ROUTER_ROUTER inproc 256,1024, two rounds of --runs 5, interleaved C→R→L then L→R→C, exactly as f4163/measure.sh (flock -o measure.lock, taskset -c 8-15, same env).
2. Queue depth: build probe variants of R and L with the same temporary counter as f4163 (instrument.py) and run probe-run.sh 5x each for R and L (C probe values exist in f4163; rerun C probe 5x too for same-session comparison). Report per-run mean latency + sampled average depth.

## Verdict rule (PM, fixed in advance)
- R high ratio near B/M (~3/10) → semantic change: name the decision.
- L high ratio clearly different from C in the same session → layout effect.
- Neither → state "neither".
Report the counts per build, depth medians, and which rule applies. No other fix work.

## Output and rules
$JOB/report.md (Korean), raw paths per table, short. Builds via `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare; 25 GB cap; MemAvailable >= 12 GiB before each stage. No push/PR/GitHub/spec edits, no sub-agents or other AI tools, no `&` background jobs, SIGTERM only, never delete raw outputs. Remove build directories and your worktrees at the end; keep prefixes.
