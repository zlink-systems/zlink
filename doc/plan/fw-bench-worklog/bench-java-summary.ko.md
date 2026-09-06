# Java with-grpc bench 요약 (Phase 3)

이 문서는 `java` 언어의 with-grpc bench 측정 결과를 하나로 모은 기록이다. Phase 6 보고서가
이 문서만 읽고도 `java` 행을 서술할 수 있도록 조건, 수치, 판정, 판정할 수 없는 항목을 함께
남긴다.

계획: [`../framework-bench-with-grpc-5lang-plan.ko.md`](../framework-bench-with-grpc-5lang-plan.ko.md)
결정: [`decisions.ko.md`](./decisions.ko.md)
규격: [`../../../framework/doc/framework/common/bench/with-grpc-local.ko.md`](../../../framework/doc/framework/common/bench/with-grpc-local.ko.md)

## 1. 결론 먼저

### 1.1 `zlink-java`의 request-window 셀 여덟 개가 모두 정지했다. 이 셀은 처리량이 아니라 정지를 측정한 값이다

`request_window` 기본값 100에서 `zlink-java`의 request-window 셀은 ROUTER 3회 × payload 2종
여섯 개와 DEALER 1회 × payload 2종 두 개, 곧 **여덟 개 전부가 정지했다.** 여덟 셀 모두
`peak_in_flight`가 100에 도달하고 `abandoned`가 100이다. 곧 window는 채워졌고, 채워진 100개가
하나도 완료되지 않은 채 남았다.

**이 셀의 처리량 값을 Java의 request-window 성능으로 인용하면 안 된다.** 표에 남는 0.00 KOPS나
0.02 KOPS는 이 경로가 지탱하는 속도가 아니라, 완료를 멈춘 socket을 active 구간 길이로 나눈
산술 결과다. 그래서 이 문서는 정지한 셀에 처리량 값을 싣지 않고 §7에 정지 자체의 관측으로
싣는다.

정지는 DEALER 구성에서도 같은 모양으로 발생했다. 소켓 종류에 딸린 성질이 아니다.

### 1.2 그래서 formula 1의 분자가 존재하지 않고, Java도 0.80 판정을 게재하지 못한다

판정 패턴은 `request-window`다(규격 §7.2, FB-003). 그 셀이 처리량을 내지 않으므로
`zlink-java / zlink-c`의 분자가 없고, `zlink-framework-java / zlink-java`의 분모도 없다. 네
판정 모두 `unsupported`다. **이것은 자료의 구멍이 아니라 §1.1이 낳은 결과다.**

.NET은 0.084로 기준에 미달했고, Node는 client 경로가 transport보다 먼저 JS thread를
포화시켜 판정을 게재하지 못했으며, Java는 raw request 경로가 정지해 판정을 게재하지 못한다.
**세 언어가 서로 다른 세 가지 이유로 판정에 이르지 못했다는 사실 자체가 이 캠페인의 관측이다.**

### 1.3 Java의 framework 행은 규격이 정한 payload를 그대로 싣고 18셀 전부를 측정했다

Node가 여섯 셀을 `unsupported`로 남긴 원인은 공개 protobuf codec에 bytes 형이 없다는 것이었다
(FB-028). Java의 `zlink-framework-codec-protobuf`는 protobuf 생성 클래스를 그대로 직렬화하므로
`bytes body`를 그대로 전달한다. **Java는 네 run 모두 18셀을 실행했고 실패 0, 오염 0이다.**
framework 계층의 절대 비용이 큰 것은 별개 관측이며 §4에 그대로 싣는다.

### 1.4 게재할 수 있는 값

`grpc-java` 12셀, `zlink-java`의 request-serial 4셀과 send-saturation 4셀,
`zlink-framework-java` 12셀은 정지 없이 측정됐고 G5도 통과한다. 이 값들은 그대로 게재한다.

## 2. 측정 대상과 조건

### 2.1 비교 대상

| 구현 이름 | 내용 |
|---|---|
| `grpc-java` | grpc-java unary RPC. proto는 `Echo`와 `Command` 둘뿐이다(FB-002) |
| `zlink-java` | framework를 거치지 않는 raw binding. ROUTER↔ROUTER |
| `zlink-framework-java` | `zlink-framework-core`의 RouteMesh channel request 처리기와 send 처리기 |

