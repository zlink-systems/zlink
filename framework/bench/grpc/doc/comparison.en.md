# gRPC Comparison Report — Direct-RID Baseline

This document records the comparison axis and publication state of the new baseline defined by the
[bench specification](../README.en.md). Earlier measurements are not compared with this baseline:
the framework row included channel-level node selection and used a different pattern set. The
supervisor will fill in new three-run values after measuring on a quiet machine.

## 1. Comparison Rows

Only the following three rows are compared within one language. Every cell has one target server
(B).

| Row | Measured path |
|---|---|
| `grpc-<lang>` | Standard gRPC unary `Echo` / `Command` connected to one server |
| `zlink-<lang>` | Raw binding ROUTER↔ROUTER path addressed directly by RID, bypassing the framework |
| `zlink-framework-<lang>` | RouteMesh `requestToNode` / `sendToNode` addressed directly to the server RID |

The framework row no longer measures channel-level round-robin through `requestToChannel` /
`sendToChannel`. It now has the same 1:1 target-selection condition as gRPC, so node selection is
not mixed into the framework/raw ratio.

## 2. Wire and Patterns

Changing the routing API does not change the application wire. The framework row in all three
languages keeps the existing fixed ZLink envelope and the protobuf body
`BenchPayload { bytes body = 1; }`. The first 29 payload bytes remain the measurement header and
the rest remains business payload.

| Pattern | In-flight meaning | Request throughput basis |
|---|---|---|
| `request-serial` | 1 | Successful source request/reply completions |
| `request-backpressure` | No application ceiling; reached depth is recorded as an outcome | Successful source request/reply completions |
| `send-saturation` | 8 logical streams, one submission per stream | Active-header messages received by the target |

`request-window` is excluded because forcing depth 100 measures whether a stack reaches that depth
rather than isolating per-request cost. The aggregator may still read archived input for diagnosis,
but only the three patterns above appear in current tables and judgements.

## 3. Measurement Contract

The existing contract for warmup, active duration, admission/backpressure, in-flight measurement,
post-active drain, error and abandoned accounting, and result JSON fields is unchanged.
`send-saturation` is calculated from target receives, not source submissions. Cells with errors,
loss, or a drain-bound violation cannot satisfy publication gates. `request-backpressure` remains
the judgement pattern, evaluated separately at 1024 and 4096 bytes.

## 4. New Baseline Status

| Language | Framework path | Patterns | Baseline values |
|---|---|---|---|
| Java | Direct RID | Three | Round 1 implementation complete; awaiting the new joint baseline run |
| .NET | Direct RID | Three | Smoke validated; awaiting three-run measurement |
| C++ | Direct RID | Three | Smoke validated; awaiting three-run measurement |
| Node.js | Direct RID with schema protobuf serializer | Three | Smoke validated; awaiting three-run measurement |

Raw-row conversion is separate work in Issues #91–#93. New ratios are published only after both
the framework and raw rows satisfy specification §1.3. Short smoke values from this change verify
executability only and are not baseline numbers.

## 5. Result Table Names

Remeasurement output is grouped by pattern and uses these exact row names at each payload size.

| Language | gRPC | Raw | Framework |
|---|---|---|---|
| .NET | `grpc-dotnet` | `zlink-dotnet` | `zlink-framework-dotnet` |
| Java | `grpc-java` | `zlink-java` | `zlink-framework-java` |
| Node.js | `grpc-node` | `zlink-node` | `zlink-framework-node` |
| C++ | `grpc-cpp` | `zlink-cpp` | `zlink-framework-cpp` |

Request patterns use KOPS; `send-saturation` uses KMSG/s. Ratios computed from the old denominator
and old `request-window` values are not carried into this report.
