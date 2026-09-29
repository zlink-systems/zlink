# Node.js messaging bench

This page records how the server-driven model of the [specification](../README.en.md) (§10) is
implemented in Node.js and how to run it. Comparison targets, patterns, units and judgement rules
are owned by the specification; this page holds only the Node-specific values.

## 1. Comparison targets

| Implementation | request | send/command | API used |
|---|---|---|---|
| `grpc-node` | `BenchService.Echo` unary (callback wrapped in a Promise) | `BenchService.Command` unary, `Empty` reply | `@grpc/grpc-js`, `@grpc/proto-loader` |
| `zlink-node` | raw ROUTER `socket.request(peer).message(...).timeout(...).submit()` | raw ROUTER `socket.send(peer).message(...).submit()` | published `@zlink-systems/zlink` |
| `zlink-framework-node` | framework channel request | framework channel send | `@zlink-systems/framework`, `@zlink-systems/nestjs` |

gRPC and raw use the same `BenchPayload` protobuf body and the 29-byte header in front of it
(spec §6). raw sends the framework envelope and the protobuf body as two parts.

## 2. How to run

The inputs are exactly the environment variables in spec [§11](../README.en.md#11-runner-inputs-and-result-layout). There are no CLI options or positional arguments. Unless `SKIP_BUILD=1`, the runner first runs `npm ci` and `npm run build`.
Measurements always go through the perf ticket queue.

```bash
# full matrix
bash scripts/perf/perf-ticket.sh submit -p 1 -o <owner> -d "node with-grpc run" -- \
  bash framework/bench/grpc/node/run_local.sh

# one cell
IMPLEMENTATIONS=zlink-node PATTERNS=request-serial PAYLOAD_SIZES=1024 RUNS=1 \
  bash framework/bench/grpc/node/run_local.sh
```

The warmup call count, the process ceiling, the route, request and drain ceilings and the settle
quiet period are fixed by the runner.

## 3. Process layout

| Implementation | source A | A ports (trigger/stats) | target B | B ports |
|---|---|---|---|---|
| `grpc-node` | `client/main.js`: one unary stub per logical stream | 5220/5221 | `grpc-server/main.js`: echo/count | gRPC 5222, stats 5223 |
| `zlink-node` | `client/main.js`: one raw ROUTER for request or eight for send | 5225/5226 | `zlink-raw-server/main.js`: separate request and command ROUTERs | request 5227, command 5228, stats 5229 |
| `zlink-framework-node` | `client/main.js` | 5232/5233 | `zlink-framework-server/main.js` | RouteMesh 5234, stats 5235 |

The trigger, stats and phase rules are owned by `shared/bench-http-application.js` alone (Node
`http`). The trigger client is the runner's `curl` and generates no load. The cell order is spec
§10.4 as is, with a fresh A/B process pair per cell. The runner checks LISTEN sockets on 5220-5239
before the run and after each cell and stops, without moving ports, if one is in use.

| Pattern | `streams.count` | `streams.inFlightPerStream` | Node implementation |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | one sequential Promise loop |
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

Results follow the §11 layout (`<OUTPUT_DIR>/run<N>/<implementation>-<pattern>-<payload>/results.json`).
A cell also keeps the diagnostics `source.log`, `target.log` and `target-stats.json`.
Only the aggregator produces tables and judgements. To measure every language and produce the
reports in one go, use `run_all.sh` (§11). To aggregate one language, pass it together with the C
results:

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang node \
  --runs-glob 'framework/bench/grpc/log/<stamp>/c/run*' \
  --runs-glob 'framework/bench/grpc/log/<stamp>/node/run*' --format full
```

## 6. Known limits

- The raw comparison is fixed to ROUTER↔ROUTER (no DEALER side run).
