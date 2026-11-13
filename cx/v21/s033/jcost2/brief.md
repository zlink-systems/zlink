# Task: add Framework thread wakeups to the Java request-backpressure 1KiB cost report (diagnosis only)

## Situation
/root/jobs/fx/cx/v21/s033/jcost/report.md split Java Framework request-backpressure 1KiB costs into (1) Spring handler creation/cleanup per request on the target and (2) source completion hand-off to ForkJoinPool.commonPool. The Framework scheduler trace was empty in that capture. It has been recaptured with a longer run: /root/jobs/fx/cx/ratio045diag/zlink-framework-java-request-backpressure-1024/ (sched.txt = perf script of sched_switch, sched_wakeup, raw_syscalls for both pids over 3 s; window.json = pids, request counts at start/end of the perf window, per-thread CPU ticks with names; source.jfr/target.jfr also re-recorded). The earlier short capture is in ...-1024.short/. Binding: /root/jobs/fx/cx/ratio045diag/zlink-java-request-backpressure-1024/.
Source tree: /root/worktree/zlink-ratio040 (main).

## Goal
1. Per completed request in the perf window: sched_wakeup count by woken thread name (group pool workers), for Framework source and target, and the same for the binding (the earlier report has binding values; recheck them with the same method).
2. Attribute wakeups to cause 2 (commonPool workers woken on the source; who wakes them) and to cause 1 if any (target handler threads, virtual-thread carriers ForkJoinPool-1). Show which thread wakes which (waker comm → wakee comm) table, top 15, per request.
3. Context switches per request per process (sched_switch prev side), Framework vs binding.
4. Recompute per-thread CPU µs/request from the new window.json and note any difference from the earlier report.
5. Write the result into /root/jobs/fx/cx/v21/s033/jcost2/report.md, Korean, plain sentences; start with a 6-line summary that the supervisor can merge with the earlier summary: cause 1 and cause 2 each as allocation bytes/request, CPU µs/request, wakeups/request.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only: do not modify any worktree or tracked file; no git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include).
- 리팩토링 점검: not applicable; state "코드 변경 없음".
- No new bench runs, no builds. Keep MemAvailable >= 12 GiB. Outputs only under /root/jobs/fx/cx/v21/s033/jcost2/.
