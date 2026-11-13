# Task: C++ Framework send-saturation 1KiB — budget breakdown and smallest structural removal (#3868, measurement-only)

## Situation
Last measurement (main 213e579f52+, condition B, 5 runs): C++ send-saturation 1KiB Framework 181,862 msg/s vs binding 600,544 → F÷B 0.303. Binding 1.665 µs/msg, so the Framework budget at 0.6 is 2.775 µs/msg; Framework is 5.50 µs/msg → over budget by about 2.72 µs/msg. 4KiB: 157,779 vs 278,282 (0.567).
Directive: budget-first structural redesign, not hotspot trimming. Use binding features; never reinvent what the binding provides.
Prior C++ studies with reusable method and tools: /root/jobs/fx/cx/v21/s033/cprof (request-path stage profile), cthin, ccut, cfloor (floor variants: public path kept with Framework work skipped, native path, bench calling binding directly), cpr2/report.md (merged #4014: request header encoded once).
Worktree: /root/worktree/zlink-csend = main d961e51e23 + CPU-pin patch.

## Do
1. Fix the actual send path of one bench send-saturation message with file:line, from the bench call to the binding send, and on the target from binding receive to the Framework handler. Name the thread of each step.
2. Per-message CPU split for the critical thread(s) (the one that limits throughput) on source and target, Framework vs binding, same session: Framework bookkeeping, encode/serialize, allocation, copies, binding/Core calls, wakeups/handoffs, other. Also total CPU per message across both processes (user+kernel) at the same rate (pace the binding to the Framework rate) — CPU per message is rate-dependent, so compare only at equal rate.
3. Floor: like cfloor, measure (a) bench calling binding send directly, (b) public Framework send with Framework work skipped where possible, so the reachable floor of the current public shape is known.
4. Budget table: list each Framework-only step with µs/msg and say which ones must go to get under 2.775 µs. Propose the smallest structural change (binding feature it reuses, decision owners before/after) and, if under 2 hours, prototype it in this copy and measure unpaced RUNS=5 1KiB and 4KiB with the binding in the same session.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/. Changes only in framework/ and bench files of this worktree (measurement copy, never committed).
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare (no outer flock). Other builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: `flock -o /root/jobs/fx/cx/measure.lock` (exclusive; wait if held — other measurements are queued), condition B (source CPUs 0-7, target 8-15).
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: list duplicate work (same check/encode/copy/handoff twice) you see, with file:line, and whether your prototype removed it; no new queue, lock, cache, flag or state in any prototype.
- Report "over budget by X µs" per message, never as a percentage of the target. Do not suggest lowering the 0.6 target.
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/csend/report.md (Korean, plain sentences): (1) path table; (2) CPU split and same-rate total CPU; (3) floor; (4) budget table, proposal, prototype numbers. Save any prototype as csend/prototype.diff.
