# Job: C++ target receive-role hand-off experiment (#3868, 0.33.0 #1639)

Worktree: /root/worktree/zlink-3868t (branch perf/3868-target-handoff, from origin/main)
Report: write /root/jobs/fx/cx/v21/s033/thand/report.md (absolute path; nowhere else)
Rules: no sub-agents or other models (no Astra). C++ builds only through
`/root/jobs/fx/cx/cpp-build-slot.sh --parallel 3 <build cmd>`; measurements under
`flock -o /root/jobs/fx/cx/measure.lock`, builds/tests under `flock -s` on the same file.
Do not commit or push.

## Background (read first)
- /root/jobs/fx/cx/v21/s033/cadvice/report.md (option B: receive-role transfer at rotation boundaries, target-only first)
- /root/jobs/fx/cx/v21/s033/zdesign/answer.md (design 2: target receive thread runs the handler, hands the receive role to an idle worker; reuse offload_executor batch owner, no new queue or flag)
- /root/jobs/fx/cx/v21/s033/fdesign/design.md (target owner drain 22.8 us/record)
- Spec: 08-messaging-hot-path §4.4 (receive role may run a turn only after handing the role off at a rotation boundary), 01-submit-and-completion §11 (a new execution turn is not another thread).

## Task
1. Implement on the target side only: at a receive-rotation boundary, the receive thread hands the receive role to an idle offload worker and then runs the handler turn itself, instead of posting the handler to a worker. Reuse the existing offload_executor ownership; add no new queue, flag or counter unless you remove an equivalent one (report before/after counts).
2. Keep ordering/FIFO and owner-turn rules unchanged. Add a test that shows handler order per owner is preserved and the receive role is never held by two threads.
3. Measure C++ request-backpressure 1K and send-saturation 1K, main vs branch, 3 runs each, same core pinning as the cprof job (source 0-7, target 8-15). Report Framework/binding ratio, per-thread µs/msg for target receive role and target workers, and context switches/msg.
4. Done criteria: build + C++ framework tests pass; report contains the numbers table, the diff summary, and a keep/drop recommendation. If the ratio does not improve by ≥0.02 in either pattern, say drop.
