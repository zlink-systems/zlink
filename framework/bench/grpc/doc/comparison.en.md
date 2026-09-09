# gRPC comparison report — framework messaging bench, round 2 (2026-09-09)

This page is the public result of the second-round measurement under the server-driven model (§10) of
the [bench specification](../README.en.md). How to run each language and its language-specific values
are in the [.NET](dotnet.en.md) · [Node.js](node.en.md) · [Java/Kotlin](java.en.md) · [C++](cpp.en.md)
pages; the judgement rules are owned by spec §7. This report quotes aggregator output only.

## 1. What is compared

The question in spec §0 is "within one language, does ZLink Framework channel messaging stand next to
a gRPC unary call, and how much does the Framework layer add on top of the raw binding". Each language
measures three implementations under the same conditions.

| Row | Meaning |
|---|---|
| `grpc-<lang>` | that language's standard gRPC unary `Echo` (request) / `Command` (send) |
| `zlink-<lang>` | the ZLink binding's raw ROUTER request/send |
| `zlink-framework-<lang>` | ZLink Framework RouteMesh typed channel request/send |
| `grpc-c` / `zlink-c` | the C harness baseline: the floor ZLink Core reaches on this machine |

Measurement model (spec §3, §10): source A receives warmup/active triggers over HTTP and sends to
target B. The 5-second active window counts completions (request) or messages B received with the
active header (send); tables carry the median of 3 runs. Patterns are `request-serial` (in-flight 1),
`request-window` (in-flight 100), `request-backpressure` (no bound) and `send-saturation` (8 streams,
no reply); payloads are 1024 and 4096 B. Everything ran on one WSL machine over loopback in a quiet
window (perf ticket queue).

| Language | Runtime | gRPC | ZLink binding / Core | Framework |
|---|---|---|---|---|
| C | GCC 13.3 | grpc 1.51.1 (C++) | Core 0.17.5 | — |
| .NET | SDK 8.0.130 / runtime 8.0.30 | Grpc.Net.Client, Grpc.AspNetCore 2.62.0 | 0.17.6 / 0.17.5 | 0.11.0 source |
| Java / Kotlin | Temurin 22.0.2, Kotlin 2.2.21 | grpc-java 1.72.0 / grpc-kotlin 1.4.1 | 0.17.6 / 0.17.5 | 0.11.0 source |
| Node.js | v22.23.2 | @grpc/grpc-js 1.14.4 | 0.17.6 / 0.17.5 | 0.11.0 source (framework rows unsupported) |
| C++ | GCC 13.3, C++20, `-O3` | grpc++ 1.51.1 | 0.17.6 / 0.17.5 | 0.11.0 source |

## 2. Results

Units: request patterns in **KOPS** (thousand request/reply completions per second), `send-saturation`
in **KMSG/s** (thousand messages per second received by B with the active header). Values are the
average over the 5-second active window after warmup, median of 3 runs. Latency is A's round trip for
request patterns and B's header-based arrival latency for send (the C harness does not measure send
latency). `drain` is the time from the close of the active window until counts stop moving (FB-051:
for send rows whose drain exceeds 10% of active, `received / (5000 + drain)` is the real consumption
rate). In the note column, `G5 n%` marks a row over the 10% 3-run reproducibility limit and `FB-nnn`
marks a row recorded as a product defect; rows carrying either are not used for judgement.

#### request-serial

