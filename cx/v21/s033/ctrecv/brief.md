Model gpt-6.1-sol, effort high: budget-first redesign of the C++ Framework receive path across several layers (port, mailbox, host dispatch, route dispatch, handler invocation) with ordering, permit and terminal contracts to keep.

# Task: #3868 — cut the C++ Framework target (receive) user CPU per message to the budget

## Situation
- Same rate 136,889 msg/s, 1 KiB, condition B (source CPU 0-7, target 8-15), µs per message, target user: binding 1.329; Framework main 9.535-9.695; with the source change on this branch 8.519-8.627. Raw: s033/csrc/raw/paced, s033/csrc2/raw/paced.
- Attribution of target user CPU (s033/ctgt/report.md, raw in ctgt/raw), stage µs/message normalized to 9.535: Framework receive dispatch (pump/coroutine/host dispatch) 2.912, Core/binding receive incl. Framework port boundary 2.047 (binding total is 1.329), route classification 1.357, envelope header decode 1.041, mailbox enqueue/claim 1.007, unattributed 0.666, completion/terminal/permit 0.397, handler+bench 0.098.
- Contract: framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md §4.1-4.4 (ingress classifies by header only and moves the record into the owner queue; at most one execution-resource switch from claim to the handler's first instruction; no per-record dispatch task or wrapper; DI lookup constant time with a prepared factory; new scope per dispatch), plus 04-application-job-queue-and-backpressure (permit order, owner queue, ready-owner set), 02-handler-turn-and-execution-gate, 01-submit-and-completion §10-11, 05-payload-ownership-and-codec §6 (typed decode once, after execution rights).
- .NET already reuses decoded headers for repeatable headers: framework/languages/dotnet .../ZLinkEnvelopeCodec.cs:377-411 and :1385-1428 (bounded cache, bytes compared, no correlation/deadline/flow/metadata). Reuse that design, do not invent another.
- Principle (대표님): budget-first redesign, reuse existing features, never lower the target. Earlier job briefs are history, not instructions.
- Worktree /root/worktree/zlink-3868-ctrecv, branch perf/3868-cpp-target-path = the source-side change 875d773ef9 (keep it; do not revert it).

## Budget (supervisor design) — target user ≤ 2.3 µs/message
| Part | Now | Budget | Design |
|---|---:|---:|---|
| Core/binding receive + Framework port boundary | 2.047 | 1.45 | binding cost plus ≤ 0.12: no per-record lifecycle mutex or native size calls in raw_mesh_node_owner_t::receive_next (raw_mesh_node_owner.cpp:3602-3660); take the port once per receive turn, not per record. |
| Classify + owner-queue commit + claim | 1.007 + part of 2.912 | 0.25 | Ingress reads the service-wire header once and moves the record into the owner queue (the mailbox is the owner queue). Worker claim takes a batch from the queue's own storage; no per-batch make_shared claim object, no per-record settlement vectors; settle by count. Keep permit order and ready-owner rules. |
| Envelope header decode | 1.041 | 0.15 | Decoded-header cache with .NET parity (bounded, bytes compared, only headers without correlation/deadline/flow/metadata); a hit returns an immutable shared header plus the prepared route entry below. Miss path unchanged. |
| Route + DI + invoke + terminal | 1.357 + rest of 2.912 + 0.397 | 0.45 | Prepared route entry per (channel, message): handler invoker, typed deserializer, filter chain, DI factory, chosen once at registration or first use. One call chain from the worker drain to the handler: no host→spot→mesh_record_dispatcher→route_packet_dispatcher re-wrapping, no per-record std::function/closure/result_t re-wrap. If the handler task is already complete when it returns, finish terminal and permit inline without a coroutine frame; allocate the coroutine only on the suspend path. New DI scope per dispatch stays. |
| Total | ~9.5 | 2.3 | |

## Earlier receive-side results (read their reports' conclusions first; do not repeat failed directions)
- cdrain, cdrain2 (gate-less multi-worker drain, wake on take): send-saturation -11 to -12%, Spot p99 15x. So this job must not change worker count, wake rules, W5 backlog rules or the ready-owner set membership rules; step (c) changes only the claim/settle representation, and after step (c) also run unpaced send-saturation RUNS=3 main vs change to show no regression.
- ctrh (receive-role handoff §4.4) and crtc (handoff alone cannot reach 0.6): handoff placement is out of scope here; do not touch offload_executor role assignment.
- cplanr recommended resolving hosted/standalone dispatch at registration and transferring the existing terminal directly instead of a temporary shared terminal; that is part of step (b) here. Check whether any of it already landed on this branch and say so with file:line.
- cpoll (source completion batching) left receive-role CPU unchanged; it is the source side and not repeated here.
- No earlier C++ job tried a decoded-header cache or a prepared route entry.
You may read these folders' report.md only: cdrain/, cdrain2/, ctrh/, crtc/, cplanr/, cpoll/.

## Do
1. Read the current receive path end to end (raw_mesh_node_owner receive_next/enqueue_received, service_mailbox, public_host_runtime dispatch_application_claim, mesh_node_host_service dispatch_application, spot_node_runtime, mesh_record_dispatcher, route_packet_dispatcher, route_handler_invoker, application_job_queue permit). Write a decision-owner table (decision / spec clause / C++ site now / owner after) and count execution-resource switches from claim to handler first instruction now vs after.
2. Implement in this order, as four separate steps: (a) header cache + prepared route entry; (b) one call chain + inline terminal on ready handler; (c) owner-queue claim/settle without per-batch shared objects; (d) port boundary per receive turn. The supervisor commits each step separately, so after each step save the cumulative framework/ diff against 875d773ef9 as ctrecv/step-a.diff, step-b.diff, step-c.diff, step-d.diff and its tests' logs, then measure (same-rate CPU, change vs previous step and vs 875d773ef9, 3 runs each, csrc/scripts method). After each step report µs saved per part against the table with the 3-run range. A step whose effect is inside the run-to-run range stays only if it reduces code (lines, decision sites, wrappers); otherwise revert that step before starting the next and say so.
3. No new queue/lock/flag/counter unless an equal one is removed; give decision-site/lock/handoff counts before/after. Keep wire format, ordering, permit, gate, terminal exactly-once and error behaviour.
4. Tests, for every step (not only at the end): show with tests that a new DI scope is created per dispatch, completion order per owner is unchanged, and the handler exception path is unchanged — including the inline path for an already-completed handler task (sync success, sync exception, suspending handler, permit returned before the first instruction, terminal exactly once). Also new tests for the header cache (hit, miss for correlation/deadline/flow/metadata, bound). Run the affected unit/contract tests per step under load; full `-L 'unit|contract'` once at the end under load.
5. Final measurement: same-rate CPU (source and target, user and kernel) main vs change, 3 runs each; unpaced F÷B RUNS=5 main vs change vs binding. Report "over budget by X µs" for target user against 2.3 and F÷B against 0.6.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only these job folders under /root/jobs/fx/cx/v21/s033: ctgt/, csrc/, csrc2/ (reuse csrc/scripts and csrc/build-main for main). Write only in ctrecv/ and this worktree.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or bindings/.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare, linux-ninja-release preset, no -g (a separate RelWithDebInfo profile build only if you need symbols); keep ctrecv/ plus its build dirs under 30 GB (`du -sh` after each build). Tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: inside `flock -o /root/jobs/fx/cx/measure.lock`, only while measuring, log uptime, wait if 1-min load > 4 outside your run. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Keep every raw output file (bench JSON, /proc snapshots, perf) under ctrecv/raw/; do not delete them at the end.
- 리팩토링 점검: list duplicate checks/handoffs/copies/wrappers on the receive path with file:line and whether removed.
- Time box about 8 hours. If a step cannot meet its part of the budget, finish the step, measure, and report the gap with the reason; do not stop the remaining steps.

## Output: /root/jobs/fx/cx/v21/s033/ctrecv/report.md (Korean, plain sentences): (1) decision-owner table and switch counts; (2) per step what changed with file:line and µs saved per part; (3) tests with logs; (4) final same-rate CPU table and F÷B RUNS=5 with over-budget µs. Save the framework/ diff against 875d773ef9 as ctrecv/change.diff.
