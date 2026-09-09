# .NET messaging bench

This page records how the server-driven model of the [specification](../README.en.md) (§10) is
implemented in .NET and how to run it. Comparison targets, patterns, units and judgement rules are
owned by the specification; this page holds only the .NET-specific values. The four language pages
share the same section numbers.

## 1. Comparison targets

| Implementation | request | send/command | API used |
|---|---|---|---|
| `grpc-dotnet` | `BenchServiceClient.EchoAsync` unary RPC | `BenchServiceClient.CommandAsync` unary RPC, `Empty` reply | `Grpc.Net.Client`, ASP.NET Core gRPC |
| `zlink-dotnet` | raw ROUTER `Request(peer).Message(...).Async()` | raw ROUTER `Send(peer).Message(...).Async()` | published `Zlink` binding |
| `zlink-framework-dotnet` | `RequestToChannel("bench", payload).Async<BenchPayload>()` | `SendToChannel("bench", payload).Async()` | `Zlink.Framework`, `Zlink.Framework.AspNetCore`, `Zlink.Framework.Codecs.Protobuf` |

All three use the same `BenchPayload` protobuf DTO and the 29-byte header in front of the body
(spec §6). raw sends the framework envelope and the protobuf body as two parts.

## 2. How to run

```bash
# full matrix (3 implementations × 4 patterns × payload 1024 and 4096)
./framework/bench/grpc/dotnet/run_local.sh

# one cell
PAYLOAD_SIZES=1024 ./framework/bench/grpc/dotnet/run_local.sh \
  --scenario request-window --implementation zlink-framework-dotnet
```

The runner builds `WithGrpcBench.sln` in Release (skipped with `SKIP_BUILD=1`). The binding is the
published package; the framework is the repository source.

| Input | Default | Behaviour |
|---|---|---|
| `PAYLOAD_SIZES` | `1024,4096` | payload list; other values fail preflight |
| `DURATION_SECONDS` | `5` | active window |
| `WARMUP` | `1000` | warmup calls before active |
| `REQUEST_WINDOW` | `100` | total in-flight of `request-window`; other values fail preflight |
| `SEND_CONCURRENCY` | `8` | stream count of `send-saturation`; other values fail preflight |
| `TIMEOUT_SECONDS` | `300` | process and operation ceiling |
| `COMMAND_SETTLE_MS` | `200` | minimum quiet period that counts as settled |
| `DRAIN_BOUND_MS` | `30000` | settle ceiling |
| `SKIP_BUILD` | `0` | `1` skips the solution build |
| `OUTPUT` | `framework/bench/grpc/log/dotnet/with_grpc_dotnet_<stamp>` | run root |
| `CONFIGURATION` | `Release` | build and `dotnet run` configuration |
| `--scenario` | `all` | `all`, `request`, or one of the four pattern names |
| `--implementation` | `all` | `all` or one of the three implementation names |

Measurements always go through the perf ticket queue (`scripts/perf/perf-ticket.sh submit -p 1 --
env SKIP_BUILD=1 bash framework/bench/grpc/dotnet/run_local.sh`).

## 3. Process layout

| Implementation | source A | A ports (trigger/stats) | target B | B ports |
|---|---|---|---|---|
| `grpc-dotnet` | `WithGrpcBench.Client`: one `GrpcChannel`, one unary stub per logical stream | 5200/5201 | `WithGrpcBench.GrpcServer`: `Echo` and `Command` | gRPC 5202, stats 5203 |
| `zlink-dotnet` | `WithGrpcBench.Client`: only the raw ROUTER sockets the cell needs | 5205/5206 | `WithGrpcBench.ZLinkRawServer`: request echo ROUTER and command count ROUTER kept separate | request 5207, command 5208, stats 5209 |
| `zlink-framework-dotnet` | `WithGrpcBench.Client`: `IZLinkRouteClient` | 5212/5213 | `WithGrpcBench.ZLinkServer`: typed request/send handlers | RouteMesh 5214, stats 5215 |

A's trigger, stats and phase rules reuse the canonical perf runner's
`ZLink.Framework.Perf.ServerSupport` (`BenchHttpApplication`) through a ProjectReference. The
trigger client is the runner's `curl` and generates no load. The cell order is spec §10.4 as is, and
every cell uses a fresh A/B process pair. The runner checks LISTEN sockets on 5200-5219 before the
run and after each cell and stops, without moving ports, if one is in use.

| Pattern | `streams.count` | `streams.inFlightPerStream` | .NET implementation |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | one sequential Task loop |
| `request-window` | 1 | 100 | 100 Tasks sharing one logical window |
| `request-backpressure` | 1 | none | submit without an application in-flight ceiling, `Task.Yield` every 256 |
| `send-saturation` | 8 | 1 | one Task with one gRPC stub or raw ROUTER per stream |

## 4. Values set differently per language

| Item | Value |
|---|---|
| warmup | 1000 calls (100 in smoke) |
| gRPC source | one `GrpcChannel`, `SocketsHttpHandler.MaxConnectionsPerServer=1`, `EnableMultipleHttp2Connections=false` |
| gRPC target | ASP.NET Core gRPC, Kestrel IPv4 loopback, HTTP/2, default `AddGrpc()` |
| .NET SDK / runtime | 8.0.130 / 8.0.30 (measured 2026-09-09) |
| gRPC packages | `Grpc.Net.Client`, `Grpc.AspNetCore`, `Grpc.Core.Api` 2.62.0 |
| ZLink binding | published 0.17.6 |
| framework | repository source ProjectReference (the measured commit is recorded in the raw file) |

## 5. Where results go

```text
framework/bench/grpc/log/dotnet/with_grpc_dotnet_<stamp>/
├── with_grpc_dotnet_<stamp>.txt
└── <implementation>-<pattern>-<payload>/
    ├── results.json        # with-grpc-cell-v1: role, trigger, streams, target_stats
    ├── report.txt          # RESULT lines
    ├── source.log / target.log
    └── target-stats.json
```

3-run aggregation:

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang dotnet --judgement-pattern request-window \
  --runs-glob 'framework/bench/grpc/log/dotnet/<run-1>/*' \
  --runs-glob 'framework/bench/grpc/log/dotnet/<run-2>/*' \
  --runs-glob 'framework/bench/grpc/log/dotnet/<run-3>/*' \
  --runs-glob 'framework/bench/grpc/log/c/<c-run>*' \
  --format full
```

## 6. Known limits

- No `unsupported` cell among the four patterns and three implementations.
- The raw comparison allows ROUTER↔ROUTER only (`RAW_SOCKET=router`).
- Framework A's RouteMesh listener uses loopback port 0; B's comparison endpoint is fixed at 5214.
- The framework `request-backpressure` cell surfaces admission rejections as request errors
  (decision records FB-042 and FB-047); it is recorded as an error cell and excluded from the
  throughput judgement.