| Implementation | payload | Throughput (KOPS) | Lat.mean (ms) | p95 (ms) | p99 (ms) | drain (ms) | Note |
|---|---:|---:|---:|---:|---:|---:|---|
| `grpc-c` | 1024 | 13.63 | 0.071 | 0.104 | 0.157 | — |  |
| `grpc-c` | 4096 | 12.90 | 0.074 | 0.110 | 0.156 | — |  |
| `zlink-c` | 1024 | 8.00 | 0.124 | 0.151 | 0.225 | — | G5 10.6% |
| `zlink-c` | 4096 | 8.00 | 0.124 | 0.152 | 0.225 | — | G5 13.3% |
| `grpc-dotnet` | 1024 | 5.88 | 0.167 | 0.322 | 0.405 | 273 | G5 18.9% |
| `grpc-dotnet` | 4096 | 5.18 | 0.189 | 0.339 | 0.422 | 273 |  |
| `zlink-dotnet` | 1024 | 7.52 | 0.131 | 0.179 | 0.250 | 276 |  |
| `zlink-dotnet` | 4096 | 6.83 | 0.143 | 0.185 | 0.260 | 272 |  |
| `zlink-framework-dotnet` | 1024 | 1.56 | 0.638 | 0.905 | 1.155 | 272 |  |
| `zlink-framework-dotnet` | 4096 | 1.55 | 0.639 | 0.909 | 1.309 | 271 |  |
| `grpc-java` | 1024 | 6.00 | 0.166 | 0.224 | 0.302 | 298 |  |
| `grpc-java` | 4096 | 6.00 | 0.167 | 0.219 | 0.302 | 299 |  |
| `zlink-java` | 1024 | 6.61 | 0.151 | 0.202 | 0.283 | 304 |  |
| `zlink-java` | 4096 | 6.64 | 0.150 | 0.198 | 0.264 | 300 |  |
| `zlink-framework-java` | 1024 | 0.47 | 2.137 | 2.579 | 2.765 | 271 |  |
| `zlink-framework-java` | 4096 | 0.47 | 2.150 | 2.564 | 2.676 | 271 |  |
| `grpc-node` | 1024 | 3.91 | 0.253 | 0.432 | 0.621 | 277 |  |
| `grpc-node` | 4096 | 3.72 | 0.266 | 0.453 | 0.660 | 279 |  |
| `zlink-node` | 1024 | 9.45 | 0.105 | 0.186 | 0.302 | 290 |  |
| `zlink-node` | 4096 | 9.27 | 0.107 | 0.178 | 0.311 | 288 |  |
| `zlink-framework-node` | 1024 | unsupported | — | — | — | — | codec has no protobuf bytes kind |
| `zlink-framework-node` | 4096 | unsupported | — | — | — | — | codec has no protobuf bytes kind |
| `grpc-cpp` | 1024 | 13.69 | 0.071 | 0.102 | 0.143 | — |  |
| `grpc-cpp` | 4096 | 13.41 | 0.072 | 0.108 | 0.147 | — |  |
| `zlink-cpp` | 1024 | 7.67 | 0.130 | 0.165 | 0.238 | — |  |
| `zlink-cpp` | 4096 | 7.39 | 0.135 | 0.170 | 0.248 | — | G5 15.5% |
| `zlink-framework-cpp` | 1024 | 0.49 | 2.020 | 2.990 | 3.321 | — |  |
| `zlink-framework-cpp` | 4096 | 0.53 | 1.881 | 2.250 | 3.190 | — |  |

#### request-window

