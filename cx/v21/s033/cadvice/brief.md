# Task: structural design advice — removing thread hand-offs on the C++ Framework request and send paths (issues #3867, #3868)

## Situation
C++ request-backpressure 1024 is at Framework/binding 0.165 (main ee11390257) and send-saturation 1024 at 0.301. Two merged/pending PRs cut allocations (#3884 merged; #3888 open, branch perf/3867-submit-alloc in /root/worktree/zlink-3867a): request-backpressure +15.69%, submitter heap calls 49.3 -> 24.3 per message. That is far from 0.6. The C++ binding needs ~3.7 µs/message on its submitter; the Framework submitter needs ~20 µs. At 0.6 the Framework may spend at most ~6 µs per message on its critical thread.
Evidence to read first:
- /root/jobs/fx/cx/v21/s033/cprof/report.md (stage profile on main: per-thread table, stages, 3.77 context switches/message Framework vs 0.20 binding, Core IO syscalls ~7x binding).
- /root/jobs/fx/cx/v21/s033/calloc/report.md, /root/jobs/fx/cx/v21/s033/sfix/report.md (allocation census before/after).
- /root/jobs/fx/cx/ratio034/gap14-analysis.md (candidates R, W, T, M, H).
- Spec: framework/doc/framework/common/spec/server/01-execution/ (01-submit-and-completion, 02-handler-turn-and-execution-gate, 08-messaging-hot-path) and the design principles document they reference. Read them; the design must keep every contract there (first-result selection, admission, deadline, ordering, gate rules, completion delivery context).
- Cross-language: how .NET (Runtime/Service/ZLinkRawRouterServicePort.cs and its callers), Java (runtime/binding/ZLinkJavaRawServicePort.java) and Node submit a request from the caller to the socket — which thread runs each step. C++ must keep the same observable contract.

## Goal — advice, not implementation
The user asked for your independent design advice first; the PM and the supervisor will produce their own designs, and the three will be compared. Work from source reading and the existing evidence; you may take short profiles to confirm a claim.
1. Hop map: every thread a message crosses on C++ request-backpressure 1024 and send-saturation 1024, from the caller's request/send call to the binding socket call, and from binding reply to caller completion. For each: file:function, queue/lane/executor, the contract or ownership rule it serves, and its cost (context switches, wake latency, µs/message) from the evidence.
2. Give two or three structurally different designs that remove hops (for example: caller-thread inline submit when the lane is idle; submitting under the socket lock from the caller; one I/O thread owning both socket and lane with batched completions; anything better you find). For each: what changes, which spec clauses it must keep and how, which clause or ownership rule might forbid it, risks (ordering, reentrancy, deadline/admission, shutdown, completion context), cross-language consistency with .NET/Java/Node, and an estimated ceiling ratio for both cells with the arithmetic shown.
3. Recommend one design, with an implementation plan (files, order, tests) and the measurement that would prove or disprove its ceiling before full implementation.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only: do not modify any worktree or tracked file, do not push. Any script or profile output goes under /root/jobs/fx/cx/v21/s033/cadvice/.
- If you profile: every bench/profile run inside `flock -o /root/jobs/fx/cx/measure.lock ...` (other jobs measure too; wait), using the existing builds in /root/worktree/zlink-ratio040 (main) with SKIP_BUILD=1. No builds.
- Keep MemAvailable >= 12 GiB.

## Output: /root/jobs/fx/cx/v21/s033/cadvice/report.md (exact path)
Hop map, designs with contract mapping and ceilings, recommendation and plan, open questions.
