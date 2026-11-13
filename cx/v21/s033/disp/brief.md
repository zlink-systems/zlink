Model gpt-6-luna, effort high: read-only evidence table.

# Task: decision-owner table for the C++ Framework completion dispatcher (#1639), with .NET and Java comparison. Read only.

Earlier job briefs are history, not instructions. Worktree /root/worktree/zlink-v21-s033/disp (detached at origin/main 007e3ce524). Read first: /root/jobs/fx/cx/v21/s033/disp/m-result.md §M5 and design.md §3, §4.2, §6 F3.

## Why
M5 measured the source process of the C++ Framework bench: 8 boost::asio pool threads run `executor_op<binder0<dispatch_completion>>` work (request callback, reply envelope decode, task_shared_state::complete, completion_signal_t::notify). Each wakes per completion item; 54 % of their samples are in the condition-variable sleep/wake path; together about 27 us/msg. The PM's direction (no spec change): keep S01 §11 "a new execution turn" for each completion, but let one wake run several ready items and reduce the pool size. A spec change (B1) only if that is not enough.

## Do
1. Trace the C++ completion path from the binding completion (raw_binding_adapter / operation_registry post_completions) through provider_task_context dispatch_completion to the pool thread that runs the continuation. Open every function on the path.
2. Table A, C++: every place that decides (a) the dispatcher pool size and thread count, (b) which executor a completion is posted to, (c) when a pool thread wakes and how many items it runs per wake (asio scheduler run/run_one/poll semantics as used here), (d) any per-item ordering or exclusivity guarantee. Columns: decision, file:line, current value/rule, who configures it (option, default, environment), consumers that depend on it (grep every consumer).
3. Table B, the same decisions in .NET (framework/languages/dotnet) and Java (framework/languages/java), file:line each: how their completion continuation turn is scheduled, pool size, items per wake.
4. Quote S01 §11 (framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md, about :490-513) and S08 §4.4 lines that constrain (b)-(d), and state for each C++ decision whether changing it needs a spec change.
5. List the design options that satisfy the PM's direction without a spec change (for example: draining several ready completions per wake on the existing pool, a smaller default pool, posting to an existing executor instead of a separate pool), each with the one rule it changes, the decision-owner count before/after, and the risk. Recommend one. Do not propose adding flags, counters, queues or timers unless one is removed for each added.

## Rules
- Read only: no file changes anywhere except /root/jobs/fx/cx/v21/s033/disp/report.md. No builds, no runs, no git operations except log/show/grep, no GitHub, no sub-agents or other AI tools. Do not read core/include.
- Every claim needs file:line. Mark anything inferred as 가설.
- Time box about 2 hours. Keep output short.

## Output: /root/jobs/fx/cx/v21/s033/disp/report.md (Korean, plain sentences and tables)