| Implementation | payload | Throughput (KOPS) | Lat.mean (ms) | p95 (ms) | p99 (ms) | drain (ms) | Note |
|---|---:|---:|---:|---:|---:|---:|---|
| `grpc-c` | 1024 | 62.58 | 1.579 | 1.802 | 2.038 | — |  |
| `grpc-c` | 4096 | 55.88 | 1.763 | 2.026 | 2.253 | — |  |
| `zlink-c` | 1024 | 463.00 | 0.177 | 0.283 | 0.357 | — | G5 12.0% |
| `zlink-c` | 4096 | 382.50 | 0.207 | 0.347 | 0.432 | — | G5 11.8% |
| `grpc-dotnet` | 1024 | 139.57 | 0.712 | 2.652 | 3.633 | 271 |  |
| `grpc-dotnet` | 4096 | 99.38 | 0.999 | 2.740 | 3.833 | 271 |  |
| `zlink-dotnet` | 1024 | 98.90 | 1.009 | 1.764 | 2.810 | 273 |  |
| `zlink-dotnet` | 4096 | 89.91 | 1.107 | 2.095 | 3.024 | 275 |  |
| `zlink-framework-dotnet` | 1024 | 12.28 | 7.882 | 9.212 | 15.535 | 274 |  |
| `zlink-framework-dotnet` | 4096 | 11.45 | 8.684 | 10.106 | 104.741 | 275 |  |
| `grpc-java` | 1024 | 111.44 | 0.915 | 1.131 | 1.305 | 452 |  |
| `grpc-java` | 4096 | 91.38 | 1.105 | 1.577 | 2.047 | 486 |  |
| `zlink-java` | 1024 | 0 | 0.000 | 0.000 | 0.000 | 272 | FB-050 |
| `zlink-java` | 4096 | 0 | 0.000 | 0.000 | 0.000 | 273 | FB-050 |
| `zlink-framework-java` | 1024 | 2.34 | 1.973 | 2.844 | 3.102 | 275 |  |
| `zlink-framework-java` | 4096 | 2.28 | 2.013 | 2.899 | 3.199 | 276 |  |
| `grpc-kotlin` | 1024 | 87.30 | 1.062 | 1.418 | 2.248 | 472 |  |
| `zlink-framework-kotlin` | 1024 | 2.26 | 2.013 | 2.892 | 3.142 | 275 |  |
| `grpc-node` | 1024 | 13.53 | 7.394 | 12.117 | 16.030 | 291 |  |
| `grpc-node` | 4096 | 9.80 | 10.214 | 16.305 | 20.703 | 285 |  |
| `zlink-node` | 1024 | 0.07 | 6,452.798 | 30,001.253 | 30,001.344 | 268 | FB-049, G5 47.7% |
| `zlink-node` | 4096 | 0.03 | 12,988.932 | 30,001.336 | 30,001.905 | 270 | FB-049, G5 769.9% |
| `zlink-framework-node` | 1024 | unsupported | — | — | — | — | codec has no protobuf bytes kind |
| `zlink-framework-node` | 4096 | unsupported | — | — | — | — | codec has no protobuf bytes kind |
| `grpc-cpp` | 1024 | 58.88 | 1.697 | 1.939 | 2.128 | — |  |
| `grpc-cpp` | 4096 | 55.98 | 1.784 | 2.048 | 2.293 | — |  |
| `zlink-cpp` | 1024 | 176.31 | 0.567 | 0.665 | 0.777 | — |  |
| `zlink-cpp` | 4096 | 162.10 | 0.616 | 0.729 | 0.842 | — |  |
| `zlink-framework-cpp` | 1024 | 1.06 | 92.962 | 105.566 | 110.318 | — |  |
| `zlink-framework-cpp` | 4096 | 1.06 | 92.724 | 111.463 | 121.507 | — |  |

#### request-backpressure

| Implementation | payload | Throughput (KOPS) | Lat.mean (ms) | p95 (ms) | p99 (ms) | drain (ms) | Note |
|---|---:|---:|---:|---:|---:|---:|---|
| `grpc-c` | 1024 | 29.80 | 2,497.207 | 3,412.125 | 3,439.610 | — |  |
| `grpc-c` | 4096 | 31.15 | 2,182.147 | 3,897.705 | 3,968.772 | — | G5 13.7% |
| `zlink-c` | 1024 | 554.47 | 0.882 | 1.599 | 1.968 | — |  |
| `zlink-c` | 4096 | 428.11 | 0.358 | 0.634 | 0.826 | — |  |
| `grpc-dotnet` | 1024 | 70.41 | 21.422 | 29.807 | 50.364 | 274 |  |
| `grpc-dotnet` | 4096 | 55.06 | 15.679 | 44.859 | 57.773 | 271 |  |
| `zlink-dotnet` | 1024 | 32.44 | 0.226 | 0.360 | 0.511 | 273 |  |
| `zlink-dotnet` | 4096 | 21.61 | 0.250 | 0.393 | 0.591 | 273 |  |
| `zlink-framework-dotnet` | 1024 | 0.60 | 12,710.811 | 43,646.096 | 43,662.458 | 274 | FB-047, G5 254.7% |
| `zlink-framework-dotnet` | 4096 | 2.94 | 405.388 | 107.218 | 10,398.138 | 273 | FB-047, G5 89.8% |
| `grpc-java` | 1024 | 244.26 | 18,675.537 | 18,806.847 | 18,819.763 | 338 | G5 10.1% |
| `grpc-java` | 4096 | 129.42 | 2,187.882 | 3,707.333 | 4,041.575 | 350 |  |
| `zlink-java` | 1024 | 0 | 22,044.028 | 22,044.028 | 22,044.028 | 278 | FB-050 |
| `zlink-java` | 4096 | 0 | 29,420.284 | 29,420.284 | 29,420.284 | 275 | FB-050 |
| `zlink-framework-java` | 1024 | 2.27 | 1.985 | 2.855 | 3.113 | 275 |  |
| `zlink-framework-java` | 4096 | 2.32 | 1.985 | 2.850 | 3.098 | 277 |  |
| `grpc-node` | 1024 | 19.45 | 4,171.189 | 4,662.692 | 4,692.328 | 291 |  |
| `grpc-node` | 4096 | 15.27 | 5,291.201 | 5,653.218 | 5,672.899 | 287 |  |
| `zlink-node` | 1024 | 2.93 | 29,507.946 | 30,351.553 | 30,358.913 | 278 | FB-049 |
| `zlink-node` | 4096 | 0.08 | 27,772.410 | 33,796.750 | 33,797.073 | 269 | FB-049, G5 65.1% |
| `zlink-framework-node` | 1024 | unsupported | — | — | — | — | codec has no protobuf bytes kind |
| `zlink-framework-node` | 4096 | unsupported | — | — | — | — | codec has no protobuf bytes kind |
| `grpc-cpp` | 1024 | 40.50 | 359.091 | 423.440 | 432.093 | — |  |
| `grpc-cpp` | 4096 | 38.36 | 254.929 | 329.454 | 421.988 | — |  |
| `zlink-cpp` | 1024 | 144.03 | 0.122 | 0.185 | 0.293 | — |  |
| `zlink-cpp` | 4096 | 133.27 | 0.125 | 0.197 | 0.284 | — |  |
| `zlink-framework-cpp` | 1024 | 1.06 | 1,584.653 | 2,916.859 | 3,030.535 | — |  |
| `zlink-framework-cpp` | 4096 | 1.16 | 690.904 | 837.247 | 863.881 | — |  |

