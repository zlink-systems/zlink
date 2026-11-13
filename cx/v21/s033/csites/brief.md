# Job: implement 11 approved per-request lock/heap removals — C++ 0.33.0 #1639 / #3867

Model/level: gpt-6.1-sol medium — middle tier: changes cross completion, dispatch and codec ownership in several
components; medium is the guide's starting level. Worktree /root/worktree/zlink-3867h, branch
perf/3867-cpp-request-sites (at 0cd1eac0c5 = PR #3919 head merged with main). No sub-agents or other models
(no Astra, no Fable). Do not run scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts. Do not read Core C
headers. No GitHub. No commit, push, branch, stash, reset, rebase, spec edits. Write only under
framework/languages/cpp/ in this worktree and /root/jobs/fx/cx/v21/s033/csites/. Report:
/root/jobs/fx/cx/v21/s033/csites/report.md.

## Settled design (supervisor-approved; implement exactly these rows, nothing else)
From /root/jobs/fx/cx/v21/s033/csites/cplanr-plan.md ("Site plan"):
- R-a `service_mailbox_t::has_application_dispatch` row: select the hosted receive implementation at the existing
  bind/start; hosted dispatch_ready and the raw_mesh_node_owner wait path stop re-querying the standalone queue.
  No cached boolean or second mutable registration state; service_mailbox stays the only binding authority.
- R-b `dispatch_application` row: keep the local terminal guard through decode and dispatch_mesh_record; promote to
  shared ownership only inside the branches that actually defer; move the untouched terminal into the ordinary
  dispatcher on the existing false return. Internal Node/Channel commands in spot_runtime.cpp must still be handled.
From /root/jobs/fx/cx/v21/s033/csites/cplanc-plan.md ("Site plan and decision ownership"), rows 1, 2, 3, 5, 6, 7, 10, 12, 17
exactly as written there (serializer binding at registration; header views; fixed profile metadata; accepted-record
views; descriptor span in request_capture_storage_t; dispatcher-owned context/completion moves; reply header view;
deadline text in request-owned storage; completion item stored with the request block).
Every other row in both reports stays as it is ("keep").
Rules: decision/terminal/turn owners unchanged (01-submit-and-completion §9-11, 08-messaging-hot-path §4,
02-handler-turn-and-execution-gate §1-3/§13). No new flags, counters, queues, pools, caches with eviction, retries,
threads. A removed allocation must not reappear elsewhere — prove it in the process total, not by a moved stack label.

## Verification
1. Counts: use the csite2 interceptor and harness unchanged (/root/jobs/fx/cx/v21/s033/csite2/), request-backpressure
   1 KiB, source CPUs 0-7 / target 8-15, 30 s warmup / 5 s active, before (0cd1eac0c5) and after. Table per role:
   locks and heap per request, plus each changed row's sampled site before/after.
2. Throughput: Framework and binding, request-backpressure and send-saturation 1 KiB, 3 rounds each, same condition,
   medians and F/B. Measurements under `flock -o /root/jobs/fx/cx/measure.lock`.
3. Tests: for each row that changes lifetime/ownership (R-b, 5, 6, 7, 17) add a test that fails if the storage is
   released early or the terminal is settled zero or two times (reply, timeout, cancel, shutdown, throwing handler).
   Full build `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset
   linux-ninja-release --parallel 3 -- -k 0`; `flock -s /root/jobs/fx/cx/measure.lock ctest --preset
   linux-ninja-release -L '^(unit|contract)$'`; `scripts/format/format.sh --check cpp`. All from framework/languages/cpp
   (format from repo root). Logs in files; read only summaries.
Report: per row file:line changed, decision sites before -> after, counts, throughput, test logs.
MemAvailable >= 12 GiB. SIGTERM/SIGINT only; never use &.
End the report with "리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목".
