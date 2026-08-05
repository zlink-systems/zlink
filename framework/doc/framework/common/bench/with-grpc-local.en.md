# .NET Messaging Local Bench Specification

This document is the standard for comparing the relative cost of gRPC .NET, ZLink raw binding
.NET, and ZLink framework .NET in the same format on a local development machine. It doesn't
represent a production environment's mesh, TLS, L7 load balancer, multi-node distribution, or
network latency.

## 1. Comparison Targets

The comparison targets are the three implementations below. The report table and the `RESULT`
line use the same names.

| Implementation Name | Meaning |
|-----------|------|
| `grpc-dotnet` | ASP.NET Core gRPC unary RPC |
| `zlink-dotnet` | The raw .NET binding's DEALER/ROUTER TCP path, bypassing the framework |
| `zlink-framework-dotnet` | The client/server channel of framework channel messaging |

The default execution order is also `grpc-dotnet`, `zlink-dotnet`, `zlink-framework-dotnet`. Since
the three implementations run the same pattern at the same payload size and the same active
duration, the table shows the three implementations side by side under one pattern.

## 2. Measurement Patterns

The initial scope uses only two payload sizes and three patterns. The default payload sizes are
`1024` and `4096` bytes. The payload size is the full size of the protobuf `bytes body` or the raw
ZLink message body. The first 29 bytes are used as the measurement header, and the rest is filled
as the business payload area. The gRPC HTTP/2 frame, protobuf field overhead, ZLink envelope, and
ZMP header aren't included in this size.

| Pattern Name | gRPC .NET | ZLink binding .NET | ZLink framework .NET | Interpretation |
|-----------|-----------|--------------------|----------------------|------|
| `request-serial` | unary RPC | raw request callback | `RequestToChannel(...).Async<TReply>()` | Sends one request and sends the next only after the reply completes |
| `request-window` | unary RPC | raw request callback | `RequestToChannel(...).Async<TReply>()` | Keeps up to `request_window` incomplete requests |
| `send-saturation` | unary RPC returning empty reply | raw `Dealer.Send().Submit()` | `SendToChannel(...).Submit()` | Compares the command/send path, which has no reply payload |

`request-serial`'s throughput is decided by a single request's round-trip latency. This value is
meant to show the cost of a "process one at a time" usage pattern.

`request-window` reflects that a ZLink request can submit the next request without waiting for the
reply. During the active phase, the client keeps up to `request_window` incomplete requests. As
soon as one reply arrives, the next request is sent immediately in that slot. The default
`request_window` is `100`.

`send-saturation` isn't averaged together with request/reply. A gRPC empty unary still waits for
the empty reply, but a ZLink send is a submission path with no reply. This pattern exists to look
at the command/send family's relative cost separately.

## 3. Execution Conditions

- Run with 1 client process and 1 server process per comparison target.
- The local runner brings up the gRPC server, ZLink raw binding server, and ZLink framework server
  separately.
- Only a loopback address (`127.0.0.1`) is used.
- Runs as a Release build.
- Runs a fixed-duration measured active window after warmup.
- The default payload size is `1024,4096` bytes.
- The default `request_window` is `100`.
- The default send concurrency is `8`.
- gRPC and ZLink framework use the same protobuf DTO. The ZLink raw binding sends the same bytes
  payload without a protobuf envelope.
- ZLink uses a manual endpoint connection with no location store.
- The ZLink raw binding's request echo endpoint and command receive endpoint are separated. In the
  command measurement, if request echo replies mix into the same socket, the one-way receive volume
  with no reply can't be observed correctly.
- TLS, compression, service mesh, gateway, and broker are not used.

## 4. Output Format

The report table is grouped by pattern, with the implementation name shown on each row. The
example follows the format below.

```text
  > Benchmarking current for request-window...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-dotnet             | 1024B    |       10.00 KOPS |   10.24 MB/s |     1.000 ms |     2.000 ms |     3.000 ms |      10.0% |  100.0 MB |      10.0% |  100.0 MB |
      | zlink-dotnet            | 1024B    |       30.00 KOPS |   30.72 MB/s |     0.300 ms |     0.600 ms |     0.900 ms |      10.0% |  100.0 MB |      10.0% |  100.0 MB |
      | zlink-framework-dotnet  | 1024B    |       20.00 KOPS |   20.48 MB/s |     0.500 ms |     0.900 ms |     1.200 ms |      10.0% |  100.0 MB |      10.0% |  100.0 MB |
```