#### send-saturation

| Implementation | payload | Throughput (KMSG/s) | Lat.mean (ms) | p95 (ms) | p99 (ms) | drain (ms) | Note |
|---|---:|---:|---:|---:|---:|---:|---|
| `grpc-c` | 1024 | 57.55 | — | — | — | — |  |
| `grpc-c` | 4096 | 49.36 | — | — | — | — |  |
| `zlink-c` | 1024 | 689.19 | — | — | — | — |  |
| `zlink-c` | 4096 | 481.24 | — | — | — | — |  |
| `grpc-dotnet` | 1024 | 34.66 | 0.117 | 0.216 | 0.266 | 326 |  |
| `grpc-dotnet` | 4096 | 32.83 | 0.126 | 0.218 | 0.269 | 327 |  |
| `zlink-dotnet` | 1024 | 705.89 | 275.974 | 341.069 | 376.532 | 1027 |  |
| `zlink-dotnet` | 4096 | 377.06 | 141.821 | 185.627 | 226.306 | 601 |  |
| `zlink-framework-dotnet` | 1024 | 106.58 | 1,692.417 | 2,857.424 | 2,992.571 | 9725 |  |
| `zlink-framework-dotnet` | 4096 | 84.80 | 1,860.129 | 3,239.903 | 3,346.605 | 9630 | G5 46.1% |
| `grpc-java` | 1024 | 40.84 | 0.106 | 0.156 | 0.191 | 375 |  |
| `grpc-java` | 4096 | 41.46 | 0.104 | 0.151 | 0.180 | 398 |  |
| `zlink-java` | 1024 | 544.59 | 0.074 | 0.202 | 0.264 | 429 |  |
| `zlink-java` | 4096 | 348.09 | 0.074 | 0.221 | 0.272 | 446 |  |
| `zlink-framework-java` | 1024 | 10.52 | 2,433.846 | 2,593.367 | 2,621.482 | 2318 | G5 16.5% |
| `zlink-framework-java` | 4096 | 9.87 | 832.710 | 905.903 | 927.347 | 920 | G5 14.4% |
| `grpc-node` | 1024 | 9.89 | 0.364 | 0.607 | 0.977 | 289 |  |
| `grpc-node` | 4096 | 9.53 | 0.381 | 0.654 | 1.076 | 288 |  |
| `zlink-node` | 1024 | 265.84 | 261.539 | 529.316 | 550.065 | 1204 |  |
| `zlink-node` | 4096 | 139.93 | 0.284 | 1.284 | 3.123 | 493 |  |
| `zlink-framework-node` | 1024 | unsupported | — | — | — | — | codec has no protobuf bytes kind |
| `zlink-framework-node` | 4096 | unsupported | — | — | — | — | codec has no protobuf bytes kind |
| `grpc-cpp` | 1024 | 41.16 | 0.123 | 0.183 | 0.226 | — |  |
| `grpc-cpp` | 4096 | 39.41 | 0.129 | 0.191 | 0.225 | — |  |
| `zlink-cpp` | 1024 | 380.51 | 0.057 | 0.103 | 0.187 | — |  |
| `zlink-cpp` | 4096 | 336.75 | 0.068 | 0.130 | 0.259 | — |  |
| `zlink-framework-cpp` | 1024 | 0 | 0.000 | 0.000 | 0.000 | — | FB-054 |
| `zlink-framework-cpp` | 4096 | 0 | 0.000 | 0.000 | 0.000 | — | FB-054 |