ZLink raw 행은 규격 §1.3대로 ROUTER↔ROUTER를 사용한다. client도 ROUTER를 만들고 자기 routing
id를 설정한 뒤 상대 ROUTER의 routing id를 지정해 전송한다. wire 모양은 envelope 헤더 part
하나와 protobuf로 인코딩한 `BenchPayload` part 하나로, `zlink-c`·`zlink-dotnet`과 같다(FB-024).

framework 행의 host는 `zlink-framework-spring-boot-starter`다. Java framework를 세우는 공개
경로가 이것이며, Node의 `@zlink-systems/nestjs`와 .NET의 `Zlink.Framework.AspNetCore`에
해당한다. 내부 package는 사용하지 않았다(G4).

### 2.2 고정 조건

| 항목 | 값 |
|---|---|
| payload 크기 | `1024`, `4096` bytes |
| `request_window` | 100 |
| send concurrency | 8 |
| warmup | **20초**(2초 segment 10개), 측정과 같은 driver로 실행 (§8) |
| active duration | 5초 |
| request timeout | 30초 |
| 반복 | ROUTER 3회, DEALER 1회 |
| transport | loopback `127.0.0.1`, 포트 대역 5091-5097 |
| 대표값 | 중앙값 |

정지를 피하려고 `request_window`를 낮추지 않았다. 표준 workload에서 정지한다는 사실 자체가
측정 결과다.

### 2.3 실행 환경과 이력

| 항목 | 값 |
|---|---|
| CPU | Intel Core Ultra 7 265K, 논리 core 20개 |
| OS | kernel 6.6.87.2-microsoft-standard-WSL2 |
| JDK | Temurin 22.0.2, OpenJDK 64-Bit Server VM (Eclipse Adoptium) |
| gRPC | grpc-java 1.72.0 (`grpc-netty-shaded`, `grpc-protobuf`, `grpc-stub`) |
| protobuf | protobuf-java 4.30.2, protobuf gradle plugin 0.9.4 |
| gRPC server 구성 | `io.grpc.ServerBuilder.forPort` 기본 구성, plaintext loopback |
| ZLink binding | `systems.zlink:zlink` 0.17.0 |
| framework | `zlink-framework-core` 0.10.0, codec `zlink-framework-codec-protobuf` 0.10.0 |
| 측정 기준 commit | `dcded04dbe` |
| 측정 구간 | 2026-09-07T03:06:55+09:00 ~ 03:53:18+09:00 |

### 2.4 측정 격리

측정 구간 전체를 `flock --exclusive /tmp/zlink-perf.lock`으로 잠갔다. Gradle 빌드는 측정 구간
바깥에서 저장소의 JVM 빌드 잠금(`/tmp/zlink-jvm-gate.lock`) 아래 먼저 끝냈고, 측정 스크립트는
`SKIP_BUILD=1`로 실행해 어떤 컴파일도 구간 안에 들어가지 않게 했다(G7). C 기준 bench도 같은
방식으로 미리 빌드했다. 네 run과 C 3회 run이 모두 종료 코드 0으로 끝났다. runner는 시작 전에
포트 대역 5091-5097이 비어 있는지 확인하고, 사용 중이면 다른 포트로 옮기지 않고 중단한다.

run마다 시작 직전 1분 load average를 확인했고, 2.0 이상이면 대기했다. 관측값은 아래와 같다.

| 시점 | 최초 판독 | 대기 후 판독 |
|---|---|---|
| span 시작 / `java-router-1` | 0.63 | — |
| `java-router-2` | 2.71 | 1.94 |
| `java-router-3` | 3.89 | 1.89 |
| `c-router-1` | 4.30 | 1.91 |
| `c-router-2` | 3.54 | 1.97 |
| `c-router-3` | 2.72 | 1.94 |
| `java-dealer-1` | 3.51 | 1.96 |
| span 종료 | 3.16 | — |

2.0을 넘은 판독은 모두 **직전 run이 끝난 직후의 잔여 부하**이고, 20~50초 대기 뒤 전부 2.0
아래로 내려갔다. 다른 job은 실행하지 않았다.

