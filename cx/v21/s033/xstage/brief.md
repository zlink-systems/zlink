Model sol high: cross-language CPU attribution (C++ native, .NET JIT perf-map, Java JIT symbols) mapped to one stage table needs careful profiling and judgment.

# Task: receive-side per-stage user CPU µs/message, C++ vs .NET vs Java Framework, same bench and same rate (#3868 input)

Earlier briefs in other folders are history, not instructions for this task. Folder: JOB=/root/jobs/fx/cx/v21/s033/xstage. Worktree for reading/building current main: /root/worktree/zlink-v21-s033/xstage (create it yourself with `git -C /root/zlink worktree add --detach /root/worktree/zlink-v21-s033/xstage origin/main` after `source /root/jobs/fx/cx/git-https.env; git -C /root/zlink fetch -q origin main`).

## Why
Two questions from 대표님 (2026-11-10): (a) is the C++ coroutine cost per message higher than the .NET async (async/await) cost per message? (b) why does per-message cost differ when the three Framework structures are the same? The answer feeds the C++ path design in s033/cpath (what to drop or keep per message). It is not used to pick "the next stage to fix". Coroutines stay in C++; the question is only what each one costs.

## Reuse (read only)
- `/root/jobs/fx/cx/v21/s033/csprof2/` — report.md §2 stage rules and the scripts in scripts/ (collect.py, classify.py, aggregate.py, chains.py, cpp-runner.sh etc.), .NET perf-map symbol resolution. Reuse its method: perf record cpu-clock, user samples normalised to measured /proc user CPU, stage sums equal measured CPU.
- `/root/jobs/fx/cx/v21/s033/ctgt/report.md` — C++ target user attribution.
- `/root/jobs/fx/cx/v21/s033/csendb/` — bench.diff and scripts/measure.py (paced send-saturation setup for C++/.NET).
- `/root/jobs/fx/cx/ratio045diag/` and `/root/jobs/fx/cx/v21/s033/jcost2/report.md` — how the Java Framework bench was built and captured.
Do not read other job folders.

## Do
1. Cells: send-saturation 1024 B and request 1024 B (the Framework bench cells used by the 0.33.0 perf work). Receive side = target process (for request also count the reply submit on the target as its own stage).
2. Build current main for all three Frameworks (Release, no -g except frame pointers as csprof2 used; C++ through `/root/jobs/fx/cx/cpp-build-slot.sh` bare, 25 GB cap; Java with -XX:+PreserveFramePointer and a JIT symbol map such as perf-map-agent or async-profiler if available on this machine — say which). All three must load the same libzlink (record SHA-256).
3. Rate: per cell, first find each language's natural rate (short runs), then measure all three paced at one common rate below the slowest natural rate (state it). 3 runs per language per cell, source CPUs 0-7, target 8-15, verify affinity.
4. Stages (rows): receive boundary (binding/Core receive), route classification, header/wire decode, mailbox enqueue·claim, dispatch·invoke (pump, coroutine/task, DI scope, invoker), handler, completion·permit (terminal, permit release, settle), reply submit (request only), metrics/bench, unattributed. Columns: C++, .NET, Java user CPU µs/message (also system CPU per language as one extra row). For each stage list the owning file:line per language.
5. For each stage where C++ is above the cheapest language: the gap in µs/message, the cheapest language, and that language's implementation file:line that does the same work, with one sentence on what it does differently (e.g. no allocation, batch claim, no virtual dispatch). Do not change any code.

6. Allocation and coroutine counts, in separate runs (never in the timing runs), same cells and rate: heap allocations per message per stage for all three languages (C++: heaptrack or an LD_PRELOAD/malloc counting hook; .NET: allocation events or EventPipe allocation tick; Java: JFR allocation events or async-profiler alloc mode; say which and its sampling bias). For C++ also count per message and per stage: coroutine frames created, task/promise shared-state allocations, std::function constructions (uprobes, a counting build with instrumented operator new/coroutine promise, or heaptrack call stacks; state the method). Put these as extra columns in the stage table. This answers a user question on C++ coroutine cost.

7. Coroutine vs .NET async (answer to (a)), same cells and rate, from steps 4 and 6 data plus any extra separate runs: one table per cell, per message on the target and on the request caller (source) side. Rows: async units created (C++ coroutine frames; .NET async state machines boxed, Task/ValueTask/TaskCompletionSource objects; Java CompletableFuture stages as a third column), heap allocations and bytes for that machinery, user CPU µs in that machinery (frame/state-machine allocation and free, promise/shared state, resume/continuation scheduling, wrappers such as std::function/closures/delegates, ExecutionContext flow), and thread hand-offs per message. State which symbols you counted as machinery per language with file:line. Give the C++ minus .NET difference in µs/message and say yes or no: is C++ coroutine machinery more expensive per message than .NET async, and by how much of the Framework÷binding gap.

## Measurement rules
- Do NOT take /root/jobs/fx/cx/measure.lock until the file /root/jobs/fx/cx/v21/s033/core124b/verdict.md exists (a release gate is measuring). Until then do only source reading, stage mapping and builds. Builds/tests: through `/root/jobs/fx/cx/measure-shared.sh` or cpp-build-slot.sh bare; never an outer flock.
- Each measurement inside `flock -o /root/jobs/fx/cx/measure.lock`, one at a time, MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no `&` background jobs.
- Keep every raw perf.data, script output and log under $JOB/raw; never delete them. Disk cap 40 GB for this job.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except fetch, worktree add/remove for your own worktree, diff, status, rev-parse. No commit, push, PR, GitHub operation. No spec edits. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
- 리팩토링 점검: while mapping stages, list duplicated work (the same check, copy or handoff done twice on the path) per language with file:line.
- Keep your own output short. Time box about 6 hours.

## Report: $JOB/report.md (Korean, plain sentences)
(1) builds, SHA-256, rates per cell; (2) the stage table per cell (C++/.NET/Java user µs/msg, system row, sums equal measured CPU; allocations/msg per language; C++ coroutine frames, shared states, std::function per msg); (3) per stage file:line in each language; (4) stages where C++ is above the cheapest language: gap, reference language, reference file:line, difference; (5) duplicated work; (6) the coroutine vs .NET async table and the yes/no answer, placed first in the report. Remove your worktree at the end (grep CMakeCache files under /root/worktree and /root/jobs for its path first).