### Ratios within one language

Two implementations of the same language divided (medians). Never read across languages (§4).

| Language | Pattern | payload | zlink raw / gRPC | framework / raw |
|---|---|---:|---:|---:|
| c | request-serial | 1024 | 0.59 | — |
| c | request-serial | 4096 | 0.62 | — |
| c | request-window | 1024 | 7.40 | — |
| c | request-window | 4096 | 6.85 | — |
| c | request-backpressure | 1024 | 18.61 | — |
| c | request-backpressure | 4096 | 13.74 | — |
| c | send-saturation | 1024 | 11.98 | — |
| c | send-saturation | 4096 | 9.75 | — |
| dotnet | request-serial | 1024 | 1.28 | 0.21 |
| dotnet | request-serial | 4096 | 1.32 | 0.23 |
| dotnet | request-window | 1024 | 0.71 | 0.12 |
| dotnet | request-window | 4096 | 0.90 | 0.13 |
| dotnet | request-backpressure | 1024 | 0.46 | 0.019 (FB-047) |
| dotnet | request-backpressure | 4096 | 0.39 | 0.14 (FB-047) |
| dotnet | send-saturation | 1024 | 20.37 | 0.15 |
| dotnet | send-saturation | 4096 | 11.49 | 0.22 |
| java | request-serial | 1024 | 1.10 | 0.071 |
| java | request-serial | 4096 | 1.11 | 0.070 |
| java | request-window | 1024 | 0.000 (FB-050) | — |
| java | request-window | 4096 | 0.000 (FB-050) | — |
| java | request-backpressure | 1024 | 0.000 (FB-050) | — |
| java | request-backpressure | 4096 | 0.000 (FB-050) | — |
| java | send-saturation | 1024 | 13.34 | 0.019 |
| java | send-saturation | 4096 | 8.40 | 0.028 |
| node | request-serial | 1024 | 2.42 | unsupported |
| node | request-serial | 4096 | 2.49 | unsupported |
| node | request-window | 1024 | 0.006 (FB-049) | unsupported |
| node | request-window | 4096 | 0.003 (FB-049) | unsupported |
| node | request-backpressure | 1024 | 0.15 (FB-049) | unsupported |
| node | request-backpressure | 4096 | 0.005 (FB-049) | unsupported |
| node | send-saturation | 1024 | 26.89 | unsupported |
| node | send-saturation | 4096 | 14.69 | unsupported |
| cpp | request-serial | 1024 | 0.56 | 0.065 |
| cpp | request-serial | 4096 | 0.55 | 0.072 |
| cpp | request-window | 1024 | 2.99 | 0.006 |
| cpp | request-window | 4096 | 2.90 | 0.007 |
| cpp | request-backpressure | 1024 | 3.56 | 0.007 |
| cpp | request-backpressure | 4096 | 3.47 | 0.009 |
| cpp | send-saturation | 1024 | 9.24 | 0.000 (FB-054) |
| cpp | send-saturation | 4096 | 8.55 | 0.000 (FB-054) |

## 3. Companion information per language

The companion information required by spec §7.1. Details are in each language page, §3 and §4.