### 2.5 client 포화 계측기 — Java는 `client_cores`를 쓰지 않는다

FB-023에 따라 harness는 계측기와 상한을 함께 선언한다. Java는 **선언 계측기를
`jvm_thread_cores`로 둔다.** 프로세스 core 수를 쓰지 않는 이유는 실측 두 가지다.

- **다른 것을 재게 된다.** `zlink-java` send 셀에서 프로세스 CPU의 약 94%가 Core의 native I/O
  thread이고 그 thread는 user 코드를 실행하지 않는다. 같은 run의 `grpc-java`에서는 그 비율이
  약 3%다. 판정식이 나누는 두 행이 서로 다른 양을 재게 된다.
- **발동하지 않는다.** 논리 core 20개를 상한으로 두면 이 머신에서 관측된 가장 큰 값이 상한의
  0.154배다. 어떤 셀도 포화로 표시될 수 없다.

| 항목 | 값 |
|---|---|
| 선언 계측기 | `jvm_thread_cores` — harness가 제출 loop를 실행하는 JVM thread의 CPU ÷ 경과 시간 |
| 선언 상한 | request 계열 driver `1`, send driver `8`(send concurrency) |
| 포화 기준 | 상한의 0.95배 이상 |

계측기와 상한이 같은 대상을 가리키도록 **제출 thread만** 센다. 모든 JVM thread를 세어 제출
병렬도와 비교하면 다시 서로 다른 양을 비교하게 된다. GC와 JIT compiler thread는 JVM thread가
아니므로 `ThreadMXBean`에 나타나지 않고 이 값을 부풀리지 않는다. 프로세스 core 수와 전체 JVM
thread core 수는 관찰값으로 셀마다 함께 기록하지만 포화를 판정하지 않는다.

## 3. 측정 표 — payload 1024

ROUTER 구성 3회 run의 중앙값이다. 처리량 단위는 request 계열이 `KOPS`,
`send-saturation`이 `KMSG/s`다. CPU는 논리 core 20개 기준 백분율, memory는 RSS(MB)다.

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | client MB | server CPU% | server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | `grpc-java` | 5.66 | 0.176 | 0.224 | 0.272 | 2.4 | 327.0 | 1.9 | 928.3 | — |
| request-serial | `zlink-java` | 6.37 | 0.157 | 0.200 | 0.263 | 2.6 | 352.3 | 1.3 | 496.1 | — |
| request-serial | `zlink-framework-java` | 0.49 | 2.057 | 2.526 | 2.683 | 1.7 | 409.6 | 1.8 | 372.9 | — |
| request-window | `grpc-java` | 117.31 | 0.832 | 1.012 | 1.240 | 15.5 | 908.0 | 13.9 | 1177.9 | — |
| request-window | `zlink-java` | **처리량 없음 — 정지(§7)** | — | — | — | 0.7 | 910.8 | 0.0 | 501.9 | — |
| request-window | `zlink-framework-java` | 2.37 | 1.906 | 2.745 | 2.980 | 5.1 | 947.4 | 4.9 | 389.5 | — |
| send-saturation | `grpc-java` | 41.34 | 0.105 | 0.155 | 0.184 | 9.8 | 1042.6 | 7.4 | 1209.3 | 313 |
| send-saturation | `zlink-java` | 477.22 | 0.073 | 0.215 | 0.272 | 10.2 | 1059.2 | 4.6 | 1044.5 | 308 |
| send-saturation | `zlink-framework-java` | 6.70 | 2319.980 | 2492.296 | 2524.796 | 13.3 | 1149.5 | 14.1 | 873.3 | 2122 |

