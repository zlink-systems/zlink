# Node.js messaging bench

This page records how the server-driven model of the [specification](../README.en.md) (§10) is
implemented in Node.js and how to run it. Comparison targets, patterns, units and judgement rules
are owned by the specification; this page holds only the Node-specific values.

## 1. Comparison targets

| Implementation | request | send/command | API used |
|---|---|---|---|
| `grpc-node` | `BenchService.Echo` unary (callback wrapped in a Promise) | `BenchService.Command` unary, `Empty` reply | `@grpc/grpc-js`, `@grpc/proto-loader` |
| `zlink-node` | raw ROUTER `socket.request(peer).message(...).timeout(...).submit()` | raw ROUTER `socket.send(peer).message(...).submit()` | published `@zlink-systems/zlink` |
| `zlink-framework-node` | framework channel request (planned) | framework channel send (planned) | `@zlink-systems/framework`, `@zlink-systems/nestjs`; every cell is `unsupported` today (§6) |

gRPC and raw use the same `BenchPayload` protobuf body and the 29-byte header in front of it
(spec §6). raw sends the framework envelope and the protobuf body as two parts.

## 2. How to run

```bash
# full matrix, always through the perf ticket queue
bash scripts/perf/perf-ticket.sh submit -p 1 -o <owner> -d "node with-grpc run" -- \
  bash framework/bench/grpc/node/run_local.sh

# one cell
env PAYLOADS=1024 SCENARIO=request-window IMPLEMENTATION=zlink-node \
  bash framework/bench/grpc/node/run_local.sh
```

| Input | Default | Behaviour |
|---|---|---|
| `RUNS` | `1` | runs per runner process (measurement tickets are per run) |
| `RUN_DEALER` | `0` | only `0` is accepted under the comparison contract |
| `DURATION` | `5` | active window in seconds |
| `WARMUP` | `1000` | warmup calls before active |
| `PAYLOADS` | `1024,4096` | payload list; other values fail preflight |
| `SCENARIO` | `all` | `all`, `request`, or one of the four pattern names |
| `IMPLEMENTATION` | `all` | `all` or one of the three implementation names |
| `WINDOW` | `100` | `request-window` in-flight; other values fail preflight |
| `STAMP` | run time | run id and result path |
| `OUTROOT` | `framework/bench/grpc/log/node/with_grpc_node_<stamp>` | run root |
| `SKIP_BUILD` | `0` | `1` skips `npm ci` and `npm run build` |

Fixed: send concurrency 8, process ceiling 300 s, route/request/drain ceiling 30 s, settle quiet
period 200 ms.

## 3. Process layout

| Implementation | source A | A ports (trigger/stats) | target B | B ports |
|---|---|---|---|---|
| `grpc-node` | `client/main.js`: one unary stub per logical stream | 5220/5221 | `grpc-server/main.js`: echo/count | gRPC 5222, stats 5223 |
| `zlink-node` | `client/main.js`: one raw ROUTER for request or eight for send | 5225/5226 | `zlink-raw-server/main.js`: separate request and command ROUTERs | request 5227, command 5228, stats 5229 |
| `zlink-framework-node` | not started (unsupported) | 5232/5233 (reserved) | `zlink-framework-server/main.js` | RouteMesh 5234, stats 5235 (reserved) |

The trigger, stats and phase rules are owned by `shared/bench-http-application.js` alone (Node
`http`). The trigger client is the runner's `curl` and generates no load. The cell order is spec
§10.4 as is, with a fresh A/B process pair per cell. The runner checks LISTEN sockets on 5220-5239
before the run and after each cell and stops, without moving ports, if one is in use.

| Pattern | `streams.count` | `streams.inFlightPerStream` | Node implementation |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | one sequential Promise loop |
| `request-window` | 1 | 100 | 100 Promise workers sharing one socket's logical window |
| `request-backpressure` | 1 | none | Promises without a ceiling, yielding to the event loop every 256 |
| `send-saturation` | 8 | 1 | one Promise worker with one gRPC stub or raw ROUTER per stream |

## 4. Values set differently per language

| Item | Value |
|---|---|
| warmup | 1000 calls (100 in smoke) |
| gRPC source | `@grpc/grpc-js` unary client stubs, insecure loopback, no tuning |
| gRPC target | `@grpc/grpc-js` `Server` with default options, insecure loopback |
| Node runtime | v22.23.2 (measured 2026-09-09) |
| gRPC packages | `@grpc/grpc-js` 1.14.4, `@grpc/proto-loader` 0.7.15 |
| ZLink binding | published 0.17.6 |
| framework | workspace source (the measured commit is recorded in the raw file) |

## 5. Where results go

```text
<OUTROOT>/
├── with_grpc_node_<stamp>.txt
├── unsupported.json            # cells not run, with the reason
└── <implementation>-<pattern>-<payload>-run<run>/
    ├── results.json            # with-grpc-cell-v1: role, trigger, streams, target_stats
    ├── report.txt
    ├── source.log / target.log
    └── target-stats.json
```

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang node --judgement-pattern request-window \
  --runs-glob '<run-1>/*' --runs-glob '<run-2>/*' --runs-glob '<run-3>/*' \
  --runs-glob 'framework/bench/grpc/log/c/<c-run>*' --format full
```

## 6. Known limits

- Every `zlink-framework-node` cell is `unsupported`: the framework's protobuf codec does not
  preserve `bytes` (a product defect handled separately). The runner starts no process and records
  the reason in `unsupported.json`.
- `zlink-node request-window` reproduces the completion loss of binding 0.17.6 (decision record
  FB-049) and is recorded as an error cell; that row is not published until the defect is fixed.
- The raw comparison is fixed to ROUTER↔ROUTER (no DEALER side run).
