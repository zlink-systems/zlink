# fwb2-06 결과 — Java server-driven runner와 Kotlin 보조 셀

## 결과

`framework/bench/grpc/java/`를 셀마다 source A와 target B를 새로 시작하는 server-driven 구조로
개정했다. runner는 B → A → warmup trigger → active trigger → bounded settle → B 통계 병합 →
count 대조 → process 종료 순서를 소유한다. Java의 세 구현과 네 패턴을 선택할 수 있고, Kotlin
runner는 `request-window @1024`의 `grpc-kotlin`·`zlink-framework-kotlin` 두 셀만 실행한다.

- 공통 `BenchHttpApplication`이 JDK `HttpServer` 두 개로 trigger와 source stats를 제공한다.
  같은 `runId`·`cellId`·`phase`의 중복 trigger는 저장한 acknowledgement를 그대로 반환한다.
- source 원본은 `with-grpc-cell-v1`이며 `role`, trigger 필수 8필드, `streams`, 병합 전
  `target_stats=null`을 기록한다. runner가 settle 뒤 `target_stats`를 채운다.
- request count는 `completed <= received <= completed + errors + abandoned`를 검사하고,
  `errors=abandoned=0`이면 등식을 추가로 검사한다.
- Java·Kotlin smoke 7셀은 모두 rc=0이고 count가 일치했다. 집계기 `--lang java --format spec4`도
  원본을 읽어 표를 출력했다.
- 별도 `repro/`는 published binding 0.17.6에서 `100`개 중 `0`개 reply 완료로 회귀를 재현했다.
  계획 §9의 중단 조건에 해당하므로 Java raw `request-window`의 3-run 측정은 진행할 수 없다.
- Core, binding, Framework runtime, 집계기, 다른 언어 bench, 규격·계획 문서는 수정하지 않았다.
  3-run 측정도 수행하지 않았다.

작업 기준 branch는 `main`, HEAD는 `d84aa56f98d83ae51328e690fcbd18ce28afea72`다.

## 프로세스 구성

| 셀 | source A | A 포트 | target B | B 포트 | 모듈·호출 |
|---|---|---|---|---|---|
| `grpc-java` | `client`의 Java source가 unary future stub으로 B를 호출 | trigger `5240`, stats `5241` | `grpc-server`가 Echo·Command와 수신 통계를 제공 | gRPC `5242`, stats `5243` | grpc-java `1.72.0` |
| `zlink-java` | `client`의 raw ROUTER가 request 또는 command B에 연결 | trigger `5245`, stats `5246` | `zlink-raw-server`의 request·command ROUTER | request `5247`, command `5248`, stats `5249` | `systems.zlink:zlink` `0.17.6` |
| `zlink-framework-java` | `client`의 `ZLinkRouteClient` channel client | trigger `5252`, stats `5253` | `zlink-framework-server`의 typed request·send handler | RouteMesh `5254`, stats `5255` | `zlink-framework-core`·protobuf codec·Spring Boot starter `0.10.0` |
| `grpc-kotlin` 보조 | `kotlin-client`의 `BenchServiceCoroutineStub` | trigger `5260`, stats `5261` | Java `grpc-server` 재사용 | gRPC `5242`, stats `5243` | grpc-kotlin `1.4.1`, coroutines `1.9.0` |
| `zlink-framework-kotlin` 보조 | `kotlin-client`의 `awaitReply` suspend 호출 | trigger `5272`, stats `5273` | Java `zlink-framework-server` 재사용 | RouteMesh `5254`, stats `5255` | `zlink-framework-kotlin` `0.10.0` |

Java runner는 시작 전과 각 셀 종료 뒤 `5240-5259`의 LISTEN socket을 확인한다. Kotlin runner는
Java 측정과 동시에 실행되지 않도록 `5240-5259`와 `5260-5279`를 함께 확인한다. Framework source는
고정 HTTP 포트를 먼저 예약한 뒤 RouteMesh의 임시 loopback listener를 시작한다. 모든 target의
request와 send handler는 active header를 읽어 같은 B counter에 기록한다.

## Trigger와 logical stream

공통 trigger 객체는 `runId`, `cellId`, `pattern`, `payloadBytes`, `phase`, `durationMs`,
`requestWindow`, `sendConcurrency`만 허용한다. source 원본의 `trigger`는 여기서 `phase`,
`requestWindow`, `sendConcurrency`를 제외하고 `warmup`, 실제 `/bench/start` endpoint,
`receivedAtUnixMs`를 더한 규격의 8필드를 기록한다.

