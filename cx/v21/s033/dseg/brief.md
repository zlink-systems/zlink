# Task: .NET request-serial 1KiB — per-request sub-segment timing with a measurement-only build (no commit, no PR)

## Situation
Read /root/jobs/fx/cx/v21/s033/nseg2/report.md and /root/jobs/fx/cx/v21/s033/nseg/report.md first. Over socket-syscall
boundaries, Framework vs binding p50: B (target receive → reply send) 106.7 vs 50.7 µs, D (source reply receive → next request
send) 126.7 vs 70.0 µs. Each contains one thread hand-off:
- B: the .NET Long Running receive thread (ZLinkManagedMeshNode.ReceiveLoopAsync / DrainRawSocket / ZLinkProcessExecutionPool
  ReceiveUntilHandoff) → execution pool → ZLinkMeshNodeRouteDispatcher.DispatchNodeRouteAsync → ZLinkRouteHandlerInvoker.InvokeRequestAsync
  → handler → reply submit.
- D: binding CompletionOwner.Drain / RequestCompletionEntry.MoveReply → ZLinkCompletionDispatcher ThreadPool work item → bench
  ExecuteRequestAsync / ObserveRequestReplyAsync continuation → next RequestNativeDurableWireAsync submit.
The PM needs, per request, hand-off wait (enqueue → execution start) vs execution time for B and D, to choose a design.

## Do
Worktree /root/worktree/zlink-dseg (detached main dc3a609aa9 plus 4 local run_local.sh CPU-pin edits; keep them). This tree is
measurement-only: it will be deleted, never committed. Edit only framework/languages/dotnet and framework/bench/grpc/dotnet there.
1. Add timestamps (Stopwatch.GetTimestamp, a preallocated per-process ring buffer of fixed-size records, no allocation, no lock on
   the hot path; dump to a file at process exit or via an env-var-gated path) at four points in each of B and D:
   B: (B1) receive thread has the record (after DrainRawSocket/receive returns it), (B2) enqueue/hand-off to the execution pool,
   (B3) execution starts on the pool thread (dispatch entry), (B4) reply submitted to the binding (before the socket call returns).
   D: (D1) completion drained (CompletionOwner.Drain / MoveReply on the source), (D2) ThreadPool work item queued by
   ZLinkCompletionDispatcher, (D3) work item starts executing, (D4) next request submitted to the binding by the caller.
   Correlate per request (serial = one in flight; a sequence number per process is enough, or the request id if cheaply available).
   Also record thread id at each point. Gate everything behind one env var so the uninstrumented path costs ~nothing.
2. During the same run, sample the source process's ThreadPool every 100 ms: ThreadPool.ThreadCount, PendingWorkItemCount,
   CompletedWorkItemCount (from inside the process on a timer thread, written with the dump), or dotnet-counters
   (/root/jobs/fx/cx/v21/p032net/tools/dotnet-counters) System.Runtime threadpool counters.
3. Build: from framework/bench/grpc: `taskset -c 0-7 bash dotnet/build.sh` (under `flock -s /root/jobs/fx/cx/measure.lock`).
   Run Framework request-serial 1024 and the binding request-serial 1024 (binding needs no instrumentation; for it report only
   throughput) with `flock -o /root/jobs/fx/cx/measure.lock env IMPLEMENTATIONS=zlink-framework-dotnet PATTERNS=request-serial
   PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=20 DURATION_SECONDS=10 SKIP_BUILD=1 RUN_STAMP=dseg OUTPUT_DIR=<job dir>/out bash dotnet/run_local.sh`
   from framework/bench/grpc of this worktree. Run once instrumented and once with the env var off, and report both throughputs
   (main uninstrumented reference: Framework 5,998 req/s, binding 11,370).
4. Report per-request p50/p90/mean for B1→B2, B2→B3 (hand-off wait), B3→B4 (execution), D1→D2, D2→D3 (hand-off wait),
   D3→D4 (execution incl. caller continuation), with thread ids/names involved, and the ThreadPool counts while D waits.
   Check that B1→B4 and D1→D4 roughly match the B and D sizes from nseg2 (state the difference).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include).
- 리팩토링 점검: the instrumentation is throwaway; list exactly which files you edited so the supervisor can delete the worktree.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Another job may hold measure.lock for a while; wait on it.
- Outputs under /root/jobs/fx/cx/v21/s033/dseg/.

## Output: /root/jobs/fx/cx/v21/s033/dseg/report.md
Korean, plain sentences. Start with a table: sub-segment, p50, p90, mean (µs), thread. Then the ThreadPool numbers, then the instrumented vs
uninstrumented throughput.
