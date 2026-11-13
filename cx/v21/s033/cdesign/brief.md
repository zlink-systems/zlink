# Task: design and upper-bound estimate for removing thread hand-offs on the C++ Framework request and send paths (issues #3867, #3868)

## Situation
C++ request-backpressure 1024 is at Framework/binding 0.165 (main ee11390257) and send-saturation 1024 at 0.301. Two merged/pending PRs cut allocations (#3884 merged; #3888 open, branch perf/3867-submit-alloc in /root/worktree/zlink-3867a): request-backpressure +15.69%, submitter heap calls 49.3 -> 24.3 per message. That is far from 0.6. The C++ binding needs ~3.7 µs/message on its submitter; the Framework submitter needs ~20 µs. At 0.6 the Framework may spend at most ~6 µs per message on its critical thread.
Evidence to read first:
- /root/jobs/fx/cx/v21/s033/cprof/report.md (stage profile on main: per-thread table, stages, 3.77 context switches/message Framework vs 0.20 binding, Core IO syscalls ~7x binding).
- /root/jobs/fx/cx/v21/s033/calloc/report.md, /root/jobs/fx/cx/v21/s033/sfix/report.md (allocation census before/after).
- /root/jobs/fx/cx/ratio034/gap14-analysis.md (candidates R, W, T, M, H).
- Spec: framework/doc/framework/common/spec/server/01-execution/ (01-submit-and-completion, 02-handler-turn-and-execution-gate, 08-messaging-hot-path) and the design principles document they reference. Read them; the design must keep every contract there (first-result selection, admission, deadline, ordering, gate rules, completion delivery context).
- Cross-language: how .NET (Runtime/Service/ZLinkRawRouterServicePort.cs and its callers), Java (runtime/binding/ZLinkJavaRawServicePort.java) and Node submit a request from the caller to the socket — which thread runs each step. C++ must keep the same observable contract.

## Goal
1. Trace the C++ request-backpressure 1024 and send-saturation 1024 paths on current origin/main + #3888 (use /root/worktree/zlink-3867a, read only): every thread a message crosses from the caller's `request_to_node`/send call to the binding socket call, and from the binding reply to the caller's completion. For each hop: file:function, the queue/lane/executor used, why it exists (which contract or ownership rule needs it), and its measured cost (context switches, wake latency, µs/message) from a profile you take.
2. Design the minimum-hop path: which hops can be removed or made inline (for example running the lane step on the caller thread when the lane is idle, submitting to the binding directly from the caller under the socket lock, batching completions), what each removal must preserve, and which contract clause makes a hop mandatory if any. No new runtime mode or flag; the new path must be the only path.
3. Upper bound before implementing: build an experimental branch in a NEW worktree /root/worktree/zlink-3867x (from perf/3867-submit-alloc, branch exp/3867-inline, never pushed) that removes the hops in the crudest correct-enough way for the benchmark only, and measure request-backpressure 1024 and send-saturation 1024 Framework msg/s against /root/worktree/zlink-ratio040 binding numbers (4 rounds). This gives the ceiling of the real design. State clearly which contracts the prototype ignores.
4. Estimate the resulting ratio for both cells with the real design, and list the work (files, tests) to implement it.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not modify /root/worktree/zlink-3867a or /root/worktree/zlink-ratio040. Do not push anything.
- Build C++ only via `/root/jobs/fx/cx/cpp-build-slot.sh <build-dir> --parallel 3` (read its usage), build dirs under /root/jobs/fx/cx/v21/s033/cdesign/. Check MemAvailable >= 14 GiB before each compile.
- Every bench/profile run inside `flock -o /root/jobs/fx/cx/measure.lock ...`; other jobs also measure, so wait on the lock. Apply /root/jobs/fx/cx/ratio034/pin-condition-b.patch to your worktree's cpp run_local.sh for measuring. run_local.sh env: PATTERNS, PAYLOAD_SIZES=1024, IMPLEMENTATIONS, RUNS=1, WARMUP_SECONDS=30, DURATION_SECONDS=5, SKIP_BUILD, RUN_STAMP, OUTPUT_DIR under your job dir.
- Keep MemAvailable >= 12 GiB.

## Output: /root/jobs/fx/cx/v21/s033/cdesign/report.md (exact path)
Hop table (request and send), design with contract mapping, prototype A/B table (each round, medians, ratio vs binding), estimated ratio of the real design, implementation plan, and what you could not finish.
