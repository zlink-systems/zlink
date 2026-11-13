# Job (read-only plan): per-request lock and heap removal plan — receive, readiness and dispatch machinery (runs on both source and target) — 0.33.0 #1639 / #3867

Model/level: gpt-6.1-sol medium — middle tier: completion/dispatch paths and ownership across several C++ components;
medium is the guide's starting level. Worktree /root/worktree/zlink-3867h (detached at 0cd1eac0c5 = PR #3919 head
merged with main), read-only. No sub-agents or other models (no Astra, no Fable). Do not run
scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts. Do not read Core C headers. No GitHub. No commit,
push, branch, stash, reset, rebase, spec edits. Do not edit the worktree. Write only under
/root/jobs/fx/cx/v21/s033/cplanr/. Report: /root/jobs/fx/cx/v21/s033/cplanr/report.md. Keep it compact.

## Why
request-backpressure 1 KiB, per completed request (exact interceptor counts, /root/jobs/fx/cx/v21/s033/csite2/report.md §1):
binding source 12.5 heap / 27.0 mutex, target 8.0 / 10.7; Framework source 36.3 / 95.8, target 44.8 / 67.1.
Single-spot fixes moved throughput only a few percent; the Framework cost is ~30 sites of 1-2 calls each.
The site table with sampled call sites is csite2/report.md §2 — use it, and do not re-measure.

## Scope: receive, readiness and dispatch machinery (runs on both source and target)
- mesh_node_host_service.cpp: node_receive_role_t::rotate (:2097), runtime::next_receive_activity_async (:2040), dispatch_application (:3664)
- execution/state_lane.cpp: schedule_drain (:134), drain_loop (:169), detail::dispatch_completion (:97)
- service_mailbox.cpp: try_enqueue (:79), has_application_dispatch (:447)
- client_server_location_runtime.cpp: application_supply_slot_t::ensure_waiter (:1282) and the surrounding pump start
- public_host_runtime.cpp: dispatch_ready (:7345)
- raw_route_port.cpp: poll (:404), receive_if_ready (:459), the lambda at :351; raw_mesh_node_owner.cpp receive_next (:3549), deliver_request_terminal (:245)
- also: how many receive rotations per request happen (csite2/chand: about 0.8) and what in Framework makes the
  rotation re-enter the binding poller wait (poller wait is 13.9 locks/request on the Framework source receive role
  vs 4.2 in binding) — Framework side only.
Leave binding/Core code (bindings/cpp, poller.cpp, completion_owner.cpp) and benchmark code out of scope; note them only.

## Deliver: one row per site
site (function, file:line on this HEAD) | calls/request now (lock, heap) | why it happens per request (read the
code) | the fix | the spec clause it must keep (file:line) | calls/request after | risk.
Allowed fix kinds (pick one per site, or say "keep" with the reason):
1. Resolve once at registration/startup instead of per request (types, serializers, handler plans, filter lists, routes).
2. Use storage the request already owns (the PR #3919 request block / request_capture_storage_t) or a view instead of a copy.
3. Drop a lock/check that re-protects state already serialized by its owner (lane, receive rotation, C1) — name the owner.
4. Reuse a buffer owned by an existing long-lived owner (socket, lane, rotation) without adding a new pool policy.
Not allowed: new flags, counters, queues, pools with their own policy, retries, caches with eviction, extra threads,
any change to who decides a terminal or a turn (spec 01-submit-and-completion §9-11, 08-messaging-hot-path §4,
02-handler-turn-and-execution-gate §1-3/§13). Decision sites must not grow; list before -> after per site.
End with: total expected lock and heap calls/request for this area, the files that would change, and how the
existing contract tests and the csite2 interceptor would prove each row.
End the report with "리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목".
