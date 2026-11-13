Model gpt-6-luna, effort high: build + one measurement for a bench-only PR. Earlier job briefs are history, not instructions.

# Task: bench pacer PR evidence — build and 22k binding vs Framework (#1639)

Worktree /root/worktree/zlink-v21-s033/pacer, branch perf/1639-bench-pacer at 576b535089 (4b40d0f10f + 8cab98383e + main merge). The change paces C++ bench submissions (--target-rate / TARGET_RATE) and pins bench processes (SOURCE_CPUS / TARGET_CPUS) from framework/bench/grpc/cpp/run_local.sh. Earlier notes: /root/jobs/fx/cx/v21/s033/pacer/report.md and pr-notes.md. Do not commit, push or open a PR.

## Do
1. Build the C++ bench (framework/bench/grpc/cpp build.sh or the README's documented path) with /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3. Build the C++ binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON or the bench's equivalent); never use /root/.cache/zlink/zlink-cpp/<ver> (stale, predates #4112). Record the build command and rc. Stop if rc != 0 and report.
2. Verify the binaries exist and --target-rate -1 and nan are rejected before any measurement (run each once, record exit code). Do not probe with --help as a liveness check.
3. Measurement, under `flock -o /root/jobs/fx/cx/measure.lock` for the whole measurement only: request-backpressure 1024 B, TARGET_RATE=22000, SOURCE_CPUS=0-7, TARGET_CPUS=8-15, binding (zlink-cpp) and Framework (zlink-framework-cpp), 3 rounds alternating order. Per cell record achieved rate, p50/p99 latency, process CPU µs/msg for source and target (user+sys from /proc before/after), and per-thread sums using the TID union (template: /root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py — any thread seen in either snapshot counts; report how many TIDs were only in one snapshot). Also one unpaced (TARGET_RATE unset) round of each to show rate 0 keeps the old path.
4. Keep every raw output under /root/jobs/fx/cx/v21/s033/pacer2/measure/. Never delete raw files.
5. Remove the bench build dir at the end if it is inside the worktree and larger than 2 GB; keep the worktree.

## Rules
- No GitHub, no sub-agents or other AI tools. Only one measurement process at a time.
- Report in Korean, short, every number with its raw file path: build rc, rejection checks, a table per round (binding vs Framework at 22k: achieved rate, p50/p99, source/target µs/msg, Framework÷binding CPU ratio), the unpaced check. State that paced-cell latency includes pacer sleep (condition value) and that the shape differs from the earlier xio pacer, so it is not a 1:1 comparison.

## Output: /root/jobs/fx/cx/v21/s033/pacer2/report.md
