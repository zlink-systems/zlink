# Task: .NET request-serial — µs of the extra receive-side handoff on the target (measurement-only, no commit)

## Situation
On the target, the .NET Framework moves each request twice before the handler runs: Core I/O thread → the Framework's managed receive
thread (Long Running Task) → route worker. The binding moves it once (I/O thread → ThreadPool worker). The PM will ask the user to add a
binding receive-delivery API that removes one hop; the user needs the size of that hop in µs on the serial round trip.
Previous evidence (under perf syscall tracing, which inflates handoffs): /root/jobs/fx/cx/v21/s033/dseg4/report.md — on experiment A
the receive thread → route worker step B2→B3 was 31.3 µs p50 (serial 1KiB). Main now has A+C (#3985). Marker definitions and the
throwaway instrumentation used for A are in /root/jobs/fx/cx/v21/s033/dhop/ (dseg-instr-a.diff applies to this tree except
framework/bench/grpc/dotnet/run_local.sh, which already has the CPU-pin patch; plus DsegBenchTrace.cs, ZLinkDsegTrace.cs).

## Do
Worktree /root/worktree/zlink-dhop (main 2f8b8e635a + CPU-pin patch; measurement-only; it will be deleted; never commit).
1. Apply the target-side markers B1 (managed receive returns the batch), B2 (just before handing to the owner/application queue),
   B3 (route dispatch entry on the worker), B4 (just before the reply submit). Adjust placement to the A+C code if needed (say file:line).
   Keep the env-var gate and the ring buffer; no perf, no syscall tracing (markers only, to avoid inflating the handoff).
2. Build (`/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash dotnet/build.sh` from framework/bench/grpc). Under
   `flock -o /root/jobs/fx/cx/measure.lock`, condition B, run zlink-framework-dotnet request-serial at 1024 and 4096, RUNS=3 each, with
   the marker gate on; then RUNS=3 each with the gate off (same build) to show the marker overhead on throughput.
3. Report per size: B1→B2, B2→B3, B3→B4 p50/p90/mean; the thread of each marker (B2→B3 thread change count); the round trip
   (1e6/throughput) with gate on and off. Then estimate the round-trip µs saved if the request ran on the thread that received it
   (B2→B3 minus the part that is dispatch work rather than waiting/wake — split B2→B3 into queue-wait/wake vs work if the markers
   allow; say plainly what is measured and what is estimated).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not modify bindings/.
- Instrumentation is throwaway; list every file you edited.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- 리팩토링 점검: not applicable (measurement-only, no product change).
- Time box about 90 minutes.

## Output: /root/jobs/fx/cx/v21/s033/dhop/report.md (Korean, plain sentences; the per-size table first)
