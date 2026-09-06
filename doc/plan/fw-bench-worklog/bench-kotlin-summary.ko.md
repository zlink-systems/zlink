# Kotlin with-grpc bench 요약 (Phase 4)

이 문서는 `kotlin` 언어의 with-grpc bench 측정 결과를 하나로 모은 기록이다. Phase 6 보고서가
이 문서만 읽고도 `kotlin` 행을 서술할 수 있도록 조건, 수치, 판정, 판정할 수 없는 항목을 함께
남긴다.

계획: [`../framework-bench-with-grpc-5lang-plan.ko.md`](../framework-bench-with-grpc-5lang-plan.ko.md)
결정: [`decisions.ko.md`](./decisions.ko.md)
규격: [`../../../framework/doc/framework/common/bench/with-grpc-local.ko.md`](../../../framework/doc/framework/common/bench/with-grpc-local.ko.md)
인접 요약: [`bench-java-summary.ko.md`](./bench-java-summary.ko.md)

## 1. 결론 먼저

### 1.1 `zlink-kotlin`의 request-window 셀 여덟 개가 모두 정지했다. Java와 같은 모양이다

`request_window` 기본값 100에서 `zlink-kotlin`의 request-window 셀은 ROUTER 3회 × payload 2종
여섯 개와 DEALER 1회 × payload 2종 두 개, 곧 **여덟 개 전부가 정지했다.** 여덟 셀 모두
`peak_in_flight`가 100에 도달하고 `abandoned`가 100이다. window는 채워졌고, 채워진 100개가
하나도 완료되지 않은 채 남았다.

**이 셀의 처리량 값을 Kotlin의 request-window 성능으로 인용하면 안 된다.** 표에 남는 0.00 KOPS는
이 경로가 지탱하는 속도가 아니라, 완료를 멈춘 socket을 active 구간 길이로 나눈 산술 결과다.
그래서 이 문서는 정지한 셀에 처리량 값을 싣지 않고 §7에 정지 자체의 관측으로 싣는다.

**이 결과를 네 번째 언어의 독립 확인으로 읽으면 안 된다.** `bindings/kotlin`에는 자체 native
binding이 없고, 그 디렉터리의 README가 스스로 "Java binding을 Kotlin에서 사용하는 예제"라고
적는다. `zlink-kotlin`이 사용하는 것은 `zlink-java`와 **같은 `systems.zlink:zlink` 아티팩트**다.
따라서 이 관측이 더하는 것은 새 binding의 사례가 아니라 아래 두 가지다.

- **호출 모양을 바꿔도 정지한다.** Java 행은 `CompletionStage`를 블로킹 `get()`으로 기다렸고
  Kotlin 행은 coroutine 안에서 `await()`로 기다린다. 제출과 완료 대기의 모양이 다른데 같은
  지점에서 정지한다.
- **DEALER에서도 정지한다.** ROUTER 여섯 셀과 DEALER 두 셀이 모두 같은 서명을 낸다. 소켓
  종류에 딸린 성질이 아니라는 Java의 관측과 일치한다.

### 1.2 그래서 formula 1의 분자가 존재하지 않고, Kotlin도 0.80 판정을 게재하지 못한다

판정 패턴은 `request-window`다(규격 §7.2, FB-003). 그 셀이 처리량을 내지 않으므로
`zlink-kotlin / zlink-c`의 분자가 없고, `zlink-framework-kotlin / zlink-kotlin`의 분모도 없다.
네 판정 모두 `unsupported`다. **이것은 자료의 구멍이 아니라 §1.1이 낳은 결과다.**

@4096에는 그것과 별개인 이유가 하나 더 있다. **`zlink-c` request-window @4096은 공유 분모이고,
FB-034에 따라 세 측정 구간 연속으로 G5를 통과하지 못했다**(Phase 0 `gated` 75.7%, `gated2`
25.7%, Phase 3 Java 구간 28.7%). 이번 구간에서 재사용한 값도 같은 28.7%다. 곧 **분자가
성립했더라도 @4096의 formula 1은 어느 언어도 게재할 수 없다.** 이것은 Kotlin의 실패가 아니라
캠페인 수준의 사실이며, 규격 §7.2가 두 payload 크기를 모두 요구하므로(FB-005) 현재 기준선
위에서는 formula 1 통과 자체가 달성 불가능하다.

### 1.3 Kotlin framework도 request-window 깊이 약 4.5에서 멈춘다. FB-033을 좁히는 값이다

`zlink-framework-kotlin`의 request-window는 설정 window 100에 대해 `peak_in_flight` **10~12**,
실제 깊이 **4.48~4.57**, abandoned 0으로 동작한다. 여덟 셀 전부가 이 범위 안에 들어온다. Java가
같은 셀에서 관측한 `peak_in_flight` 10~11, 깊이 약 4.5와 사실상 같은 값이다.

**이 일치가 무엇을 좁히는지는 정확히 적어야 한다.** Kotlin 행은 §2.6대로 Java 행의 framework
server 프로세스를 그대로 사용한다. 곧 두 관측은 **server 쪽을 공유한다.** 그러므로 이 일치가
말하는 것은 아래와 같다.

- **깊이 상한은 client 언어가 정하는 값이 아니다.** client가 Java의 `CompletionStage` 경로든
  Kotlin의 suspend 경로든 같은 4.5에서 멈춘다. 두 client API는 서로 다른 코드다.
- **따라서 원인 후보는 공유된 쪽, 곧 framework의 채널 request 경로와 server 쪽 처리로
  좁혀진다.** 이것은 FB-033이 "동시 미완료 요청 수에 따라 달라지는 완료 전달 경로"를 출발점으로
  지목한 것과 같은 방향이며, 그 출발점에서 client API 계층을 제거한다.
