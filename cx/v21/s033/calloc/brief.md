# Task: allocation-site census on the C++ Framework request submit path (issue #3867)

Profile (read first): /root/jobs/fx/cx/v21/s033/cprof/report.md. request-backpressure 1024: the Framework source application submitter thread makes 49.3 heap calls per message (binding 12.4); both processes 114.6 vs 20.7. Allocation is 7.56 µs/message of the 23.4 µs submitter CPU, the largest stage. Site-level counts were not measured; that is this task.
Reuse the counter in /root/jobs/fx/cx/v21/s033/cprof/ (alloc_counter.c, run.sh, instrumented/run_local.sh) and extend it to capture call stacks per allocation (sampled backtraces or every call; state which).

## Rules
- Evidence only: do NOT modify tracked files in the repo. Everything goes under /root/jobs/fx/cx/v21/s033/calloc/.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Worktree /root/worktree/zlink-ratio040 (main ee11390257 + pinning in */run_local.sh — keep it). Use its built Release binaries; do not rebuild them in place. Run `/root/jobs/fx/cx/bench-preflight.sh /root/worktree/zlink-ratio040 cpp` first.
- Wrap EVERY bench run in `flock -o /root/jobs/fx/cx/measure.lock ...`; wait on it if another job holds it. Any helper build goes in your job dir via `/root/jobs/fx/cx/cpp-build-slot.sh ... --parallel 3` under `flock -s /root/jobs/fx/cx/measure.lock`.
- Keep MemAvailable >= 12 GiB. Time box about 75 minutes.

## Output: /root/jobs/fx/cx/v21/s033/calloc/report.md (exact path)
1. For the Framework source submitter thread, and separately for the whole source and whole target processes: allocations per message grouped by allocating call site (file:function of the first Framework or binding frame above the allocator), with bytes per message, sorted by count. Cover at least 95% of calls.
2. The same table for the binding run's submitter, for comparison.
3. For each Framework site: what object it allocates (task state, std::function storage, vector, string, coroutine frame, native message buffer, ...), whether the object lives past the submit call, and whether the binding run has an equivalent.
4. A ranked list of concrete reductions (file:function, what to change, calls/message removed), marking which ones keep every current public result and lifetime as is, and which would need a contract change (cite the spec section under framework/doc/framework/common/spec/).
