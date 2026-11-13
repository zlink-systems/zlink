Model gpt-6-luna, effort high: read-only investigation. Earlier job briefs are history, not instructions.

# Task: Node bench cells are unjudgeable because the client event loop saturates — find how to add clients

Worktree /root/worktree/zlink-v21-s033/r4190m (commit d3fd955131). Read only: no edits, no commits, no builds, no measurements.
Evidence: /root/jobs/fx/cx/v21/s033/r4190m/raw/aggregates/node.json judgements — every Node cell is "client-saturated at 1.00 of 1 declared event_loop_utilization (spec 5.1, G6)", for both zlink-node (binding) and zlink-framework-node.

## Answer, every claim file:line
1. Where the Node bench client(s) are launched (runner script, driver, run_local*.sh used by r4190m — see r4190m/report.md "검증 및 결과 위치" and raw/*-driver.log) and where "declared event_loop_utilization = 1" comes from.
2. Does the runner already support more than one client process or worker thread for Node (binding and Framework)? Which option/env, and does the aggregator G6 then use the summed capacity? How do the other languages (C++/.NET/Java) declare client capacity (submit_thread_cores = 8) — the spec section that defines it (spec 5.1).
3. If not supported: the smallest change that gives Node the same client capacity as the other languages for both binding and Framework rows (so the ratio stays fair), files to touch, and how G6 should count it. No implementation.
4. Whether the Node serial/backpressure/send-saturation server side (target) is also saturated in the raw (target CPU/ELU numbers).

## Rules
- Do NOT run scripts/dev/internal-docs.sh, any git pull/fetch/push, GitHub, sub-agents or other AI tools.
- Korean, short.

## Output: /root/jobs/fx/cx/v21/s033/n4190c/report.md
