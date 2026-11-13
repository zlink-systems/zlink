# Task: C++ request-backpressure 1KiB — is the Framework's 4x binding/Core cost a rate effect or a usage effect? (measurement-only)

## Situation
Framework ÷ binding must reach 0.6. C++ request-backpressure 1KiB today (main c916574ec3, /root/jobs/fx/cx/ratio048p1 "before"):
Framework 52,204 req/s, source process CPU 80.5 µs/request, target 59.9; binding 249,272 req/s, source 9.6, target 9.4 µs/request.
Inside the Framework source process, the binding/Core stage alone is 40.3 µs/request and thread handoff/wake 18.8
(/root/jobs/fx/cx/v21/s033/ccall2/analysis.json). Read /root/jobs/fx/cx/v21/s033/coreio/report.md first: the Framework sends
1.18 messages per sendto versus binding 10.53, source I/O CPU 38.59 vs 5.43 µs/message, and other-thread eventfd writes 0.383 vs
0.016 per message. coreio left one question open: "whether faster Framework submission alone will produce binding-sized batches is
not measured; no rate-matched counterfactual was run." Shrinking per-request objects on the caller (PR 1) bought only 0.87 µs, so
the C++ direction now depends on this answer.

## Do
Worktree /root/worktree/zlink-cbind (measurement-only copy of main with the CPU-pin patch; it will be deleted; never commit).
1. Add a pacing option (env var, default off) to the binding driver (zlink_raw_driver_t in framework/bench/grpc/cpp/client) that
   spaces request submissions to a target rate, keeping the same in-flight cap and everything else unchanged. Build the bench
   (`/root/jobs/fx/cx/cpp-build-slot.sh` around the bench build, or cpp/build.sh under `/root/jobs/fx/cx/measure-shared.sh`).
2. Under `flock -o /root/jobs/fx/cx/measure.lock`, condition B (target CPUs 8-15, source 0-7), request-backpressure 1024,
   RUNS=1 each, run: binding unpaced; binding paced at 52,000 req/s; binding paced at 100,000; Framework unpaced. For each run
   record, with coreio's method (per-thread schedstat deltas and BPF syscall counters; reuse coreio's scripts in its folder):
   source and target process CPU µs/request, I/O-thread CPU µs/request, messages per sendto and per recvfrom, recvfrom EAGAIN per
   message, eventfd reads/writes per message by thread role, context switches per message.
3. Answer plainly with numbers: at the Framework's rate, does the binding pay the same I/O cost and the same small batches?
   (a) If yes: how much of the Framework's 80.5 µs source and 59.9 µs target is explained by rate alone, and what the Framework
   would pay at 0.6 × binding rate if it used the binding exactly as the binding bench does.
   (b) If no: list the per-request differences in how the Framework calls the binding (which binding entry points, from which
   threads, extra eventfd wakes, extra sockets or pollers, Framework-side queues or threads that duplicate what the binding does),
   each with µs/request and file:line.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not modify bindings/.
- Only bench files may change, in this worktree only; list every file you edited.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- 리팩토링 점검: not applicable (measurement-only, no product change).
- Time box about 2 hours; write the report even if an item is missing and mark it "측정 못 함".

## Output: /root/jobs/fx/cx/v21/s033/cbind/report.md (Korean, plain sentences; the run comparison table first)
