# Node DD·Java SENDSEND 수신 대기 조사

## 결과와 판정 범위

요청한 성능 목표를 달성하지 못했다. Java SENDSEND의 admission 전용 대기 결함은
회귀 테스트로 확인했으나, 수신·completion 통합 poller 후보는 DR 처리량이 기존 대비
10.6–21.3% 감소하고 latency도 대부분 악화됐다. **후보를 기각하고 코드와 실행 산출물을
원복했다. 최종 적용한 러너 수정은 없다.** 원래 초 단위 latency의 주원인은 미확정이다.

후보 diff와 테스트는 검토용
[patch](../../../../../.artifacts/perf/client-recv-cadence/java-candidate-rejected.patch)로
보존했다. 늦게 도착한 수신 메시지가 admission 완료 전에 처리되는 회귀 테스트는
원본에서 실패, 후보에서 통과했으며 이 기능상의 발견을 성능 개선과 구분한다.

Node DD는 변경 없이 1-run 재현에서 초 단위 지연이 다시 발생했다. 그러나 DD **client는
송신자**다. 실제 수신·측정은 DD server가 수행하며, 수신 건수 제한과 turn timestamp
재사용은 없다. 따라서 요청에서 가정한 “Node DD client 수신 cadence 결함”은 확인되지
않았다. Node client를 수정해 송신 부하를 낮추는 방법은 적용하지 않았다.

- 소유 계층: binding perf 러너의 submit/poll/recv 순서. completion 소비·재제출은 binding이 소유한다.
- 근거: `doc/perf/PERF_MULTI_TEST_POLICY.md:223`의 poller 대기 규칙,
  `bindings/doc/spec/async-execution-model.ko.md:62`의 단일 completion owner와 public poller 이전 계약.
- 교차언어: C·Go·Rust가 수신과 completion을 같은 poller에 등록하는 구조를 확인했다.
- 분류: Java 대기 결함은 B(기존 결함)로 확인했으나 수정 후보는 성능 기준 미달로 기각했다. Node 지연 원인은 미확정이다.
- 후보의 수정 전/후 규칙 수: Java active 대기 경로 2개(`POLLIN` poll + admission park) → 1개(수신·completion 통합 poll). 원복 후에는 기존 2개다.

## 정확한 경로와 C 대조

### Node DD

`bindings/node/perf/multi/perf_multi_dealer_dealer_client.ts:41`의
`runDealerDealerSendRounds`는 송신 전용이다. round마다 각 available socket에 한 번
제출하고 `:99`에서 `setImmediate` 기반 `yieldTurn`을 호출한다. pending 상태는
이전 **send admission**만 나타내며 echo나 application in-flight window가 아니다.
수신 poller, `recv`, latency 집계는 이 파일에 없다.

실제 수신은 `bindings/node/perf/multi/perf_multi_dealer_dealer_server.ts:103`이다.
Linux에서는 `waitPollerOne(..., -1)` 한 번 뒤 `:108`의 inner loop에서
`recv(Received, DontWait)`가 false가 될 때까지 drain한다. 건수 상한, `wait(0)` 반복,
drain 사이의 event-loop yield는 없다. C DD도
`bindings/c/perf/multi/src/perf_multi_dealer_dealer_server.cpp:222`에서 poll한 뒤
`receive_one_message`와 `drain_non_blocking_messages`를 수행한다.
질문에 제시된 C client helper `:1048`의 순수 수신 loop도 같은 구조다.

따라서 다음 두 차이는 추가 원인 후보이지, client cadence 수정의 근거가 아니다.

1. Node public `recv`는 native multipart를 읽고 part별 JS Buffer와 envelope를
   만든다(`bindings/node/native/src/addon_core.cc:659`,
   `bindings/node/native/src/addon_message_values.h:239`,
   `bindings/node/src/zlink/runtime/messaging/message_materializer.ts:195`).
   C DD는 part의 header를 읽고 즉시 닫는다(C DD server `:118`). 이 materialization은
   binding 라이브러리 소유이며 이번 수정 금지 범위다. 비용 차이는 확인했지만
   profiler로 지연 기여도를 분리한 것은 아니다.
2. Node DD server는 `:66`에서 연결 전 auto-HWM 재계산을 하고, ready barrier 뒤에는
   재계산하지 않는다. C DD server는 START 뒤 `:492`에서 재계산한다.
   정책 `PERF_MULTI_TEST_POLICY.md:356`은 연결 수를 사용하는 패턴의 연결 준비 후
   재계산을 요구한다. 이 차이가 실제 0.17.2 queue 계획을 바꿨는지는 미확정이다.
   HWM과 Node server 수정 제한을 지켜 변경하지 않았다.

