# C++ messaging bench

This page records how the server-driven model of the [specification](../README.en.md) (§10) is
implemented in C++ and how to run it. Comparison targets, patterns, units and judgement rules are
owned by the specification; this page holds only the C++-specific values.

## 1. Comparison targets

| Implementation | request | send/command | Modules used |
|---|---|---|---|
| `grpc-cpp` | `PrepareAsyncEcho` + `CompletionQueue` (unary) | `PrepareAsyncCommand`, `Empty` reply | system `libgrpc++` |
| `zlink-cpp` | raw ROUTER `request(peer).message(...).async()` | command-only raw ROUTER `send(peer).message(...).async()` | packaged `zlink::cpp` |
| `zlink-framework-cpp` | `route_client_t::request_to_channel(...).async<BenchPayload>()` | `route_client_t::send_to_channel(...).async()` | repository `zlink::framework`, `zlink::framework_codec_protobuf` |

Every row uses the same `BenchPayload` protobuf DTO and the 29-byte header in front of the body
(spec §6). Raw sends two parts (envelope and protobuf body) and separates the request and command
endpoints. The framework row uses `route_client_t`, the RouteMesh client (`channel_client_t` is for
ClientServer channels).

## 2. How to run

The runner does not build. Build first; measurements always go through the perf ticket queue.

```bash
# Build — the local package root (default .artifacts/wsl) must hold install/zlink-cpp/0.17.6 and
# install/zlink-core/0.17.5 (a symlink to the release Core prefix is fine).
cmake -S framework/bench/grpc/cpp -B framework/bench/grpc/cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build framework/bench/grpc/cpp/build --parallel 2

# Full matrix, one run — always through the perf ticket queue
bash scripts/perf/perf-ticket.sh submit -p 1 -o <owner> -d "cpp with-grpc run 1/3" -- \
  bash framework/bench/grpc/cpp/run_local.sh cpp-s3-run1

# One cell
bash framework/bench/grpc/cpp/run_local.sh smoke --implementations zlink-framework-cpp \
  --patterns request-window --payload-sizes 1024 --duration-seconds 2
```

| Input | Default | Meaning |
|---|---|---|
| first argument | `cpp-router-1` | run label (result directory name) |
| `BUILD_DIR` | `cpp/build` | location of the prebuilt executables |
| `--implementations` / `IMPLEMENTATIONS` | all three | implementation list |
| `--patterns` / `PATTERNS` | all four | pattern list |
| `--payload-sizes` / `PAYLOAD_SIZES` | `1024,4096` | body sizes (other values are rejected) |
| `--duration-seconds` / `DURATION_SECONDS` | 5 | active seconds |
| `--warmup` / `WARMUP_SECONDS` | 5 | warmup seconds |
| `--warmup-segments` / `WARMUP_SEGMENTS` | 10 | warmup throughput observation segments |
| `REQUEST_WINDOW`, `SEND_CONCURRENCY` | 100, 8 | other values are rejected |
| `REQUEST_TIMEOUT_MS` / `DRAIN_BOUND_MS` | 30000 / 30000 | request timeout / drain and settle bound |
| `COMMAND_SETTLE_MS` | 200 | quiet window for stable receive/complete counts |
| `OUTPUT_DIR` / `--output-dir` | `log/cpp/<stamp>/<label>` | result directory |
| `RUN_STAMP` | current time | run group id |
| `LOAD_GATE` | 2.0 | load-average gate before measuring |

## 3. Process layout

| Cell | source A | A ports (trigger/stats) | target B | B ports |
|---|---|---|---|---|
| `grpc-cpp` | `bench_cpp_client` — async unary stub | 5280/5281 | `bench_cpp_grpc_server` — synchronous `ServerBuilder` | gRPC 5282, stats 5283 |
| `zlink-cpp` | `bench_cpp_client` — ROUTER + coroutine/poller | 5285/5286 | `bench_cpp_zlink_server` — separate request and command ROUTERs | request 5287, command 5288, stats 5289 |
| `zlink-framework-cpp` | `bench_cpp_client` — `route_client_t` | 5292/5293 | `bench_cpp_framework_server` — RouteMesh typed echo/count handlers | RouteMesh 5294, stats 5295 |

