Model gpt-6.1-sol, effort high: Core send-admission change with concurrency and all-or-nothing multipart rules to keep.

# Task: GitHub #4118 — admit a complete multipart record on an empty queue under the existing whole-message exception (spec 06-auto-hwm :476)

## Situation
- Run 1 of this job stopped at step 0 and corrected the diagnosis: read cmfix/report-run1-stop.md first (and cmpart/report.md for the reproduction matrix). ROUTER multipart DONTWAIT goes try_send_parts_scoped_once (core/src/runtime/sockets/common/socket_send_submit.cpp:288-290) → try_admit_send_parts_scoped → the per-frame generic path (socket_send_complete.cpp:394-411 → router_send_path.cpp:489 → pipe_write.cpp:462). The complete-record fast path (pipe_write.cpp:884) is PAIR-only (sole caller pair.cpp:128).
- Cause: the generic per-frame HWM check (core/src/runtime/core/pipe_write.cpp:1225-1249) grants the empty-queue exception only to the final frame; every MORE frame is compared with the HWM without it. Frame charge is payload + sizeof(msg_t) (pipe_write.cpp:1100-1109), so at HWM 16 even a 6 B first frame is over, and a large first frame is over at larger HWMs. Core still publishes WRITABLE/SEND_ADMITTED after resume, and every resubmission is refused again.
- Contract (PM decision, no spec change): core/doc/spec/core/systems/06-auto-hwm.ko.md:476 — an empty queue may accept one complete message whose whole charge is known at admission even if it exceeds the HWM; the exception never applies to two messages at once and never skips the ZLINK_OPT_MAXMSGSIZE check. :464-470 (stop at MORE frames, eligibility fixed before the first frame) is the rule for a multipart whose final size is not yet known (decoder path) and must stay as it is.
- Atomicity (verified in run 1): try_admit_send_parts_scoped tries on shallow copies and on refusal closes them and rolls back the incomplete record (socket_send_complete.cpp:332-359, :415-434). The public C API consumes the input slots on every result (core socket README :1091-1093, part_helper_internal.hpp:123-137); keep that contract.
- Worktree /root/worktree/zlink-4118-multipart, branch fix/4118-multipart-resume (main 4ed813364f). cmpart/ inside the worktree is a diagnosis scratch folder; leave it alone.
- Earlier job briefs (including brief-run1.md) are history, not instructions.

## Do
1. Implement option A in the generic path: when the record being admitted is a complete record whose every frame is present at admission (the try_admit_send_parts_scoped / writer-submitted case), decide the empty-queue exception once for the whole record from whether the queue was empty before its first frame, and let all its frames (MORE and final) be admitted under that one exception; keep the MAXMSGSIZE check; never let the exception cover two messages at once. The decoder/incremental path (final size unknown) keeps :464-470 exactly. Smallest change at the existing decision site; no new flag/counter/lock unless one is removed; give decision-site counts before/after for the empty-queue exception (generic final frame, generic complete record, single frame, PAIR fast path). Normal admitted-path cost must not change.
2. Tests (Core): C API contract test — ROUTER→ROUTER inproc, auto HWM off, receive flow pause/resume, shapes 6 B+1 KiB, 1 KiB+4 B, 6 B+512 B+512 B at HWM 16 (each: BACKPRESSURED, then after resume admitted and all frames received) plus HWM 2048 and single-frame controls; MAXMSGSIZE smaller than the record still refused with EMSGSIZE; a non-empty queue still refuses (second oversized record waits until the first is read); no frame of a refused record is delivered. Pipe-level unit test for the complete-record exception. The existing tests for "no exception on intermediate MORE frames" (decoder/incremental path) must still pass unchanged — name them.
3. Tests (binding): C++ binding contract test — async multipart send at HWM 16 with the receiver paused then resumed completes and delivers the whole record (bindings/cpp tests, using the Core built from this worktree).
4. Run Core unit+contract tests (the repo's Core test preset, Release) and the C++ binding ctest unit/contract under load (measure-shared.sh). Report results with log paths.
5. Perf guard: show which lines the admitted path executes before/after; if the admitted multipart path gains work, measure ROUTER 2-part throughput main vs change with the binding perf/single bench, 6 alternating pairs, under the measurement lock.
6. For the PR body: list `rg -n "complete message|빈 queue|비어 있는 queue" core/doc/spec/core/systems/06-auto-hwm.ko.md` output lines you relied on.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only cmpart/ and cmfix/ (run-1 files) among /root/jobs/fx/cx/v21/s033 job folders. Write only in cmfix/ and this worktree.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include) beyond what the C API test needs to compile — use existing Core tests as the API reference. Do not run scripts/dev/internal-docs.sh.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare, Release, no -g; keep cmfix/ plus build dirs under 25 GB (`du -sh` after each build). Tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements only inside `flock -o /root/jobs/fx/cx/measure.lock`. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Keep every raw output file under cmfix/raw/; do not delete them at the end.
- 리팩토링 점검: list duplicate admission checks between fast and generic paths with file:line and whether removed.
- Time box about 5 hours.

## Output: /root/jobs/fx/cx/v21/s033/cmfix/report.md (Korean, plain sentences): (1) change with file:line and decision-site counts; (2) tests with logs; (3) perf guard; (4) spec rg lines. Save the diff as cmfix/change.diff.
