# Task: C++ prototype — Core + C++ binding "reserve admission first, publish the body later" request, measured (measurement-only)

## Situation (read first; do not repeat their measurements)
- /root/jobs/fx/cx/v21/s033/cctr/report.md — the design. Read the sections "변경 (2)의 binding 공개 API 제안", "Admission과 body의 순서",
  "Size bound와 credit", "BACKPRESSURED와 순서", "Caller에서 이동하는 CPU와 source 전체 수지", and PR 1 in "위험과 PR 분할".
  PR 1 = Core and C++ binding staged request: the caller reserves the byte credit and the request completion slot in Core's existing
  record; an existing worker builds the body later and publishes it into the same record; credit difference is returned.
- /root/jobs/fx/cx/v21/s033/cfloor/report.md — caller floor: V3 (bench calls the binding directly) 5.831 µs/request, condition B,
  request-backpressure 1KiB. That is the bar the staged path must keep.
- /root/jobs/fx/cx/v21/s033/cbind/report.md — CPU per request depends on rate; compare CPU only at equal rate (pacing method there).

## Questions to answer with numbers
1. Can Core do this? Show from Core source (core/src) where admission credit, HWM accounting, the outgoing record queue and request
   completion slots live, and how the prototype reserves and later publishes into the same record without a second queue or counter.
   If it cannot be done without one, say exactly what blocks it (file:line) and stop after step 2's measurement of whatever subset works.
2. Caller µs: the binding bench's request-backpressure 1KiB caller (submit_thread_cores × 1e6 / throughput from results.json) for
   (a) main binding request, (b) staged request where the caller only reserves and a worker builds the 1 KiB body (the same bytes the
   bench sends today) and publishes it. Target: (b) ≤ 5.831 µs and not worse than (a).
3. Same-rate source process CPU: pace (a) to (b)'s rate (or both to a common rate) and compare source process CPU µs/request
   (user+kernel, all threads), split by thread (caller, worker, Core I/O). State whether (b) ≤ (a).
4. Back-pressure: with a small HWM, show BACKPRESSURED → WRITABLE re-submit still works for staged requests, order of publication
   equals order of reservation, and a body that fails to build after reservation completes the reply with a failure and returns credit.
   Tests may be prototype-only (in the worktree), not product tests.

## Do
Worktree /root/worktree/zlink-cstage (main 213e579f52 + CPU-pin patch). Measurement-only copy; never committed; deleted after.
- You may modify core/src, bindings/cpp and framework/bench/grpc/cpp in this worktree. Put any new Core declaration the binding needs
  in a new header under core/src (do not open or edit files in core/include). Keep the change minimal; reuse Core's existing queue,
  credit and completion structures. No new poller or thread except the one worker that builds bodies (reuse an existing executor if
  the binding has one).
- Build the local package (Core + C++ binding) and the bench: C++ compile steps through `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>`
  (no outer flock); other shared steps through `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Package script:
  `scripts/local-package/build-wsl.sh cpp` from the worktree root; bench: `bash cpp/build.sh` from framework/bench/grpc.
- Measure under `flock -o /root/jobs/fx/cx/measure.lock` (exclusive; wait if held), condition B (source CPUs 0-7, target 8-15),
  warmup 30 s, active 5 s for caller runs with RUNS=5 (median and range), and a 60 s active run for the same-rate CPU comparison.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read or edit core/include.
  Do not run scripts/dev/internal-docs.sh.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: list which existing Core/binding state the staged record replaces, and confirm no second queue, counter or flag was
  added beside the existing ones (or say exactly which one had to be added and why).
- Time box about 5 hours.

## Output: /root/jobs/fx/cx/v21/s033/cstage/report.md (Korean, plain sentences)
Start with one table: (a) vs (b) caller µs (median, range), throughput, same-rate source CPU by thread; then the Core feasibility
answer with file:line; then back-pressure/order/failure results; then files changed. Save the diff as cstage/prototype.diff.
