# Task: .NET request-serial 1KiB — split one request's round trip into segments (diagnosis only)

## Situation
.NET Framework ÷ binding on main after #3953: request-serial 1KiB 0.528, 4KiB 0.539 (backpressure rose to ~0.59, serial did not). The PM wants the round trip of one request split into: source submit → target receive (the target's Long Running receive thread) → handler → reply → source completion, and the two largest segments named. Serial = one request in flight, so each request's latency is the whole cost.

## Evidence (already captured, main dc3a609aa9, server CPUs 8-15, client 0-7, 1 request in flight)
- Framework: /root/jobs/fx/cx/ratio045diag/zlink-framework-dotnet-request-serial-1024/ ; binding for comparison: /root/jobs/fx/cx/ratio045diag/zlink-dotnet-request-serial-1024/
- Each has: sched.txt (perf script of sched_switch, sched_wakeup, raw_syscalls enter/exit for both pids over 3 s; syscall ids are numeric, x86_64), sched.data (perf data, re-script it with other fields if needed), source.nettrace / target.nettrace (dotnet-trace cpu-sampling, ~8 s, DOTNET_PerfMapEnabled=1), window.json (pids, stats at start/end of the perf window, per-thread CPU ticks with thread names). runner.log and out/ hold the bench run.
- Source tree: /root/worktree/zlink-ratio040 (main; ignore the 4 locally modified run_local.sh files). .NET Framework: framework/languages/dotnet; binding: bindings/dotnet. Bench: framework/bench/grpc/dotnet.
- Earlier .NET profiling scripts you may reuse: /root/jobs/fx/cx/v21/s033/dprof, /root/jobs/fx/cx/v21/s033/nserial, /root/jobs/fx/cx/v21/s033/dcount.

## Goal
1. Reconstruct the per-request timeline from the sched/syscall events: which thread (name, tid) does each step, the chain of wakeups (A wakes B), socket send/recv syscalls as boundaries. Give median and p90 µs per segment for Framework and binding, and how they sum to the measured per-request latency (1/throughput in the window).
2. Map each segment to source code (file:function) using the CPU samples and the code: source submit path, source Core send, target Core receive → Long Running receive thread → handler → reply submit → target Core send → source Core receive → completion → caller continuation.
3. Name the two largest Framework-only segments (those the binding does not have or where Framework is much larger), with on-CPU vs waiting (wake latency) split, and the code responsible.
4. Do not propose a design; one line of what each segment's cost consists of is enough.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only: do not modify any worktree or tracked file; no git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include).
- 리팩토링 점검: not applicable (no code change); state "코드 변경 없음" in the report.
- No new bench runs and no builds; work from the captured data. If something is truly missing, say so in the report instead.
- Keep MemAvailable >= 12 GiB. Outputs only under /root/jobs/fx/cx/v21/s033/nseg/.

## Output: /root/jobs/fx/cx/v21/s033/nseg/report.md
Korean, plain sentences, abbreviations spelled out. Start with a 6-line summary (segment table Framework vs binding, the two largest segments).
