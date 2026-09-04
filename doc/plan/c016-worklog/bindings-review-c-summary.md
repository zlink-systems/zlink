# bindings/c review — 0.17.0 DONTWAIT wait-token / WRITABLE contract (port f9d0eb84d9)

Core: 50d77800f2 (0.17.0), linked from core/build (no rebuild; runner's auto-build step reported `ninja: no work to do`). Scope: bindings/c/** only. No git state changes.

The C binding has no source layer of its own (headers mirror core/include); the binding code that owns wait tokens is the perf runners (multi client helpers, relay server, stream session, DEALER_DEALER client/server) and the contract tests. Single runners keep the blocking active-phase send path (unchanged).

## Changed files

| File | Change |
|---|---|
| bindings/c/perf/multi/common/perf_multi_client_helpers.hpp | `retain_echo_send` + `submit_retained_send` on every send replaced by `submit_echo_send`: one DONTWAIT attempt straight from the stamped buffer; the payload is snapshotted only when Core answers BACKPRESSURED+token (rejection-time copy). Retry path (`submit_retained_send`/`retry_retained_send`) unchanged. |
| bindings/c/perf/multi/common/perf_multi_stream_session.hpp | `submit_packet_async` no longer copies the framed packet into `retained_packet` before each attempt; on BACKPRESSURED the bytes are rebuilt from the still-live header/body parts (`retain_packet_bytes`). Event loop calls `zlink_poller_modify` only when the interest set changes (was every iteration). |
| bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp | `pollout_suppressed` on the slot: after a NO_DATA pull with a live token, POLLOUT is dropped from the interest set (POLLCOMPLETION still wakes on the WRITABLE record); reset on WRITABLE/new token. |
| bindings/c/perf/multi/src/perf_multi_dealer_dealer_server.cpp | Same POLLOUT suppression for the latency-ACK wait state (`server_send_events`, `drain_server_writable`). |
| bindings/c/perf/single/common/perf_single_reqrep.hpp | `drain_request_completions` skips `ZLINK_COMPLETION_WRITABLE` records instead of failing the run. |
| bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp | `drain_socket_completions` skips `ZLINK_COMPLETION_WRITABLE` records likewise. |
| bindings/c/tests/test_c_dontwait_backpressure_contract.c | Two new public-API cases (see tests). |

## Bug review (checklist a–h)

| # | Item | Verdict | Where / fix |
|---|---|---|---|
| a | DONTWAIT single attempt; OK/ID 0 never waits for a completion | OK | `classify_send_result` (client helpers 324), `submit_packet_async`, `submit_retained_message`, relay `try_send_reply_now`: OK with nonzero ID is treated as EPROTO, OK/0 counts immediately, no completion pull. |
| b | BACKPRESSURED+token: binding keeps payload; resend only on WRITABLE with same token/context/RID; re-BACKPRESSURED keeps waiting on the new token | OK (perf fixed) | All four state machines match `completion_id == wait_token && user_context == socket && peer_rid == target` (`record_writable_completion` 183, relay `drain_reply_writable` 281, stream `record_writable_completion` 161, dd `record_dd_writable`/`record_server_writable`); a foreign token is EPROTO. Retry after NO_DATA, re-BACKPRESSURED installs the new token. Payload retention moved to rejection time (perf 2a). |
| c | WRITABLE with `send_result == TERMINAL` surfaces as failure, no permanent wait | OK | Every `record_*_writable` clears the token/payload and returns `send_terminal_errno` (EIO fallback); relay server maps ENOENT to "drop this reply" and continues. Pinned by the new contract case (disconnect_rid -> TERMINAL/ENOENT with same token/context/RID). |
| d | ROUTER/STREAM no route: NOT_CONNECTED fails immediately | OK | Client helpers: `send_error` -> fatal; relay: `reply_send_stale_route` -> drop and continue; stream: fatal with EHOSTUNREACH. No token expected (`wait_token != 0` on non-BACKPRESSURED is EPROTO). |
| e | One public poller drains to NO_DATA; REQUEST and WRITABLE mixed on one queue reach the right waiter; level POLLOUT/POLLCOMPLETION without busy loop / lost wake | FIXED | (1) perf_single_reqrep.hpp:262 and perf_multi_socket_reqrep.hpp:291: a stray WRITABLE (from a DONTWAIT handshake/probe send that passed `completion_id_out = NULL`; Core still reserves the token, see `register_send_writable_wait`) made `drain_request_completions` return false and failed the run — now skipped. (2) dealer_dealer client/server registered POLLOUT for the whole token lifetime; DEALER POLLOUT is an aggregate hint that can stay level-true while the token's WRITABLE is not yet published -> spin. Added the same NO_DATA -> suppress-POLLOUT rule the client helpers/relay/stream already use. Lost wake is excluded because POLLCOMPLETION stays registered and is level-true while the WRITABLE record is unread. |
| f | close/ctx term releases tokens/snapshots/native resources; thread safety | OK | Trackers/sessions are single-threaded per socket; `close_send_wait_tracker`, `release_retained_packet`, `clear_session` drop snapshots; poller destroyed before sockets; `zlink_completion_close` after every OK pull; relay `pending_reply_t` owns moved parts and closes them exactly once (move-only). |
| g | errno/error mapping | OK | EAGAIN/EWOULDBLOCK only with BACKPRESSURED+token, EPROTO for contract violations, terminal errno propagated verbatim. |
| h | REQUEST / blocking / PUB paths unchanged | OK | Single runners' active phase uses `ZLINK_SEND_FLAGS_NONE`; REQUEST submit/drain logic untouched apart from the WRITABLE skip; PUB uses `zlink_publish_part` (no completion). |

Observed but left as-is (bounded, harmless): single-runner handshake/probe sends (`perf_router_router.cpp:68/356/394`, `perf_dealer_router.cpp:214`, `perf_router_router_reqrep.cpp:56`) use DONTWAIT with `completion_id_out = NULL`; each refused attempt leaves one WRITABLE record on that socket's queue until close (<= ~1000 during a 1 s probe loop). The consumers that share those queues now tolerate it.

## Performance review (hot path)

| Item | Verdict | Fix | Measurement |
|---|---|---|---|
| 2a payload snapshot per send (client helpers `retain_echo_send`: full `vector::assign` before every submit, i.e. two byte copies per message) | Defect | Snapshot only on BACKPRESSURED (`submit_echo_send`) | multi DEALER_ROUTER_SENDSEND / ROUTER_ROUTER_SENDSEND client hot path |
| 2a stream session `retained_packet.assign` per packet | Defect | Rebuild from header/body only on rejection | multi STREAM server hot path |
| 2a stream session `zlink_poller_modify` every loop turn | Defect | Modify only when the interest set changes | idem |
| 2a relay server: `zlink_msg_copy` (refcount share) per part + small vector/deque node allocs per reply | Acceptable per brief (shared copy, no byte copy > 64 B); allocs left for the §1.2 restoration job | none | — |
| 2a dealer_dealer client: stamp directly into the reusable retained buffer, one copy into the part | OK | none | — |
| 2b spin/fixed sleep in poll loops | Fixed (dd client/server POLLOUT suppression); others already suppressed; no fixed sleeps in event loops | see (e) | — |
| 2c wait bookkeeping O(1) | OK | one slot per socket, poller user_data -> slot | — |

Single-runner DEALER_ROUTER tcp 1024 B (blocking active-phase path; not touched by this review):

| Run | throughput (msg/s) | mean (ms) | p95 | p99 | Note |
|---|---|---|---|---|---|
| baseline (previous worker, duration 3, runs 1) | 442507 | 0.1767 | 0.2416 | 0.2959 | |
| after, smoke (duration 2, runs 1) | 444346 | 0.1497 | 0.2117 | 0.2624 | load < 10 |
| after, duration 3, runs 1 (1st) | 121339 | 0.1415 | 0.3101 | 0.8972 | load avg 25 (cpp multi perf + dotnet builds running concurrently) |
| after, duration 3, runs 1 (2nd) | 222642 | 0.1893 | 0.3344 | 0.9286 | load avg 22 |

The duration-3 numbers are contention artefacts (other review jobs on the same 11 GB / 16-core box); the same binary measured 444 K msg/s during the smoke, matching the baseline.

## Smoke gate

- `bash bindings/c/tests/run_tests.sh` (ZLINK_CORE_SOURCE=local, ZLINK_BUILD_JOBS=3, core/build): contract 9/9, samples 6/6 green. New `test_c_dontwait_backpressure_contract` cases 5/5 green (plus `test_c_completion_poller_contract`, `test_c_failure_boundary_contract` in the same loop). `perf_multi_metrics_test` 5/5.
- perf single: `run_benchmarks.sh --pattern PAIR,DEALER_ROUTER,PUBSUB --transports tcp,inproc --msg-sizes 1024 --duration 2 --runs 1` rc=0

  | pattern | tcp msg/s | tcp mean/p95/p99 ms | inproc msg/s | inproc mean/p95/p99 ms |
  |---|---|---|---|---|
  | PAIR | 369152 | 0.053 / 0.132 / 0.181 | 635558 | 0.004 / 0.006 / 0.015 |
  | DEALER_ROUTER | 444346 | 0.150 / 0.212 / 0.262 | 376619 | 0.029 / 0.100 / 0.152 |
  | PUBSUB | 375324 | 0.168 / 0.226 / 0.276 | 444013 | 0.018 / 0.041 / 0.094 |

- perf multi: `CCU=8 DUR=2 SIZES=1024,65536 PATTERNS=DEALER_DEALER,DEALER_ROUTER_SENDSEND,PUBSUB TIMEOUT=300` — the CCU/DUR/SIZES/PATTERNS/TIMEOUT variables belong to `bindings/c/perf/ci_multi_smoke.sh` (which wraps `run_benchmarks_multi.sh --transports tcp`); running `run_benchmarks_multi.sh` directly with them ignores them and runs the full matrix (aborted, then rerun through ci_multi_smoke.sh). rc=0, all RESULT lines present:

  | pattern | 1024 B msg/s | 1024 B mean/p95/p99 ms | 65536 B msg/s | 65536 B mean/p95/p99 ms |
  |---|---|---|---|---|
  | MULTI_DEALER_DEALER | 120588 | 0.172 / 0.392 / 1.140 | 10403 | 0.512 / 1.234 / 3.413 |
  | MULTI_DEALER_ROUTER_SENDSEND | 64751 | 0.494 / 1.321 / 2.307 | 10048 | 0.840 / 1.936 / 3.133 |
  | MULTI_PUBSUB | 76220 | 1.177 / 4.201 / 5.773 | 60 | 1.933 / 5.919 / 9.855 |

  MULTI_PUBSUB 65536 B at 60 msg/s is nonzero and the runner completed; the PUB path (publish DONTWAIT, no completion) is outside this port, and the run overlapped other jobs' benchmarks (load 20+). Worth a re-run on a quiet machine before reading it as a regression.
- `git diff --check` (bindings/c): clean. Header mirror `diff -rq bindings/c/include core/include`: clean.

## Tests added (public API, sleep-free)

`bindings/c/tests/test_c_dontwait_backpressure_contract.c`:
1. `completion_id_out = NULL` on a refused DONTWAIT send still reserves a wait token: after the peer drains, exactly one WRITABLE (nonzero ID, NULL context, same RID, ADMITTED) reaches the queue — the premise behind the REQUEST-drain fix in (e).
2. `zlink_disconnect_rid` on the target of a live token retires it as one WRITABLE with `ZLINK_SEND_TERMINAL` / `ENOENT`, same token, context and RID — checklist (c).

## BLOCKERS

None. Note for the later §1.2 restoration job: multi runner still runs cap-1 pending, two-phase; the relay server's per-reply vector/deque allocations are the remaining hot-path overhead there.
