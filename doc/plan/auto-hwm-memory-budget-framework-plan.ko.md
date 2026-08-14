# Framework Auto HWM 메모리 예산 적용 계획

## 문서 상태

- 상태: 구현 전 계획
- 대상: Framework application/runtime 설정과 성능 튜닝
- 공통 정책: [Auto HWM 메모리 예산 및 CCU 재계산 계획](auto-hwm-memory-budget-and-ccu-recalculation-plan.ko.md)
- Core 구현: [Core 적용 계획](auto-hwm-memory-budget-core-plan.ko.md)
- Binding 연동: [Bindings 적용 계획](auto-hwm-memory-budget-bindings-plan.ko.md)

## 1. 목적

Framework는 애플리케이션 설정과 runtime 메모리 정보를 binding context에 전달한다.
Framework가 profile 비율을 계산하거나 별도 application HWM 예산을 만들지 않는다.

Core-managed messaging 예산은 항상 하나다.
Framework는 binding 공개 API를 호출하고, binding은 같은 값을 Core context 옵션으로 전달한다.
Core에서 받은 message의 budget lease를 job 완료까지 보유해 application 적체도 같은 예산에 포함한다.

## 2. 현재 구현

현재 Framework에는 애플리케이션 queue용 HWM 계산기가 있다.
언어별 runtime 또는 OS 메모리를 확인하고 Compact 2%, LowLatency 5%, Balanced 10%, Throughput 20%를 적용한다.

이 값은 Framework가 소유한 application queue의 별도 상한으로 사용되며 현재 Core에는 profile만
전달된다. 이 현재 구현은 목표 단일 budget 모델과 다르므로 resolver, 별도 limit과 pause counter를
제거하고 Core retained-credit lease로 교체한다.

현재 C++ mesh runtime은 Router completion-control API와 handler를 실제 사용한다. Connection-ready의
hello·admission, liveness probe/ack, relocation과 reply relay를 completion lane으로 보낸다. 목표
정책에서는 이 의존성을 제거한다. 같은 command를 기존 Router routed send/receive로 옮겨 application
lane의 connection별 HWM과 backpressure를 적용하고, completion lane에는 terminal reply와 error reply만
남긴다.