## 4. 측정 표 — payload 4096

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | client MB | server CPU% | server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | `grpc-java` | 5.78 | 0.173 | 0.219 | 0.267 | 2.4 | 1151.1 | 1.8 | 1209.6 | — |
| request-serial | `zlink-java` | 6.35 | 0.157 | 0.199 | 0.257 | 2.6 | 1151.7 | 1.2 | 1046.4 | — |
| request-serial | `zlink-framework-java` | 0.49 | 2.039 | 2.487 | 2.562 | 1.7 | 1153.0 | 1.4 | 899.8 | — |
| request-window | `grpc-java` | 98.50 | 0.984 | 1.349 | 1.487 | 14.4 | 1577.5 | 12.9 | 1233.4 | — |
| request-window | `zlink-java` | **처리량 없음 — 정지(§7)** | — | — | — | 0.7 | 1580.3 | 0.0 | 1047.0 | — |
| request-window | `zlink-framework-java` | 2.36 | 1.920 | 2.764 | 2.985 | 5.2 | 1591.0 | 5.0 | 913.6 | — |
| send-saturation | `grpc-java` | 39.66 | 0.110 | 0.163 | 0.191 | 9.8 | 1599.7 | 7.2 | 1233.1 | 311 |
| send-saturation | `zlink-java` | 318.94 | 0.083 | 0.239 | 0.295 | 10.6 | 1602.6 | 4.8 | 1051.4 | 362 |
| send-saturation | `zlink-framework-java` | 8.95 | 808.894 | 880.459 | 900.983 | 13.3 | 1643.4 | 13.5 | 919.5 | 1000 |

`send-saturation`의 지연은 규격 §5대로 server가 header로 계산한 수신 지연이다. framework send의
지연이 초 단위인 것은 sender가 receiver를 크게 앞지를 때 나타나는 모양이며, FB-009가 .NET에서
기록한 것과 같은 성격이다. 이 값 하나로 결함을 단정하지 않는다.

`send-saturation` 셀의 서술 규칙(규격 §2.1, FB-002)에 따라 이 셀을 전송 속도 차이로 쓰지
않는다. 응답이 필요 없는 명령을 처리할 때 gRPC는 unary 왕복을 치러야 하고 ZLink는 단방향
send로 끝난다. 이 조건에서 `zlink-java`와 `grpc-java`의 차이는 @1024에서 11.5배로 관찰됐다.

## 5. 포화와 깊이

선언 계측기는 `jvm_thread_cores`, 상한은 request driver 1과 send driver 8이다(§2.5).