- **client 언어가 다른 두 관측이 일치한다는 사실만으로 상한이 server에 있다고 단정하지는
  않는다.** 두 client가 같은 framework core를 거치므로 core의 client 측 경로도 여전히 후보다.
  이 구분은 이 캠페인이 하지 않는다.

### 1.4 게재할 수 있는 값

`grpc-kotlin` 12셀, `zlink-kotlin`의 request-serial 4셀과 send-saturation 4셀,
`zlink-framework-kotlin` 12셀은 정지 없이 측정됐다. G5는 그중 15셀이 통과하고
`zlink-framework-kotlin` send-saturation @1024 한 셀이 12.8%로 미달한다(§9). 네 run 모두 18셀을
실행했고 실패 0, 오염 0이다.

## 2. 측정 대상과 조건

### 2.1 비교 대상

| 구현 이름 | 내용 |
|---|---|
| `grpc-kotlin` | grpc-kotlin coroutine stub의 unary RPC. proto는 `Echo`와 `Command` 둘뿐이다(FB-002) |
| `zlink-kotlin` | framework를 거치지 않는 raw binding. ROUTER↔ROUTER |
| `zlink-framework-kotlin` | `zlink-framework-kotlin`의 suspend 호출로 사용하는 RouteMesh channel request와 send |

ZLink raw 행은 규격 §1.3대로 ROUTER↔ROUTER를 사용한다. client도 ROUTER를 만들고 자기 routing
id를 설정한 뒤 상대 ROUTER의 routing id를 지정해 전송한다. wire 모양은 envelope 헤더 part
하나와 protobuf로 인코딩한 `BenchPayload` part 하나로, `zlink-c`·`zlink-java`와 같다(FB-024).

framework 행의 host는 `zlink-framework-spring-boot-starter`이고, client 호출은
`zlink-framework-kotlin`의 `awaitReply`와 `await`다. 내부 package는 사용하지 않았다(G4).

### 2.2 gRPC stub 선택 — coroutine stub을 사용했다

규격 §8.1은 Kotlin이 grpc-kotlin coroutine stub을 사용하고, **사용할 수 없을 때에만** blocking
stub을 쓰며 그 사유를 결과에 남기라고 정한다. **coroutine stub을 사용할 수 있었고 사용했으므로
기록할 사유가 없다.**

- `protoc-gen-grpc-kotlin` 1.4.1이 `BenchServiceCoroutineStub`을 생성했고, 그 stub의 `echo`와
  `command`는 `suspend fun`으로 `io.grpc.kotlin.ClientCalls.unaryRpc`를 호출한다.
- 생성물은 `kotlin-client/build/generated/source/proto/main/grpckt/`에 남는다.
- `BenchPayload`와 `BenchServiceGrpc`는 Java 행의 `:shared`가 생성한 것을 그대로 쓰고, Kotlin
  모듈은 coroutine stub만 생성한다. 두 행이 같은 생성 message class를 쓰게 하기 위한 구성이다.

이 선택이 필요한 이유는 규격이 적은 그대로다. ZLink 쪽이 suspend 인터페이스를 쓰는데 gRPC 쪽만
blocking stub을 쓰면 비교가 Kotlin에 유리하게 기운다.

### 2.3 coroutine 시작 방식과 그것이 계측에 미치는 영향

세 구현 모두 측정 호출을 coroutine 안에서 수행하고, coroutine은
`CoroutineStart.UNDISPATCHED`로 시작한다. 곧 **첫 실제 중단 지점까지는 호출한 thread에서 그대로
실행된다.** 이 선택은 수치를 좋게 만들기 위한 것이 아니라 Java 행과 같은 것을 재기 위한 것이며,
이유는 두 가지다.

- **계측 대상이 어긋나는 것을 막는다.** 선언 계측기 `jvm_thread_cores`는 harness가 제출 loop를
  실행하는 thread의 CPU다(FB-032). Java 행은 stub을 그 thread에서 그대로 호출하므로 제출 비용이
  거기에 잡힌다. dispatch되는 coroutine은 제출 자체를 `Dispatchers.Default` worker에 넘기므로
  계측기가 모든 셀에서 0에 가깝게 읽히고, 상한이 서술하는 대상을 더는 재지 않게 된다. FB-023과
  FB-032가 두 번 정정한 것과 같은 형태의 오류다.
- **`request-serial`의 왕복 지연에 dispatcher를 섞지 않는다.** 요청을 보내기 전에 dispatch hop이
  하나 들어가면 어느 transport에도 속하지 않는 대기가 지연에 더해진다.

중단 이후의 continuation은 `Dispatchers.Default`에서 재개되고 그 CPU는 **관찰값
`jvm_all_thread_cores`에만 들어간다.** Java 행에서 gRPC callback이 `directExecutor`로 network
thread에서 실행되어 선언 계측기 밖에 있는 것과 같은 처리다. 곧 두 행 모두 선언 계측기는 제출
경로만 재고, 그 사실을 여기에 남긴다.

### 2.4 고정 조건

| 항목 | 값 |
|---|---|
| payload 크기 | `1024`, `4096` bytes |
| `request_window` | 100 |
| send concurrency | 8 |
| warmup | **20초**(2초 segment 10개), 측정과 같은 driver로 실행 (§8) |
| active duration | 5초 |
| request timeout | 30초 |
| 반복 | ROUTER 3회, DEALER 1회 |
| transport | loopback `127.0.0.1`, 포트 대역 5101-5107 |
| 대표값 | 중앙값(ROUTER 3회) |

