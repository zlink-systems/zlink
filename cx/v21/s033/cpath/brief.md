Model gpt-6.1-sol, effort high: a whole-path design across send, receive and request in the C++ Framework, checked on paper against a per-message budget, with spec clauses weighed against µs; this needs the stronger model.

# Task: C++ Framework per-message path design from the binding budget (#1639) — paper design only, no code changes

Earlier job briefs are history, not instructions.

## Why this job exists
대표님 asked three times for budget-first redesign. Earlier C++ jobs split the budget by today's stages (receive, classify, mailbox, header decode, route, completion) and shrank each stage while keeping the structure. Results stayed far over budget: source user 3.958 µs/message against 2.5 (s033/csrc), target user 6.775 against 2.3 (s033/ctrecv). Shrinking a stage never asks whether the stage should exist. This job designs the path starting from what the binding does per message, and adds Framework work only where a contract requires it.

## Inputs (recompute every number you use from these raw files; do not copy numbers from this brief)
All inputs are copied under /root/jobs/fx/cx/v21/s033/cpath/input/ (large binary profiles left out; text kept):
- Unpaced Framework and binding throughput, C++, 6 cells, RUNS=5: input/ratio051pr2/ratio051pr2/ (log/after/ and table.md).
- Request-backpressure 1 KiB caller-thread cost and allocation sites: input/ccall2/ccall2/ (report.md, analysis.json, profile-summary.json).
- Send-saturation 1 KiB same-rate source/target CPU and stage attribution: input/csrc__raw, input/ctgt__raw with input/ctgt__report.md, input/ctrecv__raw with input/ctrecv__report.md.
- Earlier directions that did not reach the budget (read conclusions; do not repeat them without saying why the new design differs): input/cdrain__report.md, input/cdrain2__report.md, input/csendb__report.md, input/csasm__report.md, input/pr3995-body.md (.NET #3995), input/ctrecv__report.md, input/csrc__report.md, input/csubmit__report.md.
- Other-language Framework throughput: input/ratio053java__table.md, input/ratio054net__table.md (request cells), input/ratio040__ratio040-24cells.md (send cells; older session).
- Code: /root/worktree/zlink-v21-s033/cpath (detached at origin/main 532cd83b67). Read framework/languages/cpp, bindings/cpp, framework/bench/grpc/cpp, and the spec under framework/doc/framework/common/spec. For other-language comparison you may read framework/languages/dotnet and framework/languages/java.
Read nothing else under /root/jobs. Write only in /root/jobs/fx/cx/v21/s033/cpath/ (not under input/).

## Budget definition
For each of the 6 C++ cells (send-saturation, request-backpressure, request-serial × 1024, 4096):
- Throughput budget = 0.6 × binding throughput. Per-message time budget on the bottleneck thread = 1e6 / (0.6 × binding msg/s).
- Framework share = that budget minus the binding's own per-message cost on the same thread (from the raw CPU data). State which thread is the bottleneck in each cell and how you know.
- Also give the process CPU budget per message (source and target, user+kernel) = binding CPU per message / 0.6, and the Framework share of it.
- Absolute goal (PM 11-09): C++ Framework throughput in each cell must also be ≥ the fastest other-language Framework in that cell. Use the other-language tables in input/; mark them as earlier sessions, not same-session numbers. Report which bound (0.6 or absolute) is tighter per cell.

## Design method (required)
1. Binding baseline: list every operation the C++ binding does per message for send, receive, and request/reply, with file:line. This is the floor.
2. Duplication table: for each Framework mechanism on the path, the binding feature that already provides it (if any), with file:line on both sides. Framework = binding calls + only the contract the binding lacks. Reuse binding features; do not reinvent them.
3. Proposed path: per message, list each Framework operation that remains, the spec clause that requires it (doc file:line and § number), an other-language Framework implementation that does not do this work per message if one exists (.NET or Java file:line, or "none found"), and its estimated µs with how you estimated it (measured stage cost from the raw data, or an instruction/allocation/atomic count argument). Everything else moves to bind/registration time or is amortized over a batch; say where it moves.
4. Do not keep a structure because it exists today. Today's mailbox, per-dispatch scope, completion slots, shared state, worker/wake rules and owner lanes are all candidates for removal; each one either has a clause that requires it per message, or it goes.
4b. Coroutines stay (대표님 2026-11-10: "코루틴을 없애면 안되지"). The C++ Framework keeps coroutines as its execution and API model: public awaitable APIs, handler coroutines and the internal coroutine-based flow are not replaced with callbacks or hand-written state machines. What the design may change is the cost each coroutine carries per message: frame allocation (pooled or recycled frames, allocator-aware promise), task shared state, observer/callback owner blocks, extra std::function or closure wrappers around a coroutine, and resumption hand-offs. Measure coroutine cost as its own row (frames per message, bytes, allocation and resume µs). If you think an internal nested coroutine layer should be merged into its caller, list it separately as a question with its µs; do not count it in the paper sum.
5. Paper sum per cell vs the Framework share. If the sum fits, say so. If it does not, list the clauses that block it, each with the µs it costs per message and the design that would follow if the clause changed. Do not edit the spec; the list goes to 대표님 for a decision.
6. Earlier failed directions table: one row each for cdrain, cdrain2, csendb, csasm and .NET #3995 — what it changed, its measured result, why it missed — and the column "does this design repeat it? if it touches the same area, how it differs". cdrain2's multi-worker drain hurt send-saturation and Spot p99; if the design touches worker count or wake rules, say how it avoids that.
7. Implementation plan only after the paper sum: ordered steps, each with the µs it is expected to remove and the test that counts allocations/locks/thread hand-offs per message (a PR fails if the count exceeds the design).

## Rules
- The PM reviews every line of report.md before any implementation, and blocking clauses go to 대표님 for a decision first. Do not implement anything.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No code changes, no builds, no tests, no measurements, and do not take /root/jobs/fx/cx/measure.lock. This is a reading and design job.
- No git operations except git log, git show, git diff, git status, git blame (read-only). Do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify anything under the worktree.
- Keep raw files you produce (scripts, intermediate tables) under cpath/; do not delete them or anything in input/.
- 리팩토링 점검: list duplicate checks, hand-offs, copies and wrappers on the send, receive and request paths with file:line, and whether the design removes each.
- Time box about 6 hours. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/cpath/report.md (Korean, plain sentences)
(1) budget table per cell with both goals together: F÷B ≥ 0.6 and ≥ the fastest other-language Framework in that cell (absolute msg/s, tighter one, Framework share, bottleneck thread); (2) binding baseline per message; (3) duplication table; (4) proposed per-message path with clause and µs per operation; (5) paper sum vs budget per cell; (6) blocking clauses with µs and the design if changed; (6b) earlier failed directions table; (7) implementation steps with expected µs and count tests; (8) 리팩토링 점검 table.
