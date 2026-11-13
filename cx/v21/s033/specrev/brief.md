# Job: review a spec change — gate-less owner multi-worker drain (0.33.0 #1639)

Worktree (read-only for you): /root/worktree/zlink-3868t
Change under review: /root/jobs/fx/cx/v21/s033/specrev/spec.diff (uncommitted in the worktree)
Spec root: framework/doc/framework/common/spec/server/ (01-execution/02, 04, 08 changed; ko and en)
Report: write /root/jobs/fx/cx/v21/s033/specrev/sol-review.md (absolute path; nowhere else)
Rules: no sub-agents or other models (no Astra). Do not edit the worktree. Do not build or run benchmarks.

## Why the change
C++ request-backpressure 1K is capped near 0.42 of the binding because one worker drains a gate-less
Node/ChannelName owner at a time (target drain 22.8 us/record, 0.94 core). Background:
/root/jobs/fx/cx/v21/s033/fdesign/design.md §3.3–3.4, /root/jobs/fx/cx/v21/s033/compare/compare.md.
The change lets several workers take records of one gate-less owner: the take from the owner-queue
head fixes start order (FIFO), the owner stays in the ready set while records remain, handlers run
side by side, completion order undefined, extra wake only when records remain and a worker is idle.
Owners with a gate are unchanged.

## Review questions
1. Does the change contradict any other clause in the spec tree (02 handler turn, 04 permits and ready
   set, 06 lanes, 07 serial executor layers, 08 §4.1/§4.4/§7, 00-foundation glossary and framework API
   scheduler bound, 03 cancellation/shutdown, relocation, session docs)? Cite file:line for each conflict
   and give replacement text.
2. Is anything observable to applications changed beyond what a gate-less handler could already see
   (a handler that suspends already overlaps the next one)? Check synchronous handlers, filters,
   DI scope rules, metrics, reply ordering on the same connection, ClientServer channel reply order.
3. Is the wording precise enough to implement the same way in C++, .NET, Java and Node? Is the
   wake rule ("only when records remain and a worker is idle, never once per record") sound and
   free of missed wakeups given the 04 ready-set recheck invariant?
4. Does the scheduler occupation bound (time budget per owner, 06-framework-api ~line 547) still mean
   something for a gate-less owner served by several workers? Propose text if not.
5. Verdict: approve / approve with listed edits / reject, with exact replacement text for every edit
   (ko and en).
