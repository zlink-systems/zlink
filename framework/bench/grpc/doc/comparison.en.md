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

The baseline was remeasured at five runs per language on 2026-09-12, after the
single-counter backpressure change. `tools/bench_aggregate.py` owns the aggregation
and the verdict — medians, G5 reproducibility, the §7.2 ratios, and **whether a ratio
may be published at all**.

| Language | Five runs | `request-backpressure` verdict | Peak in-flight (raw / framework) | Status |
|---|---|---|---|---|
| Java | Complete | 1.291 / 1.255 | **99–130 / 35–40** | **Pattern not reached — `#300`** |
| .NET | Complete | 1.309 / (0.948) G5 31.4% | **132 / 6,717** | **Pattern not reached — `#300`** |
| C++ | Complete | (0.087) G5 90.2% / (0.072) G5 99.7% | 2,130 / 19,014 | **Harness defect — `#296`** |
| Node.js | Remeasuring | — | — | Hung on run 2 |
| C (`zlink-c`) | Not possible | — | — | **Does not build — `#295`** |

`request-backpressure` sends with no ceiling until the pipe fills. **Only C++ reaches
that condition.** Java stays in double digits on both rows at 1.9% client CPU. Java's
`1.291` compares depth 35 against depth 99; it is not a layer cost. §7.2 already says
so: a ratio computed under a condition the rows cannot reach measures that condition,
not the layer.

**No layer is judged from these numbers yet.** Producing the baseline surfaced three
defects on the measuring side, and all three yield a number the harness made rather
than one the framework earned.

| Issue | What | What it blocks |
|---|---|---|
| `#295` | The C reference bench was left out of the whole-message migration in `#63` | `zlink-<lang> / zlink-c` does not compute, so **whether the denominator (raw) is healthy cannot be judged** |
| `#296` | The C++ bench waits a fixed 1 ms on an empty round (the canonical blocks for `min(remaining, 50 ms)`) | C++ `request-backpressure` G5 at 90–100% |
| `#300` | .NET raw caps itself at depth 132 in a pattern that has no ceiling (one tenth the throughput of C++ raw, at 6% CPU) | .NET's "1.309 pass" is a number its denominator produced |

As `#300` shows, a denominator can be sick. `#295` is therefore not supplementary
information but **a precondition of the verdict**. The §5 table is settled only after
the three are fixed and the baseline remeasured.

Raw-row conversion is separate work in Issues #91–#93. New ratios are published only after both
the framework and raw rows satisfy specification §1.3.

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
