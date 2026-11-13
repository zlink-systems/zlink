# Task: .NET request-serial 1KiB — same Core↔managed edge timing for the BINDING bench (measurement-only, no commit)

## Situation
Read /root/jobs/fx/cx/v21/s033/dseg2/report.md (your previous result and method; scripts and raw data in /root/jobs/fx/cx/v21/s033/dseg2/)
and /root/jobs/fx/cx/v21/s033/dseg/report.md. For the Framework, the four Core↔managed edges dominate the round trip
(target recvfrom→B1 42.1, B4→sendto 23.1, source recvfrom→D1 68.2, D4→next sendto 31.9 µs p50, under full syscall tracing).
The binding bench may not have the same hand-offs (its client drives the poller itself). The PM needs the same edges for the binding
so the Framework-only share is visible.

## Do
Worktree /root/worktree/zlink-dseg (measurement-only; keep the existing Framework instrumentation and env gate; it will be deleted).
1. Add the same Stopwatch/ring-buffer markers (reuse DsegBenchTrace.cs; do not modify bindings/) to the binding bench:
   framework/bench/grpc/dotnet/ZLinkRawServer (RunRequestRouter: record received → handler start (T1), reply call (T4 before
   ReplyMultipart/send)) and the client's RawBenchTransport.RequestAsync path (S1 reply observed: await resume or poll return;
   S4 next request submit). Same env-var gate.
2. Run zlink-dotnet request-serial 1024 with perf `-k CLOCK_MONOTONIC` (raw_syscalls enter/exit, sched_switch, sched_wakeup, both pids,
   3 s in the active window), exactly as dseg2. Build and run commands as in dseg2 (`flock -s` for build, `flock -o
   /root/jobs/fx/cx/measure.lock` for the run, IMPLEMENTATIONS=zlink-dotnet).
3. Report the same consecutive table for the binding: source sendto → target recvfrom → T1 → T4 → target sendto → source recvfrom → S1
   → S4 → next sendto, p50/p90/mean with threads; then a side-by-side table Framework (from dseg2) vs binding per edge, with the
   difference, and the run throughputs. Note lost events.
4. For source recvfrom → S1 in the binding: which thread drains the completion and which thread resumes the caller (same or different).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not modify bindings/.
- Instrumentation is throwaway; list every file you edited.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- Outputs under /root/jobs/fx/cx/v21/s033/dseg3/.

## Output: /root/jobs/fx/cx/v21/s033/dseg3/report.md (Korean, plain sentences; side-by-side table first)
