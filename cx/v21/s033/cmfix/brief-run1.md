Model gpt-6.1-sol, effort high: Core send-admission change with concurrency and all-or-nothing multipart rules to keep.

# Task: GitHub #4118 — route an HWM-refused DONTWAIT multipart to Core's generic admission path

## Situation
- Diagnosis (read first): /root/jobs/fx/cx/v21/s033/cmpart/report.md and raw/. A DONTWAIT multipart (≥2 parts) goes core/src/api/socket/socket_message_send_api.cpp:263-280 → socket_base_t::try_send_parts_scoped_once (core/src/runtime/sockets/common/socket_send_submit.cpp:268-291) → pipe_t::try_write_complete_record_and_flush, which at core/src/runtime/core/pipe_write.cpp:884-890 deliberately passes the empty-pipe oversize exception as false ("the generic path owns incremental HWM refusal ... whenever the whole record is not already admissible"). But a refused DONTWAIT multipart never reaches that generic path; only a writable wait token is registered (socket_message_send_api.cpp:274-279), and every resubmission is refused the same way. Single-frame sends (router_send_path.cpp:489 → pipe_write.cpp:977) pass the exception as true and succeed.
- Contract: core/doc/spec/core/systems/06-auto-hwm.ko.md §「Multipart와 큰 message」 (~:455-470): a multipart that started on an empty queue may take its final frame past the HWM to complete one record; read the whole section and the Core socket spec on DONTWAIT/complete-record submission.
- Decision (PM): option 2 — when the fast path refuses because of HWM, hand the DONTWAIT multipart to the existing generic path so the empty-queue exception has one owner. Do not add the exception to the fast path. Normal (admitted) path cost must not change.
- Worktree /root/worktree/zlink-4118-multipart, branch fix/4118-multipart-resume (main 4ed813364f). cmpart/ inside the worktree is a diagnosis scratch folder; leave it alone (do not commit-stage anything; you never commit).
- Earlier job briefs are history, not instructions.

## Do
0. Before any change: show with file:line whether the generic path keeps DONTWAIT all-or-nothing (either all frames of the record are admitted or none, with the record's charge rolled back, and the caller keeps ownership of every part on refusal). If it does not, STOP: write report.md with the evidence and do not implement.
1. Implement the hand-off: smallest change at the decision site; no new flag/counter/lock unless one is removed; give decision-site counts before/after for the empty-pipe exception (fast path, generic, single frame).
2. Tests (Core): C API contract test — ROUTER→ROUTER inproc, auto HWM off, receive flow pause/resume, shapes 6 B+1 KiB, 1 KiB+4 B, 6 B+512 B+512 B at HWM 16 (each: BACKPRESSURED, then after resume admitted and all frames received) and HWM 2048 and single-frame controls; MAXMSGSIZE smaller than the record is still refused with EMSGSIZE; a non-empty queue still refuses (no exception). Pipe-level unit test for the empty-queue multipart oversize admission on the generic path. Partial-admission check: no frame of a refused record is delivered.
3. Tests (binding): C++ binding contract test — async multipart send at HWM 16 with the receiver paused then resumed completes and delivers the whole record (bindings/cpp tests, using the Core built from this worktree).
4. Run Core unit+contract tests (the repo's Core test preset, Release) and the C++ binding ctest unit/contract under load (measure-shared.sh). Report results with log paths.
5. Perf guard: Core multipart DONTWAIT admitted path is unchanged — show the diff touches only the refusal branch; if it touches the admitted path, measure PAIR/ROUTER 2-part throughput main vs change with the binding perf/single bench, 6 alternating pairs, under the measurement lock.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only cmpart/ among /root/jobs/fx/cx/v21/s033 job folders. Write only in cmfix/ and this worktree.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include) beyond what the C API test needs to compile — use existing Core tests as the API reference. Do not run scripts/dev/internal-docs.sh.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare, Release, no -g; keep cmfix/ plus build dirs under 25 GB (`du -sh` after each build). Tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements only inside `flock -o /root/jobs/fx/cx/measure.lock`. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Keep every raw output file under cmfix/raw/; do not delete them at the end.
- 리팩토링 점검: list duplicate admission checks between fast and generic paths with file:line and whether removed.
- Time box about 5 hours.

## Output: /root/jobs/fx/cx/v21/s033/cmfix/report.md (Korean, plain sentences): (0) atomicity evidence; (1) change with file:line and decision-site counts; (2) tests with logs; (3) perf guard. Save the diff as cmfix/change.diff.