| 패턴 | `streams.count` | `streams.inFlightPerStream` | 구현 |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | Java platform submit thread 하나의 순차 `CompletableFuture` |
| `request-window` | 1 | 100 | platform submit thread 하나가 `CompletableFuture` 100개로 한 logical window를 공유 |
| `request-backpressure` | 1 | `null` | platform submit thread 하나가 상한 없이 future를 제출하고 completion에 양보 |
| `send-saturation` | 8 | 1 | platform submit thread 8개가 각각 완료 통지를 기다린 뒤 다음 send 제출 |
| Kotlin `request-window` | 1 | 100 | platform submit thread 하나가 coroutine 100개로 한 logical window를 공유 |

## 변경 파일과 diff 요지

| 파일 | 변경 내용 |
|---|---|
| `java/shared/.../BenchHttpApplication.java` | trigger exact-field 검증, 중복 acknowledgement, `idle/warmup/active/failed`, source stats를 한 곳에 추가 |
| `java/shared/.../BenchServerMetrics.java` | target stats에 `ready`, `phase`, `received`, 제출·완료·in-flight 호환 필드 추가 |
| `java/client/.../BenchClient.java` | matrix client를 셀 하나의 Java source A로 변경하고 세 transport adapter 선택 |
| `java/client/.../BenchDrivers.java` | 네 패턴을 trigger phase별 logical stream 실행으로 재구성하고 source counter·latency·자원 계측 소유 |
| `java/client/.../BenchOptions.java` | source A의 exact cell·HTTP·target CLI와 고정 측정값 검증 |
| `java/client/.../BenchResultWriter.java` | source 원본, trigger 8필드, streams, RESULT 9줄 작성 |
| `java/client/.../StatsClient.java` | 새 target `received` 필드를 읽되 기존 `activeMessages`도 수용 |
| `java/grpc-server/.../GrpcBenchServer.java` | 5242/5243 기본값과 request 수신 계측 |
| `java/zlink-raw-server/.../ZLinkRawBenchServer.java` | 5247/5248/5249 기본값, request 수신 계측, idle `NO_DATA`를 메시지 오류에서 분리 |
| `java/zlink-framework-server/.../BenchEchoHandler.java` | typed request 수신 계측 |
| `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java` | 5254/5255 기본값과 request handler metric 주입 |
| `java/kotlin-client/.../BenchKotlinClient.kt` | 두 Kotlin 보조 셀의 source A와 공통 trigger/driver/result 배선 |
| `java/runner_common.sh` | port·stats·trigger·settle·merge·count 검증·process 종료 공통 함수 |
| `java/run_local.sh` | Java 3×4 matrix의 셀별 B/A lifecycle과 5240-5259 preflight |
| `java/run_local_kotlin.sh` | Kotlin 보조 두 셀과 Java B 재사용, 두 포트 대역 preflight |

수정 전/후 규칙 수: **8 → 4**. 수정 전에는 runner별 process 묶음, client 내부 matrix,
패턴별 warmup/active/reset, Java·Kotlin별 결과 작성, server별 서로 다른 request/send 계측이
분산돼 있었다. 수정 후에는 runner가 셀 lifecycle·target 병합, `BenchHttpApplication`이 trigger
phase, `BenchDrivers`가 pattern-to-stream·source 계측, target B가 수신 snapshot을 각각 한 번만
소유한다.

## 언어별 문서 입력값

감독자가 계획 §5의 Java 문서 절 1~6과 Kotlin 보조 절로 옮길 수 있도록 구현값을 정리한다.

### 1. 비교 대상

| 구현 | request | send/command |
|---|---|---|
| `grpc-java` | grpc-java future stub의 unary `Echo` | unary `Command`와 `Empty` reply |
| `zlink-java` | raw ROUTER `request(peer)` | command 전용 raw ROUTER `send(peer)` |
| `zlink-framework-java` | `ZLinkRouteClient.requestToChannel(...).submit(BenchPayload.class)` | `sendToChannel(...).submit()` |
| `grpc-kotlin` | grpc-kotlin `BenchServiceCoroutineStub.echo` suspend 호출 | 보조 셀에 없음 |
| `zlink-framework-kotlin` | `requestToChannel(...).awaitReply` | 보조 셀에 없음 |

모든 행은 같은 `BenchPayload` protobuf DTO와 body 앞의 29-byte header를 사용한다. raw는 envelope와
protobuf body 두 part를 전송하며 request와 command endpoint를 분리한다.

