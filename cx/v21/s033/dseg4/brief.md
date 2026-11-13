# Task: .NET experiment A — the same four Core↔managed edges on the A branch (measurement-only, no commit)

## Situation
.NET experiment A (branch perf/3869-dotnet-budget-a, commit 060ce2af5d) is kept by the PM. It lowered target CPU per request but the
Framework is still over budget (serial round trip +13~20 µs, backpressure source +26~49 µs, target +14~24 µs per request).
The PM needs to know at which edge the remaining Framework-only time sits.
Previous results and methods (read first):
- /root/jobs/fx/cx/v21/s033/dseg2/report.md — Framework edges on dc3a609aa9 (before A): target recvfrom→B1 42.1, B4→sendto 23.1,
  source recvfrom→D1 68.2, D4→next sendto 31.9 µs p50; also the network legs and internal sub-segments.
- /root/jobs/fx/cx/v21/s033/dseg3/report.md — the same edges for the binding bench (17.6 / 16.7 / 34.2 / 16.2 µs p50).
- /root/jobs/fx/cx/v21/s033/dseg/report.md — marker definitions B1..B4, D1..D4.
Instrumentation used then (on dc3a609aa9): /root/jobs/fx/cx/v21/s033/dseg4/dseg-instr.diff plus the new files
DsegBenchTrace.cs and ZLinkDsegTrace.cs in /root/jobs/fx/cx/v21/s033/dseg4/. Capture/analysis scripts: capture.py, analyze.py,
b2syscalls.py (Framework, from dseg2) and capture-binding.py, analyze-binding.py (from dseg3) in the same folder.

## Do
Worktree /root/worktree/zlink-3869a (measurement-only copy of the A branch, already has the CPU-pin patch in the bench run scripts;
it will be deleted afterwards).
1. Port the Framework markers B1..B4 and D1..D4 to the A code with the same meaning (the A branch changed
   ZLinkProcessExecutionPool / route dispatch / reply delivery, so place each marker at the equivalent point and say where, file:line).
   Same Stopwatch ring buffer and env-var gate. Bench client markers as before.
2. Build (`/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash dotnet/build.sh` from framework/bench/grpc), then run
   zlink-framework-dotnet request-serial 1024 with perf `-k CLOCK_MONOTONIC` exactly as dseg2 (raw_syscalls enter/exit,
   sched_switch, sched_wakeup, both pids, 3 s in the active window), run under `flock -o /root/jobs/fx/cx/measure.lock`.
3. Report:
   a. Side-by-side table per edge p50/p90/mean: Framework before A (dseg2) | Framework A (this run) | binding (dseg3) |
      A minus binding. Edges: source sendto→target recvfrom, target recvfrom→B1, B1→B4 (and its sub-segments B1→B2, B2→B3, B3→B4),
      B4→target sendto, target sendto→source recvfrom, source recvfrom→D1, D1→D4 (D1→D2, D2→D3, D3→D4), D4→next sendto.
   b. Threads at each edge for A (which thread runs the handler now; is it the receive thread or a worker; on the source side which
      thread drains the completion and which resumes the caller).
   c. Run throughputs, lost events.
   d. For the two largest A-minus-binding edges: what work the Framework does there that the binding path does not, naming the
      code (file:line) — facts from the trace and code only, no design proposal.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not modify bindings/.
- Instrumentation is throwaway; list every file you edited.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- 리팩토링 점검: not applicable (measurement-only, no product change).
- Outputs under /root/jobs/fx/cx/v21/s033/dseg4/.

## Output: /root/jobs/fx/cx/v21/s033/dseg4/report.md (Korean, plain sentences; side-by-side table first)
