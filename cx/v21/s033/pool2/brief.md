Model gpt-6-luna, effort high: measurement experiment. Earlier job briefs are history, not instructions.

# Task: is the send-saturation drop with a 2-worker completion pool real, and where does work queue up? (#1639)

## Background
- Previous job: /root/jobs/fx/cx/v21/s033/pool/ (report.md, run-experiment.sh, measure/, copied harness). Same session 3 rounds: send-saturation 1 KiB msg/s base 179.2k–184.6k, pool8 182.0k–185.5k, pool4 172.6k/177.1k/177.0k, pool2 175.1k/176.8k/184.0k. 22k and Spot were fine for pool2.
- Measurement branch: worktree /root/worktree/zlink-v21-s033/pool, branch perf/1639-pool-exp, local commit 17c5a80757 (env ZLINK_PERF_COMPLETION_POOL sets completion_dispatcher_t worker count in framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp; workers named zl-cmpl-N). Build dir .pool-build exists. Base build: /root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build (do not rebuild).
- Boost.Asio: post wakes one idle worker (scheduler.ipp:357-378, 674-685), do_run_one wakes another when the queue is non-empty (scheduler.ipp:492-495). Header: /root/.cache/zlink/vcpkg/packages/boost-asio_x64-linux/include/boost/asio/detail/impl/scheduler.ipp.

## Measurement-only code (second local commit on perf/1639-pool-exp, never pushed)
Add a queue-depth measurement to completion_dispatcher_t: count completions posted but not yet started (increment at post, decrement when the handler begins). Only when env `ZLINK_PERF_COMPLETION_QUEUE_LOG=<path>` is set, record at handler start the depth seen and write a histogram (depth → count) plus max at process exit or dispatcher close. When the env is unset the cost must be one relaxed atomic add/sub at most; say so with file:line. Rebuild .pool-build through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), verify binaries newer than the source and contain the env strings.

## Measure
- Variants: base (psmeas-base build), pool8, pool3, pool2 (pool build with env for source and target; pool8 is the named, queue-logged stand-in for base's pool, since base has no names or log).
- Only 1 KiB Framework send-saturation. 5 rounds, every variant back to back in each round, rotate the order each round. Same CPU pinning as before (source 0-7, target 8-15), psched8 Python supervisor launch as in /root/jobs/fx/cx/v21/s033/pool.
- During each send-saturation run, take before/after /proc thread snapshots of both processes over a fixed window inside the steady part (as the 22k collector does) and enable the queue log for pool variants (both processes, separate files).
- Report per run: msg/s; each zl-cmpl thread's CPU utilisation (% of the window) and voluntary wakes/msg; source and target process total µs/msg; queue depth histogram summary (p50/p99/max) for source and target; effective GHz of the I/O threads if available.
- Thread counting rule: union of TIDs. Threads only in the after snapshot add their full after value; report the count of threads only in the before snapshot. Never use the intersection. Fix the analyzer you copy (analyze-22k.py and any send-saturation collector) to this rule, and leave the fixed copy at /root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py.
- Judgement inputs (do not decide): pool2 and pool3 median vs base's own 5-run range; if below, identify where completions queue (which stage, file:line), using the queue log and per-thread utilisation.

## Rules
- Every measurement under `flock -o /root/jobs/fx/cx/measure.lock`, one at a time.
- No test or spec changes, no push, no GitHub, no PRs, no sub-agents or other AI tools. Do not delete raw outputs.
- Open the raw files before writing any number. Progress to progress.md; write report.md once at the end.
- Time box about 2.5 hours.

## Output: /root/jobs/fx/cx/v21/s033/pool2/report.md (Korean, short tables with raw paths)
