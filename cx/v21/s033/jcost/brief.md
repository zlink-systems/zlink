# Task: Java request-backpressure 1KiB — cost of the two known causes, per request (diagnosis only)

## Situation
Java Framework ÷ binding on main after #3952: request-backpressure 1KiB 0.489 (fails 0.6); other request cells pass. Two causes were identified earlier: (1) a Spring handler object is created and cleaned up for every request on the target; (2) the source hands each completion to ForkJoinPool.commonPool. The PM wants each cause's cost split into allocations per request and thread hand-offs (wakeups) per request, with CPU per request where possible.

## Evidence (already captured, main dc3a609aa9, server CPUs 8-15, client 0-7)
- Framework: /root/jobs/fx/cx/ratio045diag/zlink-framework-java-request-backpressure-1024/ ; binding for comparison: /root/jobs/fx/cx/ratio045diag/zlink-java-request-backpressure-1024/
- Each has: source.jfr / target.jfr (JFR settings=profile, 8 s: execution samples, allocation samples, thread park etc.; read with the jfr tool: jfr print / jfr summary, JDK 25), sched.txt (perf script of sched_switch, sched_wakeup, raw_syscalls for both pids over 3 s), sched.data, window.json (pids, request counts at start/end of the perf window, per-thread CPU ticks with thread names).
- Source tree: /root/worktree/zlink-ratio040 (main; ignore the 4 locally modified run_local.sh files). Java Framework: framework/languages/java; binding: bindings/java; bench: framework/bench/grpc/java. Earlier Java work: /root/jobs/fx/cx/v21/s033/jprof, /root/jobs/fx/cx/v21/s033/jcount, /root/jobs/fx/cx/v21/s033/jfix.

## Goal
1. For each cause: allocated bytes and objects per request (JFR allocation samples are weighted estimates; say so), CPU µs per request on the threads involved, and thread wakeups/hand-offs per request (from sched_wakeup between named threads, normalized by completed requests in the window). Give the same totals for the binding.
2. Map each cost to source (file:function), including which Spring/Framework objects are created per request and which commonPool submissions happen per completion.
3. Anything else costing more than either cause (per-thread CPU table, Framework vs binding), ranked.
4. Do not propose a design; one line of what each cost consists of is enough.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only: do not modify any worktree or tracked file; no git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include).
- 리팩토링 점검: not applicable (no code change); state "코드 변경 없음" in the report.
- No new bench runs and no builds; work from the captured data. If something is truly missing, say so in the report instead.
- Keep MemAvailable >= 12 GiB. Outputs only under /root/jobs/fx/cx/v21/s033/jcost/.

## Output: /root/jobs/fx/cx/v21/s033/jcost/report.md
Korean, plain sentences, abbreviations spelled out. Start with a 6-line summary (per-request cost of cause 1 and cause 2, Framework vs binding).
