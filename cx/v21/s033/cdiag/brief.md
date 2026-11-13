# Job: read-only diagnosis — why multi-worker gate-less drain costs CPU and Spot latency (#1639)

Model/level: gpt-6.1-sol high — diagnosis across scheduler, mailbox and permit paths; two design passes
already missed their criteria, so the cause must be measured before any further change.
No sub-agents or other models (no Astra).

## Question
Branch worktree /root/worktree/zlink-1639c (uncommitted second-pass changes on top of 5a585edc93).
Reports: /root/jobs/fx/cx/v21/s033/cdrain/report.md and /root/jobs/fx/cx/v21/s033/cdrain2/report.md.
1. send-saturation 1K: target application CPU 5.5 µs/msg (main, 2 workers) -> 24.8 µs/msg (branch,
   8 workers) and throughput -12.5%. Where do the extra ~19 µs/msg go? Attribute with perf (cpu-clock,
   call graphs) per function group: mailbox mutex / futex wait+wake, executor queue lock, take/settle
   bookkeeping, cache-line contention on shared counters, handler body, idle spin. Same for
   request-backpressure 1K (19.8 -> 38.6 µs/msg).
2. PM cell: Spot request p99 0.49 ms (main) -> 7.54 ms (branch) even with helper rotation. Break one slow
   Spot request into stages with timestamps: local admission and host permit wait, owner enqueue, wait in
   the executor queue, gate acquisition, handler, completion. Which stage holds the ~7 ms?
3. For each cause, give the owning decision with spec clause file:line (spec root
   framework/doc/framework/common/spec/server/) and every place that decides it now (file:line).

## Write limits and forbidden
Do not edit the worktree or any repository file. Temporary instrumentation only in a copy under
/root/jobs/fx/cx/v21/s033/cdiag/ (copy the tree there if needed). Forbidden: commit, push, branch, stash,
reset, spec edits, GitHub operations. C++ builds via `/root/jobs/fx/cx/cpp-build-slot.sh --parallel 3 <cmd>`
under `flock -s /root/jobs/fx/cx/measure.lock`; measurements and perf under `flock -o` on the same file.
Reuse the runner scripts in /root/jobs/fx/cx/v21/s033/cdrain2/ and its condition B pinning.

## Report
/root/jobs/fx/cx/v21/s033/cdiag/report.md: per question the measured attribution table (µs/msg or ms per
stage, main vs branch), the decision-owner table, raw data paths, and what you could not measure.
No recommendations beyond what the data shows.
