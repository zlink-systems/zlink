Model gpt-6-luna, effort high: read-only evidence table. Earlier job briefs are history, not instructions.

# Task: decision-owner table for the C++ Framework completion dispatcher (#1639), and how S01 §11 "new execution turn" stays true

Worktree /root/worktree/zlink-v21-s033/disp (detached at origin/main 56f8984468). Read first: /root/jobs/fx/cx/v21/s033/disp/m-result.md §M5 and /root/jobs/fx/cx/v21/s033/disp/design.md §3, §4.2, §6 F3.

## Why
M5 measured the source process of the C++ Framework bench: 8 boost::asio pool threads run `executor_op<binder0<dispatch_completion>>` work (request callback, reply envelope decode, task_shared_state::complete, completion_signal_t::notify). Each wakes per completion item; 54 % of their samples are in the condition-variable sleep/wake path; together about 27 us/msg. PM direction (no spec change): keep S01 §11 "a new execution turn" for each completion, let one wake run several ready items, and gather the completion-delivery decision in one owner. Implementation comes only after the PM approves the table.

## Do
1. Trace the C++ completion path from the binding completion (raw_binding_adapter / operation_registry post_completions) through provider_task_context dispatch_completion to the pool thread that runs the continuation. Open every function on the path.
2. Table A, C++ (the PM's three columns first): decision | every place that decides it now (file:line, current rule) | the one owner it should move to. Decisions: (a) pool size / thread count, (b) which executor a completion is posted to, (c) when a pool thread wakes and how many items it runs per wake (asio run/run_one/poll as used here), (d) per-item ordering or exclusivity guarantees, (e) who signals the waiting continuation (completion_signal_t, task_shared_state::complete). Add who configures it (option/default/env) and every consumer (grep them all).
3. Table B, same decisions in .NET (framework/languages/dotnet) and Java (framework/languages/java), file:line each.
4. S01 §11 "new execution turn": quote the lines (framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md, about :490-513) and S08 §4.4 lines constraining (b)-(e). Write exactly what "new turn" means in testable terms (not on the caller's stack? not inline in the completer? may several completions share one wake if each runs as its own turn?), and state how each design option keeps it, with the existing test that checks it (file:line) or "no test".
5. Design options that need no spec change (e.g. one wake drains several ready completions on the existing pool, a smaller default pool, posting to an existing executor instead of a separate pool): for each, the one rule it changes, decision-owner count before/after, rule count before/after, risk. Recommend one. Do not add flags, counters, queues or timers unless one is removed for each added; no time-based rules.

## Settle with source, not 가설
- Whether one asio pool thread runs several ready handlers per wake, and when post() wakes an idle thread (cv signal / wake_one_thread_and_unlock): read /root/.cache/zlink/vcpkg/packages/boost-asio_x64-linux/include/boost/asio/detail/impl/scheduler.ipp (do_run_one, post_immediate_completion, wake_one_thread_and_unlock) and the matching posix_event/conditionally_enabled_event code, cite file:line, and say how this explains M5's 54 % sleep/wake share at ~32k completions/s over 8 threads.
- A previous attempt (summary /root/zlink/.artifacts/codex/disp2/summary.md) left this as 가설 and wrote only 15 lines; produce the full tables this time.

## Rules
- Read only: no file changes anywhere except /root/jobs/fx/cx/v21/s033/disp2/report.md. No builds, no runs, no git operations except log/show/grep, no GitHub, no sub-agents or other AI tools. Do not read core/include.
- Every claim needs file:line. Mark anything inferred as 가설.
- Time box about 2 hours. Keep output short. Write report.md once at the end.

## Output: /root/jobs/fx/cx/v21/s033/disp2/report.md (Korean, plain sentences and tables)