### 2. 실행 방법

```bash
cd framework/bench/grpc/java
./run_local.sh
./run_local_kotlin.sh

# Java 한 셀
RUNS=1 PAYLOADS=1024 SCENARIO=request-window \
  IMPLEMENTATION=zlink-framework-java ./run_local.sh

# Kotlin 보조 한 셀
RUNS=1 IMPLEMENTATION=grpc-kotlin ./run_local_kotlin.sh
```

| 입력 | Java 기본값 | Kotlin 기본값·동작 |
|---|---|---|
| `RUNS` | `3` | `3` |
| `RUN_DEALER` | `0`; 다른 값 거부 | `0`; 다른 값 거부 |
| `DURATION` | `5`초 active | `5`초 active |
| `WARMUP_SECONDS` | `20`초 | `20`초 |
| `PAYLOADS` | `1024,4096` | `1024`만 허용 |
| `SCENARIO` | `all`, `request`, 네 exact pattern, `send`·`command` alias | `all`·`request`·`request-window`가 보조 window 셀 선택 |
| `IMPLEMENTATION` | `all` 또는 Java 세 구현 | `all` 또는 Kotlin 두 구현 |
| `STAMP` | 실행 시각 | 실행 시각 |
| `OUTROOT` | `log/java/with_grpc_java_<stamp>` | `log/java/with_grpc_kotlin_<stamp>` |
| `SKIP_BUILD` | `0`; `1`이면 runner build 생략 | 동일 |

`request_window=100`, `send_concurrency=8`, request·route-ready timeout `30초`, drain 상한
`30초`, quiet settle `200ms`는 규격값으로 고정했다. runner build는 load average 10 미만에서
`./gradlew --no-daemon --max-workers=1 assemble installDist` 한 개만 실행한다.

### 3. 프로세스 구성

위 “프로세스 구성” 표와 B → A → warmup → active → settle → merge → count 검증 → 종료 순서를
사용한다. trigger client는 runner의 `curl`이며 부하를 만들지 않는다. 모든 셀은 새 A/B process
pair를 사용한다.

### 4. 언어별 값

| 항목 | 값 |
|---|---|
| Java warmup | 기본 20초, smoke 1초. active와 같은 driver·logical stream 사용 |
| active | 기본 5초, smoke 2초 |
| gRPC source | Java future stub 또는 Kotlin coroutine stub, plaintext channel 기본 구성 |
| gRPC target | `ServerBuilder.forPort`, grpc-netty-shaded 기본 구성, plaintext loopback |
| JDK | Temurin/OpenJDK `22.0.2` (`/usr/lib/jvm/temurin-22-jdk-amd64`) |
| grpc-java / grpc-kotlin | `1.72.0` / `1.4.1` |
| protobuf-java | `4.30.2` |
| Kotlin / coroutines | Kotlin Gradle plugin `2.2.21`, coroutines `1.9.0` |
| ZLink binding | published local Maven package `0.17.6` |
| Framework | source composite build `0.10.0`, HEAD `d84aa56f98` |
| 실행 OS/CPU | Ubuntu 24.04.4 LTS, Intel Core Ultra 7 265K |
| source saturation | `jvm_thread_cores`; 상한은 실제 platform submit thread 수(serial/window/backpressure 1, send 8) |

### 5. 결과 위치

```text
framework/bench/grpc/log/java/with_grpc_java_<stamp>/
├── with_grpc_java_<stamp>.txt
└── <implementation>-<pattern>-<payload>-run<n>/
    ├── results.json
    ├── report.txt
    ├── source.log
    ├── target.log
    └── target-stats.json
```

Kotlin은 run root 이름만 `with_grpc_kotlin_<stamp>`이고 셀 구조는 같다. `results.json`은
`schema=with-grpc-cell-v1`과 source cell 하나를 가지며 runner가 `target_stats`를 같은 cell에
병합한다.

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py \
  --lang java --format spec4 \
  --runs-glob 'framework/bench/grpc/log/java/<run-1>/*' \
  --runs-glob 'framework/bench/grpc/log/java/<run-2>/*' \
  --runs-glob 'framework/bench/grpc/log/java/<run-3>/*'