정지를 피하려고 `request_window`를 낮추지 않았다. 표준 workload에서 정지한다는 사실 자체가
측정 결과다.

### 2.5 실행 환경과 이력

| 항목 | 값 |
|---|---|
| CPU | Intel Core Ultra 7 265K, 논리 core 20개 |
| OS | kernel 6.6.87.2-microsoft-standard-WSL2 |
| JDK | Temurin 22.0.2, OpenJDK 64-Bit Server VM |
| Kotlin | 2.2.21, kotlinx-coroutines 1.9.0 |
| gRPC | grpc-java 1.72.0 + **grpc-kotlin 1.4.1 coroutine stub** |
| protobuf | protobuf-java 4.30.2, protobuf gradle plugin 0.9.4 |
| gRPC server 구성 | `io.grpc.ServerBuilder.forPort` 기본 구성, plaintext loopback, grpc-netty-shaded 1.72.0 |
| ZLink binding | `systems.zlink:zlink` 0.17.0. `bindings/kotlin`에 자체 native binding은 없다 |
| framework | `zlink-framework-kotlin` 0.10.0, host `zlink-framework-spring-boot-starter` 0.10.0, codec `zlink-framework-codec-protobuf` 0.10.0(Java와 같은 codec) |
| 측정 구간 | 2026-09-07T04:13:38+09:00 ~ 04:53:14+09:00 |

**commit 이력을 그대로 남긴다.** 측정 시작 시점의 HEAD는 `129627f8a5`였고, 구간 중에 HEAD가
`01518191ce`를 거쳐 `b8bd2471dc`로 두 번 움직였다. 그래서 run별 셀 원본에 기록된 commit이
서로 다르다(`kotlin-router-1`은 `01518191ce`, 나머지 세 run은 `b8bd2471dc`).

이 움직임은 측정을 오염시키지 않는다. 두 commit은 **`doc/plan/` 아래 문서 두 건뿐**이고
(`framework-bench-with-grpc-5lang-plan.ko.md`, `briefs/fwb-07.prompt`), Core·binding·framework·
bench 코드는 한 줄도 바뀌지 않았다. 측정된 바이너리는 구간 시작 전에 빌드된 것이므로 HEAD
이동과 무관하다. Kotlin bench 코드 자체는 측정 시점에 작업 트리에만 있었고 아직 commit되지
않았다. Java 행이 `dcded04dbe`에서 측정된 뒤 `acb3b252c3`으로 commit된 것과 같은 상황이다.

### 2.6 Kotlin 행은 client만 소유한다 — 이 사실을 판정과 함께 읽어야 한다

Kotlin 행은 **client 프로세스만 Kotlin으로 구현하고, server 프로세스 세 개는 Java 행의
바이너리를 kotlin 포트 대역에서 실행한다.** 규격 §9가 요구하는 대역 분리는 지켰고
(5101·5102·5103·5104·5105·5106·5107), runner는 시작 전에 대역이 비어 있는지 확인하고 사용
중이면 포트를 옮기지 않고 중단한다.

이 구성을 고른 이유는 이 Phase가 답하려는 질문이 client API 비용이기 때문이다. gRPC는 wire에서
언어 중립이고, raw server는 ROUTER echo이며, framework server는 같은 `zlink-framework-core`
host다. server를 다시 작성하면 client API와 server 배선이 함께 달라져 두 행의 차이를 어느
쪽에도 귀속시킬 수 없게 된다.

그 대신 아래 두 가지를 결과와 함께 읽어야 한다.

- **formula 2(`zlink-framework-kotlin / zlink-kotlin`)는 Java 행과 같은 server 쪽 비용을 포함한
  값이다.** 두 행 모두 server가 Java이므로 비율의 성격은 Java 행과 같고, 언어를 가로질러 비교할
  수 있는 값도 그 성격 안에서다.
- **§1.3의 깊이 일치는 server를 공유한 상태의 일치다.** 그 해석은 §1.3에 적었다.

### 2.7 측정 격리

측정 구간 전체를 `flock --exclusive /tmp/zlink-perf.lock`으로 잠갔다. Gradle 빌드는 측정 구간
바깥에서 저장소의 JVM 빌드 잠금(`/tmp/zlink-jvm-gate.lock`) 아래 먼저 끝냈고, 측정 스크립트는
`SKIP_BUILD=1`로 실행해 어떤 컴파일도 구간 안에 들어가지 않게 했다(G7). 네 run이 모두 종료 코드
0으로 끝났다.

run마다 시작 직전 1분 load average를 확인했고, 2.0 이상이면 대기했다. 관측값은 아래와 같다.

| 시점 | 최초 판독 | 대기 후 판독 |
|---|---|---|
| span 시작 / `kotlin-router-1` | 0.43 | — |
| `kotlin-router-2` | 2.84 | 1.87 |
| `kotlin-router-3` | 3.49 | 1.94 |
| `kotlin-dealer-1` | 3.78 | 1.94 |
| span 종료 | 4.70 | — |

2.0을 넘은 판독은 모두 **직전 run이 끝난 직후의 잔여 부하**이고, 25~40초 대기 뒤 전부 2.0
아래로 내려갔다. 다른 job은 실행하지 않았다.

### 2.8 `zlink-c` 기준선은 Java 구간의 값을 재사용했다

**이번 구간에서 C 기준 bench를 다시 실행하지 않았다.** `bindings/c/bench/with_grpc/log/
20260907_030655/c-router-{1,2,3}`, 곧 Phase 3 Java 구간에서 측정한 3회 run을 그대로 집계에
넣었다. 이 사실을 묻어 두지 않고 여기에 적는다.

