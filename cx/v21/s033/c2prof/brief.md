Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: C++ ladder L2 — profile what L2 still costs per message on the PM's L2 fix branch, and hand the PM the top remaining cost items with file:line and sample share. PROFILE + DIAGNOSIS ONLY: no product code changes (the PM implements the fix).

Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh or any git fetch/pull, never delete raw files. Do NOT take /root/jobs/fx/cx/measure.lock (profiles only). Never `kill -CONT` processes in state T (the measurement guard paused them; wait). Do not modify the worktree below or its build dir (read-only use of the built binaries).

## Numbers (supervisor quick compare 2026-11-14 01:34–01:38 UTC, 1 s warmup / 5 s active / 3 s gap)
Branch exp/4350-cpp-fixes 70158ead2a = PM L2 fix perf/4352-cpp-l2-envelope b5afcf4c55 + c4json + C++ ladder bench. L1→L2 loss: send 1K 12.3% (481,208 → 421,889), send 4K 2.0%, serial 1K 18.8% (median of 3), serial 4K −7.8%, backpressure 1K 17.6% (129,748 → 106,921), backpressure 4K 21.5% (137,157 → 107,612). Raw: /root/jobs/fx/cx/v21/s033/lfx-cpp/raw/measurements.jsonl. Budget 7%. The PM's fix removed copies counted by reading code (send copies 12→3, completion allocations 4→0) but throughput did not improve, so the real cost is elsewhere.
Prior C++ L2 analysis: /root/jobs/fx/cx/v21/s033/c2l2/brief.md (task text) and the PM's report summary: "이미 소유한 이름의 복사와 completion 문자열 소유 제거, 메시지당 송신 복사 12→3·completion 할당 4→0·왕복 할당 25→20". Older ladder profile: /root/jobs/fx/cx/v21/s033/cladder/report.md and its analyze-profiles.sh (thread CPU method). .NET L2 passes at 2–5%: /root/jobs/fx/cx/v21/s033/d2l2/report.md has the .NET per-message table to compare against.

## Binaries (already built, use as is)
Worktree /root/worktree/zlink-v21-s033/cfx (HEAD 70158ead2a, clean), build dir /root/worktree/zlink-v21-s033/cfx/build-cfx (Release + debug info as configured; check `readelf -S` for .debug_* and say if symbols are missing). Runner tools that launch L1/L2 cells: /root/jobs/fx/cx/v21/s033/lfx-cpp/tools/ (ZLINK_CPP_LADDER_LEVEL=1 or 2; cpp-cell-runner.sh, runner_common.sh). Copy them to /root/jobs/fx/cx/v21/s033/c2prof/tools and change only output paths and CPU pinning.
Allocation counter: /root/jobs/fx/cx/v21/s033/c4dec/tools/malloc-counter.c (LD_PRELOAD; build it into c2prof/tools with gcc -O2 -shared -fPIC -ldl). Earlier usage: c4dec/tools/run-probes.py.

## Steps
1. Cells: request-backpressure 1K and send-saturation 1K, L1 and L2, source and target. Pin every process `taskset -c 6-7,14-15` (source and target share these 4 CPUs; note it in the report), warmup 1 s, active 5 s, each process ≤ 20 s hard timeout. Check errors 0 and submitted = completed = received for every run.
2. `perf record -F 2999 -g --call-graph dwarf` (or fp if dwarf is too heavy) on source and target separately during the active window, 2 repeats per (level, cell, side). Report per thread (thread names) the self and inclusive sample share of the top 25 symbols, L1 vs L2 side by side, and the L2−L1 difference in CPU µs per message (`perf stat -e task-clock` per thread over the same window ÷ messages completed in that window).
3. Allocation count per message with the LD_PRELOAD counter (malloc/calloc/realloc/free and operator new/delete if the counter covers them), L1 vs L2, source and target, over a fixed active window ÷ messages. Attribute the L2-only allocations to call sites (perf probe on malloc with -g, or the counter's backtrace mode if present; otherwise `ltrace -c` on a short run).
4. Map every L2-added cost to file:line in framework/languages/cpp/framework/src (envelope_codec.cpp, client_call_codec.cpp, channel_reply_writer.cpp, channel_runtime.cpp, the raw mesh node owner and dispatch paths) and the bench ladder files (framework/bench/grpc/cpp/ladder/ladder_wire.hpp, ladder_driver.inc) — separate production cost from bench-only cost. Compare each item with .NET's equivalent from d2l2/report.md.
5. Also check for non-codec effects of L2: extra thread wake-ups/context switches (`perf stat -e context-switches,cpu-migrations` per process), lock contention (`perf lock` or futex counts via `perf trace -s` short run), and whether the reply path waits on anything new at L2.

## Output
/root/jobs/fx/cx/v21/s033/c2prof/report.md (Korean, short, for the PM):
- table 1: top remaining L2 cost items ranked by L2−L1 CPU µs/message — item, file:line, sample share L1 → L2 (source/target), allocations/message, .NET equivalent;
- table 2: per-thread CPU µs/message L1 vs L2, context switches/message;
- what is production cost vs bench cost; which items the PM's b5afcf4c55 already touched and why they did not move throughput;
- raw paths for every number. Raw under /root/jobs/fx/cx/v21/s033/c2prof/raw. Time cap 2 h.