```

### 6. 알려진 제약과 Kotlin 보조 절

- Kotlin은 전체 matrix가 아니다. `request-window @1024`의 gRPC와 Framework A만 Kotlin이며 B는
  Java 바이너리와 Java 포트를 재사용한다. Java 측정과 동시에 실행할 수 없다.
- `RAW_SOCKET`은 비교 계약에 따라 `router`만 허용한다.
- `request-backpressure`는 application in-flight 상한 없이 제출하므로 도달 깊이가 결과다.
- 이 작업은 smoke만 수행했다. 공개 비교값과 G5 판정에는 runtime BLOCKER 해결 뒤 별도 3-run이
  필요하다.
- Java raw ROUTER 재현은 0.17.6에서도 reply completion을 잃는다. 현재 bench raw server가
  `Received`를 반복 close하지 않는 기존 수명 패턴에서는 smoke가 통과하지만, 이를 runtime 결함의
  해결로 해석하거나 raw window 결과를 게재하면 안 된다.

## Build와 정적 검증

| 검증 | 결과 |
|---|---|
| `JAVA_HOME=/usr/lib/jvm/temurin-22-jdk-amd64 ./gradlew --no-daemon --max-workers=1 assemble installDist` | 최종 rc=0, `BUILD SUCCESSFUL` |
| `bash -n run_local.sh run_local_kotlin.sh runner_common.sh` | rc=0 |
| `git diff --check` | rc=0 |
| 집계기 `--lang java --format spec4 --payload-sizes 1024` + Java smoke cell dirs | rc=0, request-serial·request-window·send-saturation 표 출력 |

최종 전체 Gradle 검증 시작 시 load average는 `0.26`이었다. 최초 전체 build와 변경 후 focused build도
모두 load 10 미만, `--no-daemon --max-workers=1`로 직렬 실행했다.

## Smoke 검증

모든 bench와 repro 실행은 `perf-ticket.sh submit -p 2 -o sol-fwb2-06`으로 제출하고 완료를
기다렸다. 아래 throughput은 기능 확인용 1-run 값이며 비교 결과로 게재하지 않는다.

| 셀 | ticket | rc | RESULT throughput | source 완료 / target 수신 | 오류(source/target), abandoned | stream |
|---|---|---:|---:|---:|---:|---|
| `grpc-java request-serial@1024` | `2-1788947597-9603-sol-fwb2-06-final_java_serial_and_raw_send_smoke` | 0 | `2992.000` | `5984 / 5984` | `0/0`, `0` | `1×1` |
| `zlink-java request-serial@1024` | final Java serial/send ticket | 0 | `5597.500` | `11195 / 11195` | `0/0`, `0` | `1×1` |
| `zlink-framework-java request-serial@1024` | final Java serial/send ticket | 0 | `309.000` | `618 / 618` | `0/0`, `0` | `1×1` |
| `zlink-java send-saturation@1024` | final Java serial/send ticket | 0 | `531901.000` | `1063802 / 1063802` | `0/0`, `0` | `8×1` |
| `zlink-framework-java request-window@1024` | `2-1788947541-3772-sol-fwb2-06-java_kotlin_window_thread_cpu_smoke` | 0 | `1860.500` | `3721 / 3721` | `0/0`, `0` | `1×100`, peak `12` |
| `grpc-kotlin request-window@1024` | Java/Kotlin window thread CPU ticket | 0 | `64432.500` | `128865 / 128865` | `0/0`, `0` | `1×100`, peak `100` |
| `zlink-framework-kotlin request-window@1024` | Java/Kotlin window thread CPU ticket | 0 | `1736.500` | `3473 / 3473` | `0/0`, `0` | `1×100`, peak `12` |

각 최종 셀은 RESULT 9줄과 trigger 8필드, `role=source`, `streams`, settle 뒤
`target_stats(received,errors,drainMs)`를 가졌다. 첫 Framework window 시도
`2-1788946697-67685-...`은 source RouteMesh가 먼저 시작되어 고정 HTTP 포트와 충돌해 rc=1이었다.
HTTP listener를 먼저 예약하고 `ready=false` 동안 transport를 연결하도록 시작 순서를 고친 뒤 위
최종 ticket이 rc=0이었다.

## Runtime 결함

### Java raw ROUTER의 caller-owned `Received.close()` 뒤 reply completion 유실

- 재현 ticket:
  `2-1788946909-81562-sol-fwb2-06-java_0.17.6_outstanding_request_repro_sm`, process rc=0.
- 기능 결과: `route established; echoed=1` 뒤 `outstanding=100 done=0/100 succeeded=0
  serverEchoed=101`. 서버는 warmup 포함 101건을 수신하고 reply submit까지 수행했지만 모든 client
  request가 timeout terminal로 끝났다.
- 재현 경계: `framework/bench/grpc/java/repro/.../OutstandingRequestRepro.java:123-178`의
  ROUTER↔ROUTER batch와 per-iteration `Received.close()`.
- binding 경계: `bindings/java/src/main/java/systems/zlink/contracts/messaging/Received.java:621-642`의
  captured reply submit과 `:786-840`의 close 경로,
  `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterSocket.java:49-75`의
  caller-owned receive reply sender 배선이다. 이 작업은 runtime 수정이 금지되어 더 안쪽의 단일
  원인 line을 변경으로 확인하지 않았다.
- 소유 계층·spec: Core ROUTER reply lane과 Java binding의 `Received` 수명 경계.
  `core/doc/spec/core/socket/07-router.ko.md:291-303,321-324,476-480`은 successful FINAL만 token을
  소비하고 reply를 ROUTER completion progress lane으로 보내도록 정한다.
- 교차언어 대조: .NET은
  `bindings/dotnet/src/Zlink/Runtime/Messaging/Received.Operations.cs:9-33`에서 reply context를
  별도 객체로 capture하고, `.NET` raw server는 caller-owned `Received`를 반복 재사용하면서 window
  smoke가 통과했다. Java의 구조상 필요한 차이가 아니라 Java binding/Core 경계의 회귀 증상이다.
- 변경 분류: **B — 기존 결함**. bench나 timeout으로 보상하지 않았고 runtime source도 수정하지
  않았다.

## BLOCKERS

- published Java binding `0.17.6`에서 위 1차 reply 유실 재현이 다시 나타났다. 계획 §9에 따라
  Java raw `request-window` 3-run과 그 행을 사용하는 공개 비교·판정은 결함 수정 전 진행할 수 없다.
- 이 작업의 요청 범위에서는 runtime 수정 권한이 없으므로 결함 자체는 남아 있다. 그 밖의 build,
  지정 smoke, Kotlin 보조 셀에는 blocker가 없다.

## 감독자 재검증 명령

build 전 load average가 10 미만인지 확인한다.

```bash
cd /home/hep7/project/zlink/framework/bench/grpc/java
cut -d' ' -f1 /proc/loadavg
JAVA_HOME=/usr/lib/jvm/temurin-22-jdk-amd64 \
  ./gradlew --no-daemon --max-workers=1 assemble installDist
