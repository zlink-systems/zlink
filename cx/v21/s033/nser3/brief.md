# Task: .NET request-serial — actual Framework path and per-segment µs vs binding, 1KiB and 4KiB (measurement-only, no PR)

## Situation
After #3995 (main 475305bebc) .NET request-serial is over budget: Framework 155.7 µs/request vs binding 88.9 at 1KiB (budget = binding/0.6 = 148.2, over by 7.5 µs); 161.9 vs 88.6 at 4KiB (over by 14.2 µs). #3995 (RouteMesh channel handlers in the mailbox claim turn) did not move serial, so the bench's serial path is probably not the path that design assumed. Data: /root/jobs/fx/cx/ratio052net.
Earlier .NET segment studies to reuse (method, trace points, analysis scripts): /root/jobs/fx/cx/v21/s033/dseg4, dhop, dcount, dprof, nserial — read their report.md first and reuse their instrumentation where it still applies to current main.

Worktree: /root/worktree/zlink-nser = main 475305bebc + CPU-pin patch. Measurement-only copy; never committed; deleted after.

## Do
1. Fix the actual path of one bench request-serial request with file:line, in order: bench caller submit → Framework client call → wire encode → binding submit → target receive → Framework dispatch → handler → reply encode → reply submit → source receive → Framework completion → caller continuation. Say which thread/executor runs each step. State explicitly whether RouteMesh channel handler / mailbox claim code is on this path.
2. Per-segment µs (p50/p90/mean) for Framework and binding at 1KiB and 4KiB, same session, condition B (source CPUs 0–7, target 8–15), under `flock -o /root/jobs/fx/cx/measure.lock`. Put Framework and binding side by side for the comparable segments, with the Framework-only segments listed separately. Sum check: segments must add up to the measured serial µs/request within a stated error.
3. Which segment grows from 1KiB to 4KiB more in Framework than in binding (copy / encode / allocation suspects): give bytes copied and allocations per request in each segment (dotnet-counters / EventPipe allocation tick or GC.GetAllocatedBytesForCurrentThread deltas — your choice, state it).
4. Per segment: thread switches (handoffs between threads) and allocations per request.
5. Unpaced serial µs for Framework and binding with tracing off, RUNS=3, so traced numbers can be checked against untraced.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/. Instrumentation only in framework/ and bench files of this worktree (or overlays outside it).
- Builds through `/root/jobs/fx/cx/measure-shared.sh <cmd>`. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- 리팩토링 점검: for each segment, note any duplicate work you see (same check/encode/copy done twice) with file:line; do not fix it.
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/nser3/report.md (Korean, plain sentences)
Order: (1) path table with file:line and thread; (2) segment µs table Framework vs binding, 1KiB and 4KiB, with sum check; (3) 1KiB→4KiB growth by segment with copies/allocations; (4) thread switches and allocations per segment; (5) duplicate work noticed.
