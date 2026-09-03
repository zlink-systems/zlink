# Core hot-path phase 2 progress

## 2026-09-03 initial triage

- Confirmed `main` at `1344022a3e`; preserved the pre-existing untracked `doc/principal/dev/hotpath.ko.md` without edits.
- Read `perf-diagnosis.md`, decisions D-036/D-037, hot-path rule section 2, the existing single/multi sweep, and the prior commit summary.
- Priority order fixed as D (multi wake/activation stall) before A (common send cost), B (REQREP), and C (ws/wss reply admission).
- Started read-only parallel audits for D owner/signaler paths, A/B callgrind/source costs, and C websocket write activation. The primary workstream is inspecting the exact `1344022a3e` diff and reproducing D under the mandatory 16 GiB virtual-memory cap.

## 2026-09-03 D reproduction and first isolation

- Current `DEALER_ROUTER_SENDSEND/tcp/1024B` at the default 100 clients produced 198.820 Kops/s, 407.183 MB/s, and mean/p95/p99 latency 439.509/833.731/868.641 ms. The same baseline command produced 352.272 Kops/s, 721.454 MB/s, and 0.194/0.336/0.428 ms.
- With one client, the seconds-scale latency disappeared: current was 313.803 Kops/s, 642.668 MB/s, p95 0.104 ms; baseline was 439.706 Kops/s, 900.518 MB/s, p95 0.115 ms.
- At 100 clients, changing duration from one to two seconds changed current mean/p95 from 93.810/180.180 ms to 198.654/383.080 ms. Latency growing with run duration is evidence of routed-reply hot-path throughput falling below offered load and accumulating backlog, rather than a fixed timeout retry. The independent owner/signaler path audit remains in progress.
- Normalized callgrind anchors: current DEALER/DEALER `submit_completion_aware_part` used 492,447,208 instructions over 99,995 part calls; `prepare_send_step_locked` used 79,499,594/99,995; `flush_deferred_peer_controls` used 28,348,299/49,997. Current REQREP repeatedly pays completion receive and reply/send-completion work. The first implementation target is the mutex/state/spec setup plus unconditional flush/notify work.

## 2026-09-03 D wake fix and A common-path reduction

- Root cause for the multi-runner stall was a lost POLLOUT edge: after HWM refusal marked the pipe inactive, a temporary async mailbox owner could consume `activate_write` before the public DONTWAIT path marked send recovery pending. The later arm only set `pending`, so no new edge was produced and the public poller slept while the transport was already writable. The destroyed and explicit-quiesce async-owner detach paths also omitted the required primary-signaler rearm.
- Changed the recovery arm to mark pending and immediately recheck `transport_has_out()`, signal on a new pending/ready transition, routed all manual backpressure arms through that helper, and rearmed the primary signaler after both detach paths.
- Reduced the common completion-aware send path by attempting admission before the pending-map mutex/hash/vector path, tracking helper multipart activity per socket, moving the accumulated inline message buffer without rebuilding a record vector, and guarding pending-send notification and deferred-peer-control scans with atomic state.
- Focused contract tests passed 8/8: phase3 completion, TLS async admission, public inproc multipart, helper basic/interleave, router mandatory HWM, paired flow state, and polling contract.
- Reproduction after the changes, default 100 clients: `DEALER_ROUTER_SENDSEND/tcp/1024B`, one part, reached 522.539 Kops/s and 1070.159 MB/s with mean/p95/p99 0.241/0.378/0.538 ms. The default two-part case reached 345.749 Kops/s and 708.094 MB/s with 0.265/0.468/0.603 ms. Against the measured baseline two-part 352.272 Kops/s and 721.454 MB/s, throughput and bandwidth ratios are both 0.9815; the prior current result was 198.820 Kops/s with p95 833.731 ms.

## 2026-09-03 B/C first implementation and targeted measurement

- Replaced one timer task per admitted request with one socket-owned earliest-deadline task. Normal completion now removes only the pending record; the aggregate task scans due records, is generation-fenced, reschedules the next deadline, and maps scheduler failure to exactly-one terminal completion.
- Replaced reply-target `std::string` keys with bounded routing-id keys, reduced checkout from repeated lock/find turns to one, and cached an exact pipe route-binding token so an unchanged route avoids RID reconstruction and route-table locking.
- Added the no-command completion commit fast path, with an atomic in-flight drain fence covering the interval after the mailbox hint is cleared and before all command-side state is applied.
- Reworked router-reply admission waiting to release the public send lock and wait for an applied write/progress transition. The initial implementation acquired a temporary async mailbox owner and removed the old 1 ms retry slice.
- The first targeted REQREP run showed the non-WS network paths recovered (`D/R tcp` 468.025 Kops/s, `R/R tcp` 458.930 Kops/s), but inproc remained about 0.78-0.80 of the known baseline and WS/WSS was unstable. A three-run WS/WSS repetition showed p99 243-271 ms and medians of 12.094/33.667/147.275/69.106 Kops/s for D/R ws, D/R wss, R/R ws, and R/R wss. This is a confirmed unresolved wait/activation defect, not a pass.
- Static admission tracing narrows the first-frame `transport_wait` result to the initial transport-pair write hold or remote receive-flow PAUSE; framed-engine flush alone does not produce that admission enum. Low-frequency temporary counters are being added to distinguish those two states and to correlate them with owner start/reuse and wait timeout.
- Removed two additional inproc request allocation sites: the normal two-part request now transfers its inline helper buffer as one record instead of allocating/moving a `std::vector`, and completion reservation nodes now use a bounded 64-node recycle cache after dequeue/cancel. These changes still require the post-edit focused tests and targeted measurement.

