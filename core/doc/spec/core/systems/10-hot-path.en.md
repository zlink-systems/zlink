---
title: "Core hot path"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/10-hot-path/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Core Design Decisions](09-design-decisions.en.md)
<!-- zlink-nav:end -->

# Core hot path

> **What this chapter defines** — The scope of the Core code that runs once per message (the
> hot path), what that code must not do, how state is cached for it, and the performance gates a
> hot-path change has to pass.

## 1. Why a separate contract

Most of the Core's consistency contracts (reconnect, generation, pair readiness, request
correlation) are implemented as general paths that re-interpret the current state. Those paths
were designed to run once per connection change; run once per message, they cost tens of percent
of throughput. Contract tests do not see the difference. When the 0.16.0 pull-completion and
single-lane transitions cut DEALER-family throughput by 25-35% and 6-16% respectively, every
contract test was green. Both changes had the same shape: they placed general-path work inside the
message path — re-resolving the selected pipe through its endpoint string, consulting the pair
table under its mutex, allocating temporary vectors.

The hot path is therefore governed by rules that differ from consistency code, and this chapter
fixes those rules.

## 2. Scope of the hot path

The hot path is the whole call tree from the following public entry points down to a pipe write
or a pipe read. This table is normative: a change to a function in it (or to one of its callees)
is subject to the rules of §3 and the gates of §5, and a change that inserts a new function into
this tree must update the table.

| Entry point | Path |
|---|---|
| `zlink_send_part` (PAIR, DEALER, ROUTER, STREAM) | `submit_completion_aware_part` → `send_completion_submit_blocking` / `send_pending_submit` → `try_admit_send_parts_scoped` → `xsend_selected_pipe` / `xsend_configured_endpoint` / `send_direct_with_retry` → `lb_t::sendpipe_to` → `pipe_t::write_*` |
| `zlink_send_part_rid` (ROUTER, STREAM) | as above, through the `send_direct_with_retry` branch |
| `zlink_request_part` FINAL (DEALER) | `request_part_common` → `submit_pull_blocking_request` → `request_admission_submit_blocking` → `try_admit_send_parts_scoped` → `arm_socket_pending_request_timeout` |
| `zlink_reply_part` FINAL (ROUTER) | `public_router_reply_submit` → `checkout_public_router_reply_target` → `send_public_router_reply_with_wait` → `retain_reply_transport_pipe` → `send_completion_staged_frames_on_pipe` |
| `zlink_recv_part` / `zlink_router_recv_part` | `recv_dealer_message_direct` / `router_recv_part_impl` → `recv_common` / `recv_routed` → `fq_t::recvpipe` → `pipe_t::read` → `reclassify_transport_pair_application_head` → `end_public_part_receive_delivery_hold` |
| `zlink_completion_recv` | `process_submit_commands` → `drive_send_pending` → `socket_completion::recv` |
| `zlink_poll` / `zlink_poller_wait` | `get_events_internal` → `process_commands` → `xhas_in` / `xhas_out` |
| I/O thread → socket delivery | `pipe_t::flush` → `activate_read` command → `xread_activated` → `fq_t::activated`; `process_async_mailbox` |

## 3. What the hot path must not do

Code on the hot path does none of the following. The only exception is the fallback path of §4.

1. **Heap allocation.** No per-message temporary `std::vector` or `std::string`, no `new`, no
   `make_shared`. Buffers that are needed reuse member scratch owned by the socket, the load
   balancer or the pipe.
2. **Resolving identity through strings.** A pipe is never looked up by building an endpoint
   identifier or a routing ID string. The `pipe_t*` obtained at selection time is used as is,
   within the same send scope.
3. **Socket-level table lookups and their mutexes.** Socket-level containers — the transport pair
   table, pending queue maps, route history — are not searched per message. The state the message
   path asks for is answered by the caches of §4.
4. **Unconditional side work.** Work that is only occasionally needed — releasing a hold,
   reclassifying a head, flushing deferred controls — first checks an atomic flag and takes a lock
   only when the flag says so.
5. **Peeking that puts the reader to sleep.** Receive-side code that inspects a pipe head looks
   only within the prefetched range. A probe that parks the ypipe produces an `activate_read`
   command round trip per message.
