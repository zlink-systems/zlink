# Job: finish the C++ request receiver change (0.33.0 #1639 / #3867) — continuation

Model/level: gpt-6.1-sol high. Worktree /root/worktree/zlink-3867r (branch perf/3867-cpp-request-receiver,
base c8869b63b1). No sub-agents or other models (no Astra). Do not run scripts/dev/internal-docs.sh,
git pull/fetch or docs sync scripts. Do not read Core C headers under /root/.cache/zlink/core; use the
C++ binding headers and the Framework sources instead.

## State
A previous session stopped partway. Its uncommitted edits are in the worktree (`git diff`): request_task.hpp,
task.hpp, channel_runtime.cpp, mesh_node_runtime.cpp, test_cpp_framework_request_call_allocations.cpp.
Its notes, scripts and the "before" measurements are in /root/jobs/fx/cx/v21/s033/crecv/ (report.md draft,
measurements.json, measure.sh, census*.sh). Original goal and constraints:
/root/jobs/fx/cx/v21/s033/crecv/brief.md — read it; all its rules still apply.

## Open question to settle first
In `mesh_node_runtime_t::submit_node_request_reply` the registry completion lambda now holds
`receiver.borrow ()` while the owning `receiver` moves into `compose_node_request_submission`. The previous
session was checking whether the admission-failure path (compose/admitted) and the registry terminal path
can both deliver to the same receiver, and whether the borrowed view can be used after its owner has
released the capture. Answer with file:line evidence for each path:
- Which path delivers first in: immediate submit rejection, deadline expiry before native start, normal
  reply, shutdown with the request pending.
- Whether the second path can run, and what it touches.
If either is possible, change the ownership so one path owns delivery (the registry C1 take is the single
terminal authority, spec 01-submit-and-completion.ko.md:445) and add a test per case. Do the same review for
the two channel_runtime.cpp projections.

## Then
Complete the "Done when" list of the original brief:
1. Full build with -k 0.
2. unit+contract ctest.
3. format check.
4. After-measurements with the same runners as the before set.
Write the final /root/jobs/fx/cx/v21/s033/crecv2/report.md:
- the open-question answer;
- changed files;
- decision-owner table with before/after counts;
- tests;
- before/after table with raw paths.

## Write limits and forbidden
Write only framework/languages/cpp/** in the worktree plus scratch under /root/jobs/fx/cx/v21/s033/crecv2/.
Forbidden: commit, push, branch, stash, reset, rebase, checkout of files, spec or doc edits, GitHub
operations. Builds: `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh --parallel 3 ...`.
Measurements: `flock -o /root/jobs/fx/cx/measure.lock`, one at a time. MemAvailable >= 12 GiB.
SIGTERM/SIGINT only.

End your final message with:
"리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목"
