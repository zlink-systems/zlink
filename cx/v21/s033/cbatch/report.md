# TCP write batching vs. submit speed — C++ #1639 / #3919

## Result

Measured `/root/worktree/zlink-3867r` at `247b16c4be` (branch `perf/3867-cpp-request-receiver`). Each case used 1 KiB payloads, 30 s warm-up, 30 s active time, and three runs; the table shows per-run medians. Source processes were pinned to CPUs 0–7 and targets to 8–15. Each benchmark and its attached BPF collector held `flock -o /root/jobs/fx/cx/measure.lock`; runs were serialized. The benchmark’s existing request window and send concurrency of eight were unchanged.

Messages per successful send/receive are estimated as successful TCP payload bytes divided by the fixed decoded wire-record size and successful `sendto`/`recvfrom` calls. Wire sizes used from the coreio wire decode are Framework request/reply/one-way 1427/1417/1389 bytes and binding 1252/1252/1244 bytes. The current captures’ byte/message ratios agree within about 1% (partial-read and counter-boundary effects remain). HTTP statistics traffic is excluded by selecting the data TCP FD.

| Pattern / direction | Framework: msg/send | Framework: msg/receive | Binding: msg/send | Binding: msg/receive | Benchmark rate, F / B (msg/s) |
|---|---:|---:|---:|---:|---:|
| Request-backpressure, request source → target | 1.41 | 1.51 | 10.36 | 5.85 | 49,947 / 247,466 |
| Request-backpressure, reply target → source | 1.35 | 1.46 | 5.81 | 4.44 | 49,947 / 247,466 |
| Send-saturation, source → target | 4.43 | 4.38 | 13.16 | 6.59 | 180,002 / 558,133 |

## Rate-limited binding counterfactual

A limiter was added only to the copied benchmark client at `benchproj/client/bench_cpp_client.cpp`. It reserves a shared submit deadline across the eight send slots, or the request submitter, and busy-waits to that deadline. This avoids the timer wake-up granularity problem in a preliminary `sleep_until` version. The final binding run used the same request behavior and eight send slots as baseline.

| Pattern / direction | Requested submit rate | Observed benchmark rate, median | Msg/send | Msg/receive | Total I/O-thread CPU, source / target (µs/msg) | Hottest I/O thread, source / target (µs/msg) |
|---|---:|---:|---:|---:|---:|---:|
| Request-backpressure, request source → target | 49,947/s | 47,680/s | 1.01 | 1.04 | 33.95 / 37.22 | 19.15 / 18.61 |
| Request-backpressure, reply target → source | 49,947/s | 47,680/s | 1.12 | 1.12 | 33.95 / 37.22 | 19.15 / 18.61 |
| Send-saturation, source → target | 180,002/s | 179,938/s | 2.71 | 2.72 | 5.78 / 5.53 | 5.76 / 5.51 |

The request counterfactual is within 4.5% of Framework throughput and produces approximately one message per successful request send, versus 10.36 for saturated binding. Its reply batch is also near Framework’s 1.35. In send-saturation, rate-limiting binding from 558,133 to 179,938 msg/s reduces its batch from 13.16 to 2.71; Framework at 180,002 msg/s measures 4.43. Therefore batching strongly follows submit rate. The send-saturation result retains a 1.6× Framework/binding difference at the matched rate, so rate alone does not account for every batching difference in that pattern.

## Framework flush attribution

The measurements do not support a Framework per-message TCP flush. Rate-limited binding does not retain the saturated binding batch, and its request batch is effectively one. No explicit Framework TCP-flush call appears in the submit path:

- The benchmark’s `framework_driver_t::run_tasks` calls one `submit<T>` when admission permits, then tracks the preceding admission (`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:889–951`). `submit<T>` calls `send_to_node(...).async()` or `request_to_node(...).async()` (`:878–885`).
- The Framework request path reaches `raw_mesh_node_owner_t::request_to_node` and `raw_route_port_t::request_impl`, which submits the binding operation with `.async()` (`framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1324–1341`; `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:350–393`).
- The raw binding benchmark submits in `zlink_raw_driver_t::request_submitter` and `send_slot` (`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:673–745`). Neither side calls a TCP flush API per record. The eventual socket write is in Core’s `tcp_transport_t::write_some` (`core/src/runtime/transports/tcp/tcp_transport.cpp:661–745`); BPF’s sampled syscall stack included libc send and Boost.Asio’s socket-send operation.

The observable distinction is submission cadence/admission and resulting queue depth, not a Framework function issuing a separate TCP write for every message. This answers the conditional in question 3: there is no Framework per-message flush function to name from this evidence.

## Measurement effects and checks

BPF tracepoints collected syscall counts and successful byte totals during an 8.1 s active window. The copied coreio collector also records one user stack sample per TID/FD/syscall group after its fourth call. This tracing can perturb scheduling and therefore the absolute batches and CPU figures; CPU values below are measured while BPF is attached. Batch counts come from the byte/call counters, not `strace`; no `strace` was used. Full-phase benchmark rates include the BPF window. The minimum recorded `MemAvailable` was 27,041,072 KiB (about 25.8 GiB). Captures loaded Core library SHA-256 `6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01`.

The initial `sleep_until` limiter produced about 24,800 msg/s for a 49,947/s request target; those runs were excluded. The first send-saturation run with that limiter was interrupted before completion and was also excluded. Its outputs remain under `out/paced-*`; the final `spin-*` runs are the counterfactual set. No tests were run; this was a measurement-only task. Benchmark binaries were built under the prescribed build-slot wrapper into `cbatch/build` from the current worktree source and the copied client.

## Raw paths

- Baseline request captures: `/root/jobs/fx/cx/v21/s033/cbatch/out/{fw-req,bind-req}-r{1,2,3}/run1/*/capture/`; benchmark results are beside each `capture/` directory.
- Baseline send captures: `/root/jobs/fx/cx/v21/s033/cbatch/out/{fw-send,bind-send}-r{1,2,3}/run1/*/capture/`; benchmark results are beside each `capture/` directory.
- Final rate-limited binding captures: `/root/jobs/fx/cx/v21/s033/cbatch/out/spin-{req,send}-r{1,2,3}/run1/*/capture/`; benchmark results are beside each `capture/` directory.
- Counter summaries and medians: `/root/jobs/fx/cx/v21/s033/cbatch/bpf-summary.json` and `/root/jobs/fx/cx/v21/s033/cbatch/analysis-runs.json`.
- Copied measurement scripts: `/root/jobs/fx/cx/v21/s033/cbatch/runner/` and `/root/jobs/fx/cx/v21/s033/cbatch/tools/`; copied benchmark source and build: `/root/jobs/fx/cx/v21/s033/cbatch/benchproj/` and `/root/jobs/fx/cx/v21/s033/cbatch/build/`.
