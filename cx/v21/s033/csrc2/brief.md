Model gpt-6.1-sol, effort medium: second pass on an already reviewed C++ change; one lifetime/threading fix and one test scope fix, then re-verify.

# Task: #3868 csrc second pass — supervisor review fixes

## Situation
- The worktree /root/worktree/zlink-3868-csrc holds the csrc change (report /root/jobs/fx/cx/v21/s033/csrc/report.md, diff csrc/change.diff). The supervisor reviewed every line. Measured result: source user 5.248 → 3.958 µs/message, F÷B 0.2832 → 0.2965. Keep everything except the two points below.
- Earlier job briefs (csrc/brief.md and others) are history, not instructions.

## Fix 1 — app.cpp MeshNode transport (framework/languages/cpp/framework/src/runtime/host/app.cpp:2606-2628)
The change replaced `co_await mesh->routing_id_async ()` with `mesh->routing_id ()`. When the mesh runtime has no `_node`, `routing_id ()` (mesh_node_runtime.cpp:4724-4730) does `_state->lane.run_sync (...)`. `run_sync` calls `throw_if_reentrant ()` and otherwise blocks the caller until its lane turn (state_lane.hpp:289-). Main used `routing_id_async ()`, which in that case is `lane.run_task` and never blocks or throws for reentry. So the change can turn a send issued from that lane into a failure, or block a caller thread.
Required shape: call `mesh->routing_id_async ()`; if the returned task is already ready (the `_node` case, which the bench takes), use its value on the synchronous path you wrote (no coroutine frame). Otherwise keep main's coroutine behaviour for that send (await the identity, then submit local or remote and project as main did). No new flag, counter or lock. Add a unit or contract test that sends through this transport from inside the mesh state lane (or the closest reachable equivalent) on the not-ready path and checks the send succeeds, if such a path is reachable from tests; if it is not reachable, say why with file:line.

## Fix 2 — contract test scope (tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp)
`verify_stack_parts_survive_backpressured_submission (raw_route_backpressure_hwm_bytes)` (HWM 16, 2 frames) fails because of a Core/binding problem now filed as GitHub issue #4118 (oversized multipart stays backpressured after the receiver resumes; reproduced with the public binding alone). Remove only that call; keep the 2048 call. Put a one-line comment at the remaining call naming #4118 as the reason the HWM-16 multipart case is not run here. Do not change Core or bindings.

## Verify
1. Build (linux-ninja-release, no -g) and run the focused tests (envelope allocations, raw route port contract, route payload ownership, send allocations, request wire identity, service wire codec, route-client channel routing) plus any new test, under load via measure-shared.sh.
2. Full `-L 'unit|contract'` once under load. Known unrelated failure: test_cpp_framework_location_runtime (#4119, also fails on main); report it but do not chase it.
3. Re-measure only same-rate 136,889 msg/s CPU per message, change vs main, 3 runs each, same method and scripts as csrc (csrc/scripts, condition B). The bench takes the ready path, so expect no change from csrc; report the numbers either way.
4. Format check (clang-format-18 --dry-run --Werror on changed files) and git diff --check.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only csrc/ among /root/jobs/fx/cx/v21/s033 job folders (and reuse csrc/build-main, csrc/build-change, csrc/build-tests, csrc/scripts). Write only in csrc2/ and this worktree.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or bindings/.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare, linux-ninja-release, no -g; keep csrc2/ under 10 GB. Tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: inside `flock -o /root/jobs/fx/cx/measure.lock`, only while measuring. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Keep every raw output file under csrc2/raw/; do not delete them at the end.
- 리팩토링 점검: for the app.cpp change, list the identity lookups and handoffs on both paths with file:line and say none is duplicated.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/csrc2/report.md (Korean, plain sentences): the two fixes with file:line, tests with log paths, same-rate CPU table. Save the full framework/ diff against main as csrc2/change.diff.