| 패턴 | payload | 구현 | 계측기 판독 | 상한 | 포화 | peak_in_flight | 실제 깊이 | abandoned |
|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-java` | 0.069 | 1 | 아니오 | 1 | 1.0 | 0 |
| request-serial | 1024 | `zlink-java` | 0.082 | 1 | 아니오 | 1 | 1.0 | 0 |
| request-serial | 1024 | `zlink-framework-java` | 0.039 | 1 | 아니오 | 1 | 1.0 | 0 |
| request-serial | 4096 | `grpc-java` | 0.070 | 1 | 아니오 | 1 | 1.0 | 0 |
| request-serial | 4096 | `zlink-java` | 0.085 | 1 | 아니오 | 1 | 1.0 | 0 |
| request-serial | 4096 | `zlink-framework-java` | 0.038 | 1 | 아니오 | 1 | 1.0 | 0 |
| request-window | 1024 | `grpc-java` | 0.275 | 1 | 아니오 | 100 | 97.6 | 0 |
| request-window | 1024 | `zlink-java` | 0.006 | 1 | 아니오 | 100 | n/a(정지) | **100** |
| request-window | 1024 | `zlink-framework-java` | 0.173 | 1 | 아니오 | **10**(최저 9) | 4.5 | 0 |
| request-window | 4096 | `grpc-java` | 0.298 | 1 | 아니오 | 100 | 96.9 | 0 |
| request-window | 4096 | `zlink-java` | 0.006 | 1 | 아니오 | 100 | n/a(정지) | **100** |
| request-window | 4096 | `zlink-framework-java` | 0.177 | 1 | 아니오 | **11**(최저 10) | 4.5 | 0 |
| send-saturation | 1024 | `grpc-java` | 0.431 | 8 | 아니오 | 8 | 4.4 | 0 |
| send-saturation | 1024 | `zlink-java` | 1.250 | 8 | 아니오 | 8 | 35.0 | 0 |
| send-saturation | 1024 | `zlink-framework-java` | 0.554 | 8 | 아니오 | 8 | 15537.4 | 0 |
| send-saturation | 4096 | `grpc-java` | 0.443 | 8 | 아니오 | 8 | 4.4 | 0 |
| send-saturation | 4096 | `zlink-java` | 1.256 | 8 | 아니오 | 8 | 26.5 | 0 |
| send-saturation | 4096 | `zlink-framework-java` | 0.582 | 8 | 아니오 | 8 | 7243.3 | 0 |

**포화 셀은 없다.** 제출 thread는 어느 셀에서도 선언 상한에 닿지 않았다. Java에서는 client
런타임이 상한이 아니었다는 뜻이며, Node가 request 계열 네 셀 전부에서 상한에 닿았던 것과
대비된다.

두 값을 따로 적어 둔다.

- **`zlink-framework-java`의 request-window 실제 깊이는 설정값 100에 대해 약 4.5이고
  `peak_in_flight`도 10~11에 그친다.** abandoned는 0이므로 요청이 버려진 것이 아니다. 이
  경로가 그 깊이까지만 미완료 request를 유지한다는 관측이다. FB-017이 구분하려 한 두 경우
  가운데 "스택이 그 깊이까지만 낸다"에 해당한다.
- **`zlink-framework-java`의 send 실제 깊이(처리량 × 평균 지연)는 7243~15537로 매우 크다.**
  이는 동시성이 아니라 §4의 초 단위 수신 지연이 만든 값이며, 제출이 소비를 크게 앞질렀다는
  뜻이다.

## 6. 0.80 판정

| 판정식 | payload | 값 | 상태 | 막은 행과 사유 |
|---|---|---|---|---|
| `zlink-java / zlink-c` | 1024 | — | `unsupported` | 분자 `zlink-java-request-window@1024`가 처리량을 내지 않는다(정지). 세 run 모두 완료 0 |
| `zlink-java / zlink-c` | 4096 | (0.000) | `unsupported` | 분자 `zlink-java-request-window@4096` G5 100.0% 미달(정지). 분모 `zlink-c-request-window@4096`도 G5 28.7% 미달 |
| `zlink-framework-java / zlink-java` | 1024 | — | `unsupported` | 분모 `zlink-java-request-window@1024`가 위와 같은 사유로 성립하지 않는다 |
| `zlink-framework-java / zlink-java` | 4096 | (118.200) | `unsupported` | 분모 `zlink-java-request-window@4096` G5 100.0% 미달 |

**java: 미완료 — 네 판정 모두 `unsupported`.** 규격 §7.2는 두 payload 크기 모두를 요구한다
(FB-005).

괄호 안의 값은 집계기가 계산한 중앙값 비율이며 **게재값이 아니다.** 4096의 118.200은 분모가
정지한 셀의 20.0/s이기 때문에 나온 수이고, framework가 raw보다 118배 빠르다는 뜻이 아니다.
FB-011이 금지하는 바로 그 형태의 값이므로 인용하지 않는다.

분모 쪽도 따로 적어 둔다. **`zlink-c`의 request-window @4096이 이번 구간에서도 G5 28.7%로
실패했다.** Phase 0에서 75.7%, `fwb-02b`에서 25.7%였던 것과 같은 항목이다. 분자가 성립했더라도
@4096 판정은 분모 때문에 게재하지 못했을 것이다.

## 7. FB-030 정지의 실측

표준 workload(`request_window` 100)에서 `zlink-java` request-window 셀의 run별 관측이다.

| run | payload | 완료 | error | abandoned | peak_in_flight | 평균 ms | 판정 |
|---|---|---|---|---|---|---|---|
| router-1 | 1024 | 0 | 100 | 100 | 100 | — | 정지 |
| router-1 | 4096 | 0 | 100 | 100 | 100 | — | 정지 |
| router-2 | 1024 | 0 | 100 | 100 | 100 | — | 정지 |
| router-2 | 4096 | 113 | 100 | 100 | 100 | 0.453 | 정지 |
| router-3 | 1024 | 0 | 100 | 100 | 100 | — | 정지 |
| router-3 | 4096 | 100 | 100 | 100 | 100 | 0.585 | 정지 |
| dealer-1 | 1024 | 0 | 100 | 100 | 100 | — | 정지 |
| dealer-1 | 4096 | 0 | 100 | 100 | 100 | — | 정지 |

정지 셀을 알아보는 표시는 아래와 같다.

- **`peak_in_flight` 100에 `abandoned` 100이 함께 나온다.** window는 채워졌고 채워진 그대로
  하나도 완료되지 않았다. harness가 window를 채우지 못하는 FB-010 유형이 아니다.
- **완료 수가 0이거나 100건 안팎에서 멈춘다.** router-2와 router-3의 @4096이 낸 113건과 100건은
  정지 전에 통과한 소수이며, 그 뒤 5초 active 구간 내내 전진하지 않았다.
- **error가 정확히 100이다.** 이 값은 실패 응답이 아니라 window settle 상한까지 완료되지 않아
  abandoned로 계상된 미완료 요청 수다.

같은 run의 `grpc-java` request-window는 @1024에서 117.31 KOPS, error 0, 깊이 97.6으로 수렴한다.
정지가 머신이나 harness의 문제가 아니라는 대조군이다. 같은 socket을 쓰는 `zlink-java`
request-serial도 6.37 KOPS로 정상이므로, 정지는 **미완료 요청이 둘 이상일 때에만** 나타난다.

정지의 위치는 bench 밖 최소 재현으로 분리했다. 재현은
`framework/languages/java/bench/with-grpc/repro/`에 있고 `systems.zlink:zlink`만 사용한다.
harness를 전혀 import하지 않는다.

| 미완료 요청 수 | 프로세스 안 echo server | bench raw server 대상 |
|---|---|---|
| 4 | 4/4 완료 | 4/4 완료 |
| 16 | 16/16 완료 | 0/16, 5/16, 10/16 (run마다 다름) |
| 100 | **43/100 완료, server는 101건 전부 회신** | 0/100, 5/100 |

가장 중요한 값은 프로세스 안 재현의 100건이다. **server가 101건을 모두 회신했는데 client는
43건만 완료했다.** 곧 reply는 만들어졌고 server의 제출과 client의 완료 사이에서 사라진다.
Node의 FB-026이 "정지한 socket"까지만 보인 것과 달리, 이 재현은 회신이 생성된 뒤 손실된다는
것까지 보인다. 손실은 간헐적이며 고정된 상한이 아니다.

이 캠페인은 원인을 고치지 않는다. binding과 Core 중 어디인지는 계측하지 않았다.

## 8. warmup 안정화 증거 (규격 §8.2)

Java는 JIT 예열이 끝난 뒤에 정상 상태가 되므로 warmup 값과 그 근거를 함께 남긴다.

- **사용한 값: 20초.** 모든 셀에서 같은 값을 쓰고, 셀마다 다르게 조정하지 않았다.
- warmup은 측정 구간과 **같은 driver**로 실행한다. 예열되는 코드 경로가 측정되는 경로와 같아야
  하기 때문이다.
- warmup 20초를 2초 segment 10개로 나누고 **segment마다 처리량을 셀 원본에 기록한다.** 이것이
  안정화의 증거이며 산문이 아니라 자료다.

`java-router-1` payload 1024의 segment별 처리량이다(초당 완료 수).

| 셀 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| `grpc-java-request-serial` | 2240 | 4804 | 5530 | 5998 | 6010 | 5897 | 5866 | 5802 | 5925 | 5939 |
| `zlink-java-request-serial` | 4976 | 6488 | 6514 | 6210 | 6220 | 6530 | 6454 | 6944 | 6860 | 6418 |
| `zlink-framework-java-request-serial` | 371 | 425 | 440 | 446 | 458 | 460 | 454 | 458 | 462 | 492 |
| `grpc-java-request-window` | 90136 | 117162 | 115031 | 117830 | 113332 | 118101 | 117466 | 117914 | 117042 | 112046 |
| `zlink-framework-java-request-window` | 2330 | 2294 | 2370 | 2368 | 2394 | 2386 | 2358 | 2385 | 2402 | 2396 |
| `grpc-java-send-saturation` | 39382 | 41306 | 41927 | 41950 | 41530 | 42791 | 42554 | 42804 | 41848 | 42318 |
| `zlink-java-send-saturation` | 507029 | 482977 | 495473 | 506820 | 504020 | 498245 | 516270 | 493008 | 486476 | 494521 |
| `zlink-framework-java-send-saturation` | 18894 | 9802 | 10535 | 10676 | 10700 | 10744 | 10654 | 10464 | 9488 | 10700 |

예열이 실제로 관측되는 셀은 `grpc-java-request-serial`이다. segment 1의 2240에서 segment 4의
5998까지 2.7배 오른 뒤 평평해지고, 마지막 다섯 segment의 스프레드는 1.6%다. **가장 늦게
안정된 셀도 6~8초 안에 이후 모든 segment가 정상 상태 중앙값의 ±10% 안에 들어온다.** 20초는
그 값의 2.5배 이상이므로 충분하다.

`.NET`이 사용한 warmup 1000회를 그대로 적용했다면 `grpc-java-request-serial`은 약 0.2초 분량만
예열되어 정상 상태의 40% 수준을 측정했을 것이다. 규격 §8.2가 언어마다 warmup을 다르게 두라고
정한 이유가 이 표에 그대로 나타난다.

한 가지 단서를 남긴다. `zlink-framework-java-send-saturation`은 segment 1의 18894에서 segment
2의 9802로 내려간 뒤 평평하다. 이는 예열이 아니라 **초기 buffer가 빈 상태에서 제출이 잠시
앞서간 것**이며, segment 2부터는 상승 추세가 없다. segment 9의 9488은 정상 상태 중앙값에서
10.9% 떨어진 단발 값이고 추세가 아니다.

## 9. G5 재현성

| 패턴 | payload | 구현 | 스프레드 | G5 |
|---|---|---|---|---|
| request-serial | 1024 | `grpc-java` | 3.8% | 통과 |
| request-serial | 1024 | `zlink-java` | 0.4% | 통과 |
| request-serial | 1024 | `zlink-framework-java` | 3.1% | 통과 |
| request-serial | 4096 | `grpc-java` | 8.2% | 통과 |
| request-serial | 4096 | `zlink-java` | 10.6% | **미달** |
| request-serial | 4096 | `zlink-framework-java` | 2.7% | 통과 |
| request-window | 1024 | `grpc-java` | 0.7% | 통과 |
| request-window | 1024 | `zlink-framework-java` | 5.2% | 통과 |
| request-window | 4096 | `grpc-java` | 1.4% | 통과 |
| request-window | 4096 | `zlink-framework-java` | 0.4% | 통과 |
| send-saturation | 1024 | `grpc-java` | 4.9% | 통과 |
| send-saturation | 1024 | `zlink-java` | 1.3% | 통과 |
| send-saturation | 1024 | `zlink-framework-java` | 0.7% | 통과 |
| send-saturation | 4096 | `grpc-java` | 6.9% | 통과 |
| send-saturation | 4096 | `zlink-java` | 1.3% | 통과 |
| send-saturation | 4096 | `zlink-framework-java` | 0.1% | 통과 |

16셀 중 15셀이 통과한다. `zlink-java` request-serial @4096이 10.6%로 한도를 0.6%p 넘겼다.

`zlink-java`의 request-window 두 셀은 이 표에 넣지 않는다. 집계기가 내는 스프레드는
@4096에서 100.0%인데, 이는 처리량의 재현성이 아니라 정지가 얼마나 들쭉날쭉하게 발생하는지를
나타내는 값이다. 처리량이 성립하지 않는 셀에 재현성 판정을 적용하면 값의 뜻이 바뀐다.

같은 구간의 C 기준 bench에서 `zlink-c`는 request-serial @1024 37.9%, @4096 12.6%,
request-window @4096 28.7%로 세 행이 미달했다. `zlink-c`의 불안정은 이 캠페인에서 반복
관측되는 항목이다.

## 10. 게이트

| 게이트 | 결과 | 근거 |
|---|---|---|
| G1 계약 정합 | **통과** | 4 run 모두 3패턴 × 2 payload × 3구현 = 18셀을 실행했다. 실패 0, 오염 0. `RESULT` metric 9종을 채웠다 |
| G2 header 검증 | **통과** | request 계열은 reply의 29바이트 header를 client가 검증한다. 검증 실패는 0건이다. 기록된 error는 전부 §7의 미완료 요청 100건이며 header 불일치가 아니다 |
| G3 send 무결성 | **통과**(java) | Java 세 구현 모두 server 수신 수로 계산했고 `server_received_at_close`를 셀마다 남긴다. C 기준 bench는 client 제출 수를 세므로 집계기가 4셀을 판정에서 제외했다(FB-014, 기존 결함) |
| G4 공개 API만 사용 | **통과** | framework host는 공개 Spring Boot starter, 진단은 공개 `ZLinkHandlerFilter`. reflection·내부 package·두 번째 poller·재시도 상태를 넣지 않았다 |
| G5 재현성 | **부분** | Java 16셀 중 15셀 통과, `zlink-java` request-serial @4096이 10.6%. 정지한 2셀은 적용 대상이 아니다. `zlink-c` 3행 미달 |
| G6 포화 표시 | **통과** | 선언 계측기 `jvm_thread_cores`와 상한을 셀마다 기록했다. 포화 셀은 없다 |
| G7 격리 | **통과** | 측정 구간 전체가 `flock /tmp/zlink-perf.lock` 아래이고, Gradle·CMake 빌드는 구간 밖에서 끝냈다. run별 loadavg 판독을 §2.4에 남겼다 |
| G8 깊이 보고 | **통과** | 셀마다 `peak_in_flight`와 처리량 × 평균 지연으로 계산한 실제 깊이를 낸다. 설정값과 크게 다른 두 항목을 §5에 명시했다 |

## 11. 이 자료로 결론지을 수 없는 것

- **`zlink-java`의 request-window 처리량을 확정할 수 없다.** 여덟 셀이 모두 정지했다. 정상
  표본이 하나도 없어 G5를 적용할 대상 자체가 없다.
- **binding 계층 판정(formula 1)을 낼 수 없다.** 분자가 존재하지 않는다.
- **framework 추가 비용 판정(formula 2)을 낼 수 없다.** 분모가 존재하지 않는다.
  `zlink-framework-java`의 request-window는 2.37 KOPS로 안정적으로 측정됐지만, 나눌 상대가 없다.
- **`zlink-framework-java`가 request-window에서 깊이 4.5까지만 유지하는 원인을 규명하지
  못했다.** abandoned가 0이므로 요청 손실은 아니다. 값만 기록한다.
- **FB-030 정지의 근본 원인을 특정하지 못했다.** 확인한 것은 미완료 요청이 둘 이상일 때
  나타난다는 것, server는 회신을 모두 만든다는 것, 손실이 server 제출과 client 완료 사이에서
  발생한다는 것, 간헐적이라는 것, ROUTER와 DEALER 모두에서 발생한다는 것이다.
- **언어를 가로지른 절대 처리량 비교는 하지 않는다**(규격 §7.3). `grpc-java` 117.31 KOPS와
  `grpc-c` 65.86 KOPS를 나란히 놓은 값은 런타임 비교이지 ZLink 비교가 아니다.

## 12. 후속으로 넘긴 항목

| 항목 | 내용 |
|---|---|
| FB-029 | handler 생성 실패를 framework가 조용히 수용하고 버린다. 메시지를 received·admitted·dispatched로 추적한 뒤 버리고 sender에게는 성공을 돌려준다. DEBUG에도 `LOG_AND_DROP`에도 아무것도 남지 않는다. 0.18.0 후보 우선순위 0 |
| FB-030 | `zlink-java` raw request가 미완료 요청 둘 이상에서 회신을 잃는다. 재현은 `bench/with-grpc/repro/` |
| FB-031 | 네 스택이 깊이에서 각각 다르게 무너진다. C는 깊이 90.7을 유지하고, .NET은 제출 비용에 묶여 8, Node는 8 위에서 멈추며, Java는 16 위에서 회신을 잃는다 |
| `zlink-framework-java`의 request-window 깊이 4.5 | 설정 window 100에 대해 `peak_in_flight` 10~11, abandoned 0 |
| framework send 경로의 수신 지연 | @1024에서 평균 2.3초. FB-009가 .NET에서 기록한 것과 같은 성격 |
| `zlink-c` 재현성 | request-window @4096이 세 번째 측정 구간에서도 G5 미달(28.7%) |