6. **Fixed-duration sleeps.** A retry waits by parking on the socket mailbox
   (`wait_submit_progress`). A fixed slice sleep stretches every flush wait of a framed transport
   (WS, WSS) to the slice length.
7. **Missed wake-ups by temporary owners.** Every path on which an async executor consumes
   commands on the socket's behalf and then detaches re-arms the public poller with
   `rearm_primary_signaler()`. Without it a poller sleeps until its own timeout.

Allowed: the pipe's own `_in_sync` / `_out_sync`, atomic loads and stores, fixed-size stack
arrays, and the send/recv scope already held.

## 4. State caches and the fallback path

Socket-level state that the message path needs is published into the pipe as atomics at the
point where it changes, and the message path reads only that cache.

| Question | Cache | Published at |
|---|---|---|
| Is this pipe the Application lane of a ready transport pair | `pipe_t::transport_pair_application_ready_cached()` | set at pair admission, cleared at the first physical detach |
| Is this pipe lifecycle-active | `pipe_t::is_lifecycle_active()` (mirror of `_state`) | every transition that leaves `active` |
| Is a public part-receive hold published | `_public_part_receive_delivery_hold_active` (atomic) | hold begin/end |

Only when the cache cannot answer (the selected pipe detached meanwhile, or backpressure requires a
retry wait) does the code fall back to the general path. The fallback keeps these rules:

- The first attempt admits directly to the selected pipe. Only a retryable refusal (`EAGAIN`,
  `ENOTCONN`, `EHOSTUNREACH`) commits that selection to the configured endpoint and enters the
  wait loop. Any other failure returns immediately with the errno the general path would have
  produced.
- The contract of the fallback (selection is committed once at FINAL, retries stay on the same
  endpoint) is unchanged by the existence of the fast path.

## 5. Performance gates

Every change to the hot path passes both of the following gates. Both are separate from the
contract tests; a change for which either gate did not run is unverified.

### 5.1 Instruction-count gate (`hotpath_gate`)

`core/tests/perf/hotpath_gate` measures, under callgrind, the number of instructions executed
per message. Unlike wall-clock throughput it is deterministic, so no re-measurement is needed.
The measured cells and their reference values are checked in as
`core/tests/perf/hotpath_reference.json`; a cell that moves by more than ±5% fails.

| cell | Measures |
|---|---|
| `dealer_dealer_inproc` | DEALER→DEALER one-way, send+recv instructions per message |
| `dealer_router_reqrep_inproc` | DEALER request → ROUTER reply → completion, instructions per request |
| `pair_inproc` | PAIR one-way |
| `router_router_tcp` | ROUTER↔ROUTER one-way (count-2 negative control) |

The reference is the value of a verified release or an approved change; an intended cost increase
is recorded only by a supervisory decision with its rationale. Implementation work does not edit
the reference. Where valgrind is unavailable the test is not registered; in that case "gate not
run" must appear in the report and does not count as green.

### 5.2 Release comparison gate

The C perf comparison against the previous Core release is performed by
`bindings/c/perf/perf_regression_gate.py` and judged in two stages. Release procedures cite this
gate; this section owns the criteria.

1. The 5% per cell (pattern, transport, size, metric) is the measurement tolerance:
   throughput and bandwidth `>= 0.95`, latency `<= 1.05`.
2. For each (pattern, transport), message sizes `64, 256, 1024, 65536` are all run and the
   geometric mean of the per-size ratios must be `>= 1.0` for throughput and bandwidth and
   `<= 1.0` for latency. No pattern/transport may average below the previous release.

The gate passes only when both the cell judgement and the aggregate judgement pass. A failing cell
is never handled by relaxing the gate or deferring it. If the benchmark's measurement is wrong (for
example reporting saturated queue depth as latency), the benchmark is fixed, not the gate.

## 6. Change procedure

1. A change to a hot-path function states in its diff which entry point of the §2 table it
   belongs to.
2. Code that violates any item of §3 is rewritten in the cache / fallback form of §4.
3. The §5.1 gate is run before and after the change and its result recorded. Release preparation
   also runs §5.2.
4. Adding a new entry point or a new per-message function adds a row to the §2 table and a cell to
   §5.1.
