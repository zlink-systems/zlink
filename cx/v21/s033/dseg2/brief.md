# Task: .NET request-serial 1KiB — time the Core↔managed edges per request and explain B2→B3 (measurement-only build, no commit)

## Situation
Read /root/jobs/fx/cx/v21/s033/dseg/report.md (your previous job's result; its instrumentation is still in /root/worktree/zlink-dseg:
ZLinkDsegTrace.cs, DsegBenchTrace.cs and the edits listed there) and /root/jobs/fx/cx/v21/s033/nseg2/report.md (syscall-boundary method,
scripts in /root/jobs/fx/cx/v21/s033/nseg2/). Managed spans are small (B1→B4 22.5 µs, D1→D4 11.8 µs p50), but the uninstrumented round
trip is ~80 µs longer than the binding's. The rest lies at four edges the trace does not cover. The PM asked for:
1. Per request: target Core receive syscall (recvfrom on the target Core IO thread) → B1; B4 → target Core send syscall (sendto);
   source Core receive syscall → D1 (including binding CompletionOwner.Drain and the `await physical` resume; add a marker where the
   binding completion is drained / the task is completed if the managed side allows it without touching bindings — otherwise state
   what the gap contains); D4 → source Core send syscall. p50/p90/mean each.
2. In the same run: what the Long Running thread does between B2 and B3 (14.4 µs p50 on the same thread): its syscall sequence
   (poll / recv retries returning EAGAIN, eventfd, futex) and where in managed code it is (add markers inside the receive loop:
   e.g. each DrainRawSocket/receive attempt start/end, ReceiveUntilHandoff exit, other owner drains, receive high-water/management
   steps — whatever the code actually does there). Report the breakdown of B2→B3 by those markers.

## Method
- Same worktree /root/worktree/zlink-dseg. Keep the existing env-var gate. Add only the extra markers needed.
- Clock: Stopwatch.GetTimestamp on Linux is CLOCK_MONOTONIC; record perf with `-k CLOCK_MONOTONIC` (perf record -k mono) so both
  share one clock. Record raw_syscalls:sys_enter/sys_exit + sched_switch + sched_wakeup for both pids for ~3 s in the active window
  only (keep the window short to limit lost events; report lost-event counts). Correlate by time, using the managed sequence numbers.
- Build: from framework/bench/grpc `flock -s /root/jobs/fx/cx/measure.lock taskset -c 0-7 bash dotnet/build.sh`.
  Run: `flock -o /root/jobs/fx/cx/measure.lock env ZLINK_DSEG_TRACE=... IMPLEMENTATIONS=zlink-framework-dotnet PATTERNS=request-serial
  PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=20 DURATION_SECONDS=15 SKIP_BUILD=1 RUN_STAMP=dseg2 OUTPUT_DIR=<job dir>/out bash dotnet/run_local.sh`,
  attaching perf after the source reports phase active (see /root/jobs/fx/cx/ratio045diag/capture.py for how to wait for it).
  Also report this run's throughput.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not modify bindings/.
- 리팩토링 점검: instrumentation is throwaway; list every file you edited.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- Outputs under /root/jobs/fx/cx/v21/s033/dseg2/.

## Output: /root/jobs/fx/cx/v21/s033/dseg2/report.md
Korean, plain sentences. First table: the full round trip as consecutive segments (source send syscall → target recv syscall → B1 → B2 → B3 → B4
→ target send syscall → source recv syscall → D1 → D2 → D3 → D4 → next source send syscall) with p50/p90 and thread. Then the B2→B3 breakdown.