현재 비동기 최초 수용도 언어별로 같지 않다. .NET과 Node.js의 일부 request 경로는 비동기 반환 전에
native submit에서 기다릴 수 있고 Java는 Framework의 10ms polling으로 이를 피한다. Framework에 RID별
Core scheduler를 다시 구현하지 않고 [Bindings의 비동기 수용 계약과 구현 gap](auto-hwm-memory-budget-bindings-plan.ko.md#93-현재-언어별-구현-gap)을
해결한 뒤 모든 backend가 같은 binding API를 사용하게 한다.

근거:

- [C++ Framework Core HWM 설정 전달](../../framework/languages/cpp/framework/src/runtime/host/app.cpp)
- [.NET Framework Core HWM 설정 전달](../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeStateFactory.cs)
- [Java Framework Core HWM 설정 전달](../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaContext.java)
- [Node.js Framework Core HWM 설정 전달](../../framework/languages/node/packages/framework/src/runtime/backend/node/node-backend-adapter-factory.ts)
- [.NET binding backend context](../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/Adapters/ZLinkDotNetBackendRuntimeContext.cs)
- [C++ mesh completion-control 실행 경로](../../framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp)
- [C++ Router binding bridge](../../framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp)

## 3. 단일 설정 이름

Framework public API와 binding은 Core가 소유한 같은 설정 이름을 사용한다.

| 설정 | 대상 | 의미 |
|---|---|---|
| CoreHwmMemoryLimitBytes | Core | Core budget 계산에 사용할 가용 메모리 |
| CoreHwmBudgetBytes | Core | 계산을 건너뛰는 수동 Core-managed messaging 예산 |
| CoreHwmProfile | Core | Core 예산 비율과 per-queue bounds |

기존 ApplicationHwmBytes와 ApplicationHwmProfile은 제거한다. Alias, 자동 복사와 deprecated property를
제공하지 않으며 이전 이름은 configuration validation에서 unknown option으로 거절한다.

## 4. 전달 흐름

Framework 설정은 다음 한 경로로 전달한다.

~~~text
application configuration
    → Framework runtime builder
    → binding public context option
    → Core context
    → Core budget planner and admission
~~~

Framework가 C++ binding을 사용하든 managed binding을 사용하든 결과는 같은 Core context 상태여야 한다.

초기화 순서는 다음과 같다.

1. Framework 설정을 읽고 단위와 범위를 검증한다.
2. CoreHwmBudgetBytes가 있으면 binding의 manual Core budget setter를 호출한다.
3. 그렇지 않고 CoreHwmMemoryLimitBytes가 있으면 memory limit setter를 호출한다.
4. 둘 다 없으면 binding이 managed runtime hint를 제공하도록 둔다.
5. CoreHwmProfile을 설정한다.
6. context 설정을 마친 뒤 socket과 runtime owner를 생성한다.
7. Core snapshot을 조회해 effective 설정을 Framework 진단 정보에 노출한다.

Framework는 프로필 비율을 Core 값에 미리 적용하지 않는다.
CoreHwmMemoryLimitBytes는 원본 메모리 한도이고 CoreHwmBudgetBytes만 계산 완료 값이다.

## 5. 메모리 한도 선택

### 5.1 명시 설정

운영 환경에서 container 또는 VM 한도가 명확하면 CoreHwmMemoryLimitBytes를 설정하는 방식을 권장한다.
성능 시험에서 확정한 Core-managed messaging 예산을 고정하려면 CoreHwmBudgetBytes를 사용한다.

manual Core budget과 memory limit을 동시에 지정하면 manual Core budget이 우선한다.
Framework는 충돌을 경고하고 effective source를 진단 정보에 기록한다.
Core가 감지한 finite container/process hard limit보다 explicit memory 또는 manual Core budget이 크면
Core는 EINVAL을 반환한다. Framework는 시작 실패로 전달하고 값을 조용히 clamp하지 않는다.

### 5.2 managed runtime

명시 값이 없으면 Framework는 별도 예산을 계산하지 않는다.
binding이 .NET GC, JVM, V8 또는 Go runtime의 메모리 한도를 runtime hint로 Core에 전달하도록 한다.

Framework가 runtime hint를 이미 갖고 있고 binding API로 직접 전달해야 하는 구조라면 원본 byte 값만 전달한다.
프로필 비율 적용은 Core에서 한 번만 수행한다.

### 5.3 native runtime

C++ 또는 native runtime에서 명시 값이 없으면 Core가 container, process, OS 가용 한도를 확인한다.
Framework가 물리 메모리를 다시 조회해 Core로 전달할 필요가 없다.

## 6. 실행 중 연결 변화

Framework가 현재 CCU를 계산하여 Core에 전달하지 않는다.
Core가 실제 attach된 directional physical queue 수를 기준으로 재계산한다.

Framework는 연결 이벤트마다 budget setter를 다시 호출하지 않는다.
대신 snapshot의 다음 값을 관찰한다.

- active send/receive queue count
- budgetGeneration
- total planned/applied HWM bytes
- current/peak accounted bytes
- completion current/peak/pending, total messaging accounted bytes와 completion queue count
- blocked ratio
- budget insufficient와 oversize admission 계측

논리 사용자 수와 physical queue 수가 다를 수 있으므로 Framework의 CCU 지표와 Core queue 지표를 나란히 노출한다.

### 6.1 Application job credit

Framework backend는 일반 recv 대신 retained-credit recv를 사용한다. Core queue에서 빠진 message의
accounted charge는 반환되지 않고 move-only budget lease로 Framework에 전달된다.

~~~text
Core queue owner
    → Framework queued job owner
    → active handler owner
    → completion/drop/cancel
    → origin pipe credit release
~~~

Queue와 executor 사이에서는 lease 소유권만 이동한다. Payload 복사, handler 시작과 thread 변경은
새 budget을 만들지 않는다. 정상 완료, validation drop, routing 실패, cancel, exception과 shutdown
경로는 lease를 정확히 한 번 release한다.

Job 완료는 awaited handler가 끝나고 필요한 reply가 Core completion pipe에
수용된 뒤다. Request lease는 그 시점까지 origin inbound queue HWM을 점유한다. Completion pipe에는
HWM과 Core budget reservation을 적용하지 않으므로 inbound application lease가 budget을 채워도
request/reply 진행을 막지 않는다.

Oversize request도 일반 request와 같은 lease를 사용한다. Context 전체의 debt owner가 없으므로 별도의
lease-to-send exchange를 호출하지 않는다. Request lease는 inbound pipe를 점유하고, reply와 error
reply는 크기나 completion pending byte와 관계없이 HWM 없는 completion pipe에 수용된다.

Handler가 payload나 message wrapper를 job 밖의 비동기 작업, cache 또는 사용자 collection으로 넘기면
lease도 같은 owner에게 함께 이전해야 한다. Lease 없이 payload만 escape하면 해당 메모리는 Core HWM
회계 범위 밖이다. GC finalizer와 destructor는 누락 방지 fallback이며 정상 흐름의 credit 반환 시점으로
사용하지 않는다.

특정 origin pipe의 credit이 소진되면 Core가 그 pipe의 transport ingress admission을 중단한다. 이미
Core queue에서 accounting된 message를 job으로 옮기는 retained receive는 합계를 늘리지 않으므로 계속
진행한다. Framework가 별도 ApplicationHwmBytes를 비교해 context 전체 dequeue를 멈추지 않는다. Job
완료로 lease가 반환되면 Core가 해당 origin의 transport recv와 blocked sender를 깨우고 TCP
backpressure가 해제된다.

여러 connection A·B·C·D를 가진 socket에서 A의 빈 pipe가 HWM보다 큰 message 한 건을 수용하면,
Framework가 그 message를 처리하는 동안 A의 후속 ingress만 pending될 수 있다. B·C·D는 각 origin
pipe의 HWM까지 계속 수신하고 job을 만들 수 있다. Framework는 A의 큰 message 또는 context aggregate
사용량이 `CoreHwmBudgetBytes`를 넘었다는 이유로 B·C·D의 recv를 함께 pause하지 않는다.

큰 message의 크기와 동시에 예외를 사용하는 connection 수를 Framework 설정으로 제한하지 않는다.
Framework는 `CoreHwmBudgetBytes`에서 `MAXMSGSIZE`를 계산하지 않으며, 사용자가 Core의 기존
`MAXMSGSIZE`를 별도로 설정하지 않았다면 unlimited 의미를 그대로 유지한다.

### 6.2 Framework control packet 이동

Framework는 별도 completion-control handler와 pending completion-control queue를 제거한다. Hello,
admit/update/reject, liveness probe/ack, relocation과 reply relay command는 기존 Router routed send로
보내고 일반 receive/pump에서 header를 해석한다. 기존 command allowlist, 단일 record의 part 수와 payload
크기 검증은 그대로 유지한다.

일반 receive/pump는 Framework control header를 먼저 식별해 내부 protocol handler에서 소비한다. Control
record를 사용자 message handler나 application job queue로 전달하지 않으며, 처리가 끝나면 해당 origin의
retained-credit lease를 즉시 반환한다. 즉 물리 전송과 backpressure는 application lane을 공유하지만 사용자
업무 message로 오인하거나 job 수명을 불필요하게 늘리지 않는다.

Application lane backpressure가 control packet에도 적용된다. 모든 control command는 send·request와 같은
binding 비동기 전송 API와 최초 deadline, peer·route lifecycle을 적용한다. Command 종류를 이유로
우선순위, 우회 queue, update 병합, liveness marker 또는 별도 pending queue를 두지 않는다.
각 operation의 기존 deadline이나 heartbeat timeout이 만료되면 그 결과를 기존 protocol state machine에
전달해 candidate 종료, peer 제거 또는 request·relocation 실패를 결정한다.

Control packet을 application lane으로 옮겼다는 이유로 heartbeat timeout을 연장하거나 backpressure를
우회하지 않는다. 짧은 HWM 정체는 정상 drain으로 풀리고, heartbeat timeout까지 input과 output이 진행되지
않는 peer는 메시징을 수행할 수 없는 상태로 판단한다. 일반 application packet과 control packet 사이에
Core 우선순위 queue를 추가하지 않는다.

### 6.3 Binding 비동기 전송 사용

Framework는 send, request와 control마다 binding의 비동기 전송 API를 호출하고 그 operation을 await한다.
Target pipe가 HWM에 도달하면 해당 operation의 coroutine, Task, CompletableFuture 또는 Promise만 대기한다.
A가 대기하는 동안 같은 socket의 B·C·D operation은 계속 제출되고 진행할 수 있어야 한다.

Framework는 이 동작을 위해 `pendingByTarget`, RID별 deque, ready ring이나 고정 주기 retry scheduler를
구현하지 않는다. Backpressure 감지와 전송 가능 시점의 재개는 binding 계약이다. Framework가 소유하는
것은 기존 operation deadline, cancellation, peer·route lifecycle과 결과 전달뿐이다.

Framework는 Core의 socket-wide 또는 routed target-ready callback을 직접 등록하거나 A의 pipe 상태를
polling하지 않는다. Binding은 Core가 전달한 RID와 transport pair generation으로 해당 operation만
재개한다. A가 막힌 동안 B가 writable이어도 B event가 A를 깨우지 않는다. Pipe detach, socket close와
context 종료는 binding의 terminal 결과로 Framework에 정확히 한 번 전달된다.

비동기 API가 반환 객체를 만들기 전에 native blocking submit을 실행하면 같은 runtime thread를 사용하는
다른 operation까지 지연되므로 허용하지 않는다. Binding이 기다리는 동안 Framework socket owner mutex를
보유해서도 안 된다. Send operation은 Core가 message를 수용하면 완료하고 request operation은 같은 수용
대기 뒤 기존 reply 결과까지 기다린다. Completion reply와 error reply는 HWM 없는 completion lane을
사용하므로 application-lane 수용 대기를 거치지 않는다.

Request의 reply correlation 준비와 wire publish 순서는 Core와 binding이 보장한다. Framework는 provisional
callback state나 재시도 상태기를 만들지 않고 binding operation의 단일 terminal 결과만 처리한다.

## 7. Framework 진단 모델

Framework의 instance diagnostics에는 Core snapshot을 중복 계산 없이 포함한다.
필드 이름은 Core 의미를 유지한다. Application은 별도 HWM namespace가 아니라 Core snapshot의 credit
owner breakdown으로 노출한다.

예:

~~~text
coreHwm.effectiveCoreBudgetBytes
coreHwm.totalAppliedHwmBytes
coreHwm.coreQueueAccountedBytes
coreHwm.applicationAccountedBytes
coreHwm.currentAccountedBytes
coreHwm.provisionalAccountedBytes
coreHwm.peakAccountedBytes
coreHwm.completionCurrentAccountedBytes
coreHwm.completionPeakAccountedBytes
coreHwm.completionPendingMessageCount
coreHwm.totalMessagingAccountedBytes
coreHwm.monitorQueueAppliedHwmBytes
coreHwm.monitorQueueAccountedBytes
coreHwm.totalInstanceAppliedHwmBytes
coreHwm.totalInstanceAccountedBytes
coreHwm.blockedRatioPpm
coreHwm.activeDirectionalQueueCount
coreHwm.activeCompletionDirectionalQueueCount
coreHwm.activeSendQueueCount
coreHwm.activeReceiveQueueCount
~~~

totalAppliedHwmBytes는 completion을 제외한 HWM-controlled physical queue 상한의 합이며 실제 사용량이 아니다.
튜닝과 운영 경보에는 currentAccountedBytes와 peakAccountedBytes를 함께 사용한다.
currentAccountedBytes는 completion을 제외한 coreQueueAccountedBytes와 applicationAccountedBytes의 합이다.
전체 messaging 현재 사용량은 `totalMessagingAccountedBytes = currentAccountedBytes +
completionCurrentAccountedBytes`다. Completion current·peak·pending은 진단 전용이며 Core budget이나
completion admission을 바꾸지 않는다. activeSendQueueCount와 activeReceiveQueueCount는 completion을
제외한 application 관점별 값이라 합산하지 않고, 중복 없는 실제 HWM 분모는 activeDirectionalQueueCount를
사용한다. Completion queue 수는 activeCompletionDirectionalQueueCount로 따로 노출한다.
outstandingApplicationLeaseCount, retiredQueueCount와 deferredOriginCreditBytes를 함께 노출해 job 누수와
detach 뒤 남은 credit을 진단한다.

`coreHwm.totalInstanceAccountedBytes`는 Core context가 관리하는 application, completion과 monitor queue
범위다. Binding 내부의 짧은 비동기 수용 대기 상태는 별도 Framework HWM queue나 public memory budget으로
계산하지 않는다. 정확한 process memory 상한 검증에는 같은 시점의 RSS·managed heap 계측을 함께 사용한다.

Framework가 monitor를 여는 설정은 `MonitorHwmBytes` 하나만 사용해 binding의 같은 byte option으로 그대로
전달한다. 이 값에 profile 비율이나 CoreHwmBudgetBytes를 적용하지 않는다. 0은 Core 기본값 선택이다.
운영 dump에는 byte 단위를 이름과 함께 표시하며 event count 환산값을 설정값처럼 출력하지 않는다. 기존
`MonitorHwm`, monitor event count 환경 변수와 설정 key는 호환 alias 없이 제거한다.

Metrics reset은 current·pending·queue count를 유지한다. Budgeted peak와 completion peak는 각각 reset
순간의 current로 재기준화하고 blocked/admission attempt, oversize count와 largest 같은 epoch 누적값만
0으로 초기화한다. Framework는 이 결과와 새 measurementEpoch을 그대로 노출한다.

## 8. 수동 Core 예산 튜닝

### 8.1 시험 절차

1. 실제 business logic, payload 분포, multipart 비율, 연결 churn을 포함한다.
2. 목표 시스템에서 CPU budget을 예를 들어 70%로 고정한다.
3. 충분히 큰 Auto HWM 예산에서 시작해 처리량과 p99 latency 기준선을 구한다.
4. Core snapshot의 peak accounted, blocked ratio, oversize admission count와 largest message를 기록한다.
5. CoreHwmBudgetBytes를 단계적으로 낮추며 같은 시험을 반복한다.
6. 목표 처리량과 latency를 만족하는 가장 작은 예산을 선택한다.
7. 장시간 시험과 장애 복구 구간을 거쳐 안전 여유를 확정한다.

### 8.2 후보 예산

현재 자동 분배 결과를 그대로 고정하려면 totalAppliedHwmBytes를 manual Core budget 후보로 사용할 수 있다.
그러나 최적화의 중심 값은 실제 사용량이어야 한다.

~~~text
minimumRequiredCoreBudgetBytes =
    manualReservations
    + sum(activeAutoQueueMinimumBytes)

candidateCoreBudgetBytes =
    max(
        p99OrPeakAccountedBytes × safetyFactor,
        minimumRequiredCoreBudgetBytes
    )

~~~

totalAppliedHwmBytes만 사용하면 workload가 실제로 쓰지 않는 queue 여유까지 포함할 수 있다.
peakAccountedBytes만 사용하면 짧은 burst와 allocator, codec, OS socket buffer를 놓칠 수 있다.
따라서 두 값을 함께 보고 RSS 안전 여유는 Core HWM 예산 밖에서 별도로 검증한다.
가장 큰 message envelope 전체를 Core budget에 중복 포함하거나 별도 allowance로 예약하지 않는다.
빈 pipe는 budget에서 계산된 HWM보다 큰 complete message 한 건을 그대로 수용한다. 이 예외 때문에
peakAccountedBytes가 candidateCoreBudgetBytes를 넘을 수 있으며, 그 초과를 설정 오류로 판단하지 않는다.

CoreHwmBudgetBytes는 Core가 pipe별 HWM을 계산하는 정상 상태의 message backlog 예산이다. 실제 backlog의
hard cap은 아니다. Handler가 생성한 object graph, 복사된
payload, cache, allocator overhead, managed heap overhead와 OS socket buffer는 포함하지 않는다. 성능
시험은 Core snapshot뿐 아니라 process RSS와 managed heap peak를 별도 기록해야 한다.

## 9. 설정 변경과 lifecycle

초기 버전은 context 생성 전 설정을 기본으로 한다.
실행 중 변경을 허용할 경우 Framework setter는 값 저장 성공과 실제 Core 재분배 완료를 구분해야 한다.

- setter 성공: Core가 새 설정을 수락함
- budgetGeneration 변경: 새 계획이 계산됨
- totalAppliedHwmBytes 변경: pipe 적용이 진행됨
- deferred shrink 존재: drain 전까지 일부 pipe에 이전 값이 남음

종료와 재시작 없이 변경하는 운영 도구는 generation을 기다리는 timeout과 상태 출력을 제공한다.

Framework instance는 하나의 Core budget authority를 사용해야 한다. 기본 구현은 instance당 Core context
하나를 공유한다. 특수 backend가 여러 Core context를 사용하면 CoreHwmBudgetBytes를 각 context에 그대로
복제하지 않고 하나의 shared budget coordinator를 사용하거나 명시적으로 분할한다.

## 10. Breaking change

- ApplicationHwmBytes와 ApplicationHwmProfile을 public API, builder, 환경 변수와 설정 파일에서 제거한다.
- 기존 이름의 alias, 자동 migration과 deprecation 기간을 두지 않는다.
- 기존 application HWM resolver와 독립적인 inbound dispatch limit을 제거한다.
- Router completion-control handler, pending queue와 binding 호출을 제거하며 호환 alias를 두지 않는다.
- Framework control packet은 기존 Router routed application API로 전송하고 내부 receive/pump에서 소비한다.
- Core 설정이 없으면 binding runtime hint 또는 Core fallback을 사용한다.
- 설정 dump에는 source가 manual budget, explicit memory, runtime hint, Core fallback 중 무엇인지 표시한다.
- 이전 이름을 사용한 configuration은 시작 단계에서 명확한 unknown-option 오류로 실패한다.
- MonitorHwm count 설정을 제거하고 MonitorHwmBytes만 binding으로 전달한다.
- 공유 ROUTER의 Framework send·request·control에서 blocking WAIT submit을 제거한다.

## 11. 구현 순서

1. 기존 ApplicationHwm configuration과 resolver 제거
2. 공통 Framework configuration schema에 CoreHwm 필드 추가
3. 언어별 builder와 환경 변수, 설정 파일 mapping 추가
4. Binding retained-credit recv와 job lease lifecycle 연결
5. Binding 비동기 send/request를 Framework backend에 연결하고 blocking submit과 Framework retry queue 제거
6. A RID의 대기가 B·C·D와 언어 runtime thread를 막지 않는지 언어별 통합 검증
7. Hello/admission/update/liveness/relocation/relay를 같은 binding 비동기 전송과 기존 Router routed
   send/receive·pump로 이동
8. 모든 command 이관 뒤 completion-control handler, pending queue와 binding 호출 제거
9. Heartbeat·operation timeout, disconnect와 shutdown의 공용 terminal 정리 구현
10. Payload escape와 async continuation의 lease ownership 연결
11. Completion을 포함한 Core snapshot과 reset operation을 관리 API에 노출
12. Generation을 기다리는 실행 중 변경 경로 추가
13. 성능 시험 도구에 owner별 accounting, completion 계측과 backpressure 기록 추가
14. Framework monitor 설정·운영 dump·perf 실행 인자를 byte 이름으로 통일

## 12. 검증 항목

- Framework 설정값이 binding을 거쳐 Core snapshot에 그대로 나타남
- Framework와 binding에 별도 Core budget이 생성되지 않음
- CoreHwmMemoryLimitBytes에 프로필 비율이 한 번만 적용됨
- CoreHwmBudgetBytes에는 프로필 비율이 적용되지 않음
- ApplicationHwm 이름과 별도 Framework budget이 존재하지 않음
- 명시 값이 없을 때 managed runtime hint 또는 Core fallback 사용
- Runtime hint와 Core가 감지한 finite hard limit 중 작은 값 사용
- Hard limit을 넘는 explicit/manual 입력의 시작 실패 전달
- 논리 CCU 변화가 아니라 실제 Core queue attach/detach로 HWM 재계산
- 실행 중 변경에서 generation과 deferred shrink 상태 노출
- CPU 70% 성능 시험에서 snapshot, throughput, p99 latency, RSS를 함께 기록
- Candidate budget에 manual reservation과 모든 auto queue minimum 합계 포함
- Completion lane이 candidate budget, totalAppliedHwmBytes와 HWM queue 분모에서 제외됨
- Oversize message가 candidate budget을 넘어도 empty-pipe 한 건 예외로 진행하는지 확인
- Core queue에서 Framework job으로 lease를 이동할 때 currentAccountedBytes 불변
- Retained job byte가 origin connection HWM을 계속 점유하는지 확인
- Handler reply가 HWM 없는 completion pipe에 수용된 뒤 request lease를 반환하는지 확인
- Oversize request는 inbound pipe HWM을 유지하고 reply는 completion HWM 없이 진행하는지 확인
- Router completion-control binding 호출, handler와 pending completion queue가 제거됨
- Hello/admission/update/liveness/relocation/relay가 기존 routed send/receive와 일반 pump를 사용함
- Connection-ready 직후 admission 전 후보 route로 hello를 송수신할 수 있음
- Framework control packet은 내부 protocol handler가 소비하며 사용자 handler나 job queue에 노출되지 않음
- Framework control 처리 뒤 origin retained-credit lease가 즉시 한 번만 반환됨
- 모든 control command가 send·request와 같은 binding 비동기 전송을 사용하고 별도 우선순위·우회 queue가 없는지 확인
- Admission 실패, liveness timeout 제거, relocation/request timeout이 기존 protocol lifecycle로 처리되는지 확인
- 장시간 application-lane backpressure가 heartbeat timeout에 도달하면 peer가 topology에서 제거됨
- Job 완료, drop, cancel, exception과 shutdown에서 lease 단일 반환
- Payload escape와 async continuation에서 lease 수명 동반
- Origin pipe credit 소진 시 해당 TCP sender만 block되고 job 완료 후 재개
- A의 oversize message가 B·C·D의 recv와 job admission을 pause하지 않음
- Oversize allowance 설정과 budget 기반 MAXMSGSIZE 변환이 존재하지 않음
- Completion current·peak·pending, totalMessaging와 completion queue count가 Core와 같은 의미로 노출됨
- MonitorHwmBytes가 profile 계산이나 count 변환 없이 binding으로 전달됨
- Monitor queue와 instance aggregate byte가 Core snapshot과 같은 의미로 노출됨
- Legacy MonitorHwm count 설정·환경 변수·CLI가 unknown option으로 거절됨
- A RID가 계속 backpressured여도 B·C·D의 비동기 submit과 진행이 계속됨
- A가 막히고 B가 writable인 상태에서도 A의 wake가 유실되지 않고 credit 회복 뒤 진행됨
- Framework가 Core readiness callback이나 `pendingByTarget`을 직접 소유하지 않고 binding operation만 await함
- Pipe detach, socket close, context 종료, timeout과 cancellation에서 대기 operation이 정확히 한 번 완료됨
- Backpressure 대기 중 event loop, runtime worker와 Framework socket owner mutex를 점유하지 않음
- Framework에 RID별 retry queue, ready ring과 고정 주기 submit polling이 없음
- Request deadline이 최초 수용 대기 중 연장되지 않음
- Request correlation 준비와 wire publish 순서를 Framework가 다시 구현하지 않고 binding 결과를 사용함
- Disconnect·route generation 변경·shutdown·deadline에서 비동기 operation이 정확히 한 번 완료됨
- Metrics reset에서 gauge 유지, 두 peak의 current 재기준화와 epoch counter 초기화가 보존됨
- Blocking TCP, nonblocking EAGAIN, PUB drop과 inproc 동작을 각각 검증
- 다중 Core context backend에서 budget 중복 적용 방지
- Core snapshot과 RSS/managed heap peak의 범위 차이 검증