HWM을 전체 메시지 queue의 단일 4,096,000B 상한으로 볼 수는 없다.
고정 Core tag `core/v0.17.2`의 `core/src/runtime/core/auto_hwm_policy.cpp:42`는
balanced context budget의 상한을 512MiB, data queue별 상한을 1MiB로 정한다.
같은 tag의 `core/src/runtime/core/pipe.cpp:100`은 연결마다 pipepair를 만든다.
기존 Node 보고서의 실제 HWM도 1,048,576B다. 100개 연결의 송신·수신 queue와 OS buffer가
존재하므로 “약 73만 건이 쌓일 수 없다”는 이유로 backlog를 배제할 수 없다.
73만 건의 64B payload만 약 47MB이며, 정확한 점유량에는 frame overhead도 필요하다.

### Java DR·RR SENDSEND

두 client는 `PerfMultiTargetCoordinator.run`을 공유한다.
정의 파일은 이름이 다른
`bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiRoutedSendCoordinator.java`다.

수정 전 해당 파일 `:60`에서 submit round, `:68`에서 `poll(0)` 한 번,
`:76`에서 ready socket drain을 수행했다. 이어서 `:79`의
`!submitted && !drainedReply` 조건이면 `awaitAvailability(activeEnd)`로 들어갔다.
이 메서드는 `:269`의 `LockSupport.parkNanos`에서 admission 완료의 `unpark`만 기다렸다.
이후의 `POLLIN`은 이 대기를 깨우지 못했다. 이는 drain depth 제한이 아니라
**빈 poll 직후 선택한 대기의 wake 조건 결함**이다.

DR `PerfMultiDealerRouter.java:159`과 RR `PerfMultiRouterRouter.java:146`의
`drainReplies`는 이미 recv가 비거나 active deadline에 도달할 때까지 처리한다.
한 건만 수신하거나 turn당 고정 건수로 끊는 구현이 아니다.

C의 실제 SENDSEND 기준은
`bindings/c/perf/multi/common/perf_multi_client_helpers.hpp:1215`의 `run_echo_phase`다.
각 socket에 한 번 제출하고 `:1339`에서 제출이 있으면 timeout 0, 없으면 active
잔여시간으로 한 번 기다린다. 같은 tracker에 `POLLIN|POLLCOMPLETION`을 등록하며,
raw WRITABLE retry 상태에 따라 `POLLOUT`도 사용한다(`:86`). 수신 이벤트는 같은
turn에서 DONTWAIT drain한다. binding perf는 raw WRITABLE 상태를 복제하지 않는다.

기각한 후보의 coordinator `:59`는 기존 poller에 `POLLIN|POLLCOMPLETION`을 등록한다.
`:77`에서 C와 같은 timeout 선택 후 한 번 wait하고 `:86`에서 바로 drain한다.
active 종료 뒤 `:92`에서 completion bit를 제거해 ownership을 binding으로 되돌린
다음 기존 bounded admission drain을 await한다. public poller가 owner인 상태에서
wait를 중단하고 terminal만 기다리는 종료 교착을 만들지 않는다.

`wait(0)` 한 번 뒤 cooperative yield라는 정책 조항(`:270`)은 completion-only
event-loop request/reply alignment를 설명한다. Node DD의 순수 receiver나 Java의
SENDSEND loop를 매 건 yield하는 근거로 확대하지 않았다.

### 특정 패턴에서만 나타난다는 관측의 해석

Java SENDSEND는 같은 client thread에서 submit과 echo 수신을 진행하므로 admission만
기다리는 경로가 수신도 중단한다. Java DD는 송신 client와 수신 server가 별도 프로세스라
client의 admission 대기가 receiver의 진행을 직접 중단하지 않는다. PUBSUB receiver에도
이 admission 전용 대기가 없다. 따라서 확인한 Java 결함의 적용 범위는 SENDSEND다.
다만 후보 실측에서 latency가 개선되지 않아, 이것만으로 기존 지연 전체를 설명할 수 없다.

Node DD와 PUBSUB receiver는 모두 동기 poll 뒤 DONTWAIT drain을 한다.
두 언어에 공통인 “drain N건 제한”이나 “잘못된 yield 위치”는 발견하지 않았다.
PUBSUB에서 C도 깊은 fan-out queue를 갖는다는 관측은 Node DD가 C보다 느리게
수신할 때 backlog가 더 생길 수 있다는 가설을 배제하지 않는다. 이는 가능한 설명이며
실제 queue 점유와 역할별 처리율을 계측한 확정 원인은 아니다.

