# Job: fix PR #3919 review findings — C++ request receiver (0.33.0 #1639 / #3867)

Model/level: gpt-6.1-sol high. Worktree /root/worktree/zlink-3867f (detached at the head of branch perf/3867-cpp-request-receiver),
HEAD 247b16c4be (PR #3919). No sub-agents or other models (no Astra). Do not run
scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts. Do not read Core C headers.

Context: /root/jobs/fx/cx/v21/s033/crecv2/report.md (what the PR does). The PM review findings are
summarized below; do not fetch the PR or its comments.

## Findings to fix
P1. Actor path: when native admission is rejected for a registered Actor request, C1's physical receiver
(raw_mesh_node_owner.cpp:1379 -> :259/:261 -> public_host_runtime.cpp:3195 -> mesh_node_runtime.cpp:4136)
and the submission-failure observer in actor_client.cpp:896/:900 both call the same final receiver. The
receiver's projector is single-use (request_task.hpp:606/:612 `project.reset()`, stored capture at :59), so
the second call runs an empty projector. Fix: for registered requests the submission observer reports only
admission/public failure; the physical receiver alone delivers the physical result, once. Do not add an
"already called" flag or an engaged-optional check (spec 01-submit-and-completion.ko.md §9, §10.1 at :445:
single delivery).
Then do the same audit for EVERY caller of request_task_access_t::receive_public / receive_internal and
every request_receiver_t::project user (rg them): Node, ChannelName, Actor, Spot, route channels, streams,
anything else. For each, list the paths that can deliver to the final receiver (immediate rejection before
registration, native admission rejection after registration, deadline before native start, normal reply,
shutdown pending) and show with file:line that exactly one runs. Fix any other path with two deliverers the
same way.

P2. A request that throws before registration keeps its projection captures in the request block for as
long as a completed task handle lives (request_task.hpp:656 catch); before the PR they were released during
unwind. Fix so captures of a callback that will never be invoked are released when its last owner goes away
or the pre-registration failure is settled through the outermost physical receiver, without clearing captures
that a later physical delivery still needs.

P3 (perf decision input). The request block embeds a 1 KiB buffer that lives as long as any task handle.
Measure the capture bytes actually stored per request on the benchmarked Node, ChannelName and Actor paths,
then size the inline storage to fit them (or allocate exactly), and report bytes per request and retention
before/after next to the allocation counts. Keep the allocation count at its current level unless a smaller
size costs an extra allocation; if so, report both options with numbers.

## Tests
- Regression tests through the real actor_client_t::submit_request() path: native admission rejected; timeout
  while admission pending; shutdown while admission pending. Assert the physical receiver runs exactly once
  and the public result is set once. (test_cpp_framework_actor_late_result.cpp:180 bypasses
  submit_prepared_request() and does not cover this.)
- For every other path you find with two deliverers, the same style of test.
- P2: build a projection holding a weak_ptr marker, throw before registration, keep the completed handles,
  assert the marker expires.

## Done when
Full build with -k 0, unit+contract ctest, format check (commands as in crecv2/report.md "Validation"), then
re-measure csubmit request-backpressure 1K and send-saturation 1K (same runner as crecv2, condition B, 3 runs,
under `flock -o /root/jobs/fx/cx/measure.lock`). MemAvailable >= 12 GiB. SIGTERM/SIGINT only.

## Write limits and forbidden
Write only framework/languages/cpp/** in the worktree plus scratch under /root/jobs/fx/cx/v21/s033/crecv3/.
Forbidden: commit, push, branch, stash, reset, rebase, spec or doc edits, GitHub operations. Builds: `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset linux-ninja-release --parallel 3 -- -k 0`.

## Report
/root/jobs/fx/cx/v21/s033/crecv3/report.md:
- the per-path delivery table;
- fixes with file:line;
- decision-owner counts before/after;
- tests;
- bytes per request;
- before/after measurements.
End your final message with:
"리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목"
