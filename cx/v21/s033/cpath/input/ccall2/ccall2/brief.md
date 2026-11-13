# Task: C++ request-backpressure 1KiB — why PR 1 (prepared request storage) cut the caller only 0.84 µs (read-only profile)

## Situation
Approved option B PR 1 (report /root/jobs/fx/cx/v21/s033/cpr1/report.md, uncommitted tree /root/worktree/zlink-3867p, base 00f9963cc9)
replaced the admission conversion task chain and the result/observer shared_ptr control blocks with an intrusive owner and a
request arena (warmed caller: 0 heap, 0 shared_ptr ops in its counting test). The budget said the caller (submit) thread would drop
from 17.48 to 12.10 µs/request (−5.38). Measured under condition B, 3 runs (/root/jobs/fx/cx/ratio048p1/): caller 17.18 → 16.34 µs,
throughput 52,204 → 51,767 req/s; 4KiB 45,969 → 43,553. The PM needs the cause before PR 2.
Baseline split on the same base: /root/jobs/fx/cx/v21/s033/ccall/report.md (encode 2.15, registration 4.42, result observation 4.25,
binding submit 5.54, other 1.11 µs) with its scripts in /root/jobs/fx/cx/v21/s033/ccall/scripts and build in .../ccall/build.

## Do (reuse the ccall method and stage mapping exactly; do not reinvent)
1. Build the same frame-pointer profile bench from /root/worktree/zlink-3867p into THIS job directory only (do not modify that
   worktree). Builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmake --build ...> --parallel 3` (no outer flock).
2. Profile zlink-framework-cpp request-backpressure 1024 exactly as ccall (condition B, RUNS=1, SKIP_BUILD=1, under
   `flock -o /root/jobs/fx/cx/measure.lock`), plus one un-profiled run for representativeness.
3. Report a table: part | ccall before µs | PR 1 µs | change | the budget's expected change (registration and result observation
   were expected to carry the −5.38). Then the top 20 caller functions before vs after (self and inclusive µs/request), and every
   new PR 1 symbol on the caller (task_owner / arena / synchronized_pool_resource / task_ptr_t / mutex) with its µs.
4. Answer plainly: which removed costs actually left the caller thread, which moved elsewhere (e.g. to the completion or I/O
   thread), and which new costs PR 1 added (pool locks, arena mutex, larger task blocks, condition variable). Name file:function.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only for the repository: do not modify any worktree or tracked file; no git operations; no spec edits; no GitHub operations.
  Do not read Core C headers (core/include).
- 리팩토링 점검: not applicable (no product code change); write "코드 변경 없음".
- MemAvailable >= 12 GiB; delete raw profile files over 1 GB after summarizing. SIGTERM/SIGINT only; no background jobs with &.
- Time box about 90 minutes; write the report even if an item is missing and mark it "측정 못 함".

## Output: /root/jobs/fx/cx/v21/s033/ccall2/report.md (Korean, plain sentences; the comparison table first)
