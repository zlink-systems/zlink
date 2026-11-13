# Job: C++ source completion path — fewer handoffs per reply (0.33.0 #1639 / #3867)

Model/level: gpt-6.1-sol high. Worktree /root/worktree/zlink-3867c, branch perf/3867-cpp-completion-batch
(based on PR #3919 head 5ecae5de8b, still in review). No sub-agents or other models (no Astra, no Fable).
Do not run scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts. Do not read Core C headers.

## Evidence
- /root/jobs/fx/cx/v21/s033/cladder/report.md: on the source side the receive role
  (`mesh_node_host_service_t::node_receive_role_t`, `raw_route_port_t::poll`) costs ~15.7-17.3 µs/msg and
  completion dispatcher workers ~3.2 µs each; 0.6 needs <= 6.75 µs on every serial thread.
- /root/jobs/fx/cx/v21/s033/cthin/report.md §2 row R7: completion publication allocates and wakes per item
  (operation_registry.cpp:250 notify, provider_task_context.cpp:74,174 dispatch_completion); "publish existing
  independent completion reservations in bulk".
- /root/jobs/fx/cx/v21/s033/cbatch/report.md: TCP batching follows submit rate, so serial-thread cuts compound.

## Goal
Cut source-side CPU per reply on the receive role and completion workers, keeping the contract:
- Each user callback still runs in a new dispatcher turn, never on the receive/notification stack
  (spec framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:490-544,
  08-messaging-hot-path.ko.md:306-326).
- Callbacks stay independent: one slow callback must not delay others; shutdown drains accepted work.
- No new queue/lock/flag/counter authority; give a decision-owner table with before/after counts.
Candidates (profile first, pick what the profile supports):
- one dispatcher publication + wake per poll drain batch instead of per completion;
- remove per-item heap nodes / std::function boxing in the completion handoff;
- anything else the receive-role profile shows as representation-only.

## Done when
1. Full build `-k 0`, unit+contract ctest, `scripts/format/format.sh --check cpp` (commands as in
   /root/jobs/fx/cx/v21/s033/crecv3/report.md).
2. Tests: callbacks from one poll batch run on dispatcher turns (not the poll thread), a blocking callback
   does not stall the others, shutdown with a batch pending delivers each once.
3. Measure csubmit request-backpressure 1K and send-saturation 1K (runner in /root/jobs/fx/cx/v21/s033/crecv3/),
   condition B, 3 runs, before = 5ecae5de8b numbers from crecv3, per-thread CPU included, under
   `flock -o /root/jobs/fx/cx/measure.lock`.
Builds: `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset linux-ninja-release --parallel 3 -- -k 0`.
MemAvailable >= 12 GiB. SIGTERM/SIGINT only.

## Write limits and forbidden
Write only framework/languages/cpp/** in the worktree plus scratch under /root/jobs/fx/cx/v21/s033/cpoll/.
Forbidden: commit, push, branch, stash, reset, rebase, spec or doc edits, GitHub operations.

## Report
/root/jobs/fx/cx/v21/s033/cpoll/report.md:
- profile before;
- changes with file:line;
- decision-owner table;
- tests;
- before/after per-thread table.
End your final message with:
"리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목"
