# Java messaging bench (with the Kotlin supplementary cells)

This page records how the server-driven model of the [specification](../README.en.md) (§10) is
implemented in Java and how to run it. Comparison targets, patterns, units and judgement rules are
owned by the specification; this page holds only the Java-specific values. Kotlin measures only the
two supplementary cells of §10.5 (§7).

## 1. Comparison targets

| Implementation | request | send/command | API used |
|---|---|---|---|
| `grpc-java` | unary `Echo` on the grpc-java future stub | unary `Command`, `Empty` reply | grpc-java |
| `zlink-java` | raw ROUTER `request(peer)` | command-only raw ROUTER `send(peer)` | published `systems.zlink:zlink` |
| `zlink-framework-java` | `ZLinkRouteClient.requestToChannel(...).submit(BenchPayload.class)` | `sendToChannel(...).submit()` | `zlink-framework-core`, protobuf codec, Spring Boot starter |
| `grpc-kotlin` (supplementary) | `BenchServiceCoroutineStub.echo` suspend call | none | grpc-kotlin |
| `zlink-framework-kotlin` (supplementary) | `requestToChannel(...).awaitReply` | none | `zlink-framework-kotlin` |

Every row uses the same `BenchPayload` protobuf DTO and the 29-byte header in front of the body
(spec §6). raw sends the envelope and the protobuf body as two parts and keeps the request and
command endpoints separate.

## 2. How to run

The inputs are exactly the environment variables in spec [§11](../README.en.md#11-runner-inputs-and-result-layout). There are no CLI options or positional arguments. The Kotlin runner measures only the §10.5 supplementary cells
(`request-backpressure`, `1024`) and rejects other `PATTERNS` and `PAYLOAD_SIZES`. Unless
`SKIP_BUILD=1`, the runner builds first with `java/build.sh` (`./gradlew --no-daemon --max-workers=1 assemble
installDist`, Kotlin included).

```bash
cd framework/bench/grpc/java
./run_local.sh                                   # full Java matrix
./run_local_kotlin.sh                            # the two Kotlin supplementary cells
IMPLEMENTATIONS=zlink-framework-java PATTERNS=request-serial PAYLOAD_SIZES=1024 RUNS=1 ./run_local.sh
```

Measurements always go through the perf ticket queue. The Kotlin cells reuse Java's B, so they never
run at the same time as a Java measurement (the runner checks both port bands). The 20 s warmup, the
request and route-ready timeouts, the drain bound and the settle quiet period are fixed by the
runner.

## 3. Process layout

| Cell | source A | A ports (trigger/stats) | target B | B ports |
|---|---|---|---|---|
| `grpc-java` | `client`: unary future stub | 5240/5241 | `grpc-server`: echo/count | gRPC 5242, stats 5243 |
| `zlink-java` | `client`: raw ROUTER | 5245/5246 | `zlink-raw-server`: separate request and command ROUTERs | request 5247, command 5248, stats 5249 |
| `zlink-framework-java` | `client`: `ZLinkRouteClient` | 5252/5253 | `zlink-framework-server`: typed request/send handlers | RouteMesh 5254, stats 5255 |
| `grpc-kotlin` (supplementary) | `kotlin-client`: coroutine stub | 5260/5261 | Java `grpc-server` reused | 5242/5243 |
| `zlink-framework-kotlin` (supplementary) | `kotlin-client`: `awaitReply` | 5272/5273 | Java `zlink-framework-server` reused | 5254/5255 |

The trigger, stats and phase rules are owned by `shared/.../BenchHttpApplication.java` (JDK
`HttpServer`) alone. The trigger client is the runner's `curl`. The cell order is spec §10.4
(`runner_common.sh`) with a fresh A/B process pair per cell. The framework source reserves its fixed
HTTP ports first and then starts the RouteMesh listener. The runner checks LISTEN sockets on
5240-5259 (and 5260-5279 for Kotlin) before the run and after each cell.

| Pattern | `streams.count` | `streams.inFlightPerStream` | Implementation |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | sequential `CompletableFuture`s on one platform submit thread |
| `request-backpressure` | 1 | none | one submit thread submitting without a ceiling and yielding to completions |
| `send-saturation` | 8 | 1 | eight submit threads, each sending again after its completion notice |

## 4. Values set differently per language

| Item | Value |
|---|---|
| warmup | 20 s (1 s in smoke), same driver and streams as active |
| gRPC source | Java future stub or Kotlin coroutine stub, plaintext channel defaults |
| gRPC target | `ServerBuilder.forPort`, grpc-netty-shaded defaults, plaintext loopback |
| JDK | Temurin 22.0.2 (measured 2026-09-09; 25 from 1.0) |
| grpc-java / grpc-kotlin | 1.72.0 / 1.4.1 |
| protobuf-java | 4.30.2 |
| Kotlin / coroutines | 2.2.21 / 1.9.0 |
| ZLink binding | published 0.17.6 |
| framework | repository composite build (the measured commit is recorded in the raw file) |
| source saturation metric | `jvm_thread_cores`; the ceiling is the submit thread count (1 for serial/backpressure, 8 for send) |

## 5. Where results go

Results follow the §11 layout (`<OUTPUT_DIR>/run<N>/<implementation>-<pattern>-<payload>/results.json`).
A cell also keeps the diagnostics `source.log`, `target.log` and `target-stats.json`.
Only the aggregator produces tables and judgements. To measure every language and produce the
reports in one go, use `run_all.sh` (§11). To aggregate one language, pass it together with the C
results:

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang java \
  --runs-glob 'framework/bench/grpc/log/<stamp>/c/run*' \
  --runs-glob 'framework/bench/grpc/log/<stamp>/java/run*' \
  --runs-glob 'framework/bench/grpc/log/<stamp>/kotlin/run*' --format full
```

## 6. Known limits

- The raw comparison allows ROUTER↔ROUTER only.
- `request-backpressure` has no application in-flight ceiling, so the reached depth is the result.

## 7. Kotlin supplementary cells

Kotlin shares the binding, server and codec with Java, so it is excluded from the full matrix.
Only the two `request-backpressure @1024` cells (`grpc-kotlin`, `zlink-framework-kotlin`) run A in
Kotlin; B is the Java binary on the Java band. They appear next to the Java rows as supplementary
rows and are read as the cost of the Kotlin call layer.
