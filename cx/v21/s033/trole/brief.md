Model gpt-6.1-sol, effort high: DIAGNOSIS AND DESIGN ONLY — no product code changes (PM: 진단 먼저, 구현은 승인 뒤). Earlier job briefs are history, not instructions.

# Task: C++ target receive-role handoff (S08 §4.4) so replies leave together (#1639)

Worktree /root/worktree/zlink-v21-s033/trole, branch perf/1639-cpp-target-receive-role at origin/main 76a4d78506. Do not push or open a PR.

## Why
On the target, the receive thread publishes each record to an application worker (receive → handler handoff), so replies are produced one by one on different workers and leave in more, smaller TCP packets than the binding (creply/report.md: Framework 0.886 data packets/request vs binding 0.683–0.693). 22k base target: app-worker ≈ 22 µs/msg, receive ≈ 15 µs/msg, target process ≈ 66 µs/msg, target sendto 0.79–0.83/msg (bstage/report.md, TID-union numbers). S08 §4.4 allows the receive role to pass to an idle worker so the former receiver runs handler work it already has; C++ does not implement it (chand/report.md: 0 attempts).

## Read first (all under /root/jobs/fx/cx/v21/s033/)
- chand/report.md §3 alternative A and §4 acceptance conditions (written for source completion; apply the same rules to target receive → handler).
- cadvice/report.md (receive-role transfer recommendation: small placement experiment with a stop condition before any executor rewrite).
- cpath/report.md §4.2 (R1–R10, the "회전·통지 분담" row and the paragraph after the table) and §7 rows 2 and 7.
- p2/report.md (why per-record executor resubmission is forbidden: S04 §3 :209–218, S08 §4.3 :243–247).
- cdrain2/report.md (multi-drain failed: send-saturation −12.5 %, Spot p99 15×). Do not repeat backlog comparison, helper search, waking all workers, or per-record wake.
- Spec: framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md §4.4 (every table row), 04-application-job-queue-and-backpressure.ko.md §3, 02-handler-turn-and-execution-gate.ko.md §2–3 and §13, 01-submit-and-completion.ko.md §11.

## Do (design only; do not edit product code, do not build)
1. design.md with:
   a. S08 §4.4 clauses that apply, each quoted with file:line.
   b. Every place in C++ that currently decides who holds the receive role and where handler work is placed (file:line), e.g. receive rotation in mesh/raw_mesh_node_owner.cpp and mesh/mesh_node_host_service.cpp, the ready callback → offload executor submit, idle membership in dispatch/offload_executor.*.
   c. Cross-language counterparts with file:line: the .NET receive-role handoff branch at commit 6821c3b8af (read with `git show 6821c3b8af` / `git show 6821c3b8af:<path>` from /root/zlink), Java (zlink-framework-core) and Node.
   d. The single owner after the change, decision-owner table before/after, rule count before/after (no new flags, counters, timers or queues unless one is removed).
   e. The proposed algorithm: once per receive rotation, non-waiting assignment to an already-idle, already-created worker; failure keeps today's path; the former receiver runs its taken records through the normal queue/gate/permit entry only after reception has been handed off; at most one concurrent receiver; no new thread or start-confirmation wait. Map every S08 §4.4 row and chand §4 acceptance condition to how it is kept and which test would show it.
   f. Test plan and measure plan (handoff success/rotation, target sendto/msg, app-worker+receive µs/msg vs same-session base, stop condition).
2. Stop after design.md and a short report.md. The supervisor sends the table to the PM for approval before any implementation.

## Rules
- No GitHub, no sub-agents or other AI tools. Every claim file:line. Keep raw logs.
- Report in Korean, short: design summary, owner/rule counts, diff summary, tests with counts, risks left.

## Output: /root/jobs/fx/cx/v21/s033/trole/design.md and report.md
