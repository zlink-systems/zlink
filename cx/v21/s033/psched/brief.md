Model gpt-6.1-sol, effort high: changing worker scheduling (persistent owner drain, idle wake reservation, notification merging) on the C++ Framework's concurrent dispatch path needs the stronger model.

# Task: scheduler PR before step 2 of #1639 — persistent owner drain, idle wake reservation and notification merging, on the current claim model

Earlier job briefs are history, not instructions. Read first:
- /root/jobs/fx/cx/v21/s033/p2/report.md — the step-2 job stopped because this scheduler piece is missing; its section 0 explains exactly why (offload_executor one-shot items, ready callback per head, S04/S08/S02 conditions). This job builds that missing piece.
- The approved design /root/jobs/fx/cx/v21/s033/cpath/report.md: §4 intro (lines 119, 125), §6b (lines 275–288: cdrain, cdrain2, csendb rows), §7 rows 2 and 7 (line 296, 301).
- /root/jobs/fx/cx/v21/s033/cdrain/report.md (especially :25–34) and the cdrain2 job folder /root/jobs/fx/cx/v21/s033/cdrain2/ if it has a report: these tried persistent source, idle wake and worker budget moves and regressed send −11.4% and Spot p99 501→7,586 µs. Find in their reports which part caused the regression and avoid it; say in your report (section 0) what you do differently, with file:line, before you write code.
- Spec: S04 §3 :209–218, S08 §4.3 :243–247, S02 §2 :97–104 (framework/doc/framework/common/spec/server/01-execution/).

## Scope (PM decision, 2026-11-10)
- Add a persistent owner drain entry: a ready Node/Channel owner stays scheduled so one work item processes several records (S08 §4.3), instead of one executor item per ready callback.
- Idle wake reservation: when a record is published and an idle worker is available, reserve it once; merge notifications so a burst of N records does not create N items or N wakes (S04 §3).
- Keep independent execution: when handler A on a Node blocks synchronously, record B on the same Node must still get a worker (S02 §2 :97–104).
- It must work on the current claim model (step 1 is in this branch; the claim/batch/settlement removal is step 2 and comes later — do not remove claims here).
- Forbidden (cdrain's approach): more workers, moving worker budget, one executor item per record, retry or timeout changes. Coroutines stay; do not replace any coroutine with a callback or a hand-written state machine. No spec edits.
- Reuse binding features; do not reinvent what the binding provides.

## Tests (each must fail on base or with a negative control, then pass)
- Count test: executor items and wake notifications per message for a burst of N records on one Node (expected: well below N, state the bound), with negative controls for one item per record and one wake per record.
- Independence test: handler A blocks synchronously; B on the same Node completes on another worker. Negative control: drain that waits for A's terminal must fail it.
- Shutdown while the drain entry is scheduled, while a handler runs, and with records queued; Spot/Actor gate tests unchanged and their negative controls still fail when the gate is removed.
- Full C++ Framework unit and contract suite (non-install entries) with pass/fail counts and logs; the dispatch/mailbox/Spot tests 20 times under `stress-ng --cpu 16 --cpu-load 80` via /root/jobs/fx/cx/measure-shared.sh.

## Build, test and measurement
- The PM builds and runs the tests on their machine. Here, a Core release gate is measuring until /root/jobs/fx/cx/v21/s033/core124b/verdict.md exists: until then do NOT build, test or take any lock; write code and tests only, then stop and report "ready to compile" with the list of touched files. If verdict.md exists, build with `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` called bare (linux-ninja-release preset, no -g, -k 0 for all affected targets), test via /root/jobs/fx/cx/measure-shared.sh, and keep psched/ plus build dirs under 25 GB.
- Measurements (only after verdict.md exists and the tests pass; the supervisor may run them instead): inside `flock -o /root/jobs/fx/cx/measure.lock`, MemAvailable >= 12 GiB, source CPUs 0-7, target 8-15, base 26d5c811c7 vs change, 3 runs each: Spot request p99 1024 (median), send-saturation 1024 throughput (median), and target Core I/O thread µs/message per TID (schedstat, as /root/jobs/fx/cx/v21/s033/xio/raw/collect-*.py does). Any median worse beyond the run-to-run spread is a failure; say so plainly.

## Rules
- Read only the worktree /root/worktree/zlink-v21-s033/psched (branch perf/1639-cpp-sched-drain at 26d5c811c7) and the job folders named above. Write only in /root/jobs/fx/cx/v21/s033/psched/ and the worktree.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; do not commit. No GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or bindings/.
- SIGTERM/SIGINT only; no background jobs with &. Keep every raw output under psched/raw/; never delete them.
- Before you report "ready to compile", count brackets and arguments in every changed call by reading the diff once more line by line.
- Run `bash scripts/format/format.sh --check cpp` in the worktree and fix what it reports.
- 리팩토링 점검: list the ready-callback, executor submit, wake and drain sites on the Node/Channel path with file:line, which this change merged or removed, and before/after counts of executor items and wakes per burst.
- Time box about 8 hours. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/psched/report.md (Korean, plain sentences)
(0) difference from cdrain/cdrain2 with file:line; (1) what changed with file:line; (2) decision-owner table and before/after counts; (3) tests with negative controls (and logs if run); (4) measurements if run; (5) 리팩토링 점검. Save the diff against 26d5c811c7 as psched/change.diff.