재사용이 타당한 근거는 아래와 같다.

- 그 구간의 측정 commit `dcded04dbe`부터 이번 구간의 HEAD까지, 변경된 파일은 bench 코드와
  공용 집계기와 `doc/` 문서뿐이다. **Core·binding·framework source는 바뀌지 않았다.**
- 조건이 같다. active 5초, `request_window` 100, 같은 머신, 같은 커널.
- 유일한 소비처인 formula 1은 §1.1의 분자 부재로 어차피 게재되지 않고, @4096은 FB-034의 분모
  미달로도 게재되지 않는다. 곧 재실행이 바꿀 게재값이 없다.

## 3. 측정 표 — payload 1024

ROUTER 구성 3회 run의 중앙값이다. 처리량 단위는 request 계열이 `KOPS`,
`send-saturation`이 `KMSG/s`다. CPU는 논리 core 20개 기준 백분율, memory는 RSS(MB)다.

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | client MB | server CPU% | server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | `grpc-kotlin` | 4.74 | 0.211 | 0.261 | 0.324 | 3.0 | 341.1 | 1.6 | 750.4 | — |
| request-serial | `zlink-kotlin` | 5.32 | 0.188 | 0.230 | 0.292 | 2.6 | 361.1 | 1.2 | 399.6 | — |
| request-serial | `zlink-framework-kotlin` | 0.47 | 2.121 | 2.589 | 2.783 | 1.7 | 404.4 | 2.1 | 388.5 | — |
| request-window | `grpc-kotlin` | 93.75 | 0.982 | 1.239 | 1.818 | 28.2 | 943.5 | 12.3 | 1206.7 | — |
| request-window | `zlink-kotlin` | **처리량 없음 — 정지(§7)** | — | — | — | 0.6 | 946.9 | 0.0 | 404.6 | — |
| request-window | `zlink-framework-kotlin` | 2.36 | 1.920 | 2.769 | 2.994 | 5.3 | 996.4 | 4.8 | 378.6 | — |
| send-saturation | `grpc-kotlin` | 35.18 | 0.120 | 0.167 | 0.194 | 13.8 | 1033.6 | 6.6 | 1234.8 | 310 |
| send-saturation | `zlink-kotlin` | 372.46 | 0.076 | 0.232 | 0.291 | 10.1 | 1047.7 | 4.2 | 1036.0 | 311 |
| send-saturation | `zlink-framework-kotlin` | 5.67 | 2482.409 | 2655.419 | 2675.103 | 13.1 | 1132.7 | 13.8 | 847.2 | 2464 |

## 4. 측정 표 — payload 4096

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | client MB | server CPU% | server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | `grpc-kotlin` | 4.89 | 0.204 | 0.241 | 0.308 | 3.0 | 1133.5 | 1.5 | 1235.0 | — |
| request-serial | `zlink-kotlin` | 5.40 | 0.185 | 0.223 | 0.297 | 2.7 | 1136.2 | 1.2 | 1039.7 | — |
| request-serial | `zlink-framework-kotlin` | 0.49 | 2.032 | 2.505 | 2.578 | 1.7 | 1134.2 | 1.4 | 884.8 | — |
| request-window | `grpc-kotlin` | 79.46 | 1.172 | 1.555 | 1.851 | 24.4 | 1550.8 | 11.5 | 1257.3 | — |
| request-window | `zlink-kotlin` | **처리량 없음 — 정지(§7)** | — | — | — | 0.7 | 1555.4 | 0.0 | 1040.0 | — |
| request-window | `zlink-framework-kotlin` | 2.30 | 1.952 | 2.800 | 3.020 | 5.5 | 1557.7 | 4.9 | 897.7 | — |
| send-saturation | `grpc-kotlin` | 33.75 | 0.126 | 0.177 | 0.206 | 13.7 | 1573.5 | 6.5 | 1256.9 | 333 |
| send-saturation | `zlink-kotlin` | 256.76 | 0.084 | 0.233 | 0.285 | 10.4 | 1576.0 | 4.4 | 1041.2 | 375 |
| send-saturation | `zlink-framework-kotlin` | 8.67 | 819.060 | 897.205 | 918.998 | 13.3 | 1614.5 | 13.4 | 907.2 | 1051 |

`send-saturation`의 지연은 규격 §5대로 server가 header로 계산한 수신 지연이다. framework send의
지연이 초 단위인 것은 sender가 receiver를 크게 앞지를 때 나타나는 모양이며, FB-009가 `.NET`에서,
Java 행이 Java에서 기록한 것과 같은 성격이다. 이 값 하나로 결함을 단정하지 않는다.

`send-saturation` 셀의 서술 규칙(규격 §2.1, FB-002)에 따라 이 셀을 전송 속도 차이로 쓰지
않는다. 응답이 필요 없는 명령을 처리할 때 gRPC는 unary 왕복을 치러야 하고 ZLink는 단방향
send로 끝난다. 이 조건에서 `zlink-kotlin`과 `grpc-kotlin`의 차이는 @1024에서 10.6배,
@4096에서 7.6배로 관찰됐다.

## 5. 포화와 깊이

선언 계측기는 `jvm_thread_cores`, 상한은 request driver 1과 send driver 8이다. Java 행이 FB-032로
확정한 계측기를 그대로 쓴다. 프로세스 core 수(`client_cores`)와 전체 JVM thread core 수는
관찰값으로 함께 기록하지만 포화를 판정하지 않는다.

