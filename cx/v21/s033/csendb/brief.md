Model gpt-6.1-sol, effort high: this is a structural change to the target record ownership and execution of the C++ Framework, with cross-thread ownership and close/cleanup paths to keep correct.

# Task: C++ send-saturation structural change — target records run independently, source assembles the final buffer once (#3868)

## Situation
Read /root/jobs/fx/cx/v21/s033/csend/report.md fully. Reuse its scripts/, floor tools and pacing.diff; do not redo its analysis.
It found: C++ send 1KiB Framework÷binding 0.285, over budget by 2.848 µs/message. Even with the source Framework skipped (direct floor) it is over budget by 2.094 µs, because the target application worker is one serial resource: a whole Node owner drain runs one at a time (framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:158,181,453), 4.7-6.0 µs/message.
Recommended change B (report §4): keep the public API; on the target take each record from the existing FIFO exclusively and let the existing application workers run records independently (Node contract in the Framework spec S02 §2; Spot and Actor gates stay); on the source wire the existing `application_envelope_encoder_t` / `take_application_payload()` into send so the header is written into the final service payload buffer once (generalise the header kind, do not reuse the request-only kind).
Trap from the report: removing `active_messages != 0` alone is wrong; the claim points at the FIFO prefix (service_mailbox.cpp:199,207,409), so another worker could claim the same prefix. The claim must own its record, with credit, terminal and close attribution moved with it.
Worktree: /root/worktree/zlink-3868-csendb = main a0056577c4 + CPU-pin patch.

## Do
1. Read the Framework spec for Node execution and mailbox ownership (rg in framework/doc/spec for the S02 handler turn/execution gate document, S05 payload ownership, S08 messaging hot path) and quote the lines that allow independent Node record execution and those that require FIFO/credit order. If the spec does not allow it, stop and report.
PM approved this change (spec 01-execution/02 §2: the Node path has no execution gate, FIFO applies only to record take). Conditions: the claim owns its record exclusively, zero duplicate prefix claims, decision-site count 5→1, no new queue or lock.
2. Implement the target part first (record-owned claim, independent execution, credit/terminal/close kept), then the source part. Reuse existing types; no new queue, lock, cache, timer or flag.
3. Tests: existing C++ unit and E2E targets for mailbox, Node/Spot/Actor dispatch, close, taken-before-entry, BACKPRESSURED, async handler cleanup; add a test that two records for the same Node run at the same time on two workers, a test that the same FIFO prefix is never claimed twice under concurrent workers, and a negative test that a gated Actor still runs one at a time. Then run the full C++ ctest unit and contract suites (PM condition) and record their logs.
4. Measure, binding in the same session: unpaced RUNS=5 send-saturation 1KiB and 4KiB, request-backpressure 1KiB, request-serial 1KiB, main vs change; plus total CPU per message of both processes at an equal paced rate.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/. Changes only in framework/ and bench files of this worktree.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare (no outer flock). Other builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: `flock -o /root/jobs/fx/cx/measure.lock` (exclusive; other measurements are queued, keep waiting), condition B (source CPUs 0-7, target 8-15).
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: list duplicate work (same check/encode/copy/handoff twice) you see, with file:line, and whether your change removed it.
- Report "over budget by X µs" per message (budget = binding µs / 0.6), never as a percentage. Do not suggest lowering the 0.6 target.
- Time box about 6 hours; time spent waiting for the measurement lock does not count.

## Output: /root/jobs/fx/cx/v21/s033/csendb/report.md (Korean, plain sentences): (1) spec quotes; (2) what changed, file:line, decision-site count before/after; (3) tests run with log paths; (4) RUNS=5 table with Framework÷binding and over budget by X µs. Save the framework/ diff as csendb/change.diff and bench edits separately.