bash -n run_local.sh run_local_kotlin.sh runner_common.sh
git -C /home/hep7/project/zlink diff --check
```

smoke와 repro는 다음처럼 반드시 ticket으로 실행한다.

```bash
cd /home/hep7/project/zlink
bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-06 \
  -d "review Java request-serial" -- \
  env RUNS=1 DURATION=2 WARMUP_SECONDS=1 PAYLOADS=1024 \
  SCENARIO=request-serial IMPLEMENTATION=all SKIP_BUILD=1 \
  OUTROOT=/tmp/zlink-sol-fwb2-06/review-java-serial \
  bash framework/bench/grpc/java/run_local.sh

bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-06 \
  -d "review Java raw send" -- \
  env RUNS=1 DURATION=2 WARMUP_SECONDS=1 PAYLOADS=1024 \
  SCENARIO=send-saturation IMPLEMENTATION=zlink-java SKIP_BUILD=1 \
  OUTROOT=/tmp/zlink-sol-fwb2-06/review-java-send \
  bash framework/bench/grpc/java/run_local.sh

bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-06 \
  -d "review Java framework window" -- \
  env RUNS=1 DURATION=2 WARMUP_SECONDS=1 PAYLOADS=1024 \
  SCENARIO=request-window IMPLEMENTATION=zlink-framework-java SKIP_BUILD=1 \
  OUTROOT=/tmp/zlink-sol-fwb2-06/review-java-framework-window \
  bash framework/bench/grpc/java/run_local.sh

bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-06 \
  -d "review Kotlin auxiliary cells" -- \
  env RUNS=1 DURATION=2 WARMUP_SECONDS=1 PAYLOADS=1024 \
  SCENARIO=request-window IMPLEMENTATION=all SKIP_BUILD=1 \
  OUTROOT=/tmp/zlink-sol-fwb2-06/review-kotlin \
  bash framework/bench/grpc/java/run_local_kotlin.sh

bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-06 \
  -d "review Java 0.17.6 reply repro" -- \
  framework/bench/grpc/java/repro/build/install/bench-repro/bin/bench-repro 100 10
```