## Timestamp와 측정 anchor

| 경로 | 송신 stamp | 수신 clock과 집계 | 확인 결과 |
|---|---|---|---|
| Node DD | client `:86`에서 각 신규 record 제출 직전 `stampPayload`; `perf_measurement.ts:43`에서 `hrtime.bigint()` | server `:124`에서 매 수신 payload마다 clock을 읽어 `recordPayload`; collector `perf_measurement.ts:344`에서 header 검증 후 count와 latency 증가 | turn timestamp 공유 없음. send retry는 최초 stamp를 보존한다. |
| Java SENDSEND | DR `:147`, RR `:176`에서 payload 생성 때 `System.nanoTime()` | DR `:168`, RR `:162`에서 각 recv 직후 clock을 읽고 active boundary를 검사한 뒤 `recordActiveLatency` | poll wake timestamp를 사용하지 않는다. 수정에서 이 위치를 변경하지 않았다. |
| C DD | 신규 retained payload 생성 시 stamp | DD server `:148`에서 header 일치와 valid recv count 판정 뒤 clock을 읽음 | 메시지별 실제 수신 처리 시점이다. |
| C SENDSEND | helper `:1294`에서 신규 send record stamp | helper `:1246`에서 header·active 판정, `:1252`에서 count, `:1254`에서 clock과 RTT/2 표본 | poll 반환 시간이나 turn 공통 시간을 사용하지 않는다. |

정밀한 명령 순서까지 같다는 결론은 내리지 않는다. Java는 header 검증 **전**에 recv
clock을 읽고 C는 검증 **후**에 읽는다. Java의 이 차이는 검증 비용만큼 latency를
작게 하는 방향이며 수백 ms 증가의 근거가 아니다. 후보 diff도 stamp, valid recv,
count, latency sample, phase deadline을 변경하지 않았다.

Node DD server의 collector 생성(`:85`)에는 `activeStopNs`가 전달되지 않는다.
collector 기본 상한은 무한대(`perf_measurement.ts:296`)이고 inner drain에도 deadline
검사가 없어, 주석과 달리 drain 도중의 종료 시점은 collector가 보장하지 않는다.
C DD도 inner drain의 매 메시지 deadline 검사가 없다. C 기준과 정책 §4.2의
active 밖 제외를 함께 정리해야 하므로, latency를 낮추려고 Node anchor만 변경하지 않았다.

## 기각한 후보와 회귀 테스트

- `bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiRoutedSendCoordinator.java`: 위 통합 대기.
- 같은 디렉터리의 `PerfMultiRouterRouter.java`: poller 역할을 설명하는 기존 주석 갱신.
- `bindings/java/perf/multi/Zlink.BindingBench.Multi/src/test/java/systems/zlink/perf/multi/PerfMultiRoutedSendCoordinatorTest.java`: admission pending 중 지연 도착한 수신 이벤트의 wake 테스트.

테스트 peer의 50ms 예약 송신은 빈 poll 뒤 메시지 도착을 만드는 fixture다. perf 러너에
sleep·timer·새 in-flight 상한을 추가하지 않았다.

| 검증 | 결과 |
|---|---|
| 수정 전 coordinator + 새 테스트, 별도 `/tmp` Gradle build | 예상 실패: `POLLIN must wake ... expected 0 but was 1`; 수신 callback 미실행 |
| 수정 후 `PerfMultiTargetCoordinatorTest` | 5 tests, failures 0, errors 0 |
| Java `:perf-multi:installDist` | 성공, binding compile/resource/jar는 UP-TO-DATE |
| 후보 원복 뒤 `:perf-multi:installDist` | 성공. 원래 coordinator가 실행되는 산출물로 복원 |
| `git diff --check` | 통과 |

## 성능 검증

고정 환경은 `ZLINK_CORE_SOURCE=release`,
`ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.2`다.
각 실행 전에 공용 `wait-for-idle-perf.sh`를 호출하고 load ≤ 5를 확인한다.
실행 전에 lock 파일 permission 오류로 끝난 시도는 측정 run에 포함하지 않는다.
신규 측정은 모두 1-run, duration 5, clients 100, tcp, part-count 2다.
크기는 기존 비교표의 64/256/1024/4096/65536B를 유지했다.
HWM·timeout·duration·client 수·크기·Core·binding 라이브러리는 변경하지 않았다.

### Node DD