| 패턴 | payload | 구현 | 계측기 판독 | 상한 | 포화 | peak_in_flight | 실제 깊이 | abandoned | client cores(관찰) |
|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-kotlin` | 0.072 | 1 | 아니오 | 1 | 1.0 | 0 | 0.60 |
| request-serial | 1024 | `zlink-kotlin` | 0.082 | 1 | 아니오 | 1 | 1.0 | 0 | 0.52 |
| request-serial | 1024 | `zlink-framework-kotlin` | 0.039 | 1 | 아니오 | 1 | 1.0 | 0 | 0.35 |
| request-serial | 4096 | `grpc-kotlin` | 0.072 | 1 | 아니오 | 1 | 1.0 | 0 | 0.61 |
| request-serial | 4096 | `zlink-kotlin` | 0.088 | 1 | 아니오 | 1 | 1.0 | 0 | 0.54 |
| request-serial | 4096 | `zlink-framework-kotlin` | 0.040 | 1 | 아니오 | 1 | 1.0 | 0 | 0.34 |
| request-window | 1024 | `grpc-kotlin` | 0.540 | 1 | 아니오 | 100 | 92.0 | 0 | 5.63 |
| request-window | 1024 | `zlink-kotlin` | 0.006 | 1 | 아니오 | 100 | n/a(정지) | **100** | 0.13 |
| request-window | 1024 | `zlink-framework-kotlin` | 0.180 | 1 | 아니오 | **10** | 4.5 | 0 | 1.07 |
| request-window | 4096 | `grpc-kotlin` | 0.487 | 1 | 아니오 | 100 | 93.1 | 0 | 4.87 |
| request-window | 4096 | `zlink-kotlin` | 0.006 | 1 | 아니오 | 100 | n/a(정지) | **100** | 0.13 |
| request-window | 4096 | `zlink-framework-kotlin` | 0.186 | 1 | 아니오 | **12**(최저 10) | 4.5 | 0 | 1.11 |
| send-saturation | 1024 | `grpc-kotlin` | 0.578 | 8 | 아니오 | 8 | 4.2 | 0 | 2.75 |
| send-saturation | 1024 | `zlink-kotlin` | 1.261 | 8 | 아니오 | 8 | 28.4 | 0 | 2.03 |
| send-saturation | 1024 | `zlink-framework-kotlin` | 0.559 | 8 | 아니오 | 8 | 14071.3 | 0 | 2.62 |
| send-saturation | 4096 | `grpc-kotlin` | 0.575 | 8 | 아니오 | 8 | 4.3 | 0 | 2.74 |
| send-saturation | 4096 | `zlink-kotlin` | 1.273 | 8 | 아니오 | 8 | 21.6 | 0 | 2.07 |
| send-saturation | 4096 | `zlink-framework-kotlin` | 0.600 | 8 | 아니오 | 8 | 7101.9 | 0 | 2.66 |

**포화 셀은 없다.** 제출 thread는 어느 셀에서도 선언 상한에 닿지 않았다. Kotlin에서도 client
런타임이 상한이 아니었다는 뜻이며, Node가 request 계열 네 셀 전부에서 상한에 닿았던 것과
대비된다.

두 값을 따로 적어 둔다.

- **`zlink-framework-kotlin`의 request-window 실제 깊이는 설정값 100에 대해 4.48~4.57이고
  `peak_in_flight`도 10~12에 그친다.** abandoned는 0이므로 요청이 버려진 것이 아니다. run별
  값은 §7의 표에 함께 싣는다. FB-017이 구분하려 한 두 경우 가운데 "스택이 그 깊이까지만
  낸다"에 해당한다.
- **`zlink-framework-kotlin`의 send 실제 깊이는 7102~14071로 매우 크다.** 이는 동시성이 아니라
  §4의 초 단위 수신 지연이 만든 값이며, 제출이 소비를 크게 앞질렀다는 뜻이다.

## 6. 0.80 판정

| 판정식 | payload | 값 | 상태 | 막은 행과 사유 |
|---|---|---|---|---|
| `zlink-kotlin / zlink-c` | 1024 | — | `unsupported` | 분자 `zlink-kotlin-request-window@1024`가 처리량을 내지 않는다(정지). 세 run 모두 완료 0 |
| `zlink-kotlin / zlink-c` | 4096 | — | `unsupported` | 분자 `zlink-kotlin-request-window@4096`이 정지. **분모 `zlink-c-request-window@4096`도 G5 28.7% 미달(FB-034)** |
| `zlink-framework-kotlin / zlink-kotlin` | 1024 | — | `unsupported` | 분모 `zlink-kotlin-request-window@1024`가 위와 같은 사유로 성립하지 않는다 |
| `zlink-framework-kotlin / zlink-kotlin` | 4096 | — | `unsupported` | 분모 `zlink-kotlin-request-window@4096`이 위와 같은 사유로 성립하지 않는다 |

**kotlin: 미완료 — 네 판정 모두 `unsupported`.** 규격 §7.2는 두 payload 크기 모두를 요구한다
(FB-005).

집계기는 정지한 행에 대해 "3 run(s); G5 needs 3"이라는 사유를 낸다. run 수가 모자란다는 뜻이
아니라, **처리량 표본이 성립하지 않아 G5를 적용할 대상이 없다**는 뜻이다. Java 행에서 같은
행이 낸 것과 같은 표현이다.

@4096 두 줄은 분자와 분모가 **각각 따로** 게재를 막았다는 점을 적어 둔다. 분자는 §1.1의
정지이고, 분모는 FB-034의 공유 분모 미달이다. **분모 쪽은 Kotlin과 무관하다.** `zlink-c`
request-window @4096은 Phase 0 `gated` 75.7%, `gated2` 25.7%, Phase 3 Java 구간 28.7%로 세 구간
연속 미달했고, 이번 구간이 재사용한 값도 같은 28.7%다. 이 행이 formula 1의 공유 분모이므로
**분모가 안정되기 전까지 @4096 formula 1은 어느 언어도 통과할 수 없다.** 이것을 판정 기준을
완화할 근거로 쓰지 않는다. 분모를 안정시키는 것이 답이다.

## 7. FB-030 정지의 실측

표준 workload(`request_window` 100)에서 `zlink-kotlin` request-window 셀의 run별 관측이다.

| run | payload | 완료 | error | abandoned | peak_in_flight | 평균 ms | 판정 |
|---|---|---|---|---|---|---|---|
| router-1 | 1024 | 0 | 100 | 100 | 100 | — | 정지 |
| router-1 | 4096 | 0 | 100 | 100 | 100 | — | 정지 |
| router-2 | 1024 | 0 | 100 | 100 | 100 | — | 정지 |
| router-2 | 4096 | 119 | 100 | 100 | 100 | 0.590 | 정지 |
| router-3 | 1024 | 0 | 100 | 100 | 100 | — | 정지 |
| router-3 | 4096 | 0 | 100 | 100 | 100 | — | 정지 |
| dealer-1 | 1024 | 0 | 100 | 100 | 100 | — | 정지 |
| dealer-1 | 4096 | 100 | 100 | 100 | 100 | 0.845 | 정지 |

정지 셀을 알아보는 표시는 Java 행과 같다.

- **`peak_in_flight` 100에 `abandoned` 100이 함께 나온다.** window는 채워졌고 채워진 그대로
  하나도 완료되지 않았다. harness가 window를 채우지 못하는 FB-010 유형이 아니다.
- **완료 수가 0이거나 100건 안팎에서 멈춘다.** router-2와 dealer-1의 @4096이 낸 119건과 100건은
  정지 전에 통과한 소수이며, 그 뒤 5초 active 구간 내내 전진하지 않았다.
- **error가 정확히 100이다.** 실패 응답이 아니라 window settle 상한까지 완료되지 않아
  abandoned로 계상된 미완료 요청 수다. 이 여덟 셀 밖에서는 네 run 통틀어 error가 0이다.

같은 run의 `grpc-kotlin` request-window는 @1024에서 93.75 KOPS, error 0, 깊이 92.0으로
수렴한다. 정지가 머신이나 harness의 문제가 아니라는 대조군이다. 같은 socket 구성을 쓰는
`zlink-kotlin` request-serial도 5.32 KOPS로 정상이므로, 정지는 **미완료 요청이 둘 이상일
때에만** 나타난다.

**DEALER에서도 같은 모양으로 정지했다.** 소켓 종류에 딸린 성질이 아니라는 Java의 관측과
일치한다.

이 캠페인은 원인을 고치지 않는다. §1.1에 적은 대로 이 관측은 새 binding의 사례가 아니라 같은
binding을 다른 호출 모양으로 사용했을 때의 사례다. 최소 재현은 Java 행이 남긴
`framework/languages/java/bench/with-grpc/repro/`에 있고, Kotlin은 별도 재현을 만들지 않았다.

framework 행의 깊이는 이 정지와 나란히 읽어야 한다. 같은 binding 위에서 깊이만 다른 두 경로가
갈린다.

| run | payload | 처리량/s | 평균 ms | 실제 깊이 | peak_in_flight | abandoned |
|---|---|---|---|---|---|---|
| router-1 | 1024 | 2389.4 | 1.913 | 4.57 | 10 | 0 |
| router-2 | 1024 | 2360.0 | 1.920 | 4.53 | 10 | 0 |
| router-3 | 1024 | 2320.0 | 1.937 | 4.49 | 10 | 0 |
| dealer-1 | 1024 | 2355.0 | 1.919 | 4.52 | 10 | 0 |
| router-1 | 4096 | 2291.8 | 1.961 | 4.49 | 12 | 0 |
| router-2 | 4096 | 2296.8 | 1.952 | 4.48 | 12 | 0 |
| router-3 | 4096 | 2311.4 | 1.945 | 4.49 | 10 | 0 |
| dealer-1 | 4096 | 2342.6 | 1.932 | 4.53 | 12 | 0 |

여덟 셀의 깊이가 4.48~4.57 안에 모두 들어온다. Java의 약 4.5와 같은 값이고, 그 일치가 무엇을
좁히는지는 §1.3에 적었다.

## 8. warmup 안정화 증거 (규격 §8.2)

Kotlin은 Java와 같은 JVM 위에서 동작하므로 Java가 정한 20초를 출발점으로 삼되, **Kotlin 자신의
segment별 자료로 그 값을 확인했다.** coroutine dispatch가 다르게 안정될 수 있으므로 Java의
근거를 그대로 물려받지 않는다.

- **사용한 값: 20초.** 모든 셀에서 같은 값을 쓰고, 셀마다 다르게 조정하지 않았다.
- warmup은 측정 구간과 **같은 driver**로 실행한다. 예열되는 코드 경로가 측정되는 경로와 같아야
  하기 때문이다.
- warmup 20초를 2초 segment 10개로 나누고 **segment마다 처리량을 셀 원본에 기록한다.** 이것이
  안정화의 증거이며 산문이 아니라 자료다.

`kotlin-router-1` payload 1024의 segment별 처리량이다(초당 완료 수).

| 셀 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| `grpc-kotlin-request-serial` | 1580 | 3552 | 4682 | 4877 | 5004 | 4852 | 4578 | 4877 | 4903 | 4953 |
| `zlink-kotlin-request-serial` | 4100 | 5361 | 5420 | 5325 | 5360 | 5410 | 5642 | 5624 | 5686 | 5634 |
| `zlink-framework-kotlin-request-serial` | 368 | 421 | 432 | 439 | 449 | 457 | 454 | 448 | 468 | 479 |
| `grpc-kotlin-request-window` | 71340 | 93317 | 95784 | 89413 | 91178 | 93950 | 89490 | 87178 | 91050 | 92695 |
| `zlink-kotlin-request-window` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `zlink-framework-kotlin-request-window` | 2243 | 2252 | 2312 | 2240 | 2350 | 2351 | 2326 | 2368 | 2324 | 2376 |
| `grpc-kotlin-send-saturation` | 32628 | 32414 | 34900 | 35996 | 36144 | 35148 | 35143 | 35596 | 35396 | 35362 |
| `zlink-kotlin-send-saturation` | 384351 | 396658 | 395866 | 393456 | 386032 | 381436 | 376744 | 373932 | 375978 | 375162 |
| `zlink-framework-kotlin-send-saturation` | 21176 | 11250 | 11228 | 11412 | 10676 | 9892 | 8944 | 10824 | 10764 | 9972 |

예열이 실제로 관측되는 셀은 `grpc-kotlin-request-serial`이다. segment 1의 1580에서 segment 5의
5004까지 **3.2배** 오른 뒤 평평해진다. 마지막 다섯 segment의 중앙값은 4877이고 그 다섯 값의
스프레드는 6.1%다. **segment 3(4~6초 구간)부터 이후 모든 segment가 정상 상태 중앙값의 ±10%
안에 들어온다.** 20초는 그 값의 3배 이상이므로 충분하다.

Java와 비교하면 같은 셀이 Java에서는 2.7배 상승에 6~8초, Kotlin에서는 3.2배 상승에 6초다.
**coroutine 경로가 더 늦게 안정되지는 않았다.** 20초를 그대로 쓰는 것이 타당하다는 Kotlin 자신의
근거다. `.NET`이 사용한 warmup 1000회를 그대로 적용했다면 이 셀은 약 0.2초 분량만 예열되어
정상 상태의 32% 수준을 측정했을 것이다.

두 가지 단서를 남긴다.

- `zlink-kotlin-request-window`의 warmup segment가 **10개 모두 0**이다. 정지는 active 구간에서
  갑자기 나타난 것이 아니라 warmup 20초 내내 이미 발생하고 있었다. §7의 정지가 특정 구간의
  사고가 아니라는 자료다.
- `zlink-framework-kotlin-send-saturation`은 segment 1의 21176에서 segment 2의 11250으로 내려간
  뒤 평평하며 상승 추세가 없다. 예열이 아니라 **초기 buffer가 빈 상태에서 제출이 잠시
  앞서간 것**이며, Java가 같은 셀에서 기록한 것과 같은 모양이다. 이후 segment가 8944~11412로
  흔들리는 것은 §9에서 이 셀이 G5를 넘긴 것과 같은 원인으로 본다.

## 9. G5 재현성

| 패턴 | payload | 구현 | 스프레드 | G5 |
|---|---|---|---|---|
| request-serial | 1024 | `grpc-kotlin` | 1.5% | 통과 |
| request-serial | 1024 | `zlink-kotlin` | 2.9% | 통과 |
| request-serial | 1024 | `zlink-framework-kotlin` | 4.6% | 통과 |
| request-serial | 4096 | `grpc-kotlin` | 4.7% | 통과 |
| request-serial | 4096 | `zlink-kotlin` | 1.3% | 통과 |
| request-serial | 4096 | `zlink-framework-kotlin` | 2.9% | 통과 |
| request-window | 1024 | `grpc-kotlin` | 0.5% | 통과 |
| request-window | 1024 | `zlink-framework-kotlin` | 1.7% | 통과 |
| request-window | 4096 | `grpc-kotlin` | 2.9% | 통과 |
| request-window | 4096 | `zlink-framework-kotlin` | 0.6% | 통과 |
| send-saturation | 1024 | `grpc-kotlin` | 2.4% | 통과 |
| send-saturation | 1024 | `zlink-kotlin` | 4.0% | 통과 |
| send-saturation | 1024 | `zlink-framework-kotlin` | 12.8% | **미달** |
| send-saturation | 4096 | `grpc-kotlin` | 2.4% | 통과 |
| send-saturation | 4096 | `zlink-kotlin` | 3.3% | 통과 |
| send-saturation | 4096 | `zlink-framework-kotlin` | 4.0% | 통과 |

16셀 중 15셀이 통과한다. `zlink-framework-kotlin` send-saturation @1024가 12.8%로 한도를 2.8%p
넘겼다. run별 값은 5668.4 / 5393.6 / 6394.2 msg/s이고, 중앙값 5668.4에 대해 router-3이 +12.8%다.
§8이 남긴 대로 이 셀은 warmup segment도 8944~11412로 흔들린다. 제출이 소비를 크게 앞지르는
경로(§4의 초 단위 수신 지연)라 경계에서 표본화한 값이 run마다 흔들리는 것으로 보이며, 원인은
규명하지 않았다.

`zlink-kotlin`의 request-window 두 셀은 이 표에 넣지 않는다. 처리량이 성립하지 않는 셀에
재현성 판정을 적용하면 값의 뜻이 바뀐다.

같은 구간에 재사용한 C 기준 bench에서 `zlink-c`는 request-serial @1024 37.9%, @4096 12.6%,
request-window @4096 28.7%로 세 행이 미달했다. `zlink-c`의 불안정은 이 캠페인에서 반복
관측되는 항목이며, request-window @4096은 FB-034가 기록한 세 구간 연속 미달과 같은 값이다.

## 10. 게이트

| 게이트 | 결과 | 근거 |
|---|---|---|
| G1 계약 정합 | **통과** | 4 run 모두 3패턴 × 2 payload × 3구현 = 18셀을 실행했다. 실패 0, 오염 0. `RESULT` metric 9종을 채웠다 |
| G2 header 검증 | **통과** | request 계열은 reply의 29바이트 header를 client가 검증한다. 검증 실패는 0건이다. 기록된 error는 전부 §7의 미완료 요청 100건이며 header 불일치가 아니다 |
| G3 send 무결성 | **통과**(kotlin) | Kotlin 세 구현 모두 server 수신 수로 계산했고 `server_received_at_close`를 셀마다 남긴다. C 기준 bench는 client 제출 수를 세므로 집계기가 4셀을 판정에서 제외했다(FB-014, 기존 결함) |
| G4 공개 API만 사용 | **통과** | framework host는 공개 Spring Boot starter, 호출은 `zlink-framework-kotlin`의 공개 suspend 확장, gRPC는 생성된 coroutine stub. reflection·내부 package·두 번째 poller·재시도 상태를 넣지 않았다 |
| G5 재현성 | **부분** | Kotlin 16셀 중 15셀 통과, `zlink-framework-kotlin` send-saturation @1024가 12.8%. 정지한 2셀은 적용 대상이 아니다. 재사용한 `zlink-c` 3행 미달 |
| G6 포화 표시 | **통과** | 선언 계측기 `jvm_thread_cores`와 상한을 셀마다 기록했다. 포화 셀은 없다 |
| G7 격리 | **통과** | 측정 구간 전체가 `flock /tmp/zlink-perf.lock` 아래이고, Gradle 빌드는 구간 밖에서 끝냈다. run별 loadavg 판독을 §2.7에 남겼다 |
| G8 깊이 보고 | **통과** | 셀마다 `peak_in_flight`와 처리량 × 평균 지연으로 계산한 실제 깊이를 낸다. 설정값과 크게 다른 두 항목을 §5에 명시했다 |

## 11. 이 자료로 결론지을 수 없는 것

- **`zlink-kotlin`의 request-window 처리량을 확정할 수 없다.** 여덟 셀이 모두 정지했다. 정상
  표본이 하나도 없어 G5를 적용할 대상 자체가 없다.
- **binding 계층 판정(formula 1)을 낼 수 없다.** 분자가 존재하지 않는다. @4096은 그와 별개로
  분모도 성립하지 않는다(FB-034).
- **framework 추가 비용 판정(formula 2)을 낼 수 없다.** 분모가 존재하지 않는다.
  `zlink-framework-kotlin`의 request-window는 2.36 KOPS로 안정적으로 측정됐지만, 나눌 상대가 없다.
- **Kotlin이 FB-031에 네 번째 독립 binding 사례를 더하지 못한다.** `zlink-kotlin`은
  `zlink-java`와 같은 `systems.zlink:zlink`를 사용한다. 더한 것은 호출 모양과 소켓 종류를 바꿔도
  같은 지점에서 정지한다는 사실이다.
- **framework 깊이 4.5의 상한이 어디에 있는지 특정하지 못했다.** §2.6대로 Kotlin 행이 Java 행의
  framework server를 공유하므로, client 언어가 원인이 아니라는 것까지만 좁혀진다. framework
  core의 client 쪽 경로인지 server 쪽인지는 이 캠페인이 가르지 않는다.
- **`zlink-framework-kotlin` send @1024의 재현성 미달 원인을 규명하지 못했다.**
- **coroutine dispatch 비용의 크기를 분리하지 못했다.** §2.3대로 선언 계측기는 제출 경로만
  재고, continuation의 CPU는 `jvm_all_thread_cores` 관찰값에만 들어간다.
- **언어를 가로지른 절대 처리량 비교는 하지 않는다**(규격 §7.3). `grpc-kotlin` 93.75 KOPS와
  `grpc-java` 117.31 KOPS를 나란히 놓은 값은 런타임·stub 비교이지 ZLink 비교가 아니다.

## 12. 후속으로 넘긴 항목

| 항목 | 내용 |
|---|---|
| FB-030 | raw request가 미완료 요청 둘 이상에서 회신을 잃는다. Kotlin에서 호출 모양(coroutine await)과 소켓 종류(DEALER)를 바꿔도 재현된다 |
| FB-033 | framework의 request-window 깊이 상한이 Java·Kotlin 두 client API에서 같은 4.5다. client 언어가 원인이 아니라는 것까지 좁혀진다 |
| FB-034 | `zlink-c` request-window @4096이 네 번째 측정 구간에서도 G5 미달(28.7%). 이 행이 안정되기 전까지 @4096 formula 1은 어느 언어도 게재할 수 없다 |
| `zlink-framework-kotlin` send @1024 | G5 12.8% 미달. warmup segment도 함께 흔들린다 |
| framework send 경로의 수신 지연 | @1024에서 평균 2.48초. FB-009가 `.NET`에서, Java 행이 Java에서 기록한 것과 같은 성격 |
| Kotlin 행의 server 소유 | 이 Phase는 client만 Kotlin으로 구현했다. server까지 Kotlin으로 세우는 것이 필요한지는 Phase 6에서 판단한다 |
