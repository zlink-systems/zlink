# Task: Node Framework send-saturation 4KiB and borderline cells — budget breakdown and smallest structural removal (#3872, measurement-only)

## Situation
Node send-saturation 4KiB is F÷B 0.597, and several Node cells are borderline (0.601–0.620). Previous Node studies with reusable method and tools: /root/jobs/fx/cx/v21/s033/nprof (4096-byte stage profile for #3872), nseg, nseg2 (segment timing), nfix. Read them first; re-measure on current main because main moved.
Directive: budget-first structural redesign, not hotspot trimming. Use binding features; never reinvent what the binding provides.
Worktree: /root/worktree/zlink-nsend = main d961e51e23 + CPU-pin patch.

## Do
1. Re-measure all Node cells (send-saturation, request-backpressure, request-serial × 1KiB, 4KiB) on current main, Framework and binding same session, RUNS=3, median and range. Mark which are below 0.6 and which are within ±0.02 of it. Give each its budget (binding µs/msg ÷ 0.6) and "over budget by" or margin.
2. For send-saturation 4KiB: fix the path with file:line (bench call → Framework → binding send; target binding receive → Framework handler) and the thread/event-loop turn of each step.
3. Per-message CPU split of the limiting thread (event loop) on source and target, Framework vs binding: Framework bookkeeping, encode/serialize, Buffer copies and allocations (bytes per message), binding calls, promise/microtask turns, GC. Total CPU per message across both processes at the same rate (pace the binding to the Framework rate).
4. Budget table: Framework-only steps with µs/msg; which ones must go. Propose the smallest structural change (binding feature it reuses, decision owners before/after); if under 2 hours, prototype it here and re-measure the affected cells RUNS=5 with the binding in the same session.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/. Changes only in framework/ and bench files of this worktree (measurement copy, never committed).
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare (no outer flock). Other builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: `flock -o /root/jobs/fx/cx/measure.lock` (exclusive; wait if held — other measurements are queued), condition B (source CPUs 0-7, target 8-15).
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: list duplicate work (same check/encode/copy/handoff twice) you see, with file:line, and whether your prototype removed it; no new queue, lock, cache, flag or state in any prototype.
- Report "over budget by X µs" per message, never as a percentage of the target. Do not suggest lowering the 0.6 target.
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/nsend/report.md (Korean, plain sentences): (1) all-cell table with budget; (2) path; (3) CPU split and same-rate total CPU; (4) budget table, proposal, prototype numbers. Save any prototype as nsend/prototype.diff.