아래 “현행 재현”은 **수정 후 결과가 아니다**. Node 파일은 변경하지 않았다.
기존 before는 사용자가 지정한 r4node 5-run 집계이며 신규 1-run과 구분한다.

| B | C mean ms | 기존 Node mean ms | 현행 재현 mean ms | 기존 msg/s | 현행 msg/s | 현행/C |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 0.081698 | 1266.988267 | 1341.499853 | 578027.4 | 559272.4 | 16420.23x |
| 256 | 1.212618 | 1195.481651 | 1210.119106 | 548321.8 | 490420.0 | 997.94x |
| 1024 | 0.641459 | 1546.064414 | 1647.120524 | 502465.0 | 481773.6 | 2567.77x |
| 4096 | 626.645120 | 1665.420380 | 1808.266615 | 334058.6 | 319236.2 | 2.89x |
| 65536 | 7.436366 | 319.812107 | 184.985703 | 81792.4 | 72822.4 | 24.88x |

신규 재현은 `complete`, 5/5 성공이다. 모든 크기의 latency/C ≤ 2 목표는 미달이다.
코드 변경이 없어 처리량 변화는 이번 수정의 효과로 해석하지 않는다.

원본:

- Node before: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260908_092340_r4node.txt`
- C 기준: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_090843_r4node.txt`
- 신규 Node: `/tmp/zlink-recv-cadence-node-before/multi/report/perf_node_multi_linux_20260908_110022_recv_cadence_before.txt`

### Java SENDSEND 후보 1-run

| 패턴 | B | C mean ms | 기존 Java ms | 후보 ms | 기존 ops/s | 후보 ops/s | 처리량 변화 | 후보/C |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| DR | 64 | 0.195324 | 1.749556 | 1.084205 | 412049.6 | 364032.8 | -11.7% | 5.55x |
| DR | 256 | 0.209715 | 3.142684 | 13.758976 | 385708.4 | 317334.2 | -17.7% | 65.61x |
| DR | 1024 | 0.206314 | 73.886393 | 210.198078 | 362516.0 | 285389.8 | -21.3% | 1018.83x |
| DR | 4096 | 0.291016 | 516.572574 | 570.943824 | 274375.4 | 243778.6 | -11.2% | 1961.90x |
| DR | 65536 | 10.795064 | 44.754594 | 54.133967 | 49236.0 | 44031.2 | -10.6% | 5.01x |
| RR | 64 | 0.196536 | — | 0.579224 | — | 425469.6 | — | 2.95x |
| RR | 256 | 0.192868 | — | 0.579293 | — | 432378.6 | — | 3.00x |
| RR | 1024 | 0.214303 | — | 0.634194 | — | 425285.8 | — | 2.96x |
| RR | 4096 | 0.298219 | — | FAIL | — | — | — | — |
| RR | 65536 | 9.546122 | — | 미실행 | — | — | — | — |

DR은 전 크기에서 latency/C ≤ 2와 처리량 감소 5% 이내를 모두 만족하지 못했다.
RR의 완료된 세 크기도 latency/C > 2다. RR의 같은 조건 before 전 크기 자료는 확보하지 못해
처리량 유지 판정을 하지 않았다. RR 4096B에서는 server 로그에 `multi_routed_relay_failed`,
client 로그에 `multi_router_router_async_sends_timed_out`가 남았다.
client 오류는 `PerfMultiAsyncSendLoop.java:51`의 bounded admission drain에서 발생했다.
server는 `PerfMultiRoutedRelay.java:47`에서 전달한 오류이며, 기존 `PerfMain.java:28`과
`PerfPolicy.java:33`의 FAIL 문자열 변환만 저장돼 nested 예외의 타입과 stack은 알 수 없다.
따라서 이를 특정 Core·binding 결함으로 단정하지 않는다.

runner는 종료된 server FIFO에 STOP을 쓰는 `run_benchmarks.sh:1198`에서 Broken pipe로
끝났다. 이를 숨기거나 fixture 조건을 완화하지 않았다. RR 65536B는 실행되지 않았고
전체 결과는 complete가 아니다. 기준에 실패한 후보를 원복했으므로 후보의 나머지
cell을 반복 실행해 성능 판정을 바꾸지 않았다.

신규 측정의 시작 load는 4.42였다. 공용 gate의 wrapper 오탐으로 앞선 대기 시도들은
측정 전에 중단됐고, 공용 스크립트 수정은 다른 job이 수행했다. 이번 job은 이를 수정하지 않았다.

원본 및 증거:

- Java DR before: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260908_102147_r3java.txt`
- C DR: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_100243_r3java.txt`
- C RR: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_103348_r3java.txt`
- 후보 stdout 및 cell별 로그: `.artifacts/perf/client-recv-cadence/java-after.log`, `java-after/multi/tmp/`
- 원본/후보 회귀 테스트: `.artifacts/perf/client-recv-cadence/java-original-test.xml`, `java-candidate-test.xml`

### 원복 후 smoke

| 언어 | tcp 64B 패턴 | 성공 | 상태 |
|---|---|---:|---|
| Node | PUBSUB, DR REQREP, RR REQREP | 3/3 | complete, fail 0, RESULT 15/15 |
| Java | DD, PUBSUB | 2/2 | complete, fail 0, RESULT 10/10 |

Node는 11:27:18–11:27:44, Java는 11:27:44–11:28:00에 실행돼 겹치지 않았다.
실행 직전 load는 각각 4.12, 3.42였다. 상세 로그와 결과 보고서는
`.artifacts/perf/client-recv-cadence/node-smoke/`, `java-smoke/`에 보존했다.
소스와 Java 산출물을 원복한 뒤의 상태 확인이며, 기각한 후보의 회귀 통과로 해석하지 않는다.

## 다른 네 언어 대조

조사는 읽기 전용으로 위임했고 아래 수신·대기 경로는 감독 agent가 다시 열어 확인했다.
.NET·Go 파일은 다른 job이 동시에 수정 중이므로 현재 읽은 상태와 최초 상태를 혼동하지 않는다.

| 언어 | SENDSEND poll·drain | timestamp | 이번 조사 판정 |
|---|---|---|---|
| .NET | DR client `:115`의 현재 mask는 `PollIn|PollOut|PollCompletion`; recv는 `:323`에서 빌 때까지 처리 | 각 recv 직후 `Stopwatch.GetTimestamp` | 최초 조사 때 POLLIN-only였으나 다른 job이 mask를 갱신했다. 이번 작업에서는 수정하지 않았다. |
| Go | `perf_multi_dealer_router.go:103`에서 POLLIN·completion, `perf_multi_main.go:109`에서 turn마다 submit/completion 채널 drain, 같은 turn에 receive progress | `perf/internal/perfcommon/runtime.go:42`에서 메시지별 clock을 header 검증 전에 읽음 | Java의 admission-only park 구조와 다르다. |
| Rust | `perf_multi_dealer_router_client.rs:79`에서 POLLIN·completion, `:111`에서 Future ready 수거, `:135` wait 뒤 drain | 각 valid payload의 monotonic clock | 고정 recv 건수 제한 없음. |
| Python | `perf_multi_dealer_router_client.py:77`의 `poll(0)` 한 번 뒤 모든 ready socket을 drain, `:138` loop 끝에서 zero-delay yield | `perf_metrics.py:114`에서 valid header 뒤 monotonic clock | 고정 recv 건수 제한 없음. send task와 recv loop가 같은 event loop에서 진행한다. |

위임 결과 중 Go의 clock이 header 검증 뒤라는 설명은 재확인한 `runtime.go:43`과 달라
채택하지 않았다. .NET의 POLLIN-only 발견도 다른 job의 후속 변경 후 현재 상태에는
적용하지 않았다.

DD에서도 네 언어 모두 수신은 server가 담당하며, 고정 건수 drain 제한이나 turn 공통
recv timestamp는 발견하지 않았다. 종료 모델에는 별도 차이가 있다: Go·Rust의 DD는
처음부터 remaining deadline wait, .NET DD는 public timer와 `Wait(-1)`, Python DD는
stop 이전 `-1`/이후 remaining wait다. 이 차이를 이번 Java 대기 수정에 섞지 않았다.

## 남은 작업

- Node 전 크기와 Java 전 크기의 latency/C ≤ 2 및 처리량 유지 목표는 달성하지 못했다. Java 후보는 기각으로 확정했다.
- Node의 주요 지연 원인을 recv materialization 비용, 실제 queue 점유, active drain 경계로
  분리해야 한다. client 수신 루프가 있다는 전제 아래 client를 변경하지 않는다.
- Node server까지의 범위 확대는 질문했으나 아직 승인받지 않았다. binding 라이브러리 변경은
  요청에서 명시적으로 금지되어 있다.

최종 변경 파일은 이 조사 기록뿐이다. 검토용 patch와 원본 로그는 `.artifacts/perf/client-recv-cadence/`에 보존했다. commit/push, 정책·스펙·계획서·decisions 변경은 하지 않았다.