The trigger, stats and phase rules are owned by one place, `common/bench_stats_server.hpp`
(Framework public HTTP hosting). The trigger client is the runner's `curl`. Cell order follows spec
§10.4 and every cell uses a fresh A/B process pair. A starts its outbound transport only after both
HTTP listeners actually answer (on this machine `ip_local_port_range=1024 65535` lets an outbound
socket grab a fixed HTTP port otherwise). Framework A hosts the HTTP app and the channel app in one
process, started in order by a shared lifecycle. Object role is `None` on both A and B, with fixed
RIDs and manual peer connections. The runner checks LISTEN sockets on 5280-5299 before starting and
after each cell ends.

| Pattern | `streams.count` | `streams.inFlightPerStream` | Implementation |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | sequential submit/completion on one application thread |
| `request-window` | 1 | 100 | outstanding set of 100 gRPC async calls / raw coroutines / framework tasks |
| `request-backpressure` | 1 | none | submits without an application bound, yielding to the completion pump per submit |
| `send-saturation` | 8 | 1 | one stub per stream for gRPC, a coroutine slot for raw, a task slot for framework |

All three drivers observe submit/completion on one application thread. Framework task completion
is observed by polling the public `await_ready()`; that CPU is included in `submit_thread_cores`.
No driver drains transport completions directly or installs a second poller.

## 4. Values set differently per language

| Item | Value |
|---|---|
| warmup | 5 s in 10 segments; the trigger's `warmup` field is in ms (`5000`) |
| gRPC source | one channel, one unary stub per logical stream, `CompletionQueue` on the application thread |
| gRPC target | synchronous `ServerBuilder` defaults, insecure loopback |
| compiler | GNU C++ 13.3.0, C++20, Release `-O3` |
| gRPC / protobuf | system 1.51.1 / 3.21.12 |
| ZLink binding / Core | local package 0.17.6 / release 0.17.5 |
| framework | repository source (public CMake targets; the measured commit is recorded in the raw output) |
| source saturation instrument | `submit_thread_cores` (`CLOCK_THREAD_CPUTIME_ID`); ceiling 1 |
| latency sample caps | A 200,000, B 2,000,000 |

## 5. Where results go

```text
framework/bench/grpc/log/cpp/<stamp>/<label>/
├── runner.log · report.txt · load-gates.txt
└── <implementation>-<pattern>-<payload>/
    ├── results.json            # with-grpc-cell-v1: role, trigger, streams, target_stats
    ├── source.log / target.log
    ├── warmup-target-stats.json
    └── target-stats.json       # { "snapshot": <B stats response> }
```

The two stats files are diagnostic snapshots and are wrapped in a `snapshot` container (a
`role=target` object at the root would be mistaken for a standalone cell by the aggregator). In
`results.json`, `completed_at_close` and `server_received_at_close` are what A saw at the active
boundary; `completed` and `target_stats.received` are the final values after settle. The send
`KMSG/s` in the table is, per the spec, B's active-header receive count after settle divided by
`durationMs`; rows whose drain exceeds 10% of the active length are read together with `drain ms`
and the consumption rate (decision record FB-051).

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang cpp --judgement-pattern request-window \
  --runs-glob 'framework/bench/grpc/log/cpp/<run-1>/*' \
  --runs-glob 'framework/bench/grpc/log/cpp/<run-2>/*' \
  --runs-glob 'framework/bench/grpc/log/cpp/<run-3>/*' \
  --runs-glob 'framework/bench/grpc/log/c/<c-run>*' --format full
```

## 6. Known limits

- `zlink-framework-cpp` shows about 7% of raw on request-serial and roughly 1 ms of per-request
  serialisation on window (decision record FB-052). This is treated as a property of the framework
  C++ runtime, not a bench defect, and is judged on the 3-run values.
- The framework HTTP host does not return a listener bind failure as a start failure (FB-053). The
  runner confirms listener start by HTTP response readiness.
- The HTTP host exposes the same routes on all of its listen ports. The trigger URL records only the
  spec's trigger port and the runner uses only that URL.
- Raw comparison allows ROUTER↔ROUTER only. `request-backpressure` has no application in-flight
  bound, so the reached depth is the result.
