# Java messaging bench (Kotlin 보조 셀 포함)

이 문서는 [규격](../README.ko.md)의 server-driven 모델(§10)을 Java에서 어떻게 구현했는지와 실행
방법을 적는다. 비교 대상·패턴·단위·판정 규칙은 규격이 소유하고, 이 문서는 Java 고유의 값만 담는다.
Kotlin은 §10.5의 보조 셀 둘만 잰다(§7).

## 1. 비교 대상

| 구현 | request | send/command | 사용 API |
|---|---|---|---|
| `grpc-java` | grpc-java future stub의 unary `Echo` | unary `Command`, `Empty` reply | grpc-java |
| `zlink-java` | raw ROUTER `request(peer)` | command 전용 raw ROUTER `send(peer)` | published `systems.zlink:zlink` |
| `zlink-framework-java` | `ZLinkRouteClient.requestToChannel(...).submit(BenchPayload.class)` | `sendToChannel(...).submit()` | `zlink-framework-core`, protobuf codec, Spring Boot starter |
| `grpc-kotlin` (보조) | `BenchServiceCoroutineStub.echo` suspend 호출 | 없음 | grpc-kotlin |
| `zlink-framework-kotlin` (보조) | `requestToChannel(...).awaitReply` | 없음 | `zlink-framework-kotlin` |

모든 행은 같은 `BenchPayload` protobuf DTO와 body 앞의 29-byte header(규격 §6)를 쓴다. raw는
envelope와 protobuf body 두 part를 보내며 request와 command endpoint를 분리한다.

## 2. 실행 방법

```bash
cd framework/bench/grpc/java
./run_local.sh                                   # Java 전체 matrix
./run_local_kotlin.sh                            # Kotlin 보조 2셀
RUNS=1 PAYLOADS=1024 SCENARIO=request-window IMPLEMENTATION=zlink-framework-java ./run_local.sh
```

측정은 항상 perf 티켓 큐로 낸다. Kotlin 보조 셀은 Java의 B를 그대로 쓰므로 Java 측정과 동시에
돌리지 않는다(runner가 두 대역을 함께 확인한다).

| 입력 | Java 기본값 | Kotlin 기본값 |
|---|---|---|
| `RUNS` | `3` | `3` |
| `RUN_DEALER` | `0`(다른 값 거부) | `0` |
| `DURATION` | `5` | `5` |
| `WARMUP_SECONDS` | `20` | `20` |
| `PAYLOADS` | `1024,4096` | `1024`만 |
| `SCENARIO` | `all`, `request`, 네 패턴 이름 | `all`·`request`·`request-window` |
| `IMPLEMENTATION` | `all` 또는 세 구현 | `all` 또는 두 구현 |
| `STAMP` / `OUTROOT` | 실행 시각 / `log/java/with_grpc_java_<stamp>` | 실행 시각 / `log/java/with_grpc_kotlin_<stamp>` |
| `SKIP_BUILD` | `0`(`1`이면 Gradle build 생략) | 동일 |

고정값: request window 100, send concurrency 8, request·route-ready timeout 30초, drain 상한
30초, settle quiet 200ms. build는 `./gradlew --no-daemon --max-workers=1 assemble installDist`.

## 3. 프로세스 구성

| 셀 | source A | A 포트(trigger/stats) | target B | B 포트 |
|---|---|---|---|---|
| `grpc-java` | `client` — unary future stub | 5240/5241 | `grpc-server` — echo/count | gRPC 5242, stats 5243 |
| `zlink-java` | `client` — raw ROUTER | 5245/5246 | `zlink-raw-server` — request·command ROUTER 분리 | request 5247, command 5248, stats 5249 |
| `zlink-framework-java` | `client` — `ZLinkRouteClient` | 5252/5253 | `zlink-framework-server` — typed request/send handler | RouteMesh 5254, stats 5255 |
| `grpc-kotlin` (보조) | `kotlin-client` — coroutine stub | 5260/5261 | Java `grpc-server` 재사용 | 5242/5243 |
| `zlink-framework-kotlin` (보조) | `kotlin-client` — `awaitReply` | 5272/5273 | Java `zlink-framework-server` 재사용 | 5254/5255 |