The report and console output also leave a per-metric `RESULT,current,...` format alongside, for
easier handling by the perf runner. The fields of one row are in the following order.

```text
RESULT,current,<scenario>,local,<payload_size>,<metric>,<value>
```

An example follows.

```text
RESULT,current,grpc-dotnet-request-window,local,1024,throughput,10000.000
RESULT,current,zlink-dotnet-request-window,local,1024,latency,0.300
RESULT,current,zlink-framework-dotnet-request-window,local,1024,latency_p99,1.200
```

The `metric` value uses `throughput`, `bandwidth`, `latency`, `latency_p95`, `latency_p99`,
`client_cpu_percent`, `client_memory_mb`, `server_cpu_percent`, `server_memory_mb`. The raw value
of `throughput` is completions per second, and the table displays it divided into `KOPS` or
`KMSG/s`.

## 5. Metrics

The required output is the metrics below. The throughput unit is separated to match the pattern's
character.

| Metric | Meaning |
|--------|------|
| `Throughput` | Throughput during the measured window. The table shows the value and unit together in one cell, e.g. `10.000 KOPS`, `183.618 KMSG/s` |
| `Bandwidth` | The transfer volume calculated from payload size and throughput. Shown as `MB/s` |
| `Lat.Mean(ms)` | Mean latency. For request/reply this is the client round-trip latency; for send it's the receive latency the server calculates from the header |
| `Lat.P95(ms)` | p95 latency |
| `Lat.P99(ms)` | p99 latency |
| `Client CPU` | The CPU percentage the client process used during the active window |
| `Client Mem` | The client process's working set |
| `Server CPU` | The CPU percentage that implementation's server process used during the active window |
| `Server Mem` | That implementation's server process working set |

`request-serial` and `request-window` calculate `KOPS` based on the number of completions where an
echo reply came back. Here, `1 KOPS` means 1,000 request/reply completions per second.

`send-saturation` calculates `KMSG/s` based on the number of messages the server received during the
active phase. Here, `1 KMSG/s` means 1,000 messages per second. Since a ZLink send doesn't wait for
a reply, throughput isn't calculated from the client's `Submit()` call count alone.

## 6. The Measurement Payload Header

A 29-byte header with the same meaning as `bindings/c/perf`'s metric header is placed at the front
of the measured payload. This header is part of the payload body, going at the front of the
protobuf `bytes body` or the raw ZLink message body.

| Offset | Size | Value |
|--------|------|----|
| 0 | 4 | magic `0x5A4C4E4B` (`ZLNK`) |
| 4 | 4 | run id |
| 8 | 1 | phase (`0` warmup, `1` active) |
| 9 | 4 | payload size |
| 13 | 8 | sequence |
| 21 | 8 | send timestamp ns |

For `request-serial` and `request-window`, the client validates the header that came back in the
reply payload, and calculates `KOPS` from the active-phase reply count. `send-saturation` has the
server read the header to calculate the active-phase message count and server-side receive
latency. This approach puts the measurement value inside the payload so a one-way throughput isn't
overstated by a client stopwatch alone.

## 7. Interpreting Results

This bench is a small local comparison tool. To put a performance-superiority statement into a
result document or guide, keep the following information alongside it.

- CPU, OS, .NET SDK version
- Commit hash
- Payload size
- Warmup and active duration configuration
- gRPC and ZLink endpoint
- The request window value
- The send concurrency value
- The original result JSON

Performance judgment compares within the same ZLink layer, not against gRPC. The raw .NET binding
is judged against the `zlink-c` request-window result under the same conditions. If the raw .NET
binding reaches 80% or more of the C result, the binding layer's baseline performance is judged as
passing. Framework .NET is judged against the raw .NET binding result. If the framework reaches
80% or more of the raw .NET binding result, the framework's added cost is judged as passing.

This criterion is applied mainly to patterns like request-window, where ZLink can send the next
request without waiting for the reply. `request-serial` is kept as a supplementary metric for
looking at the round-trip latency of a process-one-at-a-time usage pattern.

Log units can differ per runner. The C report of `bindings/c/bench/with_grpc` records the
`throughput` value in KOPS, while the .NET with-grpc report records the `RESULT` line's
`throughput` value as completions per second. When comparing C and .NET directly, divide the .NET
value by 1000 to align it to KOPS before calculating the ratio.

Results are interpreted only as "faster/slower under this condition." No claim of general
production performance or superiority across every payload is made.