| Language | warmup | gRPC server configuration | source saturation instrument (ceiling) | Notes |
|---|---|---|---|---|
| C | default | grpc++ synchronous | none | baseline; no send latency |
| .NET | 1,000 calls | Kestrel HTTP/2 `AddGrpc()` defaults | `submit_thread_cores` (1 / send 8) | framework send drain 9.6–9.7 s; backpressure 2,511 errors (FB-047) |
| Java | 20 s | grpc-netty-shaded defaults | `jvm_thread_cores` (1 / send 8) | grpc-java backpressure accepts unbounded submissions, 1M+ in flight (reached depth) |
| Kotlin (aux) | 20 s | reuses the Java B | same | `request-window @1024` only |
| Node.js | 1,000 calls | `@grpc/grpc-js` `Server` defaults | event-loop utilisation | framework rows unsupported (codec bytes) |
| C++ | 5 s | `ServerBuilder` synchronous defaults | `submit_thread_cores` (1 / send 8) | framework send fails entirely after the warmup flood (FB-054) |

## 4. How to read it

- **No cross-language comparison** (spec §7.3): runtimes, gRPC implementations, warmup and GC differ.
  Compare only the three rows of one language. The C baseline shows what Core can reach; it is not a
  target for the other languages.
- `request-serial` is a round-trip-latency contest. In every language gRPC shows the shorter latency
  (about 0.07 vs 0.12 ms) than ZLink raw, which comes from Core's per-connection I/O thread placement
  (FB-048).
- `request-window`, `request-backpressure` and `send-saturation` are concurrency contests. C, C++ and
  Node raw run several to more than ten times above gRPC; .NET raw is 20x ahead on send but below gRPC
  on window (0.71).
- Framework rows sit at 5–24% of raw (.NET 0.12–0.24, Java 0.05–0.07, C++ 0.006–0.07). This looks like
  a shared structural cost rather than a per-language bug and was moved to the 1.0 performance items
  (FB-047, FB-052).
- `request-backpressure` has no application bound, so the reached depth is the result: grpc-java stops
  at over a million in flight, grpc-c at multi-second latency, ZLink raw in the millisecond range.
- `unsupported` means the row could not be measured in that language (Node framework: codec bytes);
  `FB-nnn` means it was measured but a defect left the value at zero or mixed in errors, so the row is
  excluded from judgement.

## 5. Limits

- One machine (WSL2, 8 vCPU) over loopback. Network, multi-node and TLS are out of scope.
- The C baseline's request rows exceed the G5 limit at 10–13%, so formula 1 (raw / C) has no usable
  denominator (FB-048).
- Node and Java raw `request-window` lose completions/replies through binding 0.17.6 defects (FB-049,
  FB-050); their values are near zero and will be re-measured after the fix.
- C++ framework `send-saturation` is zero because the RouteMesh send target disappears after the warmup
  flood (FB-054).
- Send `KMSG/s` follows the spec (active-header receptions / 5 s); rows with long drains (.NET framework
  9.7 s, Node raw 1.2 s, Java framework 2.3 s) consume slower than the table shows.

## 6. Where the raw data is

- Aggregation records: `doc/plan/fw-bench-worklog/results/{s1-dotnet-c,s2-node,s2-java-kotlin,s3-cpp}-2026-09-09.md`
  (aggregator `--format full` output as is: tables, RESULT lines, medians, diagnostics, G5, judgement).
- Cell originals (JSON, logs): `framework/bench/grpc/log/<lang>/…` (gitignored, kept on the measuring machine).
- Decision records: `doc/plan/fw-bench-worklog/decisions.ko.md` FB-045 to FB-055.

## Appendix — formula judgement (spec §7.2, `request-window`)

formula 1 = `zlink-<lang> / zlink-c`, formula 2 = `zlink-framework-<lang> / zlink-<lang>`, pass line 0.80.

| Language | formula 1 @1024 / @4096 | formula 2 @1024 / @4096 | Status |
|---|---|---|---|
| .NET | (0.214) / (0.235) unsupported — denominator zlink-c G5 12.0% / 11.8% | **0.124 / 0.127 fail** | incomplete |
| Java | unsupported — numerator zlink-java lost (FB-050), denominator G5 | unsupported — denominator lost | incomplete |
| Node.js | unsupported — numerator G5 47.7% / 769.9% (FB-049), denominator G5 | unsupported — framework not measured | incomplete |
| C++ | (0.381) / (0.424) unsupported — denominator zlink-c G5 | **0.006 / 0.007 fail** | incomplete |
