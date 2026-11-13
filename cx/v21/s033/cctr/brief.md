# Task: C++ request contract change — design study for the two changes the caller floor requires (read-only)

## Situation (read these first; do not repeat their measurements)
Framework ÷ binding must reach 0.6 (never lower the target). C++ request-backpressure 1KiB: caller budget 6.6 µs/request.
- /root/jobs/fx/cx/v21/s033/cfloor/report.md (and final.diff, experiment-source/) — the floor, measured on main 2f8b8e635a:
  V0 main 18.01 µs; V1 public request task/receiver kept, all Framework encode/framing/C1-C2 registration/owner turn skipped 9.63;
  V2 public call returns the binding's native awaitable, no Framework task state 6.77; V3 bench calls the binding directly 5.83.
  Per-layer split (driver / Framework call / task-coroutine / binding call / kernel-other) is in that report.
- /root/jobs/fx/cx/v21/s033/cdes/report.md — the earlier design study: no design reaches 6.6 within the current contract; it
  proposed a binding/Core "admission reservation, then publish the body later" feature and a change in when encode failures surface.
- /root/jobs/fx/cx/v21/s033/cproto/report.md and cpr2/report.md — steps already removed within the current contract (serial PR).
Conclusion so far: reaching 6.6 needs both (1) and (2) below together, leaving about 0.8 µs for all Framework work on the caller.

## Do (worktree /root/worktree/zlink-cctr, main 213e579f52 — read only; write this commit on the first line of the report)
1. Change (1): the public request call waits on the binding's completion directly, without Framework task state.
   - Exact proposed C++ public signatures (before → after) for request_to_node / request_to_channel and the call objects
     (framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp, channel.hpp), how timeout, cancellation,
     typed decode of the reply and error mapping are delivered without the task/receiver.
   - Impact on the other three languages (.NET, Java/Kotlin, Node): does the same public shape apply, or is it C++ only; name the
     public types each would change (file:line).
2. Change (2): binding/Core feature to move encode, framing and registration off the caller thread.
   - Exact proposed binding API (bindings/cpp public headers; describe the Core side only as behaviour, do not read core/include),
     how admission is decided before the body exists, how back-pressure (BACKPRESSURED → WRITABLE re-submit) and admission order
     are kept, which thread does the encode, and what happens when encode fails after admission.
   - Observable contract changes: when encode failures surface, admitted vs reply stage results
     (bindings/doc/spec/async-execution-model.ko.md:117-121), S01 01-submit-and-completion.ko.md §9–11, S08 08-messaging-hot-path.ko.md
     §4.4 (:300-328), 06 lane §5. Cite every clause that changes, by line.
3. Expected caller µs: starting from V2 6.77, per step what (1)+(2) leave on the caller, and the cost moved to other threads with
   the handoff cost; show the source process total is not worse than today at the same rate.
4. Also: the value actually reachable within the current contract (public task kept, Framework work done for real, not skipped
   like V1), with the steps that make it up.
5. Risks and a PR split (binding/Core first, then Framework per language), each with its own caller µs target.
Decision owners stay one each: C1 terminal, deadline, admission, typed delivery. Use binding features first; never re-implement
what the binding provides.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only: no file edits in the worktree, no builds, no measurements, no git operations, no spec edits, no GitHub operations.
  Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
- 리팩토링 점검: list which existing mechanisms each change deletes and confirm no new flag/counter/queue is added without a removal.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/cctr/report.md (Korean, plain sentences)
Start with: the recommendation, the expected caller µs table (V2 base → after (1)+(2)), and the list of user decisions.
