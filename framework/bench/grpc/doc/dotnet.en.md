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

The inputs are exactly the environment variables in spec [§11](../README.en.md#11-runner-inputs-and-result-layout). There are no CLI options or positional arguments. Unless `SKIP_BUILD=1`, the runner builds `WithGrpcBench.sln` in Release with `dotnet/build.sh`. The binding
is the published package; the framework is the repository source. Measurements always go through
the perf ticket queue.

```bash
# full matrix
bash scripts/perf/perf-ticket.sh submit -p 1 -- bash framework/bench/grpc/dotnet/run_local.sh

# one cell
IMPLEMENTATIONS=zlink-framework-dotnet PATTERNS=request-serial PAYLOAD_SIZES=1024 RUNS=1 \
  bash framework/bench/grpc/dotnet/run_local.sh
```

The warmup call count, the process and operation ceiling, the settle quiet period and the drain
bound are fixed by the runner.

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

Results follow the §11 layout (`<OUTPUT_DIR>/run<N>/<implementation>-<pattern>-<payload>/results.json`).
A cell also keeps the diagnostics `source.log`, `target.log` and `target-stats.json`.
Only the aggregator produces tables and judgements. To measure every language and produce the
reports in one go, use `run_all.sh` (§11). To aggregate one language, pass it together with the C
results:

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang dotnet \
  --runs-glob 'framework/bench/grpc/log/<stamp>/c/run*' \
  --runs-glob 'framework/bench/grpc/log/<stamp>/dotnet/run*' --format full
```

## 6. Known limits

- No `unsupported` cell among the three patterns and three implementations.
- The raw comparison allows ROUTER↔ROUTER only.
- Framework A's RouteMesh listener uses loopback port 0; B's comparison endpoint is fixed at 5214.
- The framework `request-backpressure` cell surfaces admission rejections as request errors
  (decision records FB-042 and FB-047); it is recorded as an error cell and excluded from the
  throughput judgement.