## 2026-09-03 C timeout-avalanche isolation

- A direct one-second `DEALER_ROUTER_REQREP/ws/1024B` comparison separated the part-count boundary: one-part current reached 461.661 Kops/s and 945.482 MB/s, while two-part runs varied from roughly 20 to 127 Kops/s and showed p99 near 240 ms. The two-part public request path was consuming only its shallow retry copies after successful admission; it now consumes the original caller handles as well. A dedicated two-part DEALER-to-ROUTER ownership test verifies both caller handles are empty and both independent free callbacks run exactly once. This is a confirmed correctness fix, but a post-fix run still fell to 20.210 Kops/s, so it was not the WS performance root cause.
- Low-frequency temporary counters, removed immediately after capture, showed the first roughly 65K requests completing normally followed by a timeout avalanche. At 114,688 submissions, 33,366 had timed out; reply admission was predominantly ready rather than `transport_wait`. The 200 ms onset matches `PERF_SINGLE_REQREP_TIMEOUT_MS`.
- The aggregate timeout callback currently performs a full `pending_requests` scan, removes one due request, then repeats. Once a WS run has more than one due request, this becomes O(N^2), monopolizes the timeout worker, and creates further 200 ms expirations. The active fix is a one-scan batch removal with the same generation fence, exactly-once completion, correlation release, next-deadline scheduling, and scheduler-failure semantics.

## 2026-09-03 multi HWM wake and async-owner handoff

- A second lost-wake boundary was found in pipe HWM accounting: the writer could publish its waiter after the reader had already sampled the old state and passed the LWM transition. The writer now arms with a fence plus peer-progress recheck, and the reader performs the matching waiter recheck before deciding that no activation is owed. Deferred HWM shrink also keeps the reader LWM aligned with the planned capacity so the writer is awakened at the intended boundary.
- Added a deterministic planned-HWM regression in `test_router_multiple_dealers`: it fills to 8C, stages a shrink to 4C, drains through the new 2C LWM, and verifies that the blocked writer receives an activation. The focused target and isolated case passed.
- Serialized explicit completion-owner quiesce with temporary transport-owner startup. New starts observe `async_quiesce_pending`, release API/owner locks, wait for the old mailbox executor to detach, and retry; detach releases the old receive lease before publishing the stopped state. A deterministic `test_ctx_destroy` race test and ten repeats passed.
- With these wake fixes, the default 100-client `DEALER_ROUTER_SENDSEND/tcp/1024B` reproduction reached 343.096 Kops/s and 702.661 MB/s with p95 0.477 ms, versus baseline 359.940 Kops/s and 737.156 MB/s: throughput/bandwidth ratio 0.9532. The former candidate was 200.255 Kops/s with p95 1001.804 ms.

## 2026-09-03 WS completion-backpressure cycle

- Engine/pipe timing proved three repeated stalls rather than a generic websocket flush delay. The receiving engine stopped on decoder-allocation backpressure for 200.064, 208.353, and 198.735 ms; the opposite WS writes completed after 208.132, 211.174, and 199.401 ms, and generated HWM credit only 2, 2, and 4 microseconds later. Mailbox activation itself arrived in about 1.0-1.1 ms, excluding command-delivery latency as the source.
- The cycle is: a public completion poller quiesces the async completion owner; its thread enters a blocking request send; already-arrived replies remain in the physical completion pipe; that inbound pipe fills and blocks the peer reply writer; both directions wait until the 200 ms send timeout. On physical request-admission EAGAIN, the blocking path now borrows the still-registered completion owner's gate and drains physical reply records into the existing public completion queue before sleeping. It rechecks registration under the owner mutex so a released poller cannot overlap its replacement async owner.
- Post-fix one-run results: D/R ws 316.015 Kops/s and 647.199 MB/s, D/R wss 201.028 Kops/s and 411.705 MB/s, R/R ws 359.689 Kops/s and 736.643 MB/s, R/R wss 194.819 Kops/s and 398.989 MB/s. A three-run D/R ws repetition measured 311.47-346.26 Kops/s (median 323.08); the former plateau was 51-68 Kops/s with roughly 200 ms p95 stalls.
- All temporary `ZLINK_PHASE2` pipe/mailbox/engine/completion/reply-wait instrumentation was removed. A deterministic completion-poller/backpressure regression test is being added before the next build and sweep.
