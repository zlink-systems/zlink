# Task: read-only study — run-to-completion on the C++ Framework request path (issue #1639, 0.33.0 perf)

## Situation
C++ Framework ÷ binding after #3919 (merged) and #3957 (branch perf/3867-cpp-request-sites, worktree /root/worktree/zlink-3867h, read that tree): request-backpressure 1KiB 0.205 / 4KiB 0.294, request-serial 0.514 / 0.549, send-saturation 0.280 / 0.513. Target is ≥ 0.6. Removing per-request copies and allocations did not reach it. The user approved a read-only study of a run-to-completion path.

Scope the user agreed to ("one thread" means only the Framework-added hand-offs; Core IO threads and the caller's send path stay):
- Target: the Framework receive thread that took the request runs the handler, encodes the reply and calls the reply socket itself; the receive role passes to an idle worker meanwhile (spec 08-messaging-hot-path §4.4 receive-role hand-off, :300). Removes the receive → application worker hand-off.
- Source: the receive thread that drains the binding completion also decodes and settles the result and hands it straight to the caller (or the caller drains its own completion). Removes receive → completion worker, and if possible completion worker → caller notification.

Read first:
- /root/jobs/fx/cx/v21/s033/cadvice/report.md (hop map with file:function and per-thread µs/message; its recommendation B; its caution that the source caller CPU 23.4 µs/message vs binding 3.7 µs may cap the ratio).
- /root/jobs/fx/cx/v21/s033/cprof/report.md, /root/jobs/fx/cx/v21/s033/csites/report.md.
- Spec: framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md (§9 :329, §10 :375, §11 :490–568) and 08-messaging-hot-path.ko.md (§4 :141–213, §4.4 :300), plus 02-handler-turn-and-execution-gate and the documents they reference.

## Goal
1. Updated hop map for the #3957 tree (request-backpressure, request-serial, send-saturation), marking each Framework-only hand-off.
2. For the target and the source design above: every spec clause it touches, as file:line, classified as kept as written / kept with an interpretation (state it) / conflicts (quote the sentence). Cover handler turn and execution gate, ordering per owner, admission and permits, deadline, first-result selection, completion execution context, Yield, cancellation, shutdown/drain order, reentrancy (handler blocks or calls request from inside a handler).
3. Expected ratio per cell after removing those hand-offs, with arithmetic from measured per-thread CPU and switches. State plainly whether 0.6 is reachable and, if not, what remaining cost blocks it (for example the source caller CPU) and what else it would take.
4. Cross-language: how .NET, Java and Node place the same steps today; whether the design would make C++ differ observably.
5. If recommended: implementation plan (files, order, tests, removed mechanisms with before/after count of decision owners) and the smallest experiment that would prove or disprove the expected ratio first.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only: do not modify any worktree or tracked file; no git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include); use the binding and Framework sources.
- 리팩토링 점검: for any proposed change, list what existing mechanism it replaces and confirm no second parallel mechanism remains.
- If you profile: every bench run inside `flock -o /root/jobs/fx/cx/measure.lock ...`, using existing builds with SKIP_BUILD=1, server CPUs 8-15, client 0-7. No builds. Prefer the existing evidence.
- Keep MemAvailable >= 12 GiB. Outputs only under /root/jobs/fx/cx/v21/s033/crtc/.

## Output: /root/jobs/fx/cx/v21/s033/crtc/report.md
Write it in Korean, plain sentences, internal abbreviations spelled out. Start with a 10-line summary: reachable or not, expected ratios, spec conflicts count, recommendation.

## Additional required items (PM 11-07)
(c) Ownership layers that must change and the number of decision points before and after. (d) Effect on the common four-language contract. The .NET/Java diagnosis captures hold measure.lock now; do not take measure.lock at all unless the evidence is impossible otherwise.
