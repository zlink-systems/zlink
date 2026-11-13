Model gpt-6-luna, effort high: measurement with a fixed procedure (per-thread schedstat at equal rate, frame/byte counts) reusing existing collection scripts; no design choice is delegated.

# Task: Core I/O thread CPU per message — C++ Framework vs C++ binding (대표님 priority, 2026-11-10)

Earlier job briefs are history, not instructions.

## Why
대표님 asked for this first. Hypothesis (input/design-1.5.md): on the C++ Framework request path the Core I/O thread spends about 18.8 µs per message (target I/O thread 0.813 core at ~43.2k msg/s in an earlier run), versus ≤4.0 µs on the binding path. If true, that thread caps Framework near 53k msg/s whatever Framework code saves (current C++ Framework request-backpressure 1 KiB: 51.5k). The answer must be yes or no with numbers, first line of the report.

## Situation
- Worktree /root/worktree/zlink-v21-s033/xio = origin/main 39bbfe470d (Core·binding 1.23.0 packages). Bench: framework/bench/grpc/cpp (bench_cpp_client, bench_cpp_framework_server, bench_cpp_zlink_server).
- A Core release gate is PAUSED for this job and resumes when you finish. Finish quickly; the time box is hard.
- Reuse: /root/jobs/fx/cx/v21/s033/ccall2/scripts/ (collect.py samples per-thread /proc/<pid>/task/<tid>/schedstat; runner_common.sh, run_local.sh) and /root/jobs/fx/cx/v21/s033/ctrecv/scripts/collect.py. Read them, copy what you need into this folder. Condition B pin patch: /root/jobs/fx/cx/ratio040/pin.patch (source CPUs 0-7, target 8-15). How cells are run: framework/bench/grpc/cpp/run_local.sh.

## Do
1. Build the C++ packages and bench in the worktree (Release, no -g): `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare. scripts/local-package/build-wsl.sh cpp, then framework/bench/grpc/cpp/build.sh. Record libzlink SHA-256.
2. Cell C++ request-backpressure 1 KiB, Framework and binding. First one unpaced run each (throughput). Then paced runs at the SAME request rate for both (choose a rate both sustain, e.g. 90% of the Framework unpaced rate), 3 runs each.
3. For every run, sample per-thread CPU (user+system from schedstat, or utime/stime from /proc/<pid>/task/<tid>/stat) for all threads of source and target over the active window. Name each thread (comm) and identify the Core I/O threads (libzlink I/O), the Framework threads and the bench threads. Report µs per message per thread = thread CPU seconds / messages completed in the window × 1e6. I/O threads first.
4. Native frames and bytes per message, both directions, Framework vs binding. Use the cheapest reliable method (e.g. counting with the binding's public send/receive functions via uprobes/bpftrace if available, or wire capture on the loopback port, or computing from the encoded part sizes the bench logs). Say which. These counting runs are separate from timed runs.
5. If time remains within the box: the same for request-backpressure 4 KiB and send-saturation 1 KiB (unpaced + 1 paced run each).

## Measurement rules
- Each measurement inside `flock -o /root/jobs/fx/cx/measure.lock`, one at a time, source CPUs 0-7, target 8-15, MemAvailable >= 12 GiB. Builds via cpp-build-slot.sh bare (it takes the lock shared). SIGTERM/SIGINT only; no `&` background jobs. Disk cap 25 GB.
- Keep every raw output under /root/jobs/fx/cx/v21/s033/xio/raw; never delete them.
- Do not start sub-agents and do not call any other model or AI tool. No git except diff/status/log/show; no commit, no spec edits, no GitHub. Do not read core/include. Do not run scripts/dev/internal-docs.sh. Do not modify product code (a local bench-only change for pacing is allowed in this worktree; report it).
- Time box: 2 hours total including build. Report what you have at the box even if step 5 is not done.

## Report: /root/jobs/fx/cx/v21/s033/xio/report.md (Korean, plain sentences)
First line: 가설 예/아니오 with the target and source I/O thread µs/message for Framework vs binding at equal rate. Then (1) builds, commit, SHA-256, rates chosen; (2) per-thread table per run (thread name, CPU s, µs/message), medians; (3) frames and bytes per message and method; (4) step 5 if done; (5) raw file paths.