trigger·stats·phase 규칙은 `shared/.../BenchHttpApplication.java`(JDK `HttpServer`) 한 곳이
소유한다. trigger client는 runner의 `curl`이다. 셀 순서는 규격 §10.4(`runner_common.sh`)이고 셀마다
새 A/B process 쌍을 쓴다. framework source는 고정 HTTP 포트를 먼저 예약한 뒤 RouteMesh listener를
시작한다. runner는 시작 전과 셀 종료 뒤 5240-5259(Kotlin은 5260-5279도)의 LISTEN 소켓을
확인한다.

| 패턴 | `streams.count` | `streams.inFlightPerStream` | 구현 |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | platform submit thread 하나의 순차 `CompletableFuture` |
| `request-window` | 1 | 100 | submit thread 하나가 `CompletableFuture` 100개로 한 window 공유 (Kotlin은 coroutine 100개) |
| `request-backpressure` | 1 | 없음 | submit thread 하나가 상한 없이 제출하고 completion에 양보 |
| `send-saturation` | 8 | 1 | submit thread 8개가 각각 완료 통지 뒤 다음 send |

## 4. 언어별로 다르게 둔 값

| 항목 | 값 |
|---|---|
| warmup | 20초 (smoke 1초). active와 같은 driver·stream 사용 |
| gRPC source | Java future stub 또는 Kotlin coroutine stub, plaintext channel 기본 구성 |
| gRPC target | `ServerBuilder.forPort`, grpc-netty-shaded 기본 구성, plaintext loopback |
| JDK | Temurin 22.0.2 (2026-09-09 측정; 1.0에서 25로) |
| grpc-java / grpc-kotlin | 1.72.0 / 1.4.1 |
| protobuf-java | 4.30.2 |
| Kotlin / coroutines | 2.2.21 / 1.9.0 |
| ZLink binding | published 0.17.6 |
| framework | 저장소 composite build(측정한 커밋을 원본에 기록) |
| source 포화 지표 | `jvm_thread_cores`; 상한은 submit thread 수(serial·window·backpressure 1, send 8) |

## 5. 결과 위치

```text
framework/bench/grpc/log/java/with_grpc_java_<stamp>/     # Kotlin은 with_grpc_kotlin_<stamp>
├── with_grpc_java_<stamp>.txt
└── <implementation>-<pattern>-<payload>-run<n>/
    ├── results.json        # with-grpc-cell-v1: role·trigger·streams·target_stats
    ├── report.txt
    ├── source.log / target.log
    └── target-stats.json
```

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang java --judgement-pattern request-window \
  --runs-glob 'framework/bench/grpc/log/java/<run-1>/*' \
  --runs-glob 'framework/bench/grpc/log/java/<run-2>/*' \
  --runs-glob 'framework/bench/grpc/log/java/<run-3>/*' \
  --runs-glob 'framework/bench/grpc/log/c/<c-run>*' --format full
```

## 6. 알려진 제약

- `zlink-java request-window`: binding 0.17.6에서 caller-owned `Received.close()` 뒤 reply
  completion이 전부 유실되는 결함(결정 기록 FB-050, `repro/`로 재현)이 있어 결함 수정 전에는 그
  행을 게재하지 않는다.
- raw 비교는 ROUTER↔ROUTER만 허용한다.
- `request-backpressure`는 application in-flight 상한이 없으므로 도달 깊이가 결과다.

## 7. Kotlin 보조 셀

Kotlin은 Java와 같은 binding·서버·codec을 쓰므로 전체 matrix에서 제외한다. `request-window @1024`
두 셀(`grpc-kotlin`, `zlink-framework-kotlin`)만 A를 Kotlin으로 돌리고 B는 Java 바이너리를 Java
대역에서 그대로 쓴다. 표에서는 Java 행 옆에 보조 행으로 싣고 Kotlin 호출층의 비용으로 읽는다.
