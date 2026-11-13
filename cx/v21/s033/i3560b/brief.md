Model gpt-6.1-sol, effort high: read-only diagnosis spanning Core and Framework C++ (which caller pattern drives a Core wake chain); ambiguous cause across two layers → sol. No code changes. Earlier job briefs are history, not instructions.

# Task: #3560 — why a C++ Framework request causes ~1.5 Core signaler round trips when the binding needs 0.075

Folder JOB=/root/jobs/fx/cx/v21/s033/i3560b. Worktree /root/worktree/zlink-v21-s033/i3560b (detached at origin/main). Read first: /root/jobs/fx/cx/v21/s033/i3560/report.md and its raw-condB/ (wake.bt probe, wake-counts.txt, measurement scripts). Facts: Core 1.24 candidate; per successful request on the source process: signaler send 1.462 (binding 0.075), read 0.936 (0.031), write 0.446 (0.016); Core IO thread CPU per request Framework source IO/0 20.5 + IO/2 17.5 µs vs binding 5.1 + 3.5; target IO/0 18.8 + IO/1 20.8 vs 4.6 + 4.9. Path: socket_request_reply_internal.cpp:455-466 → socket_base_dispatch.cpp:325-338 → socket_base_lifecycle.cpp:559-562 → mailbox.cpp:472-487 → signaler.cpp.

## Do
1. Find which Framework C++ calls (completion poller registration, zlink_completion_recv/poll pattern, DONTWAIT vs blocking, number of sockets/pollers, mesh ROUTER vs completion connection) make the reply publication signal a poller per request, while the binding bench coalesces. Use bpftrace/uprobes with user stacks on signaler_t::send (both processes), under `flock -o /root/jobs/fx/cx/measure.lock` for runs only; same bench build method as i3560 (reuse its scripts; Core prefix /root/jobs/fx/cx/v21/s033/core124/prefix-1.24; C++ binding from source ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON). Attribute the 1.462 sends per request to call sites with counts, and the target-side IO CPU 4x the same way (which thread does what per request).
2. Decision-owner table: the decision "when a completion becomes ready, who is woken and how" — the spec clause that owns it (core/doc/spec/core/... completion/poller sections; framework S08 §4.4 if the Framework poll loop is involved), every place that decides it now (file:line, Core and Framework), and the single owner a fix would use; counts before/after. Classify change class (supervisor guide §4: A/B/C/D) and whether the fix is Core or Framework.
3. Do NOT change code. Report sections: 결과, 판정 위치(전/후 수), 검증(명령·로그·원본 경로·수치), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목 0, 넘긴 항목"), 남은 문제.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, build dir inside your worktree; never /root/.cache/zlink/zlink-cpp/<ver>. If SIGSTOPped by the measurement guard, wait.
- Per-thread sums use the TID union. Keep every raw file in JOB. Every claim file:line or raw path. Report in Korean, short. Remove build dirs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/i3560b/report.md
